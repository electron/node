'use strict';
const common = require('../common');
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const stream = require('stream');
const firstEncoding = 'base64';
const secondEncoding = 'binary';

const examplePath = path.join(common.fixturesDir, 'x.txt');
const dummyPath = path.join(common.tmpDir, 'x.txt');

common.refreshTmpDir();

const exampleReadStream = fs.createReadStream(examplePath, {
  encoding: firstEncoding
});

const dummyWriteStream = fs.createWriteStream(dummyPath, {
  encoding: firstEncoding
});

exampleReadStream.pipe(dummyWriteStream).on('finish', function() {
  const assertWriteStream = new stream.Writable({
    write: function(chunk, enc, next) {
      const expected = new Buffer('xyz\n');
      assert(chunk.equals(expected));
    }
  });
  assertWriteStream.setDefaultEncoding(secondEncoding);
  fs.createReadStream(dummyPath, {
    encoding: secondEncoding
  }).pipe(assertWriteStream);
});
