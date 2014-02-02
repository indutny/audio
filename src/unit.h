#ifndef SRC_UNIT_H_
#define SRC_UNIT_H_

#include "node.h"
#include "node_object_wrap.h"
#include "uv.h"
#include "channel.h"

#include <stdint.h>
#include <sys/types.h>

namespace audio {

class Unit : public node::ObjectWrap {
 public:
  enum Side {
    kInput,
    kOutput
  };

  typedef void (*IncomingCallback)(const unsigned char* data, size_t size);

  Unit();
  virtual ~Unit();
  void Init();

  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual size_t GetChannelCount(Side side) = 0;
  virtual double GetHWSampleRate(Side side) = 0;

  static void Initialize(v8::Handle<v8::Object> target);

  inline void on_incoming(IncomingCallback cb) { on_incoming_ = cb; }

  static const int kSampleRate = 16000;
  static const int kSampleSize = sizeof(int16_t);
  static const int kChunkSize = 160;

 protected:
  static const int kChannelCount = 2;

  static v8::Handle<v8::Value> New(const v8::Arguments &args);
  static v8::Handle<v8::Value> Start(const v8::Arguments &args);
  static v8::Handle<v8::Value> Stop(const v8::Arguments &args);
  static v8::Handle<v8::Value> Play(const v8::Arguments &args);

  void CommitInput(size_t channel, const int16_t* in, size_t size);
  void FlushInput();
  void RenderOutput(size_t channel, int16_t* out, size_t size);

  // AEC Thread
  static void AECThread(void* arg);
  void DoAEC();
  static void AsyncCb(uv_async_t* handle, int status);

  IncomingCallback on_incoming_;

  Channel channels_[kChannelCount];
  bool running_;

  // AEC
  uv_sem_t aec_sem_;
  uv_async_t* aec_async_;
  uv_thread_t aec_thread_;
  volatile bool destroying_;
};

}  // namespace audio

#endif  // SRC_UNIT_H_
