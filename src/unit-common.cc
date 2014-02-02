#include "unit.h"
#if defined(__APPLE__)
# include "mac/unit-mac.h"
#endif

#include "common.h"
#include "node_buffer.h"
#include "aec/include/echo_cancellation.h"
#include "signal_processing/include/signal_processing_library.h"

#include <assert.h>

using namespace node;
using namespace v8;

namespace audio {

Unit::Unit() : on_incoming_(NULL), running_(false), destroying_(false) {
}


void Unit::Init() {
  PaUtilRingBuffer* rings[] = { aec_in_, aec_out_, ring_in_, ring_out_ };

  size_t in_count = this->GetChannelCount(kInput);
  size_t out_count = this->GetChannelCount(kOutput);
  if (in_count > kChannelCount)
    in_count = kChannelCount;
  if (out_count > kChannelCount)
    out_count = kChannelCount;

  // Initialize buffers
  for (size_t i = 0; i < ARRAY_SIZE(rings); i++) {
    for (size_t j = 0; j < kChannelCount; j++) {
      PaUtil_InitializeRingBuffer(&rings[i][j],
                                  kSampleSize,
                                  kBufferCapacity,
                                  new char[kSampleSize * kBufferCapacity]);
    }
  }

  for (size_t i = 0; i < kChannelCount; i++) {
    // Initailize AEC
    ASSERT(0 == WebRtcAec_Create(&aec_[i]), "Failed to create AEC");
    ASSERT(0 == WebRtcAec_Init(aec_[i],
                               kSampleRate,
                               static_cast<int32_t>(GetHWSampleRate(kOutput))),
           "Failed to initialize AEC");

    // Initialize NS
    ASSERT(0 == WebRtcNs_Create(&ns_[i]), "Failed to create NS");
    ASSERT(0 == WebRtcNs_Init(ns_[i], kSampleRate / 2), "Failed to init NS");
  }

  // Initialize QMF filter states
  memset(filters_.a_lo, 0, sizeof(filters_.a_lo));
  memset(filters_.a_hi, 0, sizeof(filters_.a_hi));
  memset(filters_.s_lo, 0, sizeof(filters_.s_lo));
  memset(filters_.s_hi, 0, sizeof(filters_.s_hi));

  // Initialize AEC thread
  ASSERT(0 == uv_sem_init(&aec_sem_, 0), "uv_sem_init");
  aec_async_ = new uv_async_t;
  aec_async_->data = this;
  ASSERT(0 == uv_async_init(uv_default_loop(), aec_async_, AsyncCb),
         "uv_async_init");
  ASSERT(0 == uv_thread_create(&aec_thread_, AECThread, this),
         "uv_thread_create");
}


static void CloseCb(uv_handle_t* handle) {
  delete handle;
}


Unit::~Unit() {
  // Terminate AEC thread
  destroying_ = true;
  uv_sem_post(&aec_sem_);
  uv_thread_join(&aec_thread_);

  PaUtilRingBuffer* rings[] = { aec_in_, aec_out_, ring_in_, ring_out_ };
  for (size_t i = 0; i < ARRAY_SIZE(rings); i++) {
    for (size_t j = 0; j < kChannelCount; j++) {
      delete[] rings[i][j].buffer;
      rings[i][j].buffer = NULL;
    }
  }

  for (size_t i = 0; i < kChannelCount; i++) {
    ASSERT(0 == WebRtcAec_Free(aec_[i]), "Failed to destroy AEC");
    aec_[i] = NULL;
  }

  uv_sem_destroy(&aec_sem_);
  uv_close(reinterpret_cast<uv_handle_t*>(aec_async_), CloseCb);

  for (size_t i = 0; i < kChannelCount; i++) {
    ASSERT(0 == WebRtcAec_Free(aec_[i]), "Failed to free AEC");
    ASSERT(0 == WebRtcNs_Free(ns_[i]), "Failed to free NS");
  }
}


void Unit::Initialize(Handle<Object> target) {
  HandleScope scope;
  Local<FunctionTemplate> tpl = FunctionTemplate::New(Unit::New);
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  NODE_SET_PROTOTYPE_METHOD(tpl, "start", Unit::Start);
  NODE_SET_PROTOTYPE_METHOD(tpl, "stop", Unit::Stop);
  NODE_SET_PROTOTYPE_METHOD(tpl, "play", Unit::Play);

  target->Set(String::NewSymbol("Unit"), tpl->GetFunction());
}


Handle<Value> Unit::New(const Arguments &args) {
  HandleScope scope;

  Unit* unit = new PlatformUnit();
  unit->Wrap(args.This());

  return scope.Close(args.This());
}


Handle<Value> Unit::Start(const Arguments &args) {
  HandleScope scope;
  Unit* unit = ObjectWrap::Unwrap<Unit>(args.This());

  unit->Start();

  return scope.Close(Undefined());
}


Handle<Value> Unit::Stop(const Arguments &args) {
  HandleScope scope;
  Unit* unit = ObjectWrap::Unwrap<Unit>(args.This());

  unit->Stop();

  return scope.Close(Undefined());
}


Handle<Value> Unit::Play(const Arguments &args) {
  HandleScope scope;
  Unit* unit = ObjectWrap::Unwrap<Unit>(args.This());

  size_t channel = args[0]->IntegerValue();
  PaUtil_WriteRingBuffer(&unit->ring_out_[channel],
                         Buffer::Data(args[1]),
                         Buffer::Length(args[1]) / kSampleSize);

  return scope.Close(Undefined());
}


void Unit::CommitInput(size_t channel, const int16_t* in, size_t size) {
  // TODO(indutny): Support output/input channel count mismatch
  // Already full, ignore
  if (0 == PaUtil_WriteRingBuffer(&aec_in_[channel], in, size))
    return;
}


void Unit::FlushInput() {
  uv_sem_post(&aec_sem_);
}


void Unit::RenderOutput(size_t channel, int16_t* out, size_t size) {
  ring_buffer_size_t avail;
  avail = PaUtil_ReadRingBuffer(&ring_out_[channel], out, size);

  // Zero-ify rest
  for (size_t i = avail; i < size; i++)
    out[i] = 0;

  // Notify AEC thread about write
  PaUtil_WriteRingBuffer(&aec_out_[channel], out, size);
}


void Unit::AECThread(void* arg) {
  Unit* unit = reinterpret_cast<Unit*>(arg);

  while (true) {
    uv_sem_wait(&unit->aec_sem_);

    if (unit->destroying_)
      break;

    unit->DoAEC();
  }
}


void Unit::DoAEC() {
  int16_t buf[kChunkSize];
  ring_buffer_size_t avail_out = PaUtil_GetRingBufferReadAvailable(
      &aec_out_[GetChannelCount(kOutput) - 1]);
  ring_buffer_size_t avail_in = PaUtil_GetRingBufferReadAvailable(
      &aec_in_[GetChannelCount(kInput)- 1]);

  for (size_t i = 0; i < kChannelCount; i++) {
    ring_buffer_size_t avail;

    // Feed playback data into AEC
    if (avail_out >= kChunkSize) {
      avail = PaUtil_ReadRingBuffer(&aec_out_[i], buf, kChunkSize);
      ASSERT(avail == kChunkSize, "Read less than expected");
      ASSERT(0 == WebRtcAec_BufferFarend(aec_[i], buf, kChunkSize),
             "Failed to queue AEC far end");
    }

    if ((size_t) avail_in >= ARRAY_SIZE(buf)) {
      // Feed capture data into AEC
      avail = PaUtil_ReadRingBuffer(&aec_in_[i], buf, ARRAY_SIZE(buf));
      ASSERT(avail == ARRAY_SIZE(buf), "Read less than expected");

      int16_t lo[ARRAY_SIZE(buf) / 2];
      int16_t hi[ARRAY_SIZE(lo)];

      // Split signal
      WebRtcSpl_AnalysisQMF(buf,
                            ARRAY_SIZE(buf),
                            lo,
                            hi,
                            filters_.a_lo[i],
                            filters_.a_hi[i]);

      // Apply AEC
      ASSERT(0 == WebRtcAec_Process(aec_[i],
                                    lo,
                                    hi,
                                    lo,
                                    hi,
                                    ARRAY_SIZE(lo),
                                    0,
                                    0),
             "Failed to queue AEC near end");

      // Apply NS
      ASSERT(0 == WebRtcNs_Process(ns_[i], lo, hi, lo, hi),
             "Failed to apply NS");

      // Join signal
      WebRtcSpl_SynthesisQMF(lo,
                             hi,
                             ARRAY_SIZE(lo),
                             buf,
                             filters_.s_lo[i],
                             filters_.s_hi[i]);

      // Write it out
      PaUtil_WriteRingBuffer(&ring_in_[i], buf, ARRAY_SIZE(buf));
    }
  }

  // Communicate back to the event loop
  uv_async_send(aec_async_);
}


void Unit::AsyncCb(uv_async_t* handle, int status) {
  if (status != 0)
    return;

  HandleScope scope;
  Unit* unit = reinterpret_cast<Unit*>(handle->data);

  size_t channels = unit->GetChannelCount(kInput);
  int16_t buf[kChunkSize];

  while (PaUtil_GetRingBufferReadAvailable(
             &unit->ring_in_[channels - 1]) > 0) {
    for (size_t i = 0; i < channels; i++) {
      ring_buffer_size_t avail;

      avail = PaUtil_ReadRingBuffer(&unit->ring_in_[i], buf, ARRAY_SIZE(buf));
      ASSERT(avail == ARRAY_SIZE(buf), "Read less than expected");

      Buffer* raw = Buffer::New(reinterpret_cast<char*>(buf), sizeof(buf));
      Local<Value> buf = Local<Value>::New(raw->handle_);

      Local<Value> argv[] = { Integer::New(i), buf };
      MakeCallback(unit->handle_, "oninput", ARRAY_SIZE(argv), argv);
    }
  }
}

}  // namespace audio
