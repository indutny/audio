#include "unit.h"
#if defined(__APPLE__)
# include "mac/unit-mac.h"
#endif

#include "common.h"
#include "node_buffer.h"

#include <assert.h>

using namespace node;
using namespace v8;

namespace audio {

Unit::Unit() : on_incoming_(NULL), running_(false), destroying_(false) {
}


void Unit::Init() {
  size_t in_count = this->GetChannelCount(kInput);
  size_t out_count = this->GetChannelCount(kOutput);
  if (in_count > kChannelCount)
    in_count = kChannelCount;
  if (out_count > kChannelCount)
    out_count = kChannelCount;

  for (size_t i = 0; i < ARRAY_SIZE(channels_); i++)
    channels_[i].Init(this);

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

  uv_sem_destroy(&aec_sem_);
  uv_close(reinterpret_cast<uv_handle_t*>(aec_async_), CloseCb);
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
  Channel* chan = &unit->channels_[channel];
  PaUtil_WriteRingBuffer(&chan->io_.out,
                         Buffer::Data(args[1]),
                         Buffer::Length(args[1]) / kSampleSize);

  return scope.Close(Undefined());
}


void Unit::CommitInput(size_t channel, const int16_t* in, size_t size) {
  Channel* chan = &channels_[channel];

  // TODO(indutny): Support output/input channel count mismatch
  // Already full, ignore
  if (0 == PaUtil_WriteRingBuffer(&chan->aec_.in, in, size))
    return;
}


void Unit::FlushInput() {
  uv_sem_post(&aec_sem_);
}


void Unit::RenderOutput(size_t channel, int16_t* out, size_t size) {
  Channel* chan = &channels_[channel];
  ring_buffer_size_t avail;
  avail = PaUtil_ReadRingBuffer(&chan->io_.out, out, size);

  // Zero-ify rest
  for (size_t i = avail; i < size; i++)
    out[i] = 0;

  // Notify AEC thread about write
  PaUtil_WriteRingBuffer(&chan->aec_.out, out, size);
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
  Channel* last_in = &channels_[GetChannelCount(kInput) - 1];
  Channel* last_out = &channels_[GetChannelCount(kOutput) - 1];
  ring_buffer_size_t avail_in =
      PaUtil_GetRingBufferReadAvailable(&last_in->aec_.out);
  ring_buffer_size_t avail_out =
      PaUtil_GetRingBufferReadAvailable(&last_out->aec_.out);

  for (size_t i = 0; i < kChannelCount; i++)
    channels_[i].Cycle(avail_in, avail_out);

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

  Channel* last = &unit->channels_[channels - 1];
  while (PaUtil_GetRingBufferReadAvailable(&last->io_.in) > 0) {
    for (size_t i = 0; i < channels; i++) {
      ring_buffer_size_t avail;
      Channel* chan = &unit->channels_[i];

      avail = PaUtil_ReadRingBuffer(&chan->io_.in, buf, ARRAY_SIZE(buf));
      ASSERT(avail == ARRAY_SIZE(buf), "Read less than expected");

      Buffer* raw = Buffer::New(reinterpret_cast<char*>(buf), sizeof(buf));
      Local<Value> buf = Local<Value>::New(raw->handle_);

      Local<Value> argv[] = { Integer::New(i), buf };
      MakeCallback(unit->handle_, "oninput", ARRAY_SIZE(argv), argv);
    }
  }
}

}  // namespace audio
