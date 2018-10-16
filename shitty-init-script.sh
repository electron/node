#!/bin/sh

set -eo pipefail

rm -rf buildtools tools/clang third_party build

mkdir -p third_party

git clone https://chromium.googlesource.com/chromium/buildtools
(cd buildtools && git checkout 94288c26d2ffe3aec9848c147839afee597acefd)
git clone https://chromium.googlesource.com/chromium/src/tools/clang tools/clang
(cd tools/clang && git checkout c893c7eec4706f8c7fc244ee254b1dadd8f8d158)
tools/clang/scripts/update.py
git clone https://chromium.googlesource.com/chromium/src/build
(cd build && git checkout b5df2518f091eea3d358f30757dec3e33db56156)

git clone https://chromium.googlesource.com/chromium/src/third_party/jinja2 third_party/jinja2
(cd third_party/jinja2 && git checkout 45571de473282bd1d8b63a8dfcb1fd268d0635d2)

git clone https://chromium.googlesource.com/chromium/src/third_party/markupsafe third_party/markupsafe
(cd third_party/markupsafe && git checkout 8f45f5cfa0009d2a70589bcda0349b8cb2b72783)
