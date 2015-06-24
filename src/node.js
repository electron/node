// Hello, and welcome to hacking node.js!
//
// This file is invoked by node::Load in src/node.cc, and responsible for
// bootstrapping the node.js core. Special caution is given to the performance
// of the startup process, so many dependencies are invoked lazily.

'use strict';

(function(process) {
  this.global = this;

  function startup() {
    var EventEmitter = NativeModule.require('events').EventEmitter;

    process.__proto__ = Object.create(EventEmitter.prototype, {
      constructor: {
        value: process.constructor
      }
    });
    EventEmitter.call(process);

    process.EventEmitter = EventEmitter; // process.EventEmitter is deprecated

    // do this good and early, since it handles errors.
    startup.processFatal();

    startup.globalVariables();
    startup.globalTimeouts();
    startup.globalConsole();

    startup.processAssert();
    startup.processConfig();
    startup.processNextTick();
    startup.processPromises();
    startup.processStdio();
    startup.processKillAndExit();
    startup.processSignalHandlers();

    // Do not initialize channel in debugger agent, it deletes env variable
    // and the main thread won't see it.
    if (process.argv[1] !== '--debug-agent')
      startup.processChannel();

    startup.processRawDebug();

    process.argv[0] = process.execPath;

    // There are various modes that Node can run in. The most common two
    // are running from a script and running the REPL - but there are a few
    // others like the debugger or running --eval arguments. Here we decide
    // which mode we run in.

    if (NativeModule.exists('_third_party_main')) {
      // To allow people to extend Node in different ways, this hook allows
      // one to drop a file lib/_third_party_main.js into the build
      // directory which will be executed instead of Node's normal loading.
      process.nextTick(function() {
        NativeModule.require('_third_party_main');
      });

    } else if (process.argv[1] == 'debug') {
      // Start the debugger agent
      var d = NativeModule.require('_debugger');
      d.start();

    } else if (process.argv[1] == '--debug-agent') {
      // Start the debugger agent
      var d = NativeModule.require('_debug_agent');
      d.start();

    } else {
      // There is user code to be run

      // If this is a worker in cluster mode, start up the communication
      // channel. This needs to be done before any user code gets executed
      // (including preload modules).
      if (process.argv[1] && process.env.NODE_UNIQUE_ID) {
        var cluster = NativeModule.require('cluster');
        cluster._setupWorker();

        // Make sure it's not accidentally inherited by child processes.
        delete process.env.NODE_UNIQUE_ID;
      }

      if (process._eval != null) {
        // User passed '-e' or '--eval' arguments to Node.
        startup.preloadModules();
        evalScript('[eval]');
      } else if (process.argv[1]) {
        // make process.argv[1] into a full path
        var path = NativeModule.require('path');
        process.argv[1] = path.resolve(process.argv[1]);

        var Module = NativeModule.require('module');
        startup.preloadModules();
        if (global.v8debug &&
            process.execArgv.some(function(arg) {
              return arg.match(/^--debug-brk(=[0-9]*)?$/);
            })) {

          // XXX Fix this terrible hack!
          //
          // Give the client program a few ticks to connect.
          // Otherwise, there's a race condition where `node debug foo.js`
          // will not be able to connect in time to catch the first
          // breakpoint message on line 1.
          //
          // A better fix would be to somehow get a message from the
          // global.v8debug object about a connection, and runMain when
          // that occurs.  --isaacs

          var debugTimeout = +process.env.NODE_DEBUG_TIMEOUT || 50;
          setTimeout(Module.runMain, debugTimeout);

        } else {
          // Main entry point into most programs:
          Module.runMain();
        }

      } else {
        var Module = NativeModule.require('module');

        // If -i or --interactive were passed, or stdin is a TTY.
        if (process._forceRepl || NativeModule.require('tty').isatty(0)) {
          // REPL
          var cliRepl = Module.requireRepl();
          cliRepl.createInternalRepl(process.env, function(err, repl) {
            if (err) {
              throw err;
            }
            repl.on('exit', function() {
              if (repl._flushing) {
                repl.pause();
                return repl.once('flushHistory', function() {
                  process.exit();
                });
              }
              process.exit();
            });
          });
        } else {
          // Read all of stdin - execute it.
          process.stdin.setEncoding('utf8');

          var code = '';
          process.stdin.on('data', function(d) {
            code += d;
          });

          process.stdin.on('end', function() {
            process._eval = code;
            evalScript('[stdin]');
          });
        }
      }
    }
  }

  startup.globalVariables = function() {
    global.process = process;
    global.global = global;
    global.GLOBAL = global;
    global.root = global;
    global.Buffer = NativeModule.require('buffer').Buffer;
    process.domain = null;
    process._exiting = false;
  };

  startup.globalTimeouts = function() {
    const timers = NativeModule.require('timers');
    global.clearImmediate = timers.clearImmediate;
    global.clearInterval = timers.clearInterval;
    global.clearTimeout = timers.clearTimeout;
    global.setImmediate = timers.setImmediate;
    global.setInterval = timers.setInterval;
    global.setTimeout = timers.setTimeout;
  };

  startup.globalConsole = function() {
    global.__defineGetter__('console', function() {
      return NativeModule.require('console');
    });
  };


  startup._lazyConstants = null;

  startup.lazyConstants = function() {
    if (!startup._lazyConstants) {
      startup._lazyConstants = process.binding('constants');
    }
    return startup._lazyConstants;
  };

  startup.processFatal = function() {
    process._makeCallbackAbortOnUncaught = function() {
      try {
        return this[1].apply(this[0], arguments);
      } catch (err) {
        process._fatalException(err);
      }
    };

    process._fatalException = function(er) {
      var caught;

      if (process.domain && process.domain._errorHandler)
        caught = process.domain._errorHandler(er) || caught;

      if (!caught)
        caught = process.emit('uncaughtException', er);

      // If someone handled it, then great.  otherwise, die in C++ land
      // since that means that we'll exit the process, emit the 'exit' event
      if (!caught) {
        try {
          if (!process._exiting) {
            process._exiting = true;
            process.emit('exit', 1);
          }
        } catch (er) {
          // nothing to be done about it at this point.
        }

      // if we handled an error, then make sure any ticks get processed
      } else {
        NativeModule.require('timers').setImmediate(process._tickCallback);
      }

      return caught;
    };
  };

  var assert;
  startup.processAssert = function() {
    assert = process.assert = function(x, msg) {
      if (!x) throw new Error(msg || 'assertion error');
    };
  };

  startup.processConfig = function() {
    // used for `process.config`, but not a real module
    var config = NativeModule._source.config;
    delete NativeModule._source.config;

    // strip the gyp comment line at the beginning
    config = config.split('\n')
        .slice(1)
        .join('\n')
        .replace(/"/g, '\\"')
        .replace(/'/g, '"');

    process.config = JSON.parse(config, function(key, value) {
      if (value === 'true') return true;
      if (value === 'false') return false;
      return value;
    });
  };

  var addPendingUnhandledRejection;
  var hasBeenNotifiedProperty = new WeakMap();
  startup.processNextTick = function() {
    var nextTickQueue = [];
    var pendingUnhandledRejections = [];
    var microtasksScheduled = false;

    // Used to run V8's micro task queue.
    var _runMicrotasks = {};

    // This tickInfo thing is used so that the C++ code in src/node.cc
    // can have easy accesss to our nextTick state, and avoid unnecessary
    var tickInfo = {};

    // *Must* match Environment::TickInfo::Fields in src/env.h.
    var kIndex = 0;
    var kLength = 1;

    process.nextTick = nextTick;
    // Needs to be accessible from beyond this scope.
    process._tickCallback = _tickCallback;
    process._tickDomainCallback = _tickDomainCallback;

    process._setupNextTick(tickInfo, _tickCallback, _runMicrotasks);

    _runMicrotasks = _runMicrotasks.runMicrotasks;

    function tickDone() {
      if (tickInfo[kLength] !== 0) {
        if (tickInfo[kLength] <= tickInfo[kIndex]) {
          nextTickQueue = [];
          tickInfo[kLength] = 0;
        } else {
          nextTickQueue.splice(0, tickInfo[kIndex]);
          tickInfo[kLength] = nextTickQueue.length;
        }
      }
      tickInfo[kIndex] = 0;
    }

    function scheduleMicrotasks() {
      if (microtasksScheduled)
        return;

      nextTickQueue.push({
        callback: runMicrotasksCallback,
        domain: null
      });

      tickInfo[kLength]++;
      microtasksScheduled = true;
    }

    function runMicrotasksCallback() {
      microtasksScheduled = false;
      _runMicrotasks();

      if (tickInfo[kIndex] < tickInfo[kLength] ||
          emitPendingUnhandledRejections())
        scheduleMicrotasks();
    }

    // Run callbacks that have no domain.
    // Using domains will cause this to be overridden.
    function _tickCallback() {
      var callback, args, tock;

      do {
        while (tickInfo[kIndex] < tickInfo[kLength]) {
          tock = nextTickQueue[tickInfo[kIndex]++];
          callback = tock.callback;
          args = tock.args;
          // Using separate callback execution functions helps to limit the
          // scope of DEOPTs caused by using try blocks and allows direct
          // callback invocation with small numbers of arguments to avoid the
          // performance hit associated with using `fn.apply()`
          if (args === undefined) {
            doNTCallback0(callback);
          } else {
            switch (args.length) {
              case 1:
                doNTCallback1(callback, args[0]);
                break;
              case 2:
                doNTCallback2(callback, args[0], args[1]);
                break;
              case 3:
                doNTCallback3(callback, args[0], args[1], args[2]);
                break;
              default:
                doNTCallbackMany(callback, args);
            }
          }
          if (1e4 < tickInfo[kIndex])
            tickDone();
        }
        tickDone();
        _runMicrotasks();
        emitPendingUnhandledRejections();
      } while (tickInfo[kLength] !== 0);
    }

    function _tickDomainCallback() {
      var callback, domain, args, tock;

      do {
        while (tickInfo[kIndex] < tickInfo[kLength]) {
          tock = nextTickQueue[tickInfo[kIndex]++];
          callback = tock.callback;
          domain = tock.domain;
          args = tock.args;
          if (domain)
            domain.enter();
          // Using separate callback execution functions helps to limit the
          // scope of DEOPTs caused by using try blocks and allows direct
          // callback invocation with small numbers of arguments to avoid the
          // performance hit associated with using `fn.apply()`
          if (args === undefined) {
            doNTCallback0(callback);
          } else {
            switch (args.length) {
              case 1:
                doNTCallback1(callback, args[0]);
                break;
              case 2:
                doNTCallback2(callback, args[0], args[1]);
                break;
              case 3:
                doNTCallback3(callback, args[0], args[1], args[2]);
                break;
              default:
                doNTCallbackMany(callback, args);
            }
          }
          if (1e4 < tickInfo[kIndex])
            tickDone();
          if (domain)
            domain.exit();
        }
        tickDone();
        _runMicrotasks();
        emitPendingUnhandledRejections();
      } while (tickInfo[kLength] !== 0);
    }

    function doNTCallback0(callback) {
      var threw = true;
      try {
        callback();
        threw = false;
      } finally {
        if (threw)
          tickDone();
      }
    }

    function doNTCallback1(callback, arg1) {
      var threw = true;
      try {
        callback(arg1);
        threw = false;
      } finally {
        if (threw)
          tickDone();
      }
    }

    function doNTCallback2(callback, arg1, arg2) {
      var threw = true;
      try {
        callback(arg1, arg2);
        threw = false;
      } finally {
        if (threw)
          tickDone();
      }
    }

    function doNTCallback3(callback, arg1, arg2, arg3) {
      var threw = true;
      try {
        callback(arg1, arg2, arg3);
        threw = false;
      } finally {
        if (threw)
          tickDone();
      }
    }

    function doNTCallbackMany(callback, args) {
      var threw = true;
      try {
        callback.apply(null, args);
        threw = false;
      } finally {
        if (threw)
          tickDone();
      }
    }

    function TickObject(c, args) {
      this.callback = c;
      this.domain = process.domain || null;
      this.args = args;
    }

    function nextTick(callback) {
      // on the way out, don't bother. it won't get fired anyway.
      if (process._exiting)
        return;

      var args;
      if (arguments.length > 1) {
        args = [];
        for (var i = 1; i < arguments.length; i++)
          args.push(arguments[i]);
      }

      nextTickQueue.push(new TickObject(callback, args));
      tickInfo[kLength]++;
    }

    function emitPendingUnhandledRejections() {
      var hadListeners = false;
      while (pendingUnhandledRejections.length > 0) {
        var promise = pendingUnhandledRejections.shift();
        var reason = pendingUnhandledRejections.shift();
        if (hasBeenNotifiedProperty.get(promise) === false) {
          hasBeenNotifiedProperty.set(promise, true);
          if (!process.emit('unhandledRejection', reason, promise)) {
            // Nobody is listening.
            // TODO(petkaantonov) Take some default action, see #830
          } else {
            hadListeners = true;
          }
        }
      }
      return hadListeners;
    }

    addPendingUnhandledRejection = function(promise, reason) {
      pendingUnhandledRejections.push(promise, reason);
      scheduleMicrotasks();
    };
  };

  startup.processPromises = function() {
    var promiseRejectEvent = process._promiseRejectEvent;

    function unhandledRejection(promise, reason) {
      hasBeenNotifiedProperty.set(promise, false);
      addPendingUnhandledRejection(promise, reason);
    }

    function rejectionHandled(promise) {
      var hasBeenNotified = hasBeenNotifiedProperty.get(promise);
      if (hasBeenNotified !== undefined) {
        hasBeenNotifiedProperty.delete(promise);
        if (hasBeenNotified === true) {
          process.nextTick(function() {
            process.emit('rejectionHandled', promise);
          });
        }

      }
    }

    process._setupPromises(function(event, promise, reason) {
      if (event === promiseRejectEvent.unhandled)
        unhandledRejection(promise, reason);
      else if (event === promiseRejectEvent.handled)
        rejectionHandled(promise);
      else
        NativeModule.require('assert').fail('unexpected PromiseRejectEvent');
    });
  };

  function evalScript(name) {
    var Module = NativeModule.require('module');
    var path = NativeModule.require('path');

    try {
      var cwd = process.cwd();
    } catch (e) {
      // getcwd(3) can fail if the current working directory has been deleted.
      // Fall back to the directory name of the (absolute) executable path.
      // It's not really correct but what are the alternatives?
      var cwd = path.dirname(process.execPath);
    }

    var module = new Module(name);
    module.filename = path.join(cwd, name);
    module.paths = Module._nodeModulePaths(cwd);
    var script = process._eval;
    var body = script;
    script = 'global.__filename = ' + JSON.stringify(name) + ';\n' +
             'global.exports = exports;\n' +
             'global.module = module;\n' +
             'global.__dirname = __dirname;\n' +
             'global.require = require;\n' +
             'return require("vm").runInThisContext(' +
             JSON.stringify(body) + ', { filename: ' +
             JSON.stringify(name) + ' });\n';
    // Defer evaluation for a tick.  This is a workaround for deferred
    // events not firing when evaluating scripts from the command line,
    // see https://github.com/nodejs/io.js/issues/1600.
    process.nextTick(function() {
      var result = module._compile(script, name + '-wrapper');
      if (process._print_eval) console.log(result);
    });
  }

  function createWritableStdioStream(fd) {
    var stream;
    var tty_wrap = process.binding('tty_wrap');

    // Note stream._type is used for test-module-load-list.js

    switch (tty_wrap.guessHandleType(fd)) {
      case 'TTY':
        var tty = NativeModule.require('tty');
        stream = new tty.WriteStream(fd);
        stream._type = 'tty';
        break;

      case 'FILE':
        var fs = NativeModule.require('fs');
        stream = new fs.SyncWriteStream(fd, { autoClose: false });
        stream._type = 'fs';
        break;

      case 'PIPE':
      case 'TCP':
        var net = NativeModule.require('net');
        stream = new net.Socket({
          fd: fd,
          readable: false,
          writable: true
        });
        stream._type = 'pipe';
        break;

      default:
        // Probably an error on in uv_guess_handle()
        throw new Error('Implement me. Unknown stream file type!');
    }

    // For supporting legacy API we put the FD here.
    stream.fd = fd;

    stream._isStdio = true;

    return stream;
  }

  startup.processStdio = function() {
    var stdin, stdout, stderr;

    process.__defineGetter__('stdout', function() {
      if (stdout) return stdout;
      stdout = createWritableStdioStream(1);
      stdout.destroy = stdout.destroySoon = function(er) {
        er = er || new Error('process.stdout cannot be closed.');
        stdout.emit('error', er);
      };
      if (stdout.isTTY) {
        process.on('SIGWINCH', function() {
          stdout._refreshSize();
        });
      }
      return stdout;
    });

    process.__defineGetter__('stderr', function() {
      if (stderr) return stderr;
      stderr = createWritableStdioStream(2);
      stderr.destroy = stderr.destroySoon = function(er) {
        er = er || new Error('process.stderr cannot be closed.');
        stderr.emit('error', er);
      };
      return stderr;
    });

    process.__defineGetter__('stdin', function() {
      if (stdin) return stdin;

      var tty_wrap = process.binding('tty_wrap');
      var fd = 0;

      switch (tty_wrap.guessHandleType(fd)) {
        case 'TTY':
          var tty = NativeModule.require('tty');
          stdin = new tty.ReadStream(fd, {
            highWaterMark: 0,
            readable: true,
            writable: false
          });
          break;

        case 'FILE':
          var fs = NativeModule.require('fs');
          stdin = new fs.ReadStream(null, { fd: fd, autoClose: false });
          break;

        case 'PIPE':
        case 'TCP':
          var net = NativeModule.require('net');

          // It could be that process has been started with an IPC channel
          // sitting on fd=0, in such case the pipe for this fd is already
          // present and creating a new one will lead to the assertion failure
          // in libuv.
          if (process._channel && process._channel.fd === fd) {
            stdin = new net.Socket({
              handle: process._channel,
              readable: true,
              writable: false
            });
          } else {
            stdin = new net.Socket({
              fd: fd,
              readable: true,
              writable: false
            });
          }
          // Make sure the stdin can't be `.end()`-ed
          stdin._writableState.ended = true;
          break;

        default:
          // Probably an error on in uv_guess_handle()
          throw new Error('Implement me. Unknown stdin file type!');
      }

      // For supporting legacy API we put the FD here.
      stdin.fd = fd;

      // stdin starts out life in a paused state, but node doesn't
      // know yet.  Explicitly to readStop() it to put it in the
      // not-reading state.
      if (stdin._handle && stdin._handle.readStop) {
        stdin._handle.reading = false;
        stdin._readableState.reading = false;
        stdin._handle.readStop();
      }

      // if the user calls stdin.pause(), then we need to stop reading
      // immediately, so that the process can close down.
      stdin.on('pause', function() {
        if (!stdin._handle)
          return;
        stdin._readableState.reading = false;
        stdin._handle.reading = false;
        stdin._handle.readStop();
      });

      return stdin;
    });

    process.openStdin = function() {
      process.stdin.resume();
      return process.stdin;
    };
  };

  startup.processKillAndExit = function() {

    process.exit = function(code) {
      if (code || code === 0)
        process.exitCode = code;

      if (!process._exiting) {
        process._exiting = true;
        process.emit('exit', process.exitCode || 0);
      }
      process.reallyExit(process.exitCode || 0);
    };

    process.kill = function(pid, sig) {
      var err;

      if (pid != (pid | 0)) {
        throw new TypeError('invalid pid');
      }

      // preserve null signal
      if (0 === sig) {
        err = process._kill(pid, 0);
      } else {
        sig = sig || 'SIGTERM';
        if (startup.lazyConstants()[sig] &&
            sig.slice(0, 3) === 'SIG') {
          err = process._kill(pid, startup.lazyConstants()[sig]);
        } else {
          throw new Error('Unknown signal: ' + sig);
        }
      }

      if (err) {
        var errnoException = NativeModule.require('util')._errnoException;
        throw errnoException(err, 'kill');
      }

      return true;
    };
  };

  startup.processSignalHandlers = function() {
    // Load events module in order to access prototype elements on process like
    // process.addListener.
    var signalWraps = {};

    function isSignal(event) {
      return event.slice(0, 3) === 'SIG' &&
             startup.lazyConstants().hasOwnProperty(event);
    }

    // Detect presence of a listener for the special signal types
    process.on('newListener', function(type, listener) {
      if (isSignal(type) &&
          !signalWraps.hasOwnProperty(type)) {
        var Signal = process.binding('signal_wrap').Signal;
        var wrap = new Signal();

        wrap.unref();

        wrap.onsignal = function() { process.emit(type); };

        var signum = startup.lazyConstants()[type];
        var err = wrap.start(signum);
        if (err) {
          wrap.close();
          var errnoException = NativeModule.require('util')._errnoException;
          throw errnoException(err, 'uv_signal_start');
        }

        signalWraps[type] = wrap;
      }
    });

    process.on('removeListener', function(type, listener) {
      if (signalWraps.hasOwnProperty(type) &&
          NativeModule.require('events').listenerCount(this, type) === 0) {
        signalWraps[type].close();
        delete signalWraps[type];
      }
    });
  };


  startup.processChannel = function() {
    // If we were spawned with env NODE_CHANNEL_FD then load that up and
    // start parsing data from that stream.
    if (process.env.NODE_CHANNEL_FD) {
      var fd = parseInt(process.env.NODE_CHANNEL_FD, 10);
      assert(fd >= 0);

      // Make sure it's not accidentally inherited by child processes.
      delete process.env.NODE_CHANNEL_FD;

      var cp = NativeModule.require('child_process');

      // Load tcp_wrap to avoid situation where we might immediately receive
      // a message.
      // FIXME is this really necessary?
      process.binding('tcp_wrap');

      cp._forkChild(fd);
      assert(process.send);
    }
  };


  startup.processRawDebug = function() {
    var format = NativeModule.require('util').format;
    var rawDebug = process._rawDebug;
    process._rawDebug = function() {
      rawDebug(format.apply(null, arguments));
    };
  };

  // Load preload modules
  startup.preloadModules = function() {
    if (process._preload_modules) {
      NativeModule.require('module')._preloadModules(process._preload_modules);
    }
  };

  // Below you find a minimal module system, which is used to load the node
  // core modules found in lib/*.js. All core modules are compiled into the
  // node binary, so they can be loaded faster.

  var ContextifyScript = process.binding('contextify').ContextifyScript;
  function runInThisContext(code, options) {
    var script = new ContextifyScript(code, options);
    return script.runInThisContext();
  }

  function NativeModule(id) {
    this.filename = id + '.js';
    this.id = id;
    this.exports = {};
    this.loaded = false;
  }

  NativeModule._source = process.binding('natives');
  NativeModule._cache = {};

  NativeModule.require = function(id) {
    if (id == 'native_module') {
      return NativeModule;
    }

    var cached = NativeModule.getCached(id);
    if (cached) {
      return cached.exports;
    }

    if (!NativeModule.exists(id)) {
      throw new Error('No such native module ' + id);
    }

    process.moduleLoadList.push('NativeModule ' + id);

    var nativeModule = new NativeModule(id);

    nativeModule.cache();
    nativeModule.compile();

    return nativeModule.exports;
  };

  NativeModule.getCached = function(id) {
    return NativeModule._cache[id];
  };

  NativeModule.exists = function(id) {
    return NativeModule._source.hasOwnProperty(id);
  };

  const EXPOSE_INTERNALS = process.execArgv.some(function(arg) {
    return arg.match(/^--expose[-_]internals$/);
  });

  if (EXPOSE_INTERNALS) {
    NativeModule.nonInternalExists = NativeModule.exists;

    NativeModule.isInternal = function(id) {
      return false;
    };
  } else {
    NativeModule.nonInternalExists = function(id) {
      return NativeModule.exists(id) && !NativeModule.isInternal(id);
    };

    NativeModule.isInternal = function(id) {
      return id.startsWith('internal/');
    };
  }


  NativeModule.getSource = function(id) {
    return NativeModule._source[id];
  };

  NativeModule.wrap = function(script) {
    return NativeModule.wrapper[0] + script + NativeModule.wrapper[1];
  };

  NativeModule.wrapper = [
    '(function (exports, require, module, __filename, __dirname) { ',
    '\n});'
  ];

  NativeModule.prototype.compile = function() {
    var source = NativeModule.getSource(this.id);
    source = NativeModule.wrap(source);

    var fn = runInThisContext(source, { filename: this.filename });
    fn(this.exports, NativeModule.require, this, this.filename);

    this.loaded = true;
  };

  NativeModule.prototype.cache = function() {
    NativeModule._cache[this.id] = this;
  };

  startup();
});
