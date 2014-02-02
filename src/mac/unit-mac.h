#ifndef SRC_MAC_UNIT_MAC_H_
#define SRC_MAC_UNIT_MAC_H_

#include "unit.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreServices/CoreServices.h>
#include <AudioUnit/AudioUnit.h>

namespace audio {

class PlatformUnit : public Unit {
 public:
  PlatformUnit();
  ~PlatformUnit();

  void Start();
  void Stop();
  void QueueForPlayback(const unsigned char* data, size_t size);
  size_t GetChannelCount(Side side);
  double GetHWSampleRate(Unit::Side side);

 protected:
  static AudioDeviceID GetDevice();
  static AudioDeviceID GetDevice(Side side);
  static AudioDeviceID GetPlugin();
  static CFMutableDictionaryRef GetAggregateDictionary();
  static CFStringRef GetDeviceUID(AudioDeviceID device);
  static void AddSubdevices(AudioDeviceID aggr,
                            AudioDeviceID in,
                            AudioDeviceID out);

  static OSStatus InputCallback(void* arg,
                                AudioUnitRenderActionFlags* flags,
                                const AudioTimeStamp* ts,
                                UInt32 bus,
                                UInt32 frame_count,
                                AudioBufferList* list);
  static OSStatus RenderCallback(void* arg,
                                 AudioUnitRenderActionFlags* flags,
                                 const AudioTimeStamp* ts,
                                 UInt32 bus,
                                 UInt32 frame_count,
                                 AudioBufferList* list);

  AudioUnit unit_;
  size_t in_channels_;
  size_t out_channels_;
  AudioBufferList* buffer_;

  static double sample_rate_in_;
  static double sample_rate_out_;
  static AudioDeviceID in_device_;
  static AudioDeviceID out_device_;
  static AudioDeviceID plugin_;
  static AudioDeviceID aggregate_;
};

}  // namespace audio

#endif  // SRC_MAC_UNIT_MAC_H_
