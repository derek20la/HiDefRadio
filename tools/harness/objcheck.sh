#!/bin/sh
# objcheck.sh - proves that a change to the C++ sources is "comments only".
#
# It compiles the app's native engine (native-lib.cpp with every header) and the stand-alone
# tests three ways - clang++ on this PC, g++ on this PC, and clang for 64-bit ARM (the phone's
# code path, NEON included; needs `pip install ziglang`) - and prints one SHA-256 per object
# file. No debug information is generated (-g0 also for zig, which adds it by default), so a comment that moves code to another line
# cannot show up; anything else that changed the program does.
#
#   cd tools/harness && sh build.sh            # once: fetches nrsc5 / librtlsdr headers etc.
#   sh objcheck.sh > before.txt                # on the old sources
#   ... edit comments ...
#   sh objcheck.sh > after.txt && diff before.txt after.txt && echo IDENTICAL
#
# Used for the documentation pass of 2026-10-15 (every object file identical before and after).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../../app/src/main/cpp}
B=$HERE/build
[ -d "$B/nrsc5" ] || { echo "run build.sh first" >&2; exit 1; }
OUT=$(mktemp -d)
mkdir -p "$OUT/src" && cp "$APP"/native-lib.cpp "$APP"/*.hpp "$OUT/src/"
cp "$HERE"/blendtest.cpp "$HERE"/drifttest.cpp "$HERE"/searchtest.cpp "$HERE"/identtest.cpp "$OUT/src/"
J=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
INC="-I $B/stub -I $B/nrsc5/include -I $B/rtl-sdr-blog/include -I $J/include -I $J/include/linux"
cd "$OUT/src"
for cxx in "clang++" "g++"; do
  for f in native-lib blendtest drifttest searchtest identtest; do
    $cxx -std=c++17 -O2 -w -fPIC $INC -c $f.cpp -o "$OUT/$cxx-$f.o"
  done
done
if python3 -m ziglang version >/dev/null 2>&1; then
  for f in native-lib blendtest drifttest searchtest identtest; do
    python3 -m ziglang c++ -target aarch64-linux-gnu -g0 -std=c++17 -O2 -w -fPIC $INC -c $f.cpp -o "$OUT/arm64-$f.o"
  done
fi
cd "$OUT" && sha256sum *.o
rm -rf "$OUT"
