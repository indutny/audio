var bindings = require('bindings');
var audio = bindings('audio');

var Unit = audio.Unit;

describe('Audio binding', function() {
  it('should work', function(cb) {
    var u = new Unit(44100.0);

    u.start();
    u.queueForPlayback(new Buffer(1024));
    setTimeout(function() {
      u.stop();
    }, 5000);
  });
});
