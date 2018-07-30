#!/usr/bin/env python
#
# Copyright 2006-2008 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# This is a utility for converting JavaScript source code into C-style
# char arrays. It is used for embedded JavaScript code in the V8
# library.

import os
import re
import sys
import string


def ToCArray(elements, step=10):
  slices = (elements[i:i+step] for i in xrange(0, len(elements), step))
  slices = map(lambda s: ','.join(str(x) for x in s), slices)
  return ',\n'.join(slices)


def ToCString(contents):
  return ToCArray(map(ord, contents), step=20)


def ReadFile(filename):
  file = open(filename, "rt")
  try:
    lines = file.read()
  finally:
    file.close()
  return lines


def ReadLines(filename):
  result = []
  for line in open(filename, "rt"):
    if '#' in line:
      line = line[:line.index('#')]
    line = line.strip()
    if len(line) > 0:
      result.append(line)
  return result


def ExpandConstants(lines, constants):
  for key, value in constants.items():
    lines = lines.replace(key, str(value))
  return lines


def ExpandMacros(lines, macros):
  def expander(s):
    return ExpandMacros(s, macros)
  for name, macro in macros.items():
    name_pattern = re.compile("\\b%s\\(" % name)
    pattern_match = name_pattern.search(lines, 0)
    while pattern_match is not None:
      # Scan over the arguments
      height = 1
      start = pattern_match.start()
      end = pattern_match.end()
      assert lines[end - 1] == '('
      last_match = end
      arg_index = [0]  # Wrap state into array, to work around Python "scoping"
      mapping = {}
      def add_arg(str):
        # Remember to expand recursively in the arguments
        if arg_index[0] >= len(macro.args):
          return
        replacement = expander(str.strip())
        mapping[macro.args[arg_index[0]]] = replacement
        arg_index[0] += 1
      while end < len(lines) and height > 0:
        # We don't count commas at higher nesting levels.
        if lines[end] == ',' and height == 1:
          add_arg(lines[last_match:end])
          last_match = end + 1
        elif lines[end] in ['(', '{', '[']:
          height = height + 1
        elif lines[end] in [')', '}', ']']:
          height = height - 1
        end = end + 1
      # Remember to add the last match.
      add_arg(lines[last_match:end-1])
      if arg_index[0] < len(macro.args) -1:
        lineno = lines.count(os.linesep, 0, start) + 1
        raise Exception('line %s: Too few arguments for macro "%s"' % (lineno, name))
      result = macro.expand(mapping)
      # Replace the occurrence of the macro with the expansion
      lines = lines[:start] + result + lines[end:]
      pattern_match = name_pattern.search(lines, start + len(result))
  return lines


class TextMacro:
  def __init__(self, args, body):
    self.args = args
    self.body = body
  def expand(self, mapping):
    result = self.body
    for key, value in mapping.items():
        result = result.replace(key, value)
    return result

class PythonMacro:
  def __init__(self, args, fun):
    self.args = args
    self.fun = fun
  def expand(self, mapping):
    args = []
    for arg in self.args:
      args.append(mapping[arg])
    return str(self.fun(*args))

CONST_PATTERN = re.compile('^const\s+([a-zA-Z0-9_]+)\s*=\s*([^;]*);$')
MACRO_PATTERN = re.compile('^macro\s+([a-zA-Z0-9_]+)\s*\(([^)]*)\)\s*=\s*([^;]*);$')
PYTHON_MACRO_PATTERN = re.compile('^python\s+macro\s+([a-zA-Z0-9_]+)\s*\(([^)]*)\)\s*=\s*([^;]*);$')

def ReadMacros(lines):
  constants = { }
  macros = { }
  for line in lines:
    hash = line.find('#')
    if hash != -1: line = line[:hash]
    line = line.strip()
    if len(line) is 0: continue
    const_match = CONST_PATTERN.match(line)
    if const_match:
      name = const_match.group(1)
      value = const_match.group(2).strip()
      constants[name] = value
    else:
      macro_match = MACRO_PATTERN.match(line)
      if macro_match:
        name = macro_match.group(1)
        args = map(string.strip, macro_match.group(2).split(','))
        body = macro_match.group(3).strip()
        macros[name] = TextMacro(args, body)
      else:
        python_match = PYTHON_MACRO_PATTERN.match(line)
        if python_match:
          name = python_match.group(1)
          args = map(string.strip, python_match.group(2).split(','))
          body = python_match.group(3).strip()
          fun = eval("lambda " + ",".join(args) + ': ' + body)
          macros[name] = PythonMacro(args, fun)
        else:
          raise Exception("Illegal line: " + line)
  return (constants, macros)


TEMPLATE = """
#include "node.h"
#include "node_javascript.h"
#include "v8.h"
#include "env.h"
#include "env-inl.h"

namespace node {{

namespace {{

{definitions}

}}  // anonymous namespace

v8::Local<v8::String> NodePerContextSource(v8::Isolate* isolate) {{
  return internal_per_context_value.ToStringChecked(isolate);
}}

v8::Local<v8::String> LoadersBootstrapperSource(Environment* env) {{
  return internal_bootstrap_loaders_value.ToStringChecked(env->isolate());
}}

v8::Local<v8::String> NodeBootstrapperSource(Environment* env) {{
  return internal_bootstrap_node_value.ToStringChecked(env->isolate());
}}

void DefineJavaScript(Environment* env, v8::Local<v8::Object> target) {{
  {initializers}
}}

}}  // namespace node
"""

ONE_BYTE_STRING = """
static const uint8_t raw_{var}[] = {{ {data} }};
static struct : public v8::String::ExternalOneByteStringResource {{
  const char* data() const override {{
    return reinterpret_cast<const char*>(raw_{var});
  }}
  size_t length() const override {{ return arraysize(raw_{var}); }}
  void Dispose() override {{ /* Default calls `delete this`. */ }}
  v8::Local<v8::String> ToStringChecked(v8::Isolate* isolate) {{
    return v8::String::NewExternalOneByte(isolate, this).ToLocalChecked();
  }}
}} {var};
"""

TWO_BYTE_STRING = """
static const uint16_t raw_{var}[] = {{ {data} }};
static struct : public v8::String::ExternalStringResource {{
  const uint16_t* data() const override {{ return raw_{var}; }}
  size_t length() const override {{ return arraysize(raw_{var}); }}
  void Dispose() override {{ /* Default calls `delete this`. */ }}
  v8::Local<v8::String> ToStringChecked(v8::Isolate* isolate) {{
    return v8::String::NewExternalTwoByte(isolate, this).ToLocalChecked();
  }}
}} {var};
"""

INITIALIZER = """\
CHECK(target->Set(env->context(),
                  v8::String::NewFromUtf8(env->isolate(),
                                          {key}.data(),
                                          v8::NewStringType::kNormal,
                                          arraysize(raw_{key})).ToLocalChecked(),
                  {value}.ToStringChecked(env->isolate())).FromJust());
"""

DEPRECATED_DEPS = """\
'use strict';
process.emitWarning(
  'Requiring Node.js-bundled \\'{module}\\' module is deprecated. Please ' +
  'install the necessary module locally.', 'DeprecationWarning', 'DEP0084');
module.exports = require('internal/deps/{module}');
"""


def Render(var, data):
  # Treat non-ASCII as UTF-8 and convert it to UTF-16.
  if any(ord(c) > 127 for c in data):
    template = TWO_BYTE_STRING
    data = map(ord, data.decode('utf-8').encode('utf-16be'))
    data = [data[i] * 256 + data[i+1] for i in xrange(0, len(data), 2)]
    data = ToCArray(data)
  else:
    template = ONE_BYTE_STRING
    data = ToCString(data)
  return template.format(var=var, data=data)


def JS2C(source, target):
  modules = []
  consts = {}
  macros = {}
  macro_lines = []

  for s in source:
    if (os.path.split(str(s))[1]).endswith('macros.py'):
      macro_lines.extend(ReadLines(str(s)))
    else:
      modules.append(s)

  # Process input from all *macro.py files
  (consts, macros) = ReadMacros(macro_lines)

  # Build source code lines
  definitions = []
  initializers = []

  for name in modules:
    lines = ReadFile(str(name))
    lines = ExpandConstants(lines, consts)
    lines = ExpandMacros(lines, macros)

    deprecated_deps = None

    # On Windows, "./foo.bar" in the .gyp file is passed as "foo.bar"
    # so don't assume there is always a slash in the file path.
    if '/' in name or '\\' in name:
      split = re.split('/|\\\\', name)
      if split[0] == 'deps':
        if split[1] == 'node-inspect' or split[1] == 'v8':
          deprecated_deps = split[1:]
        split = ['internal'] + split
      else:
        split = split[1:]
      name = '/'.join(split)

    # if its a gypi file we're going to want it as json
    # later on anyway, so get it out of the way now
    if name.endswith(".gypi"):
      lines = re.sub(r'#.*?\n', '', lines)
      lines = re.sub(r'\'', '"', lines)
    name = name.split('.', 1)[0]
    var = name.replace('-', '_').replace('/', '_')
    key = '%s_key' % var
    value = '%s_value' % var

    definitions.append(Render(key, name))
    definitions.append(Render(value, lines))
    initializers.append(INITIALIZER.format(key=key, value=value))

    if deprecated_deps is not None:
      name = '/'.join(deprecated_deps)
      name = name.split('.', 1)[0]
      var = name.replace('-', '_').replace('/', '_')
      key = '%s_key' % var
      value = '%s_value' % var

      definitions.append(Render(key, name))
      definitions.append(Render(value, DEPRECATED_DEPS.format(module=name)))
      initializers.append(INITIALIZER.format(key=key, value=value))

  # Emit result
  output = open(str(target[0]), "w")
  output.write(TEMPLATE.format(definitions=''.join(definitions),
                               initializers=''.join(initializers)))
  output.close()

def main():
  natives = sys.argv[1]
  source_files = sys.argv[2:]
  if source_files[-2] == '-t':
    global TEMPLATE
    TEMPLATE = source_files[-1]
    source_files = source_files[:-2]
  JS2C(source_files, [natives])

if __name__ == "__main__":
  main()
