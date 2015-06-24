'use strict';
var common = require('../common');
var assert = require('assert');

var net = require('net');

// Sets the server's maxConnections property to 1.
// Open 2 connections (connection 0 and connection 1).
// Connection 0  should be accepted.
// Connection 1 should be rejected.
// Closes connection 0.
// Open 2 more connections (connection 2 and 3).
// Connection 2 should be accepted.
// Connection 3 should be rejected.

var connections = [];
var received = [];
var sent = [];

var createConnection = function(index) {
  console.error('creating connection ' + index);

  return new Promise(function(resolve, reject) {
    var connection = net.createConnection(common.PORT, function() {
      var msg = '' + index;
      console.error('sending message: ' + msg);
      this.write(msg);
      sent.push(msg);
    });

    connection.on('error', function(err) {
      assert.equal(err.code, 'ECONNRESET');
      resolve();
    });

    connection.on('data', function(e) {
      console.error('connection ' + index + ' received response');
      resolve();
    });

    connection.on('end', function() {
      console.error('ending ' + index);
      resolve();
    });

    connections[index] = connection;
  });
};

var closeConnection = function(index) {
  console.error('closing connection ' + index);
  return new Promise(function(resolve, reject) {
    connections[index].on('end', function() {
      resolve();
    });
    connections[index].end();
  });
};

var server = net.createServer(function(socket) {
  socket.on('data', function(data) {
    console.error('received message: ' + data);
    received.push('' + data);
    socket.write('acknowledged');
  });
});

server.maxConnections = 1;

server.listen(common.PORT, function() {
  createConnection(0)
  .then(createConnection.bind(null, 1))
  .then(closeConnection.bind(null, 0))
  .then(createConnection.bind(null, 2))
  .then(createConnection.bind(null, 3))
  .then(server.close.bind(server))
  .then(closeConnection.bind(null, 2));
});

process.on('exit', function() {
  // Confirm that all connections tried to send data...
  assert.deepEqual(sent, [0, 1, 2, 3]);
  // ...but that only connections 0 and 2 were successful.
  assert.deepEqual(received, [0, 2]);
});

process.on('unhandledRejection', function() {
  console.error('promise rejected');
  assert.fail(null, null, 'A promise in the chain rejected');
});
