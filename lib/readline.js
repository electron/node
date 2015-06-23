// Inspiration for this code comes from Salvatore Sanfilippo's linenoise.
// https://github.com/antirez/linenoise
// Reference:
// * http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
// * http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html

'use strict';

const kHistorySize = 30;

const util = require('util');
const inherits = util.inherits;
const Buffer = require('buffer').Buffer;
const EventEmitter = require('events').EventEmitter;


exports.createInterface = function(input, output, completer, terminal) {
  var rl;
  if (arguments.length === 1) {
    rl = new Interface(input);
  } else {
    rl = new Interface(input, output, completer, terminal);
  }
  return rl;
};


function Interface(input, output, completer, terminal) {
  if (!(this instanceof Interface)) {
    // call the constructor preserving original number of arguments
    const self = Object.create(Interface.prototype);
    Interface.apply(self, arguments);
    return self;
  }

  this._sawReturn = false;

  EventEmitter.call(this);
  var historySize;

  if (arguments.length === 1) {
    // an options object was given
    output = input.output;
    completer = input.completer;
    terminal = input.terminal;
    historySize = input.historySize;
    input = input.input;
  }
  historySize = historySize || kHistorySize;

  completer = completer || function() { return []; };

  if (typeof completer !== 'function') {
    throw new TypeError('Argument \'completer\' must be a function');
  }

  if (typeof historySize !== 'number' ||
      isNaN(historySize) ||
      historySize < 0) {
    throw new TypeError('Argument \'historySize\' must be a positive number');
  }

  // backwards compat; check the isTTY prop of the output stream
  //  when `terminal` was not specified
  if (terminal === undefined && !(output === null || output === undefined)) {
    terminal = !!output.isTTY;
  }

  var self = this;

  this.output = output;
  this.input = input;
  this.historySize = historySize;

  // Check arity, 2 - for async, 1 for sync
  this.completer = completer.length === 2 ? completer : function(v, callback) {
    callback(null, completer(v));
  };

  this.setPrompt('> ');

  this.terminal = !!terminal;

  function ondata(data) {
    self._normalWrite(data);
  }

  function onend() {
    if (typeof self._line_buffer === 'string' &&
        self._line_buffer.length > 0) {
      self.emit('line', self._line_buffer);
    }
    self.close();
  }

  function ontermend() {
    if (typeof self.line === 'string' && self.line.length > 0) {
      self.emit('line', self.line);
    }
    self.close();
  }

  function onkeypress(s, key) {
    self._ttyWrite(s, key);
  }

  function onresize() {
    self._refreshLine();
  }

  if (!this.terminal) {
    input.on('data', ondata);
    input.on('end', onend);
    self.once('close', function() {
      input.removeListener('data', ondata);
      input.removeListener('end', onend);
    });
    var StringDecoder = require('string_decoder').StringDecoder; // lazy load
    this._decoder = new StringDecoder('utf8');

  } else {

    exports.emitKeypressEvents(input);

    // input usually refers to stdin
    input.on('keypress', onkeypress);
    input.on('end', ontermend);

    // Current line
    this.line = '';

    this._setRawMode(true);
    this.terminal = true;

    // Cursor position on the line.
    this.cursor = 0;

    this.history = [];
    this.historyIndex = -1;

    if (output !== null && output !== undefined)
      output.on('resize', onresize);

    self.once('close', function() {
      input.removeListener('keypress', onkeypress);
      input.removeListener('end', ontermend);
      if (output !== null && output !== undefined) {
        output.removeListener('resize', onresize);
      }
    });
  }

  input.resume();
}

inherits(Interface, EventEmitter);

Interface.prototype.__defineGetter__('columns', function() {
  var columns = Infinity;
  if (this.output && this.output.columns)
    columns = this.output.columns;
  return columns;
});

Interface.prototype.setPrompt = function(prompt) {
  this._prompt = prompt;
};


Interface.prototype._setRawMode = function(mode) {
  if (typeof this.input.setRawMode === 'function') {
    return this.input.setRawMode(mode);
  }
};


Interface.prototype.prompt = function(preserveCursor) {
  if (this.paused) this.resume();
  if (this.terminal) {
    if (!preserveCursor) this.cursor = 0;
    this._refreshLine();
  } else {
    this._writeToOutput(this._prompt);
  }
};


Interface.prototype.question = function(query, cb) {
  if (typeof cb === 'function') {
    if (this._questionCallback) {
      this.prompt();
    } else {
      this._oldPrompt = this._prompt;
      this.setPrompt(query);
      this._questionCallback = cb;
      this.prompt();
    }
  }
};


Interface.prototype._onLine = function(line) {
  if (this._questionCallback) {
    var cb = this._questionCallback;
    this._questionCallback = null;
    this.setPrompt(this._oldPrompt);
    cb(line);
  } else {
    this.emit('line', line);
  }
};

Interface.prototype._writeToOutput = function _writeToOutput(stringToWrite) {
  if (typeof stringToWrite !== 'string')
    throw new TypeError('stringToWrite must be a string');

  if (this.output !== null && this.output !== undefined)
    this.output.write(stringToWrite);
};

Interface.prototype._addHistory = function() {
  if (this.line.length === 0) return '';

  if (this.history.length === 0 || this.history[0] !== this.line) {
    this.history.unshift(this.line);

    // Only store so many
    if (this.history.length > this.historySize) this.history.pop();
  }

  this.historyIndex = -1;
  return this.history[0];
};


Interface.prototype._refreshLine = function() {
  // line length
  var line = this._prompt + this.line;
  var dispPos = this._getDisplayPos(line);
  var lineCols = dispPos.cols;
  var lineRows = dispPos.rows;

  // cursor position
  var cursorPos = this._getCursorPos();

  // first move to the bottom of the current line, based on cursor pos
  var prevRows = this.prevRows || 0;
  if (prevRows > 0) {
    exports.moveCursor(this.output, 0, -prevRows);
  }

  // Cursor to left edge.
  exports.cursorTo(this.output, 0);
  // erase data
  exports.clearScreenDown(this.output);

  // Write the prompt and the current buffer content.
  this._writeToOutput(line);

  // Force terminal to allocate a new line
  if (lineCols === 0) {
    this._writeToOutput(' ');
  }

  // Move cursor to original position.
  exports.cursorTo(this.output, cursorPos.cols);

  var diff = lineRows - cursorPos.rows;
  if (diff > 0) {
    exports.moveCursor(this.output, 0, -diff);
  }

  this.prevRows = cursorPos.rows;
};


Interface.prototype.close = function() {
  if (this.closed) return;
  this.pause();
  if (this.terminal) {
    this._setRawMode(false);
  }
  this.closed = true;
  this.emit('close');
};


Interface.prototype.pause = function() {
  if (this.paused) return;
  this.input.pause();
  this.paused = true;
  this.emit('pause');
  return this;
};


Interface.prototype.resume = function() {
  if (!this.paused) return;
  this.input.resume();
  this.paused = false;
  this.emit('resume');
  return this;
};


Interface.prototype.write = function(d, key) {
  if (this.paused) this.resume();
  this.terminal ? this._ttyWrite(d, key) : this._normalWrite(d);
};

// \r\n, \n, or \r followed by something other than \n
const lineEnding = /\r?\n|\r(?!\n)/;
Interface.prototype._normalWrite = function(b) {
  if (b === undefined) {
    return;
  }
  var string = this._decoder.write(b);
  if (this._sawReturn) {
    string = string.replace(/^\n/, '');
    this._sawReturn = false;
  }

  // Run test() on the new string chunk, not on the entire line buffer.
  var newPartContainsEnding = lineEnding.test(string);

  if (this._line_buffer) {
    string = this._line_buffer + string;
    this._line_buffer = null;
  }
  if (newPartContainsEnding) {
    this._sawReturn = /\r$/.test(string);

    // got one or more newlines; process into "line" events
    var lines = string.split(lineEnding);
    // either '' or (concievably) the unfinished portion of the next line
    string = lines.pop();
    this._line_buffer = string;
    lines.forEach(function(line) {
      this._onLine(line);
    }, this);
  } else if (string) {
    // no newlines this time, save what we have for next time
    this._line_buffer = string;
  }
};

Interface.prototype._insertString = function(c) {
  //BUG: Problem when adding tabs with following content.
  //     Perhaps the bug is in _refreshLine(). Not sure.
  //     A hack would be to insert spaces instead of literal '\t'.
  if (this.cursor < this.line.length) {
    var beg = this.line.slice(0, this.cursor);
    var end = this.line.slice(this.cursor, this.line.length);
    this.line = beg + c + end;
    this.cursor += c.length;
    this._refreshLine();
  } else {
    this.line += c;
    this.cursor += c.length;

    if (this._getCursorPos().cols === 0) {
      this._refreshLine();
    } else {
      this._writeToOutput(c);
    }

    // a hack to get the line refreshed if it's needed
    this._moveCursor(0);
  }
};

Interface.prototype._tabComplete = function() {
  var self = this;

  self.pause();
  self.completer(self.line.slice(0, self.cursor), function(err, rv) {
    self.resume();

    if (err) {
      // XXX Log it somewhere?
      return;
    }

    var completions = rv[0],
        completeOn = rv[1];  // the text that was completed
    if (completions && completions.length) {
      // Apply/show completions.
      if (completions.length === 1) {
        self._insertString(completions[0].slice(completeOn.length));
      } else {
        self._writeToOutput('\r\n');
        var width = completions.reduce(function(a, b) {
          return a.length > b.length ? a : b;
        }).length + 2;  // 2 space padding
        var maxColumns = Math.floor(self.columns / width) || 1;
        var group = [], c;
        for (var i = 0, compLen = completions.length; i < compLen; i++) {
          c = completions[i];
          if (c === '') {
            handleGroup(self, group, width, maxColumns);
            group = [];
          } else {
            group.push(c);
          }
        }
        handleGroup(self, group, width, maxColumns);

        // If there is a common prefix to all matches, then apply that
        // portion.
        var f = completions.filter(function(e) { if (e) return e; });
        var prefix = commonPrefix(f);
        if (prefix.length > completeOn.length) {
          self._insertString(prefix.slice(completeOn.length));
        }

      }
      self._refreshLine();
    }
  });
};

// this = Interface instance
function handleGroup(self, group, width, maxColumns) {
  if (group.length == 0) {
    return;
  }
  var minRows = Math.ceil(group.length / maxColumns);
  for (var row = 0; row < minRows; row++) {
    for (var col = 0; col < maxColumns; col++) {
      var idx = row * maxColumns + col;
      if (idx >= group.length) {
        break;
      }
      var item = group[idx];
      self._writeToOutput(item);
      if (col < maxColumns - 1) {
        for (var s = 0, itemLen = item.length; s < width - itemLen;
             s++) {
          self._writeToOutput(' ');
        }
      }
    }
    self._writeToOutput('\r\n');
  }
  self._writeToOutput('\r\n');
}

function commonPrefix(strings) {
  if (!strings || strings.length == 0) {
    return '';
  }
  var sorted = strings.slice().sort();
  var min = sorted[0];
  var max = sorted[sorted.length - 1];
  for (var i = 0, len = min.length; i < len; i++) {
    if (min[i] != max[i]) {
      return min.slice(0, i);
    }
  }
  return min;
}


Interface.prototype._wordLeft = function() {
  if (this.cursor > 0) {
    var leading = this.line.slice(0, this.cursor);
    var match = leading.match(/([^\w\s]+|\w+|)\s*$/);
    this._moveCursor(-match[0].length);
  }
};


Interface.prototype._wordRight = function() {
  if (this.cursor < this.line.length) {
    var trailing = this.line.slice(this.cursor);
    var match = trailing.match(/^(\s+|\W+|\w+)\s*/);
    this._moveCursor(match[0].length);
  }
};


Interface.prototype._deleteLeft = function() {
  if (this.cursor > 0 && this.line.length > 0) {
    this.line = this.line.slice(0, this.cursor - 1) +
                this.line.slice(this.cursor, this.line.length);

    this.cursor--;
    this._refreshLine();
  }
};


Interface.prototype._deleteRight = function() {
  this.line = this.line.slice(0, this.cursor) +
              this.line.slice(this.cursor + 1, this.line.length);
  this._refreshLine();
};


Interface.prototype._deleteWordLeft = function() {
  if (this.cursor > 0) {
    var leading = this.line.slice(0, this.cursor);
    var match = leading.match(/([^\w\s]+|\w+|)\s*$/);
    leading = leading.slice(0, leading.length - match[0].length);
    this.line = leading + this.line.slice(this.cursor, this.line.length);
    this.cursor = leading.length;
    this._refreshLine();
  }
};


Interface.prototype._deleteWordRight = function() {
  if (this.cursor < this.line.length) {
    var trailing = this.line.slice(this.cursor);
    var match = trailing.match(/^(\s+|\W+|\w+)\s*/);
    this.line = this.line.slice(0, this.cursor) +
                trailing.slice(match[0].length);
    this._refreshLine();
  }
};


Interface.prototype._deleteLineLeft = function() {
  this.line = this.line.slice(this.cursor);
  this.cursor = 0;
  this._refreshLine();
};


Interface.prototype._deleteLineRight = function() {
  this.line = this.line.slice(0, this.cursor);
  this._refreshLine();
};


Interface.prototype.clearLine = function() {
  this._moveCursor(+Infinity);
  this._writeToOutput('\r\n');
  this.line = '';
  this.cursor = 0;
  this.prevRows = 0;
};


Interface.prototype._line = function() {
  var line = this._addHistory();
  this.clearLine();
  this._onLine(line);
};


Interface.prototype._historyNext = function() {
  if (this.historyIndex > 0) {
    this.historyIndex--;
    this.line = this.history[this.historyIndex];
    this.cursor = this.line.length; // set cursor to end of line.
    this._refreshLine();

  } else if (this.historyIndex === 0) {
    this.historyIndex = -1;
    this.cursor = 0;
    this.line = '';
    this._refreshLine();
  }
};


Interface.prototype._historyPrev = function() {
  if (this.historyIndex + 1 < this.history.length) {
    this.historyIndex++;
    this.line = this.history[this.historyIndex];
    this.cursor = this.line.length; // set cursor to end of line.

    this._refreshLine();
  }
};


// Returns the last character's display position of the given string
Interface.prototype._getDisplayPos = function(str) {
  var offset = 0;
  var col = this.columns;
  var row = 0;
  var code;
  str = stripVTControlCharacters(str);
  for (var i = 0, len = str.length; i < len; i++) {
    code = str.codePointAt(i);
    if (code >= 0x10000) { // surrogates
      i++;
    }
    if (code === 0x0a) { // new line \n
      offset = 0;
      row += 1;
      continue;
    }
    if (isFullWidthCodePoint(code)) {
      if ((offset + 1) % col === 0) {
        offset++;
      }
      offset += 2;
    } else {
      offset++;
    }
  }
  var cols = offset % col;
  var rows = row + (offset - cols) / col;
  return {cols: cols, rows: rows};
};


// Returns current cursor's position and line
Interface.prototype._getCursorPos = function() {
  var columns = this.columns;
  var strBeforeCursor = this._prompt + this.line.substring(0, this.cursor);
  var dispPos = this._getDisplayPos(stripVTControlCharacters(strBeforeCursor));
  var cols = dispPos.cols;
  var rows = dispPos.rows;
  // If the cursor is on a full-width character which steps over the line,
  // move the cursor to the beginning of the next line.
  if (cols + 1 === columns &&
      this.cursor < this.line.length &&
      isFullWidthCodePoint(this.line.codePointAt(this.cursor))) {
    rows++;
    cols = 0;
  }
  return {cols: cols, rows: rows};
};


// This function moves cursor dx places to the right
// (-dx for left) and refreshes the line if it is needed
Interface.prototype._moveCursor = function(dx) {
  var oldcursor = this.cursor;
  var oldPos = this._getCursorPos();
  this.cursor += dx;

  // bounds check
  if (this.cursor < 0) this.cursor = 0;
  else if (this.cursor > this.line.length) this.cursor = this.line.length;

  var newPos = this._getCursorPos();

  // check if cursors are in the same line
  if (oldPos.rows === newPos.rows) {
    var diffCursor = this.cursor - oldcursor;
    var diffWidth;
    if (diffCursor < 0) {
      diffWidth = -getStringWidth(
          this.line.substring(this.cursor, oldcursor)
          );
    } else if (diffCursor > 0) {
      diffWidth = getStringWidth(
          this.line.substring(this.cursor, oldcursor)
          );
    }
    exports.moveCursor(this.output, diffWidth, 0);
    this.prevRows = newPos.rows;
  } else {
    this._refreshLine();
  }
};


// handle a write from the tty
Interface.prototype._ttyWrite = function(s, key) {
  key = key || {};

  // Ignore escape key - Fixes #2876
  if (key.name == 'escape') return;

  if (key.ctrl && key.shift) {
    /* Control and shift pressed */
    switch (key.name) {
      case 'backspace':
        this._deleteLineLeft();
        break;

      case 'delete':
        this._deleteLineRight();
        break;
    }

  } else if (key.ctrl) {
    /* Control key pressed */

    switch (key.name) {
      case 'c':
        if (EventEmitter.listenerCount(this, 'SIGINT') > 0) {
          this.emit('SIGINT');
        } else {
          // This readline instance is finished
          this.close();
        }
        break;

      case 'h': // delete left
        this._deleteLeft();
        break;

      case 'd': // delete right or EOF
        if (this.cursor === 0 && this.line.length === 0) {
          // This readline instance is finished
          this.close();
        } else if (this.cursor < this.line.length) {
          this._deleteRight();
        }
        break;

      case 'u': // delete the whole line
        this.cursor = 0;
        this.line = '';
        this._refreshLine();
        break;

      case 'k': // delete from current to end of line
        this._deleteLineRight();
        break;

      case 'a': // go to the start of the line
        this._moveCursor(-Infinity);
        break;

      case 'e': // go to the end of the line
        this._moveCursor(+Infinity);
        break;

      case 'b': // back one character
        this._moveCursor(-1);
        break;

      case 'f': // forward one character
        this._moveCursor(+1);
        break;

      case 'l': // clear the whole screen
        exports.cursorTo(this.output, 0, 0);
        exports.clearScreenDown(this.output);
        this._refreshLine();
        break;

      case 'n': // next history item
        this._historyNext();
        break;

      case 'p': // previous history item
        this._historyPrev();
        break;

      case 'z':
        if (process.platform == 'win32') break;
        if (EventEmitter.listenerCount(this, 'SIGTSTP') > 0) {
          this.emit('SIGTSTP');
        } else {
          process.once('SIGCONT', (function(self) {
            return function() {
              // Don't raise events if stream has already been abandoned.
              if (!self.paused) {
                // Stream must be paused and resumed after SIGCONT to catch
                // SIGINT, SIGTSTP, and EOF.
                self.pause();
                self.emit('SIGCONT');
              }
              // explicitly re-enable "raw mode" and move the cursor to
              // the correct position.
              // See https://github.com/joyent/node/issues/3295.
              self._setRawMode(true);
              self._refreshLine();
            };
          })(this));
          this._setRawMode(false);
          process.kill(process.pid, 'SIGTSTP');
        }
        break;

      case 'w': // delete backwards to a word boundary
      case 'backspace':
        this._deleteWordLeft();
        break;

      case 'delete': // delete forward to a word boundary
        this._deleteWordRight();
        break;

      case 'left':
        this._wordLeft();
        break;

      case 'right':
        this._wordRight();
        break;
    }

  } else if (key.meta) {
    /* Meta key pressed */

    switch (key.name) {
      case 'b': // backward word
        this._wordLeft();
        break;

      case 'f': // forward word
        this._wordRight();
        break;

      case 'd': // delete forward word
      case 'delete':
        this._deleteWordRight();
        break;

      case 'backspace': // delete backwards to a word boundary
        this._deleteWordLeft();
        break;
    }

  } else {
    /* No modifier keys used */

    // \r bookkeeping is only relevant if a \n comes right after.
    if (this._sawReturn && key.name !== 'enter')
      this._sawReturn = false;

    switch (key.name) {
      case 'return':  // carriage return, i.e. \r
        this._sawReturn = true;
        this._line();
        break;

      case 'enter':
        if (this._sawReturn)
          this._sawReturn = false;
        else
          this._line();
        break;

      case 'backspace':
        this._deleteLeft();
        break;

      case 'delete':
        this._deleteRight();
        break;

      case 'tab': // tab completion
        this._tabComplete();
        break;

      case 'left':
        this._moveCursor(-1);
        break;

      case 'right':
        this._moveCursor(+1);
        break;

      case 'home':
        this._moveCursor(-Infinity);
        break;

      case 'end':
        this._moveCursor(+Infinity);
        break;

      case 'up':
        this._historyPrev();
        break;

      case 'down':
        this._historyNext();
        break;

      default:
        if (s instanceof Buffer)
          s = s.toString('utf-8');

        if (s) {
          var lines = s.split(/\r\n|\n|\r/);
          for (var i = 0, len = lines.length; i < len; i++) {
            if (i > 0) {
              this._line();
            }
            this._insertString(lines[i]);
          }
        }
    }
  }
};


exports.Interface = Interface;


/**
 * accepts a readable Stream instance and makes it emit "keypress" events
 */

const KEYPRESS_DECODER = Symbol('keypress-decoder');
const ESCAPE_DECODER = Symbol('escape-decoder');

function emitKeypressEvents(stream) {
  if (stream[KEYPRESS_DECODER]) return;
  var StringDecoder = require('string_decoder').StringDecoder; // lazy load
  stream[KEYPRESS_DECODER] = new StringDecoder('utf8');

  stream[ESCAPE_DECODER] = emitKeys(stream);
  stream[ESCAPE_DECODER].next();

  function onData(b) {
    if (EventEmitter.listenerCount(stream, 'keypress') > 0) {
      var r = stream[KEYPRESS_DECODER].write(b);
      if (r) {
        for (var i = 0; i < r.length; i++) {
          stream[ESCAPE_DECODER].next(r[i]);
        }
      }
    } else {
      // Nobody's watching anyway
      stream.removeListener('data', onData);
      stream.on('newListener', onNewListener);
    }
  }

  function onNewListener(event) {
    if (event == 'keypress') {
      stream.on('data', onData);
      stream.removeListener('newListener', onNewListener);
    }
  }

  if (EventEmitter.listenerCount(stream, 'keypress') > 0) {
    stream.on('data', onData);
  } else {
    stream.on('newListener', onNewListener);
  }
}
exports.emitKeypressEvents = emitKeypressEvents;

/*
  Some patterns seen in terminal key escape codes, derived from combos seen
  at http://www.midnight-commander.org/browser/lib/tty/key.c

  ESC letter
  ESC [ letter
  ESC [ modifier letter
  ESC [ 1 ; modifier letter
  ESC [ num char
  ESC [ num ; modifier char
  ESC O letter
  ESC O modifier letter
  ESC O 1 ; modifier letter
  ESC N letter
  ESC [ [ num ; modifier char
  ESC [ [ 1 ; modifier letter
  ESC ESC [ num char
  ESC ESC O letter

  - char is usually ~ but $ and ^ also happen with rxvt
  - modifier is 1 +
                (shift     * 1) +
                (left_alt  * 2) +
                (ctrl      * 4) +
                (right_alt * 8)
  - two leading ESCs apparently mean the same as one leading ESC
*/

// Regexes used for ansi escape code splitting
const metaKeyCodeReAnywhere = /(?:\x1b)([a-zA-Z0-9])/;
const functionKeyCodeReAnywhere = new RegExp('(?:\x1b+)(O|N|\\[|\\[\\[)(?:' + [
  '(\\d+)(?:;(\\d+))?([~^$])',
  '(?:M([@ #!a`])(.)(.))', // mouse
  '(?:1;)?(\\d+)?([a-zA-Z])'
].join('|') + ')');


function* emitKeys(stream) {
  while (true) {
    var ch = yield;
    var s = ch;
    var escaped = false;
    var key = {
      sequence: null,
      name: undefined,
      ctrl: false,
      meta: false,
      shift: false
    };

    if (ch === '\x1b') {
      escaped = true;
      s += (ch = yield);

      if (ch === '\x1b') {
        s += (ch = yield);
      }
    }

    if (escaped && (ch === 'O' || ch === '[')) {
      // ansi escape sequence
      var code = ch;
      var modifier = 0;

      if (ch === 'O') {
        // ESC O letter
        // ESC O modifier letter
        s += (ch = yield);

        if (ch >= '0' && ch <= '9') {
          modifier = (ch >> 0) - 1;
          s += (ch = yield);
        }

        code += ch;

      } else if (ch === '[') {
        // ESC [ letter
        // ESC [ modifier letter
        // ESC [ [ modifier letter
        // ESC [ [ num char
        s += (ch = yield);

        if (ch === '[') {
          // \x1b[[A
          //      ^--- escape codes might have a second bracket
          code += ch;
          s += (ch = yield);
        }

        /*
         * Here and later we try to buffer just enough data to get
         * a complete ascii sequence.
         *
         * We have basically two classes of ascii characters to process:
         *
         *
         * 1. `\x1b[24;5~` should be parsed as { code: '[24~', modifier: 5 }
         *
         * This particular example is featuring Ctrl+F12 in xterm.
         *
         *  - `;5` part is optional, e.g. it could be `\x1b[24~`
         *  - first part can contain one or two digits
         *
         * So the generic regexp is like /^\d\d?(;\d)?[~^$]$/
         *
         *
         * 2. `\x1b[1;5H` should be parsed as { code: '[H', modifier: 5 }
         *
         * This particular example is featuring Ctrl+Home in xterm.
         *
         *  - `1;5` part is optional, e.g. it could be `\x1b[H`
         *  - `1;` part is optional, e.g. it could be `\x1b[5H`
         *
         * So the generic regexp is like /^((\d;)?\d)?[A-Za-z]$/
         *
         */
        const cmdStart = s.length - 1;

        // skip one or two leading digits
        if (ch >= '0' && ch <= '9') {
          s += (ch = yield);

          if (ch >= '0' && ch <= '9') {
            s += (ch = yield);
          }
        }

        // skip modifier
        if (ch === ';') {
          s += (ch = yield);

          if (ch >= '0' && ch <= '9') {
            s += (ch = yield);
          }
        }

        /*
         * We buffered enough data, now trying to extract code
         * and modifier from it
         */
        const cmd = s.slice(cmdStart);
        var match;

        if ((match = cmd.match(/^(\d\d?)(;(\d))?([~^$])$/))) {
          code += match[1] + match[4];
          modifier = (match[3] || 1) - 1;
        } else if ((match = cmd.match(/^((\d;)?(\d))?([A-Za-z])$/))) {
          code += match[4];
          modifier = (match[3] || 1) - 1;
        } else {
          code += cmd;
        }
      }

      // Parse the key modifier
      key.ctrl = !!(modifier & 4);
      key.meta = !!(modifier & 10);
      key.shift = !!(modifier & 1);
      key.code = code;

      // Parse the key itself
      switch (code) {
        /* xterm/gnome ESC O letter */
        case 'OP': key.name = 'f1'; break;
        case 'OQ': key.name = 'f2'; break;
        case 'OR': key.name = 'f3'; break;
        case 'OS': key.name = 'f4'; break;

        /* xterm/rxvt ESC [ number ~ */
        case '[11~': key.name = 'f1'; break;
        case '[12~': key.name = 'f2'; break;
        case '[13~': key.name = 'f3'; break;
        case '[14~': key.name = 'f4'; break;

        /* from Cygwin and used in libuv */
        case '[[A': key.name = 'f1'; break;
        case '[[B': key.name = 'f2'; break;
        case '[[C': key.name = 'f3'; break;
        case '[[D': key.name = 'f4'; break;
        case '[[E': key.name = 'f5'; break;

        /* common */
        case '[15~': key.name = 'f5'; break;
        case '[17~': key.name = 'f6'; break;
        case '[18~': key.name = 'f7'; break;
        case '[19~': key.name = 'f8'; break;
        case '[20~': key.name = 'f9'; break;
        case '[21~': key.name = 'f10'; break;
        case '[23~': key.name = 'f11'; break;
        case '[24~': key.name = 'f12'; break;

        /* xterm ESC [ letter */
        case '[A': key.name = 'up'; break;
        case '[B': key.name = 'down'; break;
        case '[C': key.name = 'right'; break;
        case '[D': key.name = 'left'; break;
        case '[E': key.name = 'clear'; break;
        case '[F': key.name = 'end'; break;
        case '[H': key.name = 'home'; break;

        /* xterm/gnome ESC O letter */
        case 'OA': key.name = 'up'; break;
        case 'OB': key.name = 'down'; break;
        case 'OC': key.name = 'right'; break;
        case 'OD': key.name = 'left'; break;
        case 'OE': key.name = 'clear'; break;
        case 'OF': key.name = 'end'; break;
        case 'OH': key.name = 'home'; break;

        /* xterm/rxvt ESC [ number ~ */
        case '[1~': key.name = 'home'; break;
        case '[2~': key.name = 'insert'; break;
        case '[3~': key.name = 'delete'; break;
        case '[4~': key.name = 'end'; break;
        case '[5~': key.name = 'pageup'; break;
        case '[6~': key.name = 'pagedown'; break;

        /* putty */
        case '[[5~': key.name = 'pageup'; break;
        case '[[6~': key.name = 'pagedown'; break;

        /* rxvt */
        case '[7~': key.name = 'home'; break;
        case '[8~': key.name = 'end'; break;

        /* rxvt keys with modifiers */
        case '[a': key.name = 'up'; key.shift = true; break;
        case '[b': key.name = 'down'; key.shift = true; break;
        case '[c': key.name = 'right'; key.shift = true; break;
        case '[d': key.name = 'left'; key.shift = true; break;
        case '[e': key.name = 'clear'; key.shift = true; break;

        case '[2$': key.name = 'insert'; key.shift = true; break;
        case '[3$': key.name = 'delete'; key.shift = true; break;
        case '[5$': key.name = 'pageup'; key.shift = true; break;
        case '[6$': key.name = 'pagedown'; key.shift = true; break;
        case '[7$': key.name = 'home'; key.shift = true; break;
        case '[8$': key.name = 'end'; key.shift = true; break;

        case 'Oa': key.name = 'up'; key.ctrl = true; break;
        case 'Ob': key.name = 'down'; key.ctrl = true; break;
        case 'Oc': key.name = 'right'; key.ctrl = true; break;
        case 'Od': key.name = 'left'; key.ctrl = true; break;
        case 'Oe': key.name = 'clear'; key.ctrl = true; break;

        case '[2^': key.name = 'insert'; key.ctrl = true; break;
        case '[3^': key.name = 'delete'; key.ctrl = true; break;
        case '[5^': key.name = 'pageup'; key.ctrl = true; break;
        case '[6^': key.name = 'pagedown'; key.ctrl = true; break;
        case '[7^': key.name = 'home'; key.ctrl = true; break;
        case '[8^': key.name = 'end'; key.ctrl = true; break;

        /* misc. */
        case '[Z': key.name = 'tab'; key.shift = true; break;
        default: key.name = 'undefined'; break;
      }

    } else if (ch === '\r') {
      // carriage return
      key.name = 'return';

    } else if (ch === '\n') {
      // enter, should have been called linefeed
      key.name = 'enter';

    } else if (ch === '\t') {
      // tab
      key.name = 'tab';

    } else if (ch === '\b' || ch === '\x7f') {
      // backspace or ctrl+h
      key.name = 'backspace';
      key.meta = escaped;

    } else if (ch === '\x1b') {
      // escape key
      key.name = 'escape';
      key.meta = escaped;

    } else if (ch === ' ') {
      key.name = 'space';
      key.meta = escaped;

    } else if (!escaped && ch <= '\x1a') {
      // ctrl+letter
      key.name = String.fromCharCode(ch.charCodeAt(0) + 'a'.charCodeAt(0) - 1);
      key.ctrl = true;

    } else if (/^[0-9A-Za-z]$/.test(ch)) {
      // letter, number, shift+letter
      key.name = ch.toLowerCase();
      key.shift = /^[A-Z]$/.test(ch);
      key.meta = escaped;
    }

    key.sequence = s;

    if (key.name !== undefined) {
      /* Named character or sequence */
      stream.emit('keypress', escaped ? undefined : s, key);
    } else if (s.length === 1) {
      /* Single unnamed character, e.g. "." */
      stream.emit('keypress', s);
    } else {
      /* Unrecognized or broken escape sequence, don't emit anything */
    }
  }
}


/**
 * moves the cursor to the x and y coordinate on the given stream
 */

function cursorTo(stream, x, y) {
  if (stream === null || stream === undefined)
    return;

  if (typeof x !== 'number' && typeof y !== 'number')
    return;

  if (typeof x !== 'number')
    throw new Error("Can't set cursor row without also setting it's column");

  if (typeof y !== 'number') {
    stream.write('\x1b[' + (x + 1) + 'G');
  } else {
    stream.write('\x1b[' + (y + 1) + ';' + (x + 1) + 'H');
  }
}
exports.cursorTo = cursorTo;


/**
 * moves the cursor relative to its current location
 */

function moveCursor(stream, dx, dy) {
  if (stream === null || stream === undefined)
    return;

  if (dx < 0) {
    stream.write('\x1b[' + (-dx) + 'D');
  } else if (dx > 0) {
    stream.write('\x1b[' + dx + 'C');
  }

  if (dy < 0) {
    stream.write('\x1b[' + (-dy) + 'A');
  } else if (dy > 0) {
    stream.write('\x1b[' + dy + 'B');
  }
}
exports.moveCursor = moveCursor;


/**
 * clears the current line the cursor is on:
 *   -1 for left of the cursor
 *   +1 for right of the cursor
 *    0 for the entire line
 */

function clearLine(stream, dir) {
  if (stream === null || stream === undefined)
    return;

  if (dir < 0) {
    // to the beginning
    stream.write('\x1b[1K');
  } else if (dir > 0) {
    // to the end
    stream.write('\x1b[0K');
  } else {
    // entire line
    stream.write('\x1b[2K');
  }
}
exports.clearLine = clearLine;


/**
 * clears the screen from the current position of the cursor down
 */

function clearScreenDown(stream) {
  if (stream === null || stream === undefined)
    return;

  stream.write('\x1b[0J');
}
exports.clearScreenDown = clearScreenDown;


/**
 * Returns the number of columns required to display the given string.
 */

function getStringWidth(str) {
  var width = 0;
  str = stripVTControlCharacters(str);
  for (var i = 0, len = str.length; i < len; i++) {
    var code = str.codePointAt(i);
    if (code >= 0x10000) { // surrogates
      i++;
    }
    if (isFullWidthCodePoint(code)) {
      width += 2;
    } else {
      width++;
    }
  }
  return width;
}
exports.getStringWidth = getStringWidth;


/**
 * Returns true if the character represented by a given
 * Unicode code point is full-width. Otherwise returns false.
 */

function isFullWidthCodePoint(code) {
  if (isNaN(code)) {
    return false;
  }

  // Code points are derived from:
  // http://www.unicode.org/Public/UNIDATA/EastAsianWidth.txt
  if (code >= 0x1100 && (
      code <= 0x115f ||  // Hangul Jamo
      0x2329 === code || // LEFT-POINTING ANGLE BRACKET
      0x232a === code || // RIGHT-POINTING ANGLE BRACKET
      // CJK Radicals Supplement .. Enclosed CJK Letters and Months
      (0x2e80 <= code && code <= 0x3247 && code !== 0x303f) ||
      // Enclosed CJK Letters and Months .. CJK Unified Ideographs Extension A
      0x3250 <= code && code <= 0x4dbf ||
      // CJK Unified Ideographs .. Yi Radicals
      0x4e00 <= code && code <= 0xa4c6 ||
      // Hangul Jamo Extended-A
      0xa960 <= code && code <= 0xa97c ||
      // Hangul Syllables
      0xac00 <= code && code <= 0xd7a3 ||
      // CJK Compatibility Ideographs
      0xf900 <= code && code <= 0xfaff ||
      // Vertical Forms
      0xfe10 <= code && code <= 0xfe19 ||
      // CJK Compatibility Forms .. Small Form Variants
      0xfe30 <= code && code <= 0xfe6b ||
      // Halfwidth and Fullwidth Forms
      0xff01 <= code && code <= 0xff60 ||
      0xffe0 <= code && code <= 0xffe6 ||
      // Kana Supplement
      0x1b000 <= code && code <= 0x1b001 ||
      // Enclosed Ideographic Supplement
      0x1f200 <= code && code <= 0x1f251 ||
      // CJK Unified Ideographs Extension B .. Tertiary Ideographic Plane
      0x20000 <= code && code <= 0x3fffd)) {
    return true;
  }
  return false;
}
exports.isFullWidthCodePoint = isFullWidthCodePoint;


/**
 * Returns the Unicode code point for the character at the
 * given index in the given string. Similar to String.charCodeAt(),
 * but this function handles surrogates (code point >= 0x10000).
 */

function codePointAt(str, index) {
  var code = str.charCodeAt(index);
  var low;
  if (0xd800 <= code && code <= 0xdbff) { // High surrogate
    low = str.charCodeAt(index + 1);
    if (!isNaN(low)) {
      code = 0x10000 + (code - 0xd800) * 0x400 + (low - 0xdc00);
    }
  }
  return code;
}
exports.codePointAt = util.deprecate(codePointAt,
    'codePointAt() is deprecated. Use String.prototype.codePointAt');


/**
 * Tries to remove all VT control characters. Use to estimate displayed
 * string width. May be buggy due to not running a real state machine
 */
function stripVTControlCharacters(str) {
  str = str.replace(new RegExp(functionKeyCodeReAnywhere.source, 'g'), '');
  return str.replace(new RegExp(metaKeyCodeReAnywhere.source, 'g'), '');
}
exports.stripVTControlCharacters = stripVTControlCharacters;
