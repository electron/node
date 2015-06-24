'use strict';
var common = require('../common');
var assert = require('assert');
var os = require('os');

// first grab js api's
var smalloc = require('smalloc');
var alloc = smalloc.alloc;
var dispose = smalloc.dispose;
var copyOnto = smalloc.copyOnto;
var kMaxLength = smalloc.kMaxLength;
var Types = smalloc.Types;
// sliceOnto is volatile and cannot be exposed to users.
var sliceOnto = process.binding('smalloc').sliceOnto;


// verify allocation

var b = alloc(5, {});
assert.ok(typeof b === 'object');
for (var i = 0; i < 5; i++)
  assert.ok(b[i] !== undefined);


var b = {};
var c = alloc(5, b);
assert.equal(b, c);
assert.deepEqual(b, c);


var b = alloc(5, {});
var c = {};
c._data = sliceOnto(b, c, 0, 5);
assert.ok(typeof c._data === 'object');
assert.equal(b, c._data);
assert.deepEqual(b, c._data);


// verify writes

var b = alloc(5, {});
for (var i = 0; i < 5; i++)
  b[i] = i;
for (var i = 0; i < 5; i++)
  assert.equal(b[i], i);


var b = alloc(1, Types.Uint8);
b[0] = 256;
assert.equal(b[0], 0);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Int8);
b[0] = 128;
assert.equal(b[0], -128);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Uint16);
b[0] = 65536;
assert.equal(b[0], 0);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Int16);
b[0] = 32768;
assert.equal(b[0], -32768);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Uint32);
b[0] = 4294967296;
assert.equal(b[0], 0);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Int32);
b[0] = 2147483648;
assert.equal(b[0], -2147483648);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Float);
b[0] = 0.1111111111111111;
assert.equal(b[0], 0.1111111119389534);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Double);
b[0] = 0.1111111111111111;
assert.equal(b[0], 0.1111111111111111);
assert.equal(b[1], undefined);


var b = alloc(1, Types.Uint8Clamped);
b[0] = 300;
assert.equal(b[0], 255);
assert.equal(b[1], undefined);


var b = alloc(6, {});
var c0 = {};
var c1 = {};
c0._data = sliceOnto(b, c0, 0, 3);
c1._data = sliceOnto(b, c1, 3, 6);
for (var i = 0; i < 3; i++) {
  c0[i] = i;
  c1[i] = i + 3;
}
for (var i = 0; i < 3; i++)
  assert.equal(b[i], i);
for (var i = 3; i < 6; i++)
  assert.equal(b[i], i);


var a = alloc(6, {});
var b = alloc(6, {});
var c = alloc(12, {});
for (var i = 0; i < 6; i++) {
  a[i] = i;
  b[i] = i * 2;
}
copyOnto(a, 0, c, 0, 6);
copyOnto(b, 0, c, 6, 6);
for (var i = 0; i < 6; i++) {
  assert.equal(c[i], i);
  assert.equal(c[i + 6], i * 2);
}


var b = alloc(1, Types.Double);
var c = alloc(2, Types.Uint32);
if (os.endianness() === 'LE') {
  c[0] = 2576980378;
  c[1] = 1069128089;
} else {
  c[0] = 1069128089;
  c[1] = 2576980378;
}
copyOnto(c, 0, b, 0, 2);
assert.equal(b[0], 0.1);

var b = alloc(1, Types.Uint16);
var c = alloc(2, Types.Uint8);
c[0] = c[1] = 0xff;
copyOnto(c, 0, b, 0, 2);
assert.equal(b[0], 0xffff);

var b = alloc(2, Types.Uint8);
var c = alloc(1, Types.Uint16);
c[0] = 0xffff;
copyOnto(c, 0, b, 0, 1);
assert.equal(b[0], 0xff);
assert.equal(b[1], 0xff);


// verify checking external if has external memory

// check objects
var b = {};
assert.ok(!smalloc.hasExternalData(b));
alloc(1, b);
assert.ok(smalloc.hasExternalData(b));
var f = function() { };
alloc(1, f);
assert.ok(smalloc.hasExternalData(f));

// and non-objects
assert.ok(!smalloc.hasExternalData(true));
assert.ok(!smalloc.hasExternalData(1));
assert.ok(!smalloc.hasExternalData('string'));
assert.ok(!smalloc.hasExternalData(null));
assert.ok(!smalloc.hasExternalData());


// verify alloc throws properly

// arrays are not supported
assert.throws(function() {
  alloc(0, []);
}, TypeError);


// no allocations larger than kMaxLength
assert.throws(function() {
  alloc(kMaxLength + 1);
}, RangeError);


// properly convert to uint32 before checking overflow
assert.throws(function() {
  alloc(-1);
}, RangeError);


// no allocating on what's been allocated
assert.throws(function() {
  alloc(1, alloc(1));
}, TypeError);


// throw for values passed that are not objects
assert.throws(function() {
  alloc(1, 'a');
}, TypeError);
assert.throws(function() {
  alloc(1, true);
}, TypeError);
assert.throws(function() {
  alloc(1, null);
}, TypeError);


// should not throw allocating to most objects
alloc(1, function() { });
alloc(1, /abc/);
alloc(1, new Date());


// range check on external array enumeration
assert.throws(function() {
  alloc(1, 0);
}, TypeError);
assert.throws(function() {
  alloc(1, 10);
}, TypeError);

// very copyOnto throws properly

// source must have data
assert.throws(function() {
  copyOnto({}, 0, alloc(1), 0, 0);
}, Error);


// dest must have data
assert.throws(function() {
  copyOnto(alloc(1), 0, {}, 0, 0);
}, Error);


// copyLength <= sourceLength
assert.throws(function() {
  copyOnto(alloc(1), 0, alloc(3), 0, 2);
}, RangeError);


// copyLength <= destLength
assert.throws(function() {
  copyOnto(alloc(3), 0, alloc(1), 0, 2);
}, RangeError);


// sourceStart <= sourceLength
assert.throws(function() {
  copyOnto(alloc(1), 3, alloc(1), 0, 1);
}, RangeError);


// destStart <= destLength
assert.throws(function() {
  copyOnto(alloc(1), 0, alloc(1), 3, 1);
}, RangeError);


// sourceStart + copyLength <= sourceLength
assert.throws(function() {
  copyOnto(alloc(3), 1, alloc(3), 0, 3);
}, RangeError);


// destStart + copyLength <= destLength
assert.throws(function() {
  copyOnto(alloc(3), 0, alloc(3), 1, 3);
}, RangeError);


// copy_length * array_size <= dest_length
assert.throws(function() {
  copyOnto(alloc(2, Types.Double), 0, alloc(2, Types.Uint32), 0, 2);
}, RangeError);


// test disposal
var b = alloc(5, {});
dispose(b);
for (var i = 0; i < 5; i++)
  assert.equal(b[i], undefined);


// verify dispose throws properly

// only allow object to be passed to dispose
assert.throws(function() {
  smalloc.dispose(null);
});


// can't dispose a Buffer
assert.throws(function() {
  smalloc.dispose(new Buffer());
});

assert.throws(function() {
  smalloc.dispose(new Uint8Array(new ArrayBuffer(1)));
});

assert.throws(function() {
  smalloc.dispose({});
});


// Types should be immutable
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(smalloc, 'Types'), {
  value: smalloc.Types,
  writable: false,
  enumerable: true,
  configurable: false
});

var types = Object.keys(smalloc.Types);
var Types = smalloc.Types;

for (var i = 0; i < types.length; i++)
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(Types, types[i]), {
    value: Types[types[i]],
    writable: false,
    enumerable: true,
    configurable: false
  });
