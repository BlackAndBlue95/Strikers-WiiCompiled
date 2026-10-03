#!/usr/bin/env bash
# Builds a code mod's module: compiles its sources with the compiler the game was built with
# (CodeWarrior GC/3.0a5), against the decomp's headers, and links them with Kamek into a dynamic
# module (a relocatable .bin that the Strikers Mod Loader, or the port, loads) plus a symbol map.
#
#   build-module.sh <out.bin> <source.cpp|.c|.S>...
#
# Run fetch-toolchain.sh once first. Hooks use sdk/kamek/include/kamek.h (kmBranchDefCpp, kmCall,
# kmWrite32, ...); game functions and globals are named as the decomp's headers declare them and
# resolved through externals-R4QE01.txt.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,10p' "$0" >&2
    exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
tc="$here/.toolchain"
decomp="$tc/decomp"
if [ ! -x "$tc/kamek/Kamek" ] || [ ! -d "$decomp/include" ]; then
    echo "build-module: run $here/fetch-toolchain.sh first" >&2
    exit 1
fi
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) run=() ;;  # Windows runs the compiler directly
*) run=("$tc/wibo") ;;
esac

out=$1
shift
objdir=$(mktemp -d)
trap 'rm -rf "$objdir"' EXIT

# The game's own code flags (the decomp's cflags_game), no small data (a module's data isn't in the
# game's small-data areas), no exceptions or RTTI.
cflags=(-nodefaults -proc gekko -align powerpc -enum int -fp hardware -Cpp_exceptions off -RTTI off
    -O4,p -pragma "cats off" -pragma "warn_notinlined off" -maxerrors 5 -nosyspath -fp_contract on
    -str reuse -use_lmw_stmw on -pool off -inline auto -sdata 0 -sdata2 0
    -i "$here/include" -i "$decomp/include" -i "$decomp/libs/Runtime/include"
    -i "$decomp/libs/MSL_C/include" -i "$decomp/libs/RVL_SDK/include"
    -DVERSION_R4QE01 -DdNODEBUG=1 -DdSINGLE=1)

objects=()
for source in "$@"; do
    object="$objdir/$(basename "${source%.*}").o"
    case "$source" in
    *.S | *.s) MWCIncludes=. "${run[@]}" "$tc/compilers/GC/3.0a5/mwasmeppc.exe" -proc gekko -i "$here/include" -c -o "$object" "$source" ;;
    *) MWCIncludes=. "${run[@]}" "$tc/compilers/GC/3.0a5/mwcceppc.exe" "${cflags[@]}" -i "$(dirname "$source")" \
        -c -o "$object" "$source" ;;
    esac
    objects+=("$object")
done

"$tc/kamek/Kamek" "${objects[@]}" -dynamic -externals="$here/externals-R4QE01.txt" \
    -output-kamek="$out" -output-map="${out%.bin}.map" >/dev/null
echo "$out ($(wc -c <"$out" | tr -d ' ') bytes)"
