#include "unit-mac.h"
#include "common.h"

#include <assert.h>
#include <math.h>

namespace audio {

double PlatformUnit::sample_rate_in_;
double PlatformUnit::sample_rate_out_;
AudioDeviceID PlatformUnit::in_device_ = kAudioObjectUnknown;
AudioDeviceID PlatformUnit::out_device_ = kAudioObjectUnknown;
AudioDeviceID PlatformUnit::plugin_ = kAudioObjectUnknown;
AudioDeviceID PlatformUnit::aggregate_ = kAudioObjectUnknown;


PlatformUnit::PlatformUnit() : in_channels_(0), out_channels_(0) {
  // Find Remote IO audio component
  AudioComponentDescription desc;

  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  desc.componentFlags = 0;
  desc.componentFlagsMask = 0;

  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  ASSERT(comp != NULL, "HALOutput AudioComponent is not present in your OS");

  // Instantiate component (AudioUnit)
  OSStatus err = AudioComponentInstanceNew(comp, &unit_);
  OSERR_CHECK(err, "Failed to instantiate AudioComponent");

  // Enable IO on it
  static int scopes[] = { kAudioUnitScope_Input, kAudioUnitScope_Output };
  static int elems[] = { 1, 0 };

  for (size_t i = 0; i < ARRAY_SIZE(scopes); i++) {
    UInt32 enable = 1;
    err = AudioUnitSetProperty(unit_,
                               kAudioOutputUnitProperty_EnableIO,
                               scopes[i],
                               elems[i],
                               &enable,
                               sizeof(enable));
    OSERR_CHECK(err, "Failed to enable IO on AudioUnit");
  }

  // Set current device
  AudioDeviceID device = GetDevice();
  err = AudioUnitSetProperty(unit_,
                             kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global,
                             0,
                             &device,
                             sizeof(device));
  OSERR_CHECK(err, "Failed to set AudioUnit device");

  // Set input/output format
  // IMPORTANT NOTE:
  // Elements are reversed here, because we are setting a format of their
  // sources.
  for (size_t i = 0; i < ARRAY_SIZE(scopes); i++) {
    AudioStreamBasicDescription desc;
    UInt32 size = sizeof(desc);

    // Get number of channels
    err = AudioUnitGetProperty(unit_,
                               kAudioUnitProperty_StreamFormat,
                               scopes[i],
                               elems[i] == 0 ? 1 : 0,
                               &desc,
                               &size);
    OSERR_CHECK(err, "Failed to get input/output format");
    ASSERT(size == sizeof(desc), "Stream format size mismatch");

    if (desc.mChannelsPerFrame > kChannelCount)
      desc.mChannelsPerFrame = kChannelCount;

    if (scopes[i] == kAudioUnitScope_Input)
      in_channels_ = desc.mChannelsPerFrame;
    else
      out_channels_ = desc.mChannelsPerFrame;

    // Set the rest of format
    desc.mSampleRate = kSampleRate;
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = kAudioFormatFlagsNativeEndian |
                        kAudioFormatFlagIsSignedInteger |
                        kAudioFormatFlagIsPacked |
                        kAudioFormatFlagIsNonInterleaved;
    desc.mBytesPerPacket = kSampleSize;
    desc.mFramesPerPacket = 1;
    desc.mBytesPerFrame = kSampleSize;
    desc.mBitsPerChannel = kSampleSize * 8;
    desc.mReserved = 0;

    err = AudioUnitSetProperty(unit_,
                               kAudioUnitProperty_StreamFormat,
                               scopes[i],
                               elems[i] == 0 ? 1 : 0,
                               &desc,
                               sizeof(desc));
    OSERR_CHECK(err, "Failed to set input/output format");

    // Set buffer size
    UInt32 chunk_size = kChunkSize;
    err = AudioUnitSetProperty(unit_,
                               kAudioDevicePropertyBufferFrameSize,
                               scopes[i],
                               elems[i],
                               &chunk_size,
                               sizeof(chunk_size));
    OSERR_CHECK(err, "Failed to set buffer frame size");
  }

  // Set HW Sample rate
  SetHWSampleRate(kInput, kSampleRate);

  // Set callbacks
  AURenderCallbackStruct cb;

  cb.inputProcRefCon = this;
  cb.inputProc = InputCallback;
  err = AudioUnitSetProperty(unit_,
                             kAudioOutputUnitProperty_SetInputCallback,
                             kAudioUnitScope_Global,
                             0,
                             &cb,
                             sizeof(cb));
  OSERR_CHECK(err, "Failed to set input callback");

  cb.inputProcRefCon = this;
  cb.inputProc = RenderCallback;
  err = AudioUnitSetProperty(unit_,
                             kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Global,
                             0,
                             &cb,
                             sizeof(cb));
  OSERR_CHECK(err, "Failed to set input callback");

  // Finally, initalize it!
  err = AudioUnitInitialize(unit_);
  OSERR_CHECK(err, "Failed to initialize AudioUnit");

  // Allocate buffer
  buffer_ = AllocateBuffer(kInput);

  // Initialize common unit
  Init();
}


PlatformUnit::~PlatformUnit() {
  AudioComponentInstanceDispose(unit_);
  DestroyBuffer(buffer_);
  buffer_ = NULL;
}


void PlatformUnit::Start() {
  OSStatus err = AudioOutputUnitStart(unit_);
  OSERR_CHECK(err, "Failed to start AudioUnit");
}


void PlatformUnit::Stop() {
  OSStatus err = AudioOutputUnitStop(unit_);
  OSERR_CHECK(err, "Failed to stop AudioUnit");
}


size_t PlatformUnit::GetChannelCount(Unit::Side side) {
  if (side == kInput)
    return in_channels_;
  else
    return out_channels_;
}


AudioBufferList* PlatformUnit::AllocateBuffer(Unit::Side side) {
  size_t channels = GetChannelCount(side);
  size_t buffer_size = sizeof(*buffer_) + channels * sizeof(AudioBuffer);

  AudioBufferList* res =
      reinterpret_cast<AudioBufferList*>(new char[buffer_size]);
  res->mNumberBuffers = channels;

  for (size_t i = 0; i < channels; i++) {
    int16_t* data = new int16_t[kChunkSize];

    res->mBuffers[i].mNumberChannels = 1;
    res->mBuffers[i].mData = data;
    res->mBuffers[i].mDataByteSize = sizeof(*data) * kChunkSize;
  }

  return res;
}


void PlatformUnit::DestroyBuffer(AudioBufferList* buffer) {
  for (size_t i = 0; i < buffer->mNumberBuffers; i++) {
    int16_t* data = reinterpret_cast<int16_t*>(buffer->mBuffers[i].mData);
    buffer->mBuffers[i].mData = NULL;
    delete[] data;
  }
  delete[] buffer;
}


OSStatus PlatformUnit::InputCallback(void* arg,
                                     AudioUnitRenderActionFlags* flags,
                                     const AudioTimeStamp* ts,
                                     UInt32 bus,
                                     UInt32 frame_count,
                                     AudioBufferList* list) {
  PlatformUnit* unit = reinterpret_cast<PlatformUnit*>(arg);

  OSStatus err = AudioUnitRender(unit->unit_,
                                 flags,
                                 ts,
                                 bus,
                                 frame_count,
                                 unit->buffer_);
  if (err != noErr)
    return err;

  // Commit input for every channel
  for (size_t i = 0; i < unit->buffer_->mNumberBuffers; i++) {
    int16_t* data =
        reinterpret_cast<int16_t*>(unit->buffer_->mBuffers[i].mData);
    unit->CommitInput(i, data, frame_count);
  }

  unit->FlushInput();

  return noErr;
}


OSStatus PlatformUnit::RenderCallback(void* arg,
                                      AudioUnitRenderActionFlags* flags,
                                      const AudioTimeStamp* ts,
                                      UInt32 bus,
                                      UInt32 frame_count,
                                      AudioBufferList* list) {
  PlatformUnit* unit = reinterpret_cast<PlatformUnit*>(arg);
  UInt32 i;

  for (i = 0; i < list->mNumberBuffers; i++) {
    AudioBuffer* buf = &list->mBuffers[i];

    unit->RenderOutput(i,
                       reinterpret_cast<int16_t*>(buf->mData),
                       buf->mDataByteSize / kSampleSize);
  }

  return noErr;
}


double PlatformUnit::GetHWSampleRate(Unit::Side side) {
  double* sample_rate;

  sample_rate = side == kInput ? &sample_rate_in_ : &sample_rate_out_;
  if (*sample_rate != 0.0)
    return *sample_rate;

  static AudioObjectPropertyAddress addr = {
    kAudioDevicePropertyNominalSampleRate,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  UInt32 size = sizeof(*sample_rate);
  OSStatus err = AudioObjectGetPropertyData(GetDevice(side),
                                            &addr,
                                            0,
                                            NULL,
                                            &size,
                                            sample_rate);
  OSERR_CHECK(err, "Failed to obtain default device's sample rate");
  ASSERT(size == sizeof(*sample_rate), "Sample rate size mismatch");

  return *sample_rate;
}


void PlatformUnit::SetHWSampleRate(Unit::Side side, double sample_rate) {
  static AudioObjectPropertyAddress addr = {
    kAudioDevicePropertyNominalSampleRate,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  Float64 rate = sample_rate;
  UInt32 size = sizeof(rate);
  OSStatus err = AudioObjectSetPropertyData(GetDevice(side),
                                            &addr,
                                            0,
                                            NULL,
                                            size,
                                            &rate);
  OSERR_CHECK(err, "Failed to set device's sample rate");
}


AudioDeviceID PlatformUnit::GetDevice(Side side) {
  AudioDeviceID* device = side == kInput ? &in_device_ : &out_device_;

  // Probe cache
  if (*device != kAudioObjectUnknown)
    return *device;

  AudioObjectPropertyAddress addr = {
    side == kInput ? kAudioHardwarePropertyDefaultInputDevice :
                     kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  UInt32 size = sizeof(*device);
  OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                            &addr,
                                            0,
                                            NULL,
                                            &size,
                                            device);
  OSERR_CHECK(err, "Failed to obtain default input device");
  ASSERT(size == sizeof(*device), "Device size mismatch");
  ASSERT(*device != kAudioObjectUnknown, "Device is unknown");

  return *device;
}


AudioDeviceID PlatformUnit::GetDevice() {
  if (aggregate_ != kAudioObjectUnknown)
    return aggregate_;

  AudioDeviceID in = GetDevice(kInput);
  AudioDeviceID out = GetDevice(kOutput);

  // The same device (quite unlikely)
  if (in == out)
    return in;

  // Set up settings dict for aggregate device
  CFMutableDictionaryRef dict = GetAggregateDictionary();

  // Create aggregate device
  static AudioObjectPropertyAddress addr = {
    kAudioPlugInCreateAggregateDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };
  UInt32 size = sizeof(aggregate_);
  OSStatus err = AudioObjectGetPropertyData(GetPlugin(),
                                            &addr,
                                            sizeof(dict),
                                            &dict,
                                            &size,
                                            &aggregate_);
  OSERR_CHECK(err, "Failed to create aggregate device");
  ASSERT(size == sizeof(aggregate_), "Aggregate size mismatch");
  CFRelease(dict);

  // Add subdevices
  AddSubdevices(aggregate_, in, out);

  return aggregate_;
}


AudioDeviceID PlatformUnit::GetPlugin() {
  if (plugin_ != kAudioObjectUnknown)
    return plugin_;

  static AudioObjectPropertyAddress addr = {
    kAudioHardwarePropertyTranslateBundleIDToPlugIn,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  CFStringRef uuid = CFSTR("com.apple.audio.CoreAudio");
  UInt32 size = sizeof(plugin_);
  OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                            &addr,
                                            sizeof(uuid),
                                            &uuid,
                                            &size,
                                            &plugin_);
  OSERR_CHECK(err, "Failed to fetch com.apple.audio.CoreAudio plugin");
  ASSERT(size == sizeof(plugin_), "Plugin size mismatch");

  return plugin_;
}


CFMutableDictionaryRef PlatformUnit::GetAggregateDictionary() {
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
      NULL,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  ASSERT(dict != NULL, "Failed to allocate aggregate settings dict");

  CFDictionarySetValue(dict,
                       CFSTR(kAudioAggregateDeviceUIDKey),
                       CFSTR("com.node-js.audio"));
  CFDictionarySetValue(dict,
                       CFSTR(kAudioAggregateDeviceNameKey),
                       CFSTR("Node.js audio module"));

  int ion = 1;
  CFNumberRef on = CFNumberCreate(NULL, kCFNumberIntType, &ion);
  ASSERT(on != NULL, "Failed to allocate `one`");
  CFDictionarySetValue(dict,
                       CFSTR(kAudioAggregateDeviceIsPrivateKey),
                       on);
  CFRelease(on);

  return dict;
}


CFStringRef PlatformUnit::GetDeviceUID(AudioDeviceID device) {
  static AudioObjectPropertyAddress addr = {
    kAudioDevicePropertyDeviceUID,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  CFStringRef uuid;
  UInt32 size = sizeof(uuid);
  OSStatus err = AudioObjectGetPropertyData(device,
                                            &addr,
                                            0,
                                            NULL,
                                            &size,
                                            &uuid);
  OSERR_CHECK(err, "Failed to fetch device UID");
  ASSERT(size == sizeof(uuid), "Device UID size mismatch");

  return uuid;
}


void PlatformUnit::AddSubdevices(AudioDeviceID aggr,
                                 AudioDeviceID in,
                                 AudioDeviceID out) {
  CFMutableArrayRef subdevices =
      CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  ASSERT(subdevices != NULL, "Failed to allocate subdevice list");

  CFStringRef in_uid = GetDeviceUID(in);
  CFStringRef out_uid = GetDeviceUID(out);
  CFArrayAppendValue(subdevices, in_uid);
  CFArrayAppendValue(subdevices, out_uid);
  CFRelease(out_uid);

  static AudioObjectPropertyAddress subdevice_addr = {
    kAudioAggregateDevicePropertyFullSubDeviceList,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  UInt32 size = sizeof(subdevices);
  OSStatus err = AudioObjectSetPropertyData(aggr,
                                            &subdevice_addr,
                                            0,
                                            NULL,
                                            size,
                                            &subdevices);
  OSERR_CHECK(err, "Failed to set subdevice list");
  CFRelease(subdevices);

  // Make input stream a master
  static AudioObjectPropertyAddress master_addr = {
    kAudioAggregateDevicePropertyMasterSubDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMaster
  };

  size = sizeof(in_uid);
  err = AudioObjectSetPropertyData(aggr,
                                   &master_addr,
                                   0,
                                   NULL,
                                   size,
                                   &in_uid);
  OSERR_CHECK(err, "Failed to set aggregate's master");
  CFRelease(in_uid);
}

}  // namespace audio
