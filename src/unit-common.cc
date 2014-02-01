#include "unit.h"
#if defined(__APPLE__)
# include "mac/unit-mac.h"
#endif

#include "node_buffer.h"

using namespace node;
using namespace v8;

namespace audio {

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

  if (args.Length() < 1) {
    return ThrowException(Exception::Error(String::New(
        "Not enough arguments")));
  }

  double sample_rate = args[0]->NumberValue();
  Unit* unit = new PlatformUnit(sample_rate);
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


float* Unit::PrepareInput(size_t channel, size_t size) {
  static float buf[1024];

  return buf;
}


void Unit::CommitInput(size_t size) {
}


void Unit::RenderOutput(size_t channel, float* out, size_t size) {
  for (size_t i = 0; i < size; i++)
    out[i] = 0.0;
}

}  // namespace audio
