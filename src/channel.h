#ifndef SRC_CHANNEL_H_
#define SRC_CHANNEL_H_

#include "ns/include/noise_suppression.h"
#include "pa_ringbuffer.h"

#include <stdint.h>
#include <sys/types.h>

namespace audio {

// Forward declaration
class Unit;

class Channel {
 public:
  Channel();
  ~Channel();

  void Init(Unit* unit);

  void Cycle(ring_buffer_size_t avail_in, ring_buffer_size_t avail_out);

  // IO
  struct {
    PaUtilRingBuffer in;
    PaUtilRingBuffer out;
    void* handle;
  } aec_;
  struct {
    PaUtilRingBuffer in;
    PaUtilRingBuffer out;
  } io_;

 protected:
  static const int kBufferCapacity = 16 * 1024;  // in samples

  void AEC(int16_t* lo, int16_t* hi, size_t len);
  void PreAGC(int16_t* lo, int16_t* hi, size_t len);
  void PostAGC(int16_t* lo, int16_t* hi, size_t len);
  void NS(int16_t* lo, int16_t* hi);

  // AEC
  struct {
    int32_t a_lo[6];
    int32_t a_hi[6];
    int32_t s_lo[6];
    int32_t s_hi[6];
  } filters_;
  bool has_echo_;

  // AGC
  void* agc_;
  int32_t agc_level_;

  // NS
  NsHandle* ns_;
};

} // namespace audio

#endif  // SRC_CHANNEL_H_
