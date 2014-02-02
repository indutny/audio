var bindings = require('bindings');
var audio = bindings('audio');

var Unit = audio.Unit;

describe('Audio binding', function() {
  it('should work', function(cb) {
    var u = new Unit();

    u.start();
    u.play(0, new Buffer(1024));
    u.oninput = function() {};
    setTimeout(function() {
      u.stop();
    }, 5000);
  });
});
