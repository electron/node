'use strict';
var common = require('../common');
var assert = require('assert');
var path = require('path');
var fs = require('fs');

var expectFilePath = process.platform === 'win32' ||
                     process.platform === 'linux' ||
                     process.platform === 'darwin';

var watchSeenOne = 0;
var watchSeenTwo = 0;
var watchSeenThree = 0;

var testDir = common.tmpDir;

var filenameOne = 'watch.txt';
var filepathOne = path.join(testDir, filenameOne);

var filenameTwo = 'hasOwnProperty';
var filepathTwo = filenameTwo;
var filepathTwoAbs = path.join(testDir, filenameTwo);

var filenameThree = 'newfile.txt';
var testsubdir = path.join(testDir, 'testsubdir');
var filepathThree = path.join(testsubdir, filenameThree);


process.on('exit', function() {
  assert.ok(watchSeenOne > 0);
  assert.ok(watchSeenTwo > 0);
  assert.ok(watchSeenThree > 0);
});

common.refreshTmpDir();

fs.writeFileSync(filepathOne, 'hello');

assert.doesNotThrow(
    function() {
      var watcher = fs.watch(filepathOne);
      watcher.on('change', function(event, filename) {
        assert.equal('change', event);

        if (expectFilePath) {
          assert.equal('watch.txt', filename);
        }
        watcher.close();
        ++watchSeenOne;
      });
    }
);

setTimeout(function() {
  fs.writeFileSync(filepathOne, 'world');
}, 20);


process.chdir(testDir);

fs.writeFileSync(filepathTwoAbs, 'howdy');

assert.doesNotThrow(
    function() {
      var watcher = fs.watch(filepathTwo, function(event, filename) {
        assert.equal('change', event);

        if (expectFilePath) {
          assert.equal('hasOwnProperty', filename);
        }
        watcher.close();
        ++watchSeenTwo;
      });
    }
);

setTimeout(function() {
  fs.writeFileSync(filepathTwoAbs, 'pardner');
}, 20);

try { fs.unlinkSync(filepathThree); } catch (e) {}
try { fs.mkdirSync(testsubdir, 0o700); } catch (e) {}

assert.doesNotThrow(
    function() {
      var watcher = fs.watch(testsubdir, function(event, filename) {
        var renameEv = process.platform === 'sunos' ? 'change' : 'rename';
        assert.equal(renameEv, event);
        if (expectFilePath) {
          assert.equal('newfile.txt', filename);
        } else {
          assert.equal(null, filename);
        }
        watcher.close();
        ++watchSeenThree;
      });
    }
);

setTimeout(function() {
  var fd = fs.openSync(filepathThree, 'w');
  fs.closeSync(fd);
}, 20);

// https://github.com/joyent/node/issues/2293 - non-persistent watcher should
// not block the event loop
fs.watch(__filename, {persistent: false}, function() {
  assert(0);
});

// whitebox test to ensure that wrapped FSEvent is safe
// https://github.com/joyent/node/issues/6690
var oldhandle;
assert.throws(function() {
  var w = fs.watch(__filename, function(event, filename) { });
  oldhandle = w._handle;
  w._handle = { close: w._handle.close };
  w.close();
}, TypeError);
oldhandle.close(); // clean up

assert.throws(function() {
  var w = fs.watchFile(__filename, {persistent:false}, function() {});
  oldhandle = w._handle;
  w._handle = { stop: w._handle.stop };
  w.stop();
}, TypeError);
oldhandle.stop(); // clean up
