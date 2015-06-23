'use strict';
var common = require('../common');
var assert = require('assert');
var os = require('os');
var path = require('path');


process.env.TMPDIR = '/tmpdir';
process.env.TMP = '/tmp';
process.env.TEMP = '/temp';
if (common.isWindows) {
  assert.equal(os.tmpdir(), '/temp');
  process.env.TEMP = '';
  assert.equal(os.tmpdir(), '/tmp');
  process.env.TMP = '';
  var expected = (process.env.SystemRoot || process.env.windir) + '\\temp';
  assert.equal(os.tmpdir(), expected);
  process.env.TEMP = '\\temp\\';
  assert.equal(os.tmpdir(), '\\temp');
  process.env.TEMP = '\\tmpdir/';
  assert.equal(os.tmpdir(), '\\tmpdir/');
  process.env.TEMP = '\\';
  assert.equal(os.tmpdir(), '\\');
  process.env.TEMP = 'C:\\';
  assert.equal(os.tmpdir(), 'C:\\');
} else {
  assert.equal(os.tmpdir(), '/tmpdir');
  process.env.TMPDIR = '';
  assert.equal(os.tmpdir(), '/tmp');
  process.env.TMP = '';
  assert.equal(os.tmpdir(), '/temp');
  process.env.TEMP = '';
  assert.equal(os.tmpdir(), '/tmp');
  process.env.TMPDIR = '/tmpdir/';
  assert.equal(os.tmpdir(), '/tmpdir');
  process.env.TMPDIR = '/tmpdir\\';
  assert.equal(os.tmpdir(), '/tmpdir\\');
  process.env.TMPDIR = '/';
  assert.equal(os.tmpdir(), '/');
}

var endianness = os.endianness();
console.log('endianness = %s', endianness);
assert.ok(/[BL]E/.test(endianness));

var hostname = os.hostname();
console.log('hostname = %s', hostname);
assert.ok(hostname.length > 0);

var uptime = os.uptime();
console.log('uptime = %d', uptime);
assert.ok(uptime > 0);

var cpus = os.cpus();
console.log('cpus = ', cpus);
assert.ok(cpus.length > 0);

var type = os.type();
console.log('type = ', type);
assert.ok(type.length > 0);

var release = os.release();
console.log('release = ', release);
assert.ok(release.length > 0);

var platform = os.platform();
console.log('platform = ', platform);
assert.ok(platform.length > 0);

var arch = os.arch();
console.log('arch = ', arch);
assert.ok(arch.length > 0);

if (process.platform != 'sunos') {
  // not implemeneted yet
  assert.ok(os.loadavg().length > 0);
  assert.ok(os.freemem() > 0);
  assert.ok(os.totalmem() > 0);
}


var interfaces = os.networkInterfaces();
console.error(interfaces);
switch (platform) {
  case 'linux':
    var filter = function(e) { return e.address == '127.0.0.1'; };
    var actual = interfaces.lo.filter(filter);
    var expected = [{ address: '127.0.0.1', netmask: '255.0.0.0',
                      mac: '00:00:00:00:00:00', family: 'IPv4',
                      internal: true }];
    assert.deepEqual(actual, expected);
    break;
  case 'win32':
    var filter = function(e) { return e.address == '127.0.0.1'; };
    var actual = interfaces['Loopback Pseudo-Interface 1'].filter(filter);
    var expected = [{ address: '127.0.0.1', netmask: '255.0.0.0',
                      mac: '00:00:00:00:00:00', family: 'IPv4',
                      internal: true }];
    assert.deepEqual(actual, expected);
    break;
}

var EOL = os.EOL;
assert.ok(EOL.length > 0);


var home = os.homedir();

console.log('homedir = ' + home);
assert.ok(typeof home === 'string');
assert.ok(home.indexOf(path.sep) !== -1);

if (common.isWindows && process.env.USERPROFILE) {
  assert.equal(home, process.env.USERPROFILE);
  delete process.env.USERPROFILE;
  assert.ok(os.homedir().indexOf(path.sep) !== -1);
  process.env.USERPROFILE = home;
} else if (!common.isWindows && process.env.HOME) {
  assert.equal(home, process.env.HOME);
  delete process.env.HOME;
  assert.ok(os.homedir().indexOf(path.sep) !== -1);
  process.env.HOME = home;
}
