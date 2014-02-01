#ifndef SRC_UNIT_H_
#define SRC_UNIT_H_

#include "node.h"
#include "node_object_wrap.h"

namespace audio {

class Unit : public node::ObjectWrap {
 public:
  enum Side {
    kInput,
    kOutput
  };

  typedef void (*IncomingCallback)(const unsigned char* data, size_t size);

  Unit() : on_incoming_(NULL) {
  }

  virtual ~Unit() = 0;

  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual void QueueForPlayback(const unsigned char* data, size_t size) = 0;
  virtual int GetChannelCount(Side side) = 0;

  static void Initialize(v8::Handle<v8::Object> target);

  inline void on_incoming(IncomingCallback cb) { on_incoming_ = cb; }

 protected:
  static v8::Handle<v8::Value> New(const v8::Arguments &args);
  static v8::Handle<v8::Value> Start(const v8::Arguments &args);
  static v8::Handle<v8::Value> Stop(const v8::Arguments &args);
  static v8::Handle<v8::Value> QueueForPlayback(const v8::Arguments &args);

  float* PrepareInput(size_t channel, size_t size);
  void CommitInput(size_t size);
  void RenderOutput(size_t channel, float* out, size_t size);

  IncomingCallback on_incoming_;
};

}  // namespace audio

#endif  // SRC_UNIT_H_
