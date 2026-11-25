#!/bin/sh
# regress.sh - the macOS port's proof (step 0a): runs a fixed set of harness tests and prints one
# SHA-256 per output. Every refactor step must print the same checksums as the baseline.
#
#   cd tools/harness
#   sh regress.sh > regress-baseline.txt          # once, on the code before the refactor
#   sh regress.sh check                           # every later step: runs, then compares
#
# RECORDINGS=<folder> = where the five .cu8 recordings are (they stay outside the repo).
# PORT=12345 = the local port for the rtl_tcp run.  NOBUILD=1 = use the build/ that is there.
# OUT=<folder> = where the outputs go (default build/regress; emptied first).
#
# Every recording run is in virtual time (IQSTEP=1): the same input gives the same numbers.
# WAV and .cu8 outputs are compared byte for byte. Text logs (stdout and stderr kept apart, so
# their order cannot vary) are compared after clean(): it blanks the figures the engine and
# the harness measure with the real clock, and the folder paths.
# Lines starting "# run:" (commit, engine hash, date) are informational; "check" ignores them.
# The "# build:" lines (compiler, flags) must match: a difference then can only be our code.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${CORE:-$HERE/../../core}/src
R=${RECORDINGS:-"$HOME/Downloads/1260 AM/IQ"}
PORT=${PORT:-12345}
O=${OUT:-$HERE/build/regress}
B=$HERE/build
MODE=${1:-print}

if [ "$MODE" = check ]; then
    [ -f "$HERE/regress-baseline.txt" ] || { echo "no regress-baseline.txt" >&2; exit 1; }
    NOW=$(mktemp "${TMPDIR:-/tmp}/regress.XXXXXX")
    sh "$0" print > "$NOW"
    grep -v '^# run:' "$HERE/regress-baseline.txt" > "$NOW.base"; grep -v '^# run:' "$NOW" > "$NOW.now"
    if diff "$NOW.base" "$NOW.now"; then echo "IDENTICAL to the baseline"; rm -f "$NOW" "$NOW.base" "$NOW.now"; exit 0; fi
    echo "NOT IDENTICAL (above: < baseline, > now; outputs in $O)"; rm -f "$NOW" "$NOW.base" "$NOW.now"; exit 1
fi

for f in 1260-3.cu8 950AM_XEKA_11-21_1118.cu8 kklq.cu8 \
         HiDefRadio_2026-11-16_17-43-35_103.5MHz_1488375sps.cu8 HiDefRadio_2026-10-26_10-39-53_90.3MHz_1488375sps.cu8; do
    [ -f "$R/$f" ] || { echo "missing recording: $R/$f (set RECORDINGS=...)" >&2; exit 1; }
done
KMZT="$R/1260-3.cu8"; XEKA="$R/950AM_XEKA_11-21_1118.cu8"; KKLQ="$R/kklq.cu8"
KOST="$R/HiDefRadio_2026-11-16_17-43-35_103.5MHz_1488375sps.cu8"; Z90="$R/HiDefRadio_2026-10-26_10-39-53_90.3MHz_1488375sps.cu8"

if [ -z "$NOBUILD" ]; then
    mkdir -p "$B"; sh "$HERE/build.sh" > "$B/build.log" 2>&1 || { tail -20 "$B/build.log" >&2; echo "build failed (log: $B/build.log)" >&2; exit 1; }
fi
rm -rf "$O" && mkdir -p "$O" && cd "$O"

# One test: run NAME [VAR=value ...] program [args ...]. stdout -> NAME.out, stderr -> NAME.err,
# a non-zero exit is written into NAME.out (so it shows in the checksum).
run() {
    name=$1; shift
    env "$@" > "$O/$name.out" 2> "$O/$name.err" || echo "EXIT CODE $?" >> "$O/$name.out"
}
A=$B/apptest
# ---- recording runs (virtual time) ----
run kmzt-hd     IQFILE="$KMZT" IQSTEP=1 UNMATCHED=hd WAVOUT="$O/kmzt-hd.wav"   "$A" 1260000 0 24
run kmzt-auto   IQFILE="$KMZT" IQSTEP=1 UNMATCHED=hd WAVOUT="$O/kmzt-auto.wav" "$A" 1260000 2 24
run xeka-hd     IQFILE="$XEKA" IQSTEP=1 WAVOUT="$O/xeka-hd.wav"   "$A" 950000 0 45
run xeka-auto   IQFILE="$XEKA" IQSTEP=1 WAVOUT="$O/xeka-auto.wav" "$A" 950000 2 45
run kklq-hd     IQFILE="$KKLQ" IQSTEP=1 WAVOUT="$O/kklq-hd.wav"   "$A" 100300000 0 40
run kklq-auto   IQFILE="$KKLQ" IQSTEP=1 WAVOUT="$O/kklq-auto.wav" "$A" 100300000 2 40
run kklq-nodrift IQFILE="$KKLQ" IQSTEP=1 DRIFT=off WAVOUT="$O/kklq-nodrift.wav" "$A" 100300000 2 40
run kost-hd     IQFILE="$KOST" IQSTEP=1 WAVOUT="$O/kost-hd.wav"     "$A" 103500000 0 15
run kost-auto   IQFILE="$KOST" IQSTEP=1 WAVOUT="$O/kost-auto.wav"   "$A" 103500000 2 15
run kost-analog IQFILE="$KOST" IQSTEP=1 WAVOUT="$O/kost-analog.wav" "$A" 103500000 1 15
run kost-prog   IQFILE="$KOST" IQSTEP=1 WAVOUT="$O/kost-prog.wav"   "$A" 103500000 2 15 5:prog:1 10:prog:0
run kost-again  IQFILE="$KOST" IQSTEP=1 GAIN=auto WAVOUT="$O/kost-again.wav" "$A" 103500000 2 15
run kost-iqrec  IQFILE="$KOST" IQSTEP=1 IQREC="$O/kost-iqrec.cu8" WAVOUT="$O/kost-iqrec.wav" "$A" 103500000 2 10 2:rec:5
run z90-hd      IQFILE="$Z90" IQSTEP=1 WAVOUT="$O/z90-hd.wav"   "$A" 90300000 0 30
run z90-auto    IQFILE="$Z90" IQSTEP=1 WAVOUT="$O/z90-auto.wav" "$A" 90300000 2 30
run z90-mono    IQFILE="$Z90" IQSTEP=1 STEREO=mono WAVOUT="$O/z90-mono.wav" "$A" 90300000 2 30
# the rtl_tcp path: rtltcp_replay.py in lockstep mode (one client, then it exits)
python3 "$HERE/rtltcp_replay.py" "$KOST" "$PORT" > "$O/tcp-server.log" 2>&1 &
SERVER=$!
n=0; until grep -q listening "$O/tcp-server.log" 2>/dev/null; do
    n=$((n + 1)); [ $n -gt 100 ] && { echo "rtl_tcp stand-in did not start (see $O/tcp-server.log)" >&2; kill $SERVER; exit 1; }
    sleep 0.1
done
run kost-tcp    SOURCE=tcp:127.0.0.1:$PORT IQSTEP=1 WAVOUT="$O/kost-tcp.wav" "$A" 103500000 2 12
kill $SERVER 2>/dev/null || true; wait $SERVER 2>/dev/null || true
# ---- stand-alone tests ----
run blendtest  "$B/blendtest"
run drifttest  "$B/drifttest"
run amtest     "$B/amtest"
run identtest  "$B/identtest" "$HERE/../../app/src/main/assets/stations/fm.txt"
run pstest     "$B/pstest"
run gaintest   "$B/gaintest" "$KMZT" "$XEKA" "$KKLQ" "$KOST" "$Z90"
run searchtest "$B/searchtest"

# The real-clock figures blanked before the text logs are checksummed, and the folder paths.
# Found by running everything twice and diffing (2026-11-24); stream-time figures (alignment ms,
# frames, ppm, MER ...) are deterministic and stay.
#   stderr: "SYNC ... / First HDn audio decoded! N ms after tuning", "auto-gain took N ms (N ms since tuning)"
#   stdout: the status line's load figures "hd <state> N% demod N%", and the COST line's
#           demod / HD decode / whole-harness percentages (its "HD search resting" stays)
clean() {
    R="$R" O="$O" HERE="$HERE" perl -pe '
        s/\Q$ENV{R}\E/<RECORDINGS>/g; s/\Q$ENV{O}\E/<OUT>/g; s/\Q$ENV{HERE}\E/<HARNESS>/g;
        s/\b\d+ ms after tuning/<ms> ms after tuning/g;
        s/took \d+ ms \(\d+ ms since tuning\)/took <ms> ms (<ms> ms since tuning)/g;
        s/( hd .{4} )[ \d]{2}% demod [ \d]{2}%/$1--% demod --%/;
        s/^(COST over \S+ s: demod )\S+( %, HD decode )\S+( %.*whole harness )\S+( %)/$1--$2--$3--$4/;'
}

echo "# run: regress.sh, $(date '+%Y-%m-%d')"
echo "# run: commit $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- . "$SRC" || echo ' (+ uncommitted changes)')"
echo "# run: engine sources $(cat "$SRC"/native-lib.cpp "$SRC"/*.hpp | shasum -a 256 | cut -c1-16) (native-lib.cpp + *.hpp)"
echo "# run: recordings $R"
clang --version | head -1 | sed 's/^/# build: /'
sed 's/^/# build: /' "$B/flags.txt"
for f in "$O"/*.out "$O"/*.err; do
    mkdir -p "$O/clean" && clean < "$f" > "$O/clean/$(basename "$f")"
    printf '%s  %s\n' "$(shasum -a 256 < "$O/clean/$(basename "$f")" | cut -c1-64)" "$(basename "$f")"
done
for f in "$O"/*.wav "$O"/*.cu8*; do
    [ -f "$f" ] && printf '%s  %s\n' "$(shasum -a 256 < "$f" | cut -c1-64)" "$(basename "$f")"
done
exit 0
