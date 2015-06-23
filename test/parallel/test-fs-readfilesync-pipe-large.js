'use strict';
var common = require('../common');
var assert = require('assert');
var path = require('path');

// simulate `cat readfile.js | node readfile.js`

// TODO: Have some way to make this work on windows.
if (process.platform === 'win32') {
  console.error('No /dev/stdin on windows.  Skipping test.');
  process.exit();
}

var fs = require('fs');

var filename = path.join(common.tmpDir, '/readfilesync_pipe_large_test.txt');
var dataExpected = new Array(1000000).join('a');
common.refreshTmpDir();
fs.writeFileSync(filename, dataExpected);

if (process.argv[2] === 'child') {
  process.stdout.write(fs.readFileSync('/dev/stdin', 'utf8'));
  return;
}

var exec = require('child_process').exec;
var f = JSON.stringify(__filename);
var node = JSON.stringify(process.execPath);
var cmd = 'cat ' + filename + ' | ' + node + ' ' + f + ' child';
exec(cmd, { maxBuffer: 1000000 }, function(err, stdout, stderr) {
  if (err) console.error(err);
  assert(!err, 'it exits normally');
  assert(stdout === dataExpected, 'it reads the file and outputs it');
  assert(stderr === '', 'it does not write to stderr');
  console.log('ok');
});

process.on('exit', function() {
  fs.unlinkSync(filename);
});
