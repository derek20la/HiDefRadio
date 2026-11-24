# Harness only (build 41): replays the gain watchdog on real recordings (gainkeeperreplay.kt),
# the current GainKeeper next to build 40's (taken from git: d2656b5 = 1.4, as "GainKeeper40").
# OLD41A=<file> adds a third (e.g. an earlier version of the current file, as "GainKeeper41a").
# Needs kotlinc and java on the PATH, the git history, and traces made with apptest's FMTRACE=1:
#
#   one trace per recording of a gain-step set, named <station>_<gain in tenths>.txt, e.g.
#   GAINFILES=402:klos_402.cu8 GAINSTEPS=files GAIN=402 IQSTEP=1 GAINFILES_QUIET=1 FMTRACE=1 PRINT=1000 HDREST=off \
#       build/apptest 95500000 2 60 | grep ^TRACE > traces/klos_402.txt
#   (source 2 = Auto: the HD decoder AND the analog demodulator run, so the trace has the quieting too;
#    HDREST=off: the HD search never rests, so every second says whether the HD is there)
#
#   sh gainkeeperreplay.sh traces klos 402 0 600 v4
#       = KLOS from 40.2 dB, no quality sweep (the 11-20 "no HD sidebands" case), 10 minutes, V4 steps
HERE=$(cd "$(dirname "$0")" && pwd)
K=app/src/main/java/io/github/derek20la/hidefradio/GainKeeper.kt
W=$(mktemp -d)
git -C "$HERE" show "${OLD40:-d2656b5}:$K" | sed 's/GainKeeper/GainKeeper40/g' > "$W/GainKeeper40.kt"
[ -n "$OLD41A" ] && sed 's/GainKeeper/GainKeeper41a/g' "$OLD41A" > "$W/GainKeeper41a.kt"
kotlinc "$HERE/../../$K" "$W"/GainKeeper4*.kt "$HERE/gainkeeperreplay.kt" -include-runtime -d "$W/replay.jar" 2>/dev/null
java -cp "$W/replay.jar" io.github.derek20la.hidefradio.GainkeeperreplayKt "$@"
rm -rf "$W"
