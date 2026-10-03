#!/usr/bin/env bash
# Fetches the tools Riivolution code mods are built with into sdk/kamek/.toolchain (never committed):
#   - Kamek, which links compiled objects into a mod's code module (MIT, github.com/Treeki/Kamek);
#   - the CodeWarrior compiler the game itself was built with (GC/3.0a5), from the decomp community's
#     archive, as decomp projects fetch it (CodeWarrior isn't redistributable: each modder fetches it);
#   - wibo, to run that Windows compiler on macOS and Linux (github.com/decompals/wibo);
#   - powerpc-eabi binutils, to look at what came out (github.com/encounter/gc-wii-binutils).
# Players never need any of this: only people writing code mods.
set -euo pipefail

KAMEK_TAG=2026-02-24
COMPILERS_TAG=20251118
COMPILER=GC/3.0a5
WIBO_TAG=1.0.3
BINUTILS_TAG=2.42-2
# The Mario Strikers Charged decomp (CC0, github.com/yannicksuter/mscharged-decomp): its headers are
# the game's own classes, so mods use them; its symbol map makes externals-R4QE01.txt.
DECOMP_REPO=yannicksuter/mscharged-decomp
DECOMP_COMMIT=936868b

here=$(cd "$(dirname "$0")" && pwd)
dest="$here/.toolchain"
mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "$(uname -s)-$(uname -m)" in
Darwin-arm64)
    kamek_asset=osx-arm64.tar.gz
    wibo_asset=wibo-macos
    binutils_asset=macos-universal.zip
    ;;
Linux-x86_64)
    kamek_asset=linux-x64.tar.gz
    wibo_asset=wibo-x86_64
    binutils_asset=linux-x86_64.zip
    ;;
*)
    echo "fetch-toolchain: no prebuilt Kamek for $(uname -s) $(uname -m); on Windows use fetch-toolchain.ps1" >&2
    exit 1
    ;;
esac

fetch() { # url file
    echo "  $1"
    curl -fsSL --retry 3 -o "$2" "$1"
}

if [ ! -x "$dest/kamek/Kamek" ]; then
    echo "Kamek $KAMEK_TAG"
    fetch "https://github.com/Treeki/Kamek/releases/download/$KAMEK_TAG/$kamek_asset" "$tmp/kamek.tar.gz"
    rm -rf "$dest/kamek" && mkdir -p "$dest/kamek"
    tar -xzf "$tmp/kamek.tar.gz" -C "$dest/kamek"
    # Some archives wrap everything in one folder.
    if [ ! -e "$dest/kamek/Kamek" ] && [ "$(ls "$dest/kamek" | wc -l)" -eq 1 ]; then
        inner="$dest/kamek/$(ls "$dest/kamek")"
        mv "$inner"/* "$dest/kamek/" && rmdir "$inner"
    fi
    chmod +x "$dest/kamek/Kamek" 2>/dev/null || true
fi

if [ ! -e "$dest/compilers/$COMPILER/mwcceppc.exe" ]; then
    echo "CodeWarrior $COMPILER (compilers_$COMPILERS_TAG)"
    mkdir -p "$dest/cache"
    archive="$dest/cache/compilers_$COMPILERS_TAG.zip"
    [ -s "$archive" ] || fetch "https://files.decomp.dev/compilers_$COMPILERS_TAG.zip" "$archive"
    rm -rf "$dest/compilers/$COMPILER" && mkdir -p "$dest/compilers"
    unzip -o -q "$archive" "$COMPILER/*" -d "$dest/compilers"
    if [ ! -e "$dest/compilers/$COMPILER/mwcceppc.exe" ]; then
        echo "fetch-toolchain: $COMPILER/mwcceppc.exe isn't in compilers_$COMPILERS_TAG.zip" >&2
        exit 1
    fi
fi

if [ ! -x "$dest/wibo" ]; then
    echo "wibo $WIBO_TAG"
    fetch "https://github.com/decompals/wibo/releases/download/$WIBO_TAG/$wibo_asset" "$dest/wibo"
    chmod +x "$dest/wibo"
fi

if [ ! -e "$dest/binutils/powerpc-eabi-objdump" ]; then
    echo "binutils $BINUTILS_TAG"
    fetch "https://github.com/encounter/gc-wii-binutils/releases/download/$BINUTILS_TAG/$binutils_asset" "$tmp/binutils.zip"
    rm -rf "$dest/binutils" && mkdir -p "$dest/binutils"
    unzip -q "$tmp/binutils.zip" -d "$dest/binutils"
    chmod +x "$dest/binutils"/* 2>/dev/null || true
fi

if [ ! -e "$dest/decomp/$DECOMP_COMMIT" ]; then
    echo "decomp headers ($DECOMP_REPO @ $DECOMP_COMMIT)"
    fetch "https://codeload.github.com/$DECOMP_REPO/zip/$DECOMP_COMMIT" "$tmp/decomp.zip"
    rm -rf "$dest/decomp" && mkdir -p "$tmp/decomp"
    unzip -q "$tmp/decomp.zip" -d "$tmp/decomp"
    root=$(ls -d "$tmp/decomp"/*/ | head -1)
    mkdir -p "$dest/decomp/libs" "$dest/decomp/config"
    cp -R "$root/include" "$dest/decomp/include"
    for lib in Runtime MSL_C RVL_SDK; do
        mkdir -p "$dest/decomp/libs/$lib" && cp -R "$root/libs/$lib/include" "$dest/decomp/libs/$lib/include"
    done
    cp -R "$root/config/R4QE01" "$dest/decomp/config/R4QE01"
    cp "$root/LICENSE" "$dest/decomp/LICENSE"
    touch "$dest/decomp/$DECOMP_COMMIT"
fi

echo "Toolchain ready in $dest"
