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

Unit::Unit() : on_incoming_(NULL), destroying_(false) {
}


void Unit::Init() {
  PaUtilRingBuffer* rings[] = { aec_in_, aec_out_, ring_in_, ring_out_ };

  size_t in_count = this->GetChannelCount(kInput);
  size_t out_count = this->GetChannelCount(kOutput);
  if (in_count > kChannelCount)
    in_count = kChannelCount;
  if (out_count > kChannelCount)
    out_count = kChannelCount;

  for (size_t i = 0; i < ARRAY_SIZE(rings); i++) {
    for (size_t j = 0; j < kChannelCount; j++) {
      PaUtil_InitializeRingBuffer(&rings[i][j],
                                  kSampleSize,
                                  kBufferCapacity,
                                  new char[kSampleSize * kBufferCapacity]);
    }
  }

  for (size_t i = 0; i < kChannelCount; i++) {
    ASSERT(0 == WebRtcAec_Create(&aec_[i]), "Failed to create AEC");
    ASSERT(0 == WebRtcAec_Init(aec_[i],
                               kSampleRate,
                               static_cast<int32_t>(GetHWSampleRate(kOutput))),
           "Failed to initialize AEC");
  }

  memset(filt1_, 0, sizeof(filt1_));
  memset(filt2_, 0, sizeof(filt2_));

  ASSERT(0 == uv_sem_init(&aec_sem_, 0), "uv_sem_init");
  ASSERT(0 == uv_thread_create(&aec_thread_, AECThread, this),
         "uv_thread_create");
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
}


void Unit::Initialize(Handle<Object> target) {
  HandleScope scope;
  Local<FunctionTemplate> tpl = FunctionTemplate::New(Unit::New);
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  NODE_SET_PROTOTYPE_METHOD(tpl, "start", Unit::Start);
  NODE_SET_PROTOTYPE_METHOD(tpl, "stop", Unit::Stop);
  NODE_SET_PROTOTYPE_METHOD(tpl, "queueForPlayback", Unit::QueueForPlayback);

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


Handle<Value> Unit::QueueForPlayback(const Arguments &args) {
  HandleScope scope;
  Unit* unit = ObjectWrap::Unwrap<Unit>(args.This());

  unit->QueueForPlayback(
      reinterpret_cast<const unsigned char*>(Buffer::Data(args[0])),
      Buffer::Length(args[0]));

  return scope.Close(Undefined());
}


int16_t* Unit::PrepareInput(size_t channel, size_t* size) {
  static int16_t buf[1024];

  // If channel is too high or we don't have enough space to process input -
  // just read everything into a static buffer
  if (channel >= ARRAY_SIZE(aec_in_) ||
      0 == PaUtil_GetRingBufferWriteAvailable(&aec_in_[channel])) {
    return buf;
  }

  void* data1;
  ring_buffer_size_t in_len1;
  void* data2;
  ring_buffer_size_t in_len2;
  PaUtil_GetRingBufferWriteRegions(&aec_in_[channel],
                                   *size,
                                   &data1,
                                   &in_len1,
                                   &data2,
                                   &in_len2);
  *size = in_len1;

  // NOTE: Second buffer is intentionally ignored
  return reinterpret_cast<int16_t*>(data1);
}


void Unit::CommitInput(size_t size) {
  // Already full, ignore
  if (0 == PaUtil_GetRingBufferWriteAvailable(&aec_in_[0]))
    return;

  for (size_t i = 0; i < ARRAY_SIZE(aec_in_); i++) {
    assert(i < ARRAY_SIZE(aec_in_));

    // Advance buffer
    PaUtil_AdvanceRingBufferWriteIndex(&aec_in_[i], size);
  }

  // Notify AEC thread about read
  uv_sem_post(&aec_sem_);
}


void Unit::RenderOutput(size_t channel, int16_t* out, size_t size) {
  ring_buffer_size_t avail;
  if (channel >= ARRAY_SIZE(ring_out_)) {
    avail = 0;
  } else {
    avail = PaUtil_ReadRingBuffer(&ring_out_[channel], out, size);
  }

  // Zero-ify rest
  for (size_t i = avail; i < size; i++)
    out[i] = 0.0;

  // Notify AEC thread about write
  PaUtil_WriteRingBuffer(&aec_out_[channel], out, size);
  uv_sem_post(&aec_sem_);
}


void Unit::AECThread(void* arg) {
  Unit* unit = reinterpret_cast<Unit*>(arg);

  while (true) {
    uv_sem_wait(&unit->aec_sem_);
    if (unit->destroying_)
      return;

    unit->DoAEC();
  }
}


void Unit::DoAEC() {
  int16_t in_l[kChunkSize];
  int16_t in_h[kChunkSize];
  int16_t out_l[kChunkSize];
  int16_t out_h[kChunkSize];

  for (size_t i = 0; i < kChannelCount; i++) {
    int16_t* data1;
    ring_buffer_size_t in_len1;
    void* data2;
    ring_buffer_size_t in_len2;

    // Feed playback data into AEC
    while (PaUtil_GetRingBufferReadAvailable(&aec_in_[i]) >= kChunkSize ||
           PaUtil_GetRingBufferReadAvailable(&aec_out_[i]) >= kChunkSize) {
      if (PaUtil_GetRingBufferReadAvailable(&aec_out_[i]) >= kChunkSize) {
        PaUtil_GetRingBufferReadRegions(&aec_out_[i],
                                        kChunkSize,
                                        reinterpret_cast<void**>(&data1),
                                        &in_len1,
                                        &data2,
                                        &in_len2);

        ASSERT(0 == WebRtcAec_BufferFarend(aec_[i], data1, in_len1),
               "Failed to queue AEC far end");
      }

      if (PaUtil_GetRingBufferReadAvailable(&aec_in_[i]) >= kChunkSize) {
        // Feed capture data into AEC
        PaUtil_GetRingBufferReadRegions(&aec_in_[i],
                                        kChunkSize,
                                        reinterpret_cast<void**>(&data1),
                                        &in_len1,
                                        &data2,
                                        &in_len2);
        ASSERT(in_len1 == kChunkSize, "Odd read");

        // Split signal
        WebRtcSpl_AnalysisQMF(data1, in_len1, in_l, in_h, filt1_[i], filt2_[i]);

        // Apply AEC
        ASSERT(0 == WebRtcAec_Process(aec_[i],
                                      in_l,
                                      in_h,
                                      out_l,
                                      out_h,
                                      kChunkSize,
                                      0,
                                      0),
               "Failed to queue AEC near end");

        // Join signal
        int16_t out[1024];
        WebRtcSpl_SynthesisQMF(out_l,
                               out_h,
                               kChunkSize,
                               out,
                               filt1_[i],
                               filt2_[i]);
      }
    }
  }
}

}  // namespace audio
