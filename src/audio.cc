#include "node.h"
#include "uv.h"

#include "unit.h"

using namespace node;
using namespace v8;

namespace audio {

static Handle<Value> Initialize(Handle<Object> target) {
  HandleScope scope;

  Unit::Initialize(target);

  return Null();
}

}  // namespace audio

NODE_MODULE(audio, audio::Initialize);
