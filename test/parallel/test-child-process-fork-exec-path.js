'use strict';
var assert = require('assert');
var cp = require('child_process');
var fs = require('fs');
var path = require('path');
var common = require('../common');
var msg = {test: 'this'};
var nodePath = process.execPath;
var copyPath = path.join(common.tmpDir, 'node-copy.exe');

if (process.env.FORK) {
  assert(process.send);
  assert.equal(process.argv[0], copyPath);
  process.send(msg);
  process.exit();
}
else {
  common.refreshTmpDir();
  try {
    fs.unlinkSync(copyPath);
  }
  catch (e) {
    if (e.code !== 'ENOENT') throw e;
  }
  fs.writeFileSync(copyPath, fs.readFileSync(nodePath));
  fs.chmodSync(copyPath, '0755');

  // slow but simple
  var envCopy = JSON.parse(JSON.stringify(process.env));
  envCopy.FORK = 'true';
  var child = require('child_process').fork(__filename, {
    execPath: copyPath,
    env: envCopy
  });
  child.on('message', common.mustCall(function(recv) {
    assert.deepEqual(msg, recv);
  }));
  child.on('exit', common.mustCall(function(code) {
    fs.unlinkSync(copyPath);
    assert.equal(code, 0);
  }));
}
