#!/usr/bin/env bash
# Builds the hello pack: <out>/riivolution/hello.xml and <out>/hello/hello.bin, laid out like an SD card.
# Copy <out>'s contents into the data folder's Riivolution folder (macOS: ~/Library/Application
# Support/MSCRecomp/Riivolution), then run build.sh.
#
#   build-pack.sh [out]   (default: ./pack)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/pack}
mkdir -p "$out/riivolution" "$out/hello"
cp "$here/riivolution/hello.xml" "$out/riivolution/"
"$here/../../build-module.sh" "$out/hello/hello.bin" "$here/hello.cpp"
