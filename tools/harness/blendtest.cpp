// 13b step 2 test: the Blender alone, with made-up audio whose every sample says where it came from.
//   analog frame f  -> L = R = 1000 (constant)          HD frame h -> L = (h % 20000) - 10000 (never 1000 +- 0 for long), R = -L
// so the output tells exactly which stream (and which HD frame) is playing.
#include <cstdio>
#include "blend.hpp"
using namespace blend;
static Blender b;
static long long aW = 0;                       // analog frames pushed = "now"
static const long long OFF = 109597;           // 2485.2 ms: the analog lags the HD by this
static int16_t hdL(long long h) { return (int16_t)((h % 20000) - 10000); }
static bool dropAt(long long h, long long from, long long to) { return h >= from && h < to; }
static long long dropFrom = -1, dropTo = -1;
static void pushSecond(double secs) {          // the streaming thread: both streams arrive
    long long n = (long long)(secs * RATE);
    static std::vector<float> a(2048 * 2, 1000.0f / 32767.0f);
    static std::vector<int16_t> h(2048 * 2);
    for (long long done = 0; done < n; done += 2048) {
        b.pushAnalog(a.data(), 2048);
        bool drop = dropAt(aW, dropFrom, dropTo);
        for (int i = 0; i < 2048; i++) { h[2 * i] = drop ? 0 : hdL(aW + i); h[2 * i + 1] = drop ? 0 : (int16_t)-h[2 * i]; }
        b.pushHd(h.data(), 2048 * 2, aW);      // HD audio index = its place on the IQ time line
        aW += 2048;
    }
}
static align::Status al;
static std::vector<int16_t> out(2048 * 2);
// plays `secs` like the audio thread does: pushes and reads in step. Returns the last state.
static int play(double secs, long long *firstHdFrame = nullptr, int *firstState = nullptr, bool *pureHd = nullptr) {
    long long n = (long long)(secs * RATE);
    bool first = true;
    for (long long done = 0; done < n; done += 2048) {
        pushSecond(2048.0 / RATE);
        size_t got = b.read(out.data(), 2048, 0, al);
        Status st = b.status();
        if (first && got > 0) {
            first = false;
            if (firstState) *firstState = st.state;
            if (pureHd) { *pureHd = true; for (size_t i = 0; i < got; i++) if (out[2 * i] != (int16_t)-out[2 * i + 1] || out[2*i] == 1000) *pureHd = false; }
            (void)firstHdFrame;
        }
    }
    return b.status().state;
}
static int fails = 0;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }
int main() {
    b.setUnmatchedPlaysHd(false);
    // --- a tune: analog first (nothing measured yet), HD once aligned
    al = align::Status();
    int fs = -1; bool pure = false;
    play(3.0, nullptr, &fs, &pure);
    check("after a tune: starts on the analog", fs == ANALOG && b.status().state == ANALOG && b.status().toHd == 0);
    al.state = 2; al.offsetFrames = OFF; al.accepted = 9; al.gainDb = 0;
    play(4.0);
    check("aligned: crossfades to the HD (counted once)", b.status().state == HD && b.status().toHd == 1);

    // --- HD1 -> sub-channel for 6 s (nobody reads the blender) -> back to HD1
    pushSecond(6.0);
    b.restart();
    play(2048.0 / RATE, nullptr, &fs, &pure);
    check("back from a sub-channel: on the HD at the first read", fs == HD);
    check("... and that first block is pure HD audio (no analog mixed in)", pure);
    check("... not counted as a fade (still 1), no fade to analog", b.status().toHd == 1 && b.status().toAnalog == 0);
    check("... aligned flag kept", b.status().aligned);
    // the HD frame played must be the time-aligned one: output frame p plays HD frame p - OFF
    { pushSecond(2048.0 / RATE); long long before = aW; size_t got = b.read(out.data(), 2048, 0, al);
      // play position = aWritten - RESTART_LATENCY at the restart, advanced by what was read since; find it from the sample:
      bool cont = got > 1; for (size_t i = 1; i < got; i++) if ((int16_t)(out[2*i] - out[2*(i-1)]) != 1 && out[2*i] != -10000) cont = false;
      check("... consecutive HD frames (no jump inside a block)", cont); (void)before; }
    play(3.0);
    check("stays on the HD afterwards", b.status().state == HD && b.status().toHd == 1 && b.status().toAnalog == 0);

    // --- sub-channel again, but the HD1 had a dropout 1 s before coming back -> analog first, as before
    pushSecond(4.0);
    dropFrom = aW; dropTo = aW + RATE / 2; pushSecond(1.5);
    b.restart();
    play(2048.0 / RATE, nullptr, &fs, &pure);
    check("back while a dropout is ahead: starts on the analog", fs == ANALOG);
    play(6.0);
    check("... and crossfades to the HD once it is clean (counted: 2)", b.status().state == HD && b.status().toHd == 2);

    // --- the aligner lost its measurement meanwhile (state measuring) -> analog first
    pushSecond(3.0); al.state = 1; b.restart();
    play(2048.0 / RATE, nullptr, &fs, &pure);
    check("back with no valid alignment: starts on the analog", fs == ANALOG && b.status().state == ANALOG);
    al.state = 2; play(1.0);
    check("... fades to the HD when the alignment is back (counted: 3)", (b.status().state == HD || b.status().state == TO_HD) && b.status().toHd == 3);

    // --- the resume chance is only the FIRST decision after a restart
    play(3.0); pushSecond(3.0); al.state = 1; b.restart(); play(0.5); al.state = 2;
    play(2048.0 / RATE, nullptr, &fs, &pure);
    check("alignment arrives 0.5 s after the restart: a normal crossfade, not a jump", b.status().state == TO_HD && b.status().toHd == 4);

    // --- a new tune: reset() -> analog first even with a stale 'ok' status passed in by mistake
    b.reset(); aW = 0; dropFrom = dropTo = -1;
    play(2048.0 / RATE, nullptr, &fs, &pure);
    check("after reset(): on the analog (the HD ring is empty)", fs == ANALOG);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails;
}
