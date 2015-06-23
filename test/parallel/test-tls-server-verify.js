'use strict';
var common = require('../common');

if (!common.opensslCli) {
  console.error('Skipping because node compiled without OpenSSL CLI.');
  process.exit(0);
}

// This is a rather complex test which sets up various TLS servers with node
// and connects to them using the 'openssl s_client' command line utility
// with various keys. Depending on the certificate authority and other
// parameters given to the server, the various clients are
// - rejected,
// - accepted and "unauthorized", or
// - accepted and "authorized".

var testCases =
    [{ title: 'Do not request certs. Everyone is unauthorized.',
      requestCert: false,
      rejectUnauthorized: false,
      renegotiate: false,
      CAs: ['ca1-cert'],
      clients:
       [{ name: 'agent1', shouldReject: false, shouldAuth: false },
        { name: 'agent2', shouldReject: false, shouldAuth: false },
        { name: 'agent3', shouldReject: false, shouldAuth: false },
        { name: 'nocert', shouldReject: false, shouldAuth: false }
       ]
    },

    { title: 'Allow both authed and unauthed connections with CA1',
      requestCert: true,
      rejectUnauthorized: false,
      renegotiate: false,
      CAs: ['ca1-cert'],
      clients:
       [{ name: 'agent1', shouldReject: false, shouldAuth: true },
        { name: 'agent2', shouldReject: false, shouldAuth: false },
        { name: 'agent3', shouldReject: false, shouldAuth: false },
        { name: 'nocert', shouldReject: false, shouldAuth: false }
       ]
    },

    { title: 'Do not request certs at connection. Do that later',
      requestCert: false,
      rejectUnauthorized: false,
      renegotiate: true,
      CAs: ['ca1-cert'],
      clients:
       [{ name: 'agent1', shouldReject: false, shouldAuth: true },
        { name: 'agent2', shouldReject: false, shouldAuth: false },
        { name: 'agent3', shouldReject: false, shouldAuth: false },
        { name: 'nocert', shouldReject: false, shouldAuth: false }
       ]
    },

    { title: 'Allow only authed connections with CA1',
      requestCert: true,
      rejectUnauthorized: true,
      renegotiate: false,
      CAs: ['ca1-cert'],
      clients:
       [{ name: 'agent1', shouldReject: false, shouldAuth: true },
        { name: 'agent2', shouldReject: true },
        { name: 'agent3', shouldReject: true },
        { name: 'nocert', shouldReject: true }
       ]
    },

    { title: 'Allow only authed connections with CA1 and CA2',
      requestCert: true,
      rejectUnauthorized: true,
      renegotiate: false,
      CAs: ['ca1-cert', 'ca2-cert'],
      clients:
       [{ name: 'agent1', shouldReject: false, shouldAuth: true },
        { name: 'agent2', shouldReject: true },
        { name: 'agent3', shouldReject: false, shouldAuth: true },
        { name: 'nocert', shouldReject: true }
       ]
    },


    { title: 'Allow only certs signed by CA2 but not in the CRL',
      requestCert: true,
      rejectUnauthorized: true,
      renegotiate: false,
      CAs: ['ca2-cert'],
      crl: 'ca2-crl',
      clients:
       [
        { name: 'agent1', shouldReject: true, shouldAuth: false },
        { name: 'agent2', shouldReject: true, shouldAuth: false },
        { name: 'agent3', shouldReject: false, shouldAuth: true },
        // Agent4 has a cert in the CRL.
        { name: 'agent4', shouldReject: true, shouldAuth: false },
        { name: 'nocert', shouldReject: true }
       ]
    }
    ];

if (!common.hasCrypto) {
  console.log('1..0 # Skipped: missing crypto');
  process.exit();
}
var tls = require('tls');

var constants = require('constants');
var assert = require('assert');
var fs = require('fs');
var spawn = require('child_process').spawn;


function filenamePEM(n) {
  return require('path').join(common.fixturesDir, 'keys', n + '.pem');
}


function loadPEM(n) {
  return fs.readFileSync(filenamePEM(n));
}


var serverKey = loadPEM('agent2-key');
var serverCert = loadPEM('agent2-cert');


function runClient(prefix, port, options, cb) {

  // Client can connect in three ways:
  // - Self-signed cert
  // - Certificate, but not signed by CA.
  // - Certificate signed by CA.

  var args = ['s_client', '-connect', '127.0.0.1:' + port];

  // for the performance issue in s_client on Windows
  if (process.platform === 'win32')
    args.push('-no_rand_screen');

  console.log(prefix + '  connecting with', options.name);

  switch (options.name) {
    case 'agent1':
      // Signed by CA1
      args.push('-key');
      args.push(filenamePEM('agent1-key'));
      args.push('-cert');
      args.push(filenamePEM('agent1-cert'));
      break;

    case 'agent2':
      // Self-signed
      // This is also the key-cert pair that the server will use.
      args.push('-key');
      args.push(filenamePEM('agent2-key'));
      args.push('-cert');
      args.push(filenamePEM('agent2-cert'));
      break;

    case 'agent3':
      // Signed by CA2
      args.push('-key');
      args.push(filenamePEM('agent3-key'));
      args.push('-cert');
      args.push(filenamePEM('agent3-cert'));
      break;

    case 'agent4':
      // Signed by CA2 (rejected by ca2-crl)
      args.push('-key');
      args.push(filenamePEM('agent4-key'));
      args.push('-cert');
      args.push(filenamePEM('agent4-cert'));
      break;

    case 'nocert':
      // Do not send certificate
      break;

    default:
      throw new Error(prefix + 'Unknown agent name');
  }

  // To test use: openssl s_client -connect localhost:8000
  var client = spawn(common.opensslCli, args);

  var out = '';

  var rejected = true;
  var authed = false;
  var goodbye = false;

  client.stdout.setEncoding('utf8');
  client.stdout.on('data', function(d) {
    out += d;

    if (!goodbye && /_unauthed/g.test(out)) {
      console.error(prefix + '  * unauthed');
      goodbye = true;
      client.kill();
      authed = false;
      rejected = false;
    }

    if (!goodbye && /_authed/g.test(out)) {
      console.error(prefix + '  * authed');
      goodbye = true;
      client.kill();
      authed = true;
      rejected = false;
    }
  });

  //client.stdout.pipe(process.stdout);

  client.on('exit', function(code) {
    //assert.equal(0, code, prefix + options.name +
    //      ": s_client exited with error code " + code);
    if (options.shouldReject) {
      assert.equal(true, rejected, prefix + options.name +
          ' NOT rejected, but should have been');
    } else {
      assert.equal(false, rejected, prefix + options.name +
          ' rejected, but should NOT have been');
      assert.equal(options.shouldAuth, authed, prefix +
          options.name + ' authed is ' + authed +
          ' but should have been ' + options.shouldAuth);
    }

    cb();
  });
}


// Run the tests
var successfulTests = 0;
function runTest(port, testIndex) {
  var prefix = testIndex + ' ';
  var tcase = testCases[testIndex];
  if (!tcase) return;

  console.error(prefix + "Running '%s'", tcase.title);

  var cas = tcase.CAs.map(loadPEM);

  var crl = tcase.crl ? loadPEM(tcase.crl) : null;

  var serverOptions = {
    key: serverKey,
    cert: serverCert,
    ca: cas,
    crl: crl,
    requestCert: tcase.requestCert,
    rejectUnauthorized: tcase.rejectUnauthorized
  };

  var connections = 0;

  /*
   * If renegotiating - session might be resumed and openssl won't request
   * client's certificate (probably because of bug in the openssl)
   */
  if (tcase.renegotiate) {
    serverOptions.secureOptions =
        constants.SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;
  }

  var renegotiated = false;
  var server = tls.Server(serverOptions, function handleConnection(c) {
    c.on('error', function(e) {
      // child.kill() leads ECONNRESET errro in the TLS connection of
      // openssl s_client via spawn(). A Test result is already
      // checked by the data of client.stdout before child.kill() so
      // these tls errors can be ignored.
    });
    if (tcase.renegotiate && !renegotiated) {
      renegotiated = true;
      setTimeout(function() {
        console.error(prefix + '- connected, renegotiating');
        c.write('\n_renegotiating\n');
        return c.renegotiate({
          requestCert: true,
          rejectUnauthorized: false
        }, function(err) {
          assert(!err);
          c.write('\n_renegotiated\n');
          handleConnection(c);
        });
      }, 200);
      return;
    }

    connections++;
    if (c.authorized) {
      console.error(prefix + '- authed connection: ' +
                    c.getPeerCertificate().subject.CN);
      c.write('\n_authed\n');
    } else {
      console.error(prefix + '- unauthed connection: %s', c.authorizationError);
      c.write('\n_unauthed\n');
    }
  });

  function runNextClient(clientIndex) {
    var options = tcase.clients[clientIndex];
    if (options) {
      runClient(prefix + clientIndex + ' ', port, options, function() {
        runNextClient(clientIndex + 1);
      });
    } else {
      server.close();
      successfulTests++;
      runTest(port, nextTest++);
    }
  }

  server.listen(port, function() {
    if (tcase.debug) {
      console.error(prefix + 'TLS server running on port ' + port);
    } else {
      if (tcase.renegotiate) {
        runNextClient(0);
      } else {
        var clientsCompleted = 0;
        for (var i = 0; i < tcase.clients.length; i++) {
          runClient(prefix + i + ' ', port, tcase.clients[i], function() {
            clientsCompleted++;
            if (clientsCompleted === tcase.clients.length) {
              server.close();
              successfulTests++;
              runTest(port, nextTest++);
            }
          });
        }
      }
    }
  });
}


var nextTest = 0;
runTest(common.PORT, nextTest++);
runTest(common.PORT + 1, nextTest++);


process.on('exit', function() {
  assert.equal(successfulTests, testCases.length);
});
