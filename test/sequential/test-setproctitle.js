'use strict';
// Original test written by Jakub Lekstan <kuebzky@gmail.com>

// FIXME add sunos support
if ('linux freebsd darwin'.indexOf(process.platform) === -1) {
  console.error('Skipping test, platform not supported.');
  process.exit();
}

var common = require('../common');
var assert = require('assert');
var exec = require('child_process').exec;
var path = require('path');

// The title shouldn't be too long; libuv's uv_set_process_title() out of
// security considerations no longer overwrites envp, only argv, so the
// maximum title length is possibly quite short.
var title = 'testme';

assert.notEqual(process.title, title);
process.title = title;
assert.equal(process.title, title);

exec('ps -p ' + process.pid + ' -o args=', function(error, stdout, stderr) {
  assert.equal(error, null);
  assert.equal(stderr, '');

  // freebsd always add ' (procname)' to the process title
  if (process.platform === 'freebsd')
    title += ` (${path.basename(process.execPath)})`;

  // omitting trailing whitespace and \n
  assert.equal(stdout.replace(/\s+$/, ''), title);
});
