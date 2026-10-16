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

    // ================= build 10: the analog on this frequency is ANOTHER station's =================
    // ("the HD plays alone", blend.hpp). Same made-up audio: analog = 1000 on both channels,
    // HD = a ramp with R = -L, silence = 0 / 0 - every output frame says what was heard.
    struct Heard { long long analog = 0, hd = 0, silent = 0, mixed = 0; };
    auto hear = [&](double secs) {
        Heard h; long long n = (long long)(secs * RATE);
        for (long long done = 0; done < n; done += 2048) {
            pushSecond(2048.0 / RATE);
            size_t got = b.read(out.data(), 2048, 0, al);
            for (size_t i = 0; i < got; i++) {
                int16_t l = out[2 * i], r = out[2 * i + 1];
                if (l == 1000 && r == 1000) h.analog++;
                else if (l == 0 && r == 0) h.silent++;        // (one HD frame in 20,000 is 0 / 0 too)
                else if (l == (int16_t)-r) h.hd++;
                else h.mixed++;
            }
        }
        return h;
    };
    b.reset(); aW = 0; dropFrom = dropTo = -1;
    b.setUnmatchedPlaysHd(true);                   // the app's default: "play the HD anyway"
    b.setAnalogForeign(true);
    al = align::Status(); al.state = 3;            // the aligner: no match (it is not asked in this mode)
    Heard h = hear(1.5);
    check("foreign: the other station's analog first (HD not yet good for 2 s)", b.status().state == ANALOG && h.hd == 0 && h.silent == 0 && h.analog > 0);
    hear(1.5);
    check("... then a crossfade to the HD, counted once", (b.status().state == HD || b.status().state == TO_HD) && b.status().toHd == 1);
    hear(0.5); h = hear(2.0);
    check("... the HD alone: pure HD, flagged 'alone'", b.status().state == HD && b.status().alone && !b.status().silent && h.analog == 0 && h.mixed == 0 && h.hd > 80000);
    // a 3 s dropout: silence, never the other station
    dropFrom = aW + RATE / 2; dropTo = dropFrom + 3 * RATE;
    h = hear(3.0);
    check("dropout: fades to SILENCE - not one frame of the analog", h.analog == 0 && h.silent > 2 * RATE && b.status().silent && b.status().alone);
    check("... counted as a silence, not as a fade to analog", b.status().silences == 1 && b.status().toAnalog == 0);
    h = hear(1.5);
    check("the HD back: HD again within a second, no waiting time", b.status().state == HD && !b.status().silent && h.analog == 0 && b.status().toHd == 1);
    // a 13 s dropout: after 10 s of silence the analog is given back; the HD returns after 5 s good
    hear(1.0); dropFrom = aW; dropTo = aW + 13 * RATE;
    h = hear(9.0);
    check("9 s without HD: still silence", b.status().silent && h.analog == 0);
    h = hear(3.5);
    check("10 s without HD: the other station's analog is given back (safety net)", !b.status().silent && !b.status().alone && h.analog > 0 && b.status().toAnalog == 1 && b.status().state == ANALOG);
    h = hear(4.0);                                 // the HD has been back for ~3.5 s now
    check("... the HD good for 3.5 s: not yet (5 s needed after the net)", b.status().state == ANALOG && h.hd == 0);
    hear(3.0);
    check("... the HD good for 5 s: back on the HD, alone", b.status().state == HD && b.status().alone && b.status().toHd == 2);
    // the setting "stay on analog": the HD is never put on
    b.setUnmatchedPlaysHd(false);
    hear(1.0); h = hear(5.0);
    check("'stay on analog': the analog plays, the HD never comes", b.status().state == ANALOG && !b.status().alone && h.hd == 0 && h.silent == 0 && h.analog > 4 * RATE);
    b.setUnmatchedPlaysHd(true);
    hear(1.5); h = hear(1.0);
    check("... setting back to 'play the HD': the HD alone again", b.status().state == HD && b.status().alone && h.analog == 0);
    // the contest is over: back to the analog, then the ordinary ALIGNED blend
    al.state = 2; al.offsetFrames = OFF; al.accepted = 9; al.gainDb = 0;
    b.setAnalogForeign(false);
    hear(0.7);
    check("contest over: fades back to the analog", b.status().state == ANALOG || b.status().state == TO_HD);
    hear(4.0); h = hear(1.0);
    check("... and from there the ordinary aligned blend", b.status().state == HD && b.status().aligned && !b.status().alone && h.analog == 0 && h.silent < 10);
    // foreign while the ALIGNED HD is playing: it stays, a dropout gives silence
    b.setAnalogForeign(true); al.state = 3;
    h = hear(1.0);
    check("foreign while the aligned HD plays: the HD stays on (no fade)", b.status().state == HD && b.status().alone && h.analog == 0 && h.mixed == 0);
    dropFrom = aW + RATE / 2; dropTo = dropFrom + 2 * RATE;
    h = hear(4.0);
    // (The aligned HD plays 2.5 s behind live. When its dropout comes up, the HD at the live
    //  position is already good again - so the silence is a moment, and the HD goes on from
    //  there, unaligned. Third silence of this test: 3 s, 13 s, and this one.)
    check("... and its dropout is a moment of silence, then the live HD - never the analog",
          h.analog == 0 && h.silent > 100 && b.status().silences == 3 && b.status().state == HD && !b.status().aligned);
    // the HD decoder is switched off for 6 s (Analog only and back): no stale HD from the ring
    hear(2.0);
    { static std::vector<float> an(2048 * 2, 1000.0f / 32767.0f);
      for (int k = 0; k < 6 * RATE / 2048; k++) { b.pushAnalog(an.data(), 2048); aW += 2048; }   // analog goes on, no HD comes
      b.restart();
      long long stale = 0; size_t got;
      for (int k = 0; k < 10; k++) { b.pushAnalog(an.data(), 2048); aW += 2048; got = b.read(out.data(), 2048, 0, al);
          for (size_t i = 0; i < got; i++) if (out[2 * i] != 0 && out[2 * i] != 1000) stale++; }
      check("the HD decoder off for 6 s: nothing old is played from the ring (silence)", stale == 0 && b.status().silent); }
    h = hear(1.5);
    check("... and the HD is back as soon as it really arrives again", b.status().state == HD && b.status().alone && h.analog == 0);
    b.setAnalogForeign(false); b.setUnmatchedPlaysHd(false);

    // --- build 18, AM: the HD IN STEP with the analog (offset ~0), the blend 3 s behind live ---
    // On FM the HD runs 2.5 s ahead, so a 0.3 s latency leaves 2 s of look-ahead. On AM there
    // is no such lead: nrsc5's AM audio comes out in step with the station's analog, so the
    // blend plays ~3 s behind the newest analog (setRestartLatency) - from the first read on,
    // not only after a restart - and the look-ahead is the same 2 s.
    {
        b.reset(); al = align::Status(); aW = 0;                 // (the time line starts over with the tune)
        b.setRestartLatency(RATE * 3); b.setOffsetTolerance(200);
        // the HD now arrives at its own index = the analog's (offset 0): change hdL's anchor
        auto pushAm = [&](double secs) {
            long long n = (long long)(secs * RATE);
            static std::vector<float> a(2048 * 2, 1000.0f / 32767.0f);
            static std::vector<int16_t> hh(2048 * 2);
            for (long long done = 0; done < n; done += 2048) {
                b.pushAnalog(a.data(), 2048);
                bool drop = dropAt(aW, dropFrom, dropTo);
                for (int i = 0; i < 2048; i++) { hh[2 * i] = drop ? 0 : hdL(aW + i); hh[2 * i + 1] = drop ? 0 : (int16_t)-hh[2 * i]; }
                b.pushHd(hh.data(), 2048 * 2, aW);
                aW += 2048;
            }
        };
        auto playAm = [&](double secs) {
            long long n = (long long)(secs * RATE), got = 0;
            for (long long done = 0; done < n; done += 2048) { pushAm(2048.0 / RATE); got += (long long)b.read(out.data(), 2048, 0, al); }
            return got;
        };
        dropFrom = dropTo = -1;
        long long got = playAm(2.5);
        check("AM: the first 2.7 s after the tune nothing is played yet (the hold-back fills)", got == 0 && b.available() == 0);
        got = playAm(1.0);
        check("AM: then the analog plays, ~3 s behind live", got > 0 && b.status().state == ANALOG);
        al.state = 2; al.offsetFrames = 0; al.accepted = 2; al.gainDb = 0;     // in step: offset 0
        playAm(3.0);
        check("AM: aligned at offset 0 -> the HD, with the 2 s look-ahead the hold-back gives", b.status().state == HD && b.status().toHd == 1 && b.status().aligned);
        // a dropout ~3.9 s ahead of the play position: 3.3 s later it is 0.6 s ahead and the
        // fade to analog has started (as on FM, 0.5 s fade + 0.15 s guard before the gap)
        dropFrom = aW + (long long)(0.4 * RATE); dropTo = dropFrom + RATE;
        playAm(3.3);
        check("AM: a dropout ahead is seen coming - fading to the analog before it plays", b.status().state == ANALOG || b.status().state == TO_ANALOG);
        dropFrom = dropTo = -1;
        playAm(6.0);
        check("AM: ... and the HD again once the next 2 s are clean", b.status().state == HD && b.status().toHd == 2);
        // the aligner's figure wobbles by 100 frames (an envelope alignment): no re-alignment on AM
        al.offsetFrames = 150; playAm(1.0);
        check("AM: the alignment moving by 150 frames does not re-align (tolerance 200)", b.status().state == HD && b.status().toAnalog == 1);
        al.offsetFrames = 500; playAm(2.0);
        check("AM: ... 500 frames does (as 3 would on FM)", b.status().toAnalog == 2);
        b.setRestartLatency(RESTART_LATENCY); b.setOffsetTolerance(OFFSET_TOLERANCE); b.reset(); al = align::Status(); aW = 0;
        got = playAm(0.5);
        check("back to FM's figures: audio from the first block again (hold-back 0)", got > 0);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails;
}
