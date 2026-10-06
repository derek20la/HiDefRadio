#!/bin/sh
# Builds the desktop harness: the app's whole native engine (native-lib.cpp) playing a .cu8
# recording on a PC (Linux / macOS; needs git, cmake, gcc, clang++ and a JDK for jni.h).
#
#   cd tools/harness && sh build.sh
#
# Everything it downloads or builds goes into ./build (ignored by git). The app's own files
# are copied from app/src/main/cpp, so the harness always runs the code that is in the repo.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../../app/src/main/cpp}
mkdir -p "$HERE/build" && cd "$HERE/build"
cp "$APP"/native-lib.cpp "$APP"/*.hpp .
mkdir -p cfg && cp "$APP/nrsc5-config/config.h" cfg/
cp "$HERE/apptest.cpp" "$HERE/blendtest.cpp" "$HERE/drifttest.cpp" "$HERE/mkdrift.cpp" "$HERE/rtl_stub.c" "$HERE/fftw3.h" .
[ -d nrsc5 ] || git clone --depth 1 --branch v3.2.0 https://github.com/theori-io/nrsc5.git
[ -d rtl-sdr-blog ] || git clone --depth 1 https://github.com/rtlsdrblog/rtl-sdr-blog.git
[ -d faad2 ] || { git clone --depth 1 --branch 2.11.2 https://github.com/knik0/faad2.git; (cd faad2 && patch -p1 < ../nrsc5/support/faad2-hdc-support.patch); }
(mkdir -p faad2/build && cd faad2/build && cmake -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DCMAKE_BUILD_TYPE=Release .. >/dev/null && make faad_hdc)
mkdir -p shim obj stub/android && cp fftw3.h shim/
printf '#pragma once\n#include <stdio.h>\n#define ANDROID_LOG_DEBUG 3\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_WARN 5\n#define ANDROID_LOG_ERROR 6\n#define __android_log_print(prio, tag, ...) (fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\\n"))\n' > stub/android/log.h
printf '#pragma once\n#include_next <rtl-sdr.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\nint rtlsdr_open_fd(rtlsdr_dev_t **dev, int fd);\n#ifdef __cplusplus\n}\n#endif\n' > stub/rtl-sdr.h
for f in nrsc5/src/*.c; do b=$(basename $f .c); [ $b = main ] && continue; [ $b = strndup ] && continue
  gcc -O2 -w -std=gnu11 -D_GNU_SOURCE -DHAVE_FAAD2 -DUSE_FAAD2 -I cfg -I shim -I nrsc5/src -I nrsc5/include -I rtl-sdr-blog/include -I faad2/include -c $f -o obj/$b.o; done
ar rcs libnrsc5.a obj/*.o
gcc -O2 -w -I rtl-sdr-blog/include -c rtl_stub.c -o rtl_stub.o
J=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
clang++ -std=c++17 -O2 -w -I stub -I nrsc5/include -I rtl-sdr-blog/include -I "$J/include" -I "$J/include/linux" -I "$J/include/darwin" apptest.cpp rtl_stub.o libnrsc5.a faad2/build/libfaad_hdc.a -lpthread -lm -o apptest
clang++ -std=c++17 -O2 -w blendtest.cpp -o blendtest
clang++ -std=c++17 -O2 -w drifttest.cpp -o drifttest
clang++ -std=c++17 -O2 -w mkdrift.cpp -o mkdrift
echo 'built build/apptest, build/blendtest, build/drifttest and build/mkdrift. e.g.:'
echo '  IQFILE=klos.cu8 IQPACE=1 WAVOUT=out.wav build/apptest 95500000 2 34 4.5:prog:1 22:prog:0'
