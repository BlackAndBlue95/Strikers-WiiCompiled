#!/usr/bin/env bash
# Builds the Strikers Mod Loader: Kamek's loader (kamekLoader.cpp, from the Kamek release
# fetch-toolchain.sh fetched) and its bootstrap for the game (strikers.cpp), compiled with the
# game's compiler and linked static at 0x80004060, as Riivolution memory patches:
#
#   build-loader.sh <out dir>   ->  <out dir>/loader.bin, <out dir>/loader.xml
#
# loader.xml is a partial Riivolution patch (the loader's code, from loader.bin, and its call from
# nlInit); packs/loader/build-pack.sh puts it in a pack.
set -euo pipefail

if [ $# -ne 1 ]; then
    sed -n '2,10p' "$0" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
tc="$here/../.toolchain"
if [ ! -x "$tc/kamek/Kamek" ] || [ ! -f "$tc/kamek/loader/kamekLoader.cpp" ]; then
    echo "build-loader: run $here/../fetch-toolchain.sh first" >&2
    exit 1
fi
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) run=() ;;
*) run=("$tc/wibo") ;;
esac

out=$1
mkdir -p "$out"
objdir=$(mktemp -d)
trap 'rm -rf "$objdir"' EXIT

# As Kamek builds its own loader (its build scripts), against its own small standard library.
cflags=(-nodefaults -proc gekko -i "$tc/kamek/loader" -I- -i "$tc/kamek/k_stdlib" -Cpp_exceptions off -RTTI off
    -enum int -Os -use_lmw_stmw on -fp hard -rostr -sdata 0 -sdata2 0)
for source in "$tc/kamek/loader/kamekLoader.cpp" "$here/strikers.cpp"; do
    MWCIncludes=. "${run[@]}" "$tc/compilers/GC/3.0a5/mwcceppc.exe" "${cflags[@]}" -c \
        -o "$objdir/$(basename "${source%.*}").o" "$source"
done

"$tc/kamek/Kamek" "$objdir/kamekLoader.o" "$objdir/strikers.o" -static=0x80004060 \
    -externals="$here/../externals-R4QE01.txt" -output-code="$out/loader.bin" -output-riiv="$out/loader.xml" \
    -valuefile=loader.bin >/dev/null
size=$(wc -c <"$out/loader.bin" | tr -d ' ')
# MetroTRK's interrupt table, which the loader takes, ends at 0x80005F88.
if [ "$size" -gt $((0x80005F88 - 0x80004060)) ]; then
    echo "build-loader: loader.bin is $size bytes, more than fits before 0x80005F88" >&2
    exit 1
fi
echo "$out/loader.bin ($size bytes), $out/loader.xml"
