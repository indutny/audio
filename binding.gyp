{
  "targets": [{
    "target_name": "audio",
    "include_dirs": [ "src" ],
    "sources": [
      "src/audio.cc",
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
