'use strict';
var common = require('../common');
var assert = require('assert');
var path = require('path');
var Buffer = require('buffer').Buffer;
var fs = require('fs');

common.refreshTmpDir();

var fn = path.join(common.tmpDir, 'write-string-coerce.txt');
var data = true;
var expected = data + '';
var found;

fs.open(fn, 'w', 0o644, function(err, fd) {
  if (err) throw err;
  console.log('open done');
  fs.write(fd, data, 0, 'utf8', function(err, written) {
    console.log('write done');
    if (err) throw err;
    assert.equal(Buffer.byteLength(expected), written);
    fs.closeSync(fd);
    found = fs.readFileSync(fn, 'utf8');
    console.log('expected: "%s"', expected);
    console.log('found: "%s"', found);
    fs.unlinkSync(fn);
  });
});


process.on('exit', function() {
  assert.equal(expected, found);
});
