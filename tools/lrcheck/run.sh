#!/bin/sh
# Left/right check of the FM stereo decoder (see README.md).   sh run.sh [recording.cu8]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$HERE/../../app/src/main/cpp}
mkdir -p "$HERE/build" && cd "$HERE/build"
[ -d ngsoftfm ] || git clone --depth 1 https://github.com/f4exb/ngsoftfm.git
# one portability fix: "abs" there is the integer one with some compilers, which stops the pilot PLL from locking
sed 's/abs(phasor_q)/std::fabs(phasor_q)/' ngsoftfm/sfmbase/FmDecode.cpp > ref_FmDecode.cpp
clang++ -std=c++17 -O2 -I "$APP" "$HERE/demod_app.cpp" -o demod_app
g++ -std=c++11 -O2 -w -I ngsoftfm/include "$HERE/demod_ref.cpp" ref_FmDecode.cpp ngsoftfm/sfmbase/Filter.cpp -o demod_ref
echo "--- 1. a 1 kHz tone on the LEFT only, built from the standard's formula"
python3 "$HERE/gen_left.py" left_only.cu8
./demod_app left_only.cu8 app.f32; ./demod_ref left_only.cu8 ref.f32
python3 "$HERE/tone.py" app.f32 ref.f32
if [ -n "$1" ]; then
  echo "--- 2. a real station: $1"
  ./demod_app "$1" app_real.f32; ./demod_ref "$1" ref_real.f32
  python3 "$HERE/compare.py" app_real.f32 ref_real.f32 "the app's decoder vs the reference"
fi
