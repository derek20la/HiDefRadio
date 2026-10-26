#!/bin/sh
# Builds the desktop harness: the app's whole native engine (native-lib.cpp) playing a .cu8
# recording on a PC (Linux / macOS; needs cmake, gcc, clang++ and a JDK for jni.h).
#
#   cd tools/harness && sh build.sh
#
# Everything it builds goes into ./build (ignored by git). The app's own files - and the
# vendored nrsc5, FAAD2 and librtlsdr - are copied from app/src/main/cpp, so the harness
# always runs the code that is in the repo.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../../app/src/main/cpp}
mkdir -p "$HERE/build" && cd "$HERE/build"
cp "$APP"/native-lib.cpp "$APP"/*.hpp .
mkdir -p cfg && cp "$APP/nrsc5-config/config.h" cfg/
cp "$HERE/apptest.cpp" "$HERE/blendtest.cpp" "$HERE/drifttest.cpp" "$HERE/mkdrift.cpp" "$HERE/searchtest.cpp" "$HERE/mkhdgap.cpp" "$HERE/mkcochannel.cpp" "$HERE/identtest.cpp" "$HERE/amtest.cpp" "$HERE/pstest.cpp" "$HERE/rtl_stub.c" "$HERE/fftw3.h" .
# nrsc5, rtl-sdr-blog and FAAD2 come from the APP's vendored copies (app/src/main/cpp/external),
# not from GitHub, so the harness runs exactly the decoder the app ships - including the
# HiDef Radio patches to nrsc5 (2026-10-24: the faster HD search in sync.c). nrsc5 is
# re-copied on every build so a changed sync.c is picked up; FAAD2 (already carrying nrsc5's
# HDC patch) only once, because its CMake build is slow. NRSC5_SRC=<dir> builds another
# nrsc5 tree instead (e.g. an unpatched v3.2.0 clone, for before / after comparisons).
NRSC5_SRC=${NRSC5_SRC:-$APP/external/nrsc5}
rm -rf nrsc5 && cp -r "$NRSC5_SRC" nrsc5
[ -d rtl-sdr-blog ] || cp -r "$APP/external/rtl-sdr-blog" rtl-sdr-blog
[ -d faad2 ] || cp -r "$APP/external/faad2" faad2
(mkdir -p faad2/build && cd faad2/build && cmake -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DCMAKE_BUILD_TYPE=Release .. >/dev/null && make faad_hdc)
mkdir -p shim obj stub/android && cp fftw3.h shim/
printf '#pragma once\n#include <stdio.h>\n#define ANDROID_LOG_DEBUG 3\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_WARN 5\n#define ANDROID_LOG_ERROR 6\n#define __android_log_print(prio, tag, ...) (fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\\n"))\n' > stub/android/log.h
printf '#pragma once\n#include_next <rtl-sdr.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\nint rtlsdr_open_fd(rtlsdr_dev_t **dev, int fd);\n#ifdef __cplusplus\n}\n#endif\n' > stub/rtl-sdr.h
for f in nrsc5/src/*.c; do b=$(basename $f .c); [ $b = main ] && continue; [ $b = strndup ] && continue
  gcc -O2 -w -std=gnu11 -D_GNU_SOURCE -DHAVE_FAAD2 -DUSE_FAAD2 -I cfg -I shim -I nrsc5/src -I nrsc5/include -I rtl-sdr-blog/include -I faad2/include -c $f -o obj/$b.o; done
ar rcs libnrsc5.a obj/*.o
gcc -O2 -w -I rtl-sdr-blog/include -c rtl_stub.c -o rtl_stub.o
J=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
clang++ -std=c++17 -O2 -w -DHIDEF_HARNESS -I stub -I nrsc5/include -I rtl-sdr-blog/include -I "$J/include" -I "$J/include/linux" -I "$J/include/darwin" apptest.cpp rtl_stub.o libnrsc5.a faad2/build/libfaad_hdc.a -lpthread -lm -o apptest
clang++ -std=c++17 -O2 -w blendtest.cpp -o blendtest
clang++ -std=c++17 -O2 -w drifttest.cpp -o drifttest
clang++ -std=c++17 -O2 -w mkdrift.cpp -o mkdrift
clang++ -std=c++17 -O2 -w searchtest.cpp -o searchtest
clang++ -std=c++17 -O3 -w mkhdgap.cpp -o mkhdgap
clang++ -std=c++17 -O3 -w mkcochannel.cpp -o mkcochannel
clang++ -std=c++17 -O2 -w identtest.cpp -o identtest
clang++ -std=c++17 -O2 -w amtest.cpp -o amtest
clang++ -std=c++17 -O2 -w pstest.cpp -o pstest
echo 'built build/apptest, build/blendtest, build/drifttest, build/identtest, build/amtest, build/pstest, build/mkdrift, build/searchtest, build/mkhdgap and build/mkcochannel. e.g.:'
echo '  IQFILE=klos.cu8 IQPACE=1 WAVOUT=out.wav build/apptest 95500000 2 34 4.5:prog:1 22:prog:0'
