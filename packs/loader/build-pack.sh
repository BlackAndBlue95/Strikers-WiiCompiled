#!/usr/bin/env bash
# Builds the Strikers Mod Loader pack, which loads code mods (Kamek modules, sml_*.bin at the disc
# root) on a Wii or in Dolphin; Strikers Recharged builds them in instead and ignores this pack.
#   <out>/riivolution/strikersloader.xml
#   <out>/strikersloader/loader.bin
#
#   build-pack.sh [out]   (out: default ./pack)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/pack}
mkdir -p "$out/riivolution" "$out/strikersloader"
"$here/../../sdk/kamek/loader/build-loader.sh" "$out/strikersloader" >/dev/null
# Kamek's patches, the call only where nlInit has its own (bl nlInitFileSystem, 480B30E5).
sed -e 's|^|    |' -e 's|offset="0x802B44FC" value="\([0-9A-F]*\)" />|offset="0x802B44FC" value="\1" original="480B30E5" />|' \
    "$out/strikersloader/loader.xml" >"$out/strikersloader/patches.xml"
printf '\n' >>"$out/strikersloader/patches.xml"  # Kamek's file ends without one
sed -e "/@LOADER_PATCHES@/{r $out/strikersloader/patches.xml" -e 'd;}' "$here/riivolution/strikersloader.xml" \
    >"$out/riivolution/strikersloader.xml"
rm "$out/strikersloader/loader.xml" "$out/strikersloader/patches.xml"
echo "Strikers Mod Loader pack: $out"
