(function(process, scriptPath) {
  this.global = this;

  var Script = process.binding('evals').NodeScript;
  var runInThisContext = Script.runInThisContext;

  // In normal page, we evaluate built-in modules in node context, and all
  // pages share the same built-in module code.
  var NativeModule = process.NativeModule;

  // Every window should has its own process object.
  global.process = {};
  global.process.__proto__ = process;

  // Add setImmediate.
  global.setImmediate = function() {
    global.process.activateUvLoop();
    var t = NativeModule.require('timers');
    return t.setImmediate.apply(this, arguments);
  };
  global.clearImmediate = function() {
    var t = NativeModule.require('timers');
    return t.clearImmediate.apply(this, arguments);
  };
  global.process.nextTick = global.setImmediate;

  // Inject globals.
  global.global = global;
  global.GLOBAL = global;
  global.root = global;
  global.Buffer = NativeModule.require('buffer').Buffer;

  // Force use module.js in window context, so required third party code will
  // run under window context.
  var source = NativeModule.getSource('module');
  source = NativeModule.wrap(source);

  var modulejs = new NativeModule('module');
  var fn = runInThisContext(source, modulejs.filename, true);
  fn(modulejs.exports, NativeModule.require, modulejs, modulejs.filename);

  var Module = modulejs.exports

  // Emulate node.js script's execution everionment
  var module = new Module('.', null);
  global.process.mainModule = module;

  global.__filename = process.platform == 'win32' ?
      scriptPath.substr(1) : scriptPath;
  global.__dirname = NativeModule.require('path').dirname(global.__filename);

  module.filename = global.__filename;
  module.paths = Module._nodeModulePaths(global.__dirname);
  module.loaded = true;
  module._compile('global.module = module;\n' +
                  'global.require = require;\n', 'nw-emulate-node');

  // Redirect window.onerror to uncaughtException.
  window.onerror = function(error) {
    if (global.process.listeners('uncaughtException').length > 0) {
      global.process.emit('uncaughtException', error);
      return true;
    } else {
      return false;
    }
  }
});
