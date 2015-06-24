'use strict';
var common = require('../common');
var assert = require('assert');
var http = require('http');

var outstanding_reqs = 0;
var test_req_body = 'some stuff...\n';
var test_res_body = 'other stuff!\n';
var sent_continue = false;
var got_continue = false;

function handler(req, res) {
  assert.equal(sent_continue, true, 'Full response sent before 100 Continue');
  common.debug('Server sending full response...');
  res.writeHead(200, {
    'Content-Type' : 'text/plain',
    'ABCD' : '1'
  });
  res.end(test_res_body);
}

var server = http.createServer(handler);
server.on('checkContinue', function(req, res) {
  common.debug('Server got Expect: 100-continue...');
  res.writeContinue();
  sent_continue = true;
  setTimeout(function() {
    handler(req, res);
  }, 100);
});
server.listen(common.PORT);


server.on('listening', function() {
  var req = http.request({
    port: common.PORT,
    method: 'POST',
    path: '/world',
    headers: { 'Expect': '100-continue' }
  });
  common.debug('Client sending request...');
  outstanding_reqs++;
  var body = '';
  req.on('continue', function() {
    common.debug('Client got 100 Continue...');
    got_continue = true;
    req.end(test_req_body);
  });
  req.on('response', function(res) {
    assert.equal(got_continue, true,
                 'Full response received before 100 Continue');
    assert.equal(200, res.statusCode,
                 'Final status code was ' + res.statusCode + ', not 200.');
    res.setEncoding('utf8');
    res.on('data', function(chunk) { body += chunk; });
    res.on('end', function() {
      common.debug('Got full response.');
      assert.equal(body, test_res_body, 'Response body doesn\'t match.');
      assert.ok('abcd' in res.headers, 'Response headers missing.');
      outstanding_reqs--;
      if (outstanding_reqs == 0) {
        server.close();
        process.exit();
      }
    });
  });
});
