#!/usr/bin/env bash
# Builds japanese_ime.dll (32-bit) with mingw-w64.
# Usage: ./build.sh /path/to/ashita/sdk
#   The SDK folder is VanaCore's plugins\ashita\sdk (Ashita.h, d3d8\, ffxi\ ...). It is not part of this repository.
# Needs: apt install g++-mingw-w64-i686
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sdk="${1:-$here/sdk}"
[ -f "$sdk/Ashita.h" ] || { echo "Ashita SDK not found: $sdk" >&2; exit 1; }

out="$here/build"
rm -rf "$out"; mkdir -p "$out/sdk"
# The SDK is written for MSVC: copy it and drop the old DOS end-of-file bytes (0x1A) GCC rejects.
cp -r "$sdk/." "$out/sdk/"
find "$out/sdk" -type f \( -name '*.h' -o -name '*.inl' \) -exec sed -i 's/\x1a//g' {} +

i686-w64-mingw32-g++ -std=c++20 -O2 -shared -fpermissive -w \
    -fno-rtti \
    -I"$here/shim" -I"$out/sdk" \
    "$here/src/japanese_ime.cpp" "$here/src/japanese_ime.def" \
    -o "$out/japanese_ime.dll" \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--enable-stdcall-fixup -Wl,--nxcompat -Wl,--dynamicbase \
    -limm32 -lcomctl32 -luser32 -lkernel32
i686-w64-mingw32-strip "$out/japanese_ime.dll"
rm -rf "$out/sdk"
sha256sum "$out/japanese_ime.dll"
