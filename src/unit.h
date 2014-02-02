#ifndef SRC_UNIT_H_
#define SRC_UNIT_H_

#include "node.h"
#include "node_object_wrap.h"
#include "pa_ringbuffer.h"

#include <stdint.h>

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
  virtual void QueueForPlayback(const unsigned char* data, size_t size) = 0;
  virtual size_t GetChannelCount(Side side) = 0;
  virtual double GetHWSampleRate(Side side) = 0;

  static void Initialize(v8::Handle<v8::Object> target);

  inline void on_incoming(IncomingCallback cb) { on_incoming_ = cb; }

 protected:
  static const int kSampleRate = 8000;
  static const int kSampleSize = sizeof(int16_t);
  static const int kChunkSize = kSampleRate / 100;
  static const int kBufferCapacity = 2048;  // in samples
  static const int kChannelCount = 2;

  static v8::Handle<v8::Value> New(const v8::Arguments &args);
  static v8::Handle<v8::Value> Start(const v8::Arguments &args);
  static v8::Handle<v8::Value> Stop(const v8::Arguments &args);
  static v8::Handle<v8::Value> QueueForPlayback(const v8::Arguments &args);

  void CommitInput(size_t channel, const int16_t* in, size_t size);
  void RenderOutput(size_t channel, int16_t* out, size_t size);
  void Flush();

  // AEC Thread
  static void AECThread(void* arg);
  void DoAEC();

  IncomingCallback on_incoming_;

  PaUtilRingBuffer aec_in_[kChannelCount];
  PaUtilRingBuffer aec_out_[kChannelCount];
  PaUtilRingBuffer ring_in_[kChannelCount];
  PaUtilRingBuffer ring_out_[kChannelCount];

  // AEC
  void* aec_[kChannelCount];
  int32_t filt1_[kChannelCount][6];
  int32_t filt2_[kChannelCount][6];
  uv_sem_t aec_sem_;
  uv_thread_t aec_thread_;
  volatile bool destroying_;
};

}  // namespace audio

#endif  // SRC_UNIT_H_
