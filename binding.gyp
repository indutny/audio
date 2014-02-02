{
  "targets": [{
    "target_name": "audio",

    "variables": {
      "library": "static_library",
    },

    "dependencies": [
      "deps/aec/aec.gyp:aec",
      "deps/aec/aec.gyp:ns",
      "deps/aec/aec.gyp:signal_processing",
      "deps/pa_ringbuffer/pa_ringbuffer.gyp:pa_ringbuffer",
    ],

    "include_dirs": [ "src" ],
    "sources": [
      "src/audio.cc",
      "src/channel.cc",
      "src/unit-common.cc",
    ],
    "conditions": [
      ["OS == 'mac'", {
        "sources": [ "src/mac/unit-mac.cc" ],
        "libraries": [ "AudioUnit.framework" ],
      }],
    ],
  }]
}
