#!/usr/bin/env bash
# Builds the Strikers Tweaks pack, laid out like an SD card:
#   <out>/riivolution/tweaks.xml
#   <out>/tweaks/sml_20_tweaks.bin   the code (code/*.cpp, built with sdk/kamek)
#   <out>/tweaks/on                  the file a tweak's option puts on the disc
# Gameplay and menu tweaks as options, the same in Strikers Recharged, in Dolphin and on a Wii (with
# the Strikers Mod Loader pack there). Strikers Recharged's build.sh installs it in the data folder.
#
#   build-pack.sh [--code] [out]   (out: default ./pack)
#
# The built code is kept in the repo (sml_20_tweaks.bin), so building the game doesn't take Kamek and
# CodeWarrior. --code compiles code/*.cpp first (run sdk/kamek/fetch-toolchain.sh once), refreshing it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
code=0
if [ "${1:-}" = "--code" ]; then
    code=1
    shift
fi
out=${1:-$here/pack}
if [ "$code" -eq 1 ]; then
    "$here/../../sdk/kamek/build-module.sh" "$here/sml_20_tweaks.bin" "$here"/code/*.cpp
    rm -f "$here/sml_20_tweaks.map"
fi
mkdir -p "$out/riivolution" "$out/tweaks"
cp "$here/riivolution/tweaks.xml" "$out/riivolution/tweaks.xml"
cp "$here/sml_20_tweaks.bin" "$out/tweaks/sml_20_tweaks.bin"
printf 'on\n' >"$out/tweaks/on"
echo "Strikers Tweaks pack: $out"
