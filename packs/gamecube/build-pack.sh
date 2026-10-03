#!/usr/bin/env bash
# Builds the GameCube controllers pack, laid out like an SD card:
#   <out>/riivolution/gamecube.xml
#   <out>/gamecube/sml_10_gamecube.bin   the code (code/gamecube.cpp, built with sdk/kamek)
# A GameCube controller in port N plays as player N when no Wii Remote is on channel N, with Super
# Mario Strikers' controls in matches. For a Wii or Dolphin, next to the Strikers Mod Loader pack;
# Strikers Recharged has this built in.
#
#   build-pack.sh [out]   (out: default ./pack)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/pack}
mkdir -p "$out/riivolution" "$out/gamecube"
cp "$here/riivolution/gamecube.xml" "$out/riivolution/gamecube.xml"
"$here/../../sdk/kamek/build-module.sh" "$out/gamecube/sml_10_gamecube.bin" "$here"/code/*.cpp
rm -f "$out/gamecube/sml_10_gamecube.map"
echo "GameCube controllers pack: $out"
