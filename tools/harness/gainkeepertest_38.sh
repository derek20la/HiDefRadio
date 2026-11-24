#!/bin/sh
# Harness only (build 39): runs gainkeepertest.kt with build 38's GainKeeper compiled in beside
# the current one (as "GainKeeper38", taken from git: commit 3149837 = 1.3.1), so test 7 can
# compare the two answer for answer. Build 41: also build 40's (as "GainKeeper40", commit d2656b5
# = 1.4) for tests 11-12. Needs kotlinc and java on the PATH, and the git history.
#   cd tools/harness && sh gainkeepertest_38.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
K=app/src/main/java/io/github/derek20la/hidefradio/GainKeeper.kt
mkdir -p "$HERE/build" && cd "$HERE/build"
git -C "$HERE" show "${OLD:-3149837}:$K" | sed 's/GainKeeper/GainKeeper38/g' > GainKeeper38.kt
git -C "$HERE" show "${OLD40:-d2656b5}:$K" | sed 's/GainKeeper/GainKeeper40/g' > GainKeeper40.kt
kotlinc "$HERE/../../$K" GainKeeper38.kt GainKeeper40.kt "$HERE/gainkeepertest.kt" -include-runtime -d gk.jar 2>&1 | grep -v "^warning" || true
java -jar gk.jar
