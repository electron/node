#!/bin/sh

set -eo pipefail

git clone https://chromium.googlesource.com/chromium/buildtools
(cd buildtools && git checkout 94288c26d2ffe3aec9848c147839afee597acefd)
git clone https://chromium.googlesource.com/chromium/src/tools/clang tools/clang
(cd tools/clang && git checkout c893c7eec4706f8c7fc244ee254b1dadd8f8d158)
git clone https://chromium.googlesource.com/chromium/src/build
(cd build && git checkout b5df2518f091eea3d358f30757dec3e33db56156)
