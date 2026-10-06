// drifttest - the clock correction (drift.hpp) with the aligner and the blender, on made-up
// audio and in made-up time: minutes and hours of "radio" in seconds, with a crystal error
// we choose. No radio signal, no nrsc5 - only the three classes that have to work together:
//
//   one "program" (band-limited noise that swells and fades like music)
//     -> the HD copy: the station's clock, 2.4838 s EARLY, handed over in 2,048-frame pieces
//        as IQ blocks go by, like nrsc5 does
//     -> the analog copy: the DONGLE's clock (off by so many ppm), a little noisy, and its
//        timing wanders by a frame or so the way real stations' audio processing makes it
//     -> Stretch -> Aligner <-> Tracker, and the Blender plays the result.
//
//   drifttest                       all scenarios, PASS / FAIL for each (exit code = failures)
//   drifttest <ppm> <seconds> [guess error in ppm | none] [off] [v]
//                                   one run. "none" = no first guess from the tuning error,
//                                   "off" = no correction at all (the app before build 6),
//                                   "lost=18" = the dongle loses 18 frames' worth of samples at 70 s,
//                                   "v" = print every measurement.
//
// build:  clang++ -std=c++17 -O2 -I ../../app/src/main/cpp drifttest.cpp -o drifttest
#include "aligner.hpp"
#include "blend.hpp"
#include "drift.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <functional>
#include <string>

static const double FS = 44100.0;
static const double DELAY = 2.4838 * FS;             // the station's diversity delay, frames
static const long long BLK = 131072;                 // IQ samples per USB block (88 ms)
static const double STATION_HZ = 94.5e6;

// ---- the program: random, but the same every run; kept for the last 12 s ----
struct Program {
    std::vector<float> ring; long long made = 0;
    std::mt19937 rng{7}; std::normal_distribution<float> g{0, 1};
    float y1 = 0, y2 = 0;
    Program() : ring((size_t)(12 * FS), 0.f) {}
    void makeUpTo(long long i) {
        for (; made <= i; made++) {
            y1 += 0.25f * (g(rng) - y1); y2 += 0.25f * (y1 - y2);
            ring[(size_t)(made % (long long)ring.size())] = 0.25f * y2 * (0.6f + 0.4f * std::sin((float)(made % 31416000) * 2e-4f));
        }
    }
    float at(double x) {                             // between two samples: straight line
        if (x < 0) return 0;
        long long i = (long long)x; makeUpTo(i + 1);
        float f = (float)(x - (double)i), a = ring[(size_t)(i % (long long)ring.size())], b = ring[(size_t)((i + 1) % (long long)ring.size())];
        return a * (1 - f) + b * f;
    }
};

struct Setup {
    std::function<double(double)> ppm = [](double) { return 0.0; };   // the crystal's error over time (seconds)
    double seconds = 120;
    bool haveGuess = true; double guessErrPpm = 1.0;  // the tuning error says ppm + this
    bool correction = true;
    double hdJumpAt = -1;                             // at this time nrsc5 "re-times" its output by one 2,048-frame piece
    double lostAt = -1; double lostFrames = 0;        // at this time the dongle loses this many frames' worth of samples (a USB hiccup)
    double wanderFrames = 1.0;                        // rms wander of the analog's timing (frames)
    bool verbose = false;
    unsigned seed = 5;                                // another seed = other noise, other wander
};
struct Outcome {
    double firstOk = -1;          // seconds until the aligner first said "ok"
    double okShare = 0;           // share of the time after that it stayed "ok"
    double toHdAt = -1;           // when the blend reached the HD
    int toHd = 0, toAnalog = 0;   // fades
    double worst = 0;             // largest move of the TRUE offset after the lock (frames)
    double ppmErr = 0;            // the tracker's clock error figure minus the truth, at the end
    double hdShare = 0;           // share of the time the blend played HD after it first got there
    int epochs = 0, measurements = 0, accepted = 0;
};

static Outcome run(const Setup &su) {
    Program prog;
    align::Aligner al; blend::Blender bl; drift::Stretch stretch; drift::Tracker tracker;
    tracker.setEnabled(su.correction);
    bl.setUnmatchedPlaysHd(false);
    std::mt19937 rng(11 + su.seed); std::normal_distribution<float> noise(0, 1);
    // slow wander of the analog timing: a few sine waves with periods of 3-20 s
    double wf[5], wp[5]; { std::mt19937 r2(su.seed); std::uniform_real_distribution<double> u(0, 1); for (int i = 0; i < 5; i++) { wf[i] = 1.0 / (3 + 17 * u(r2)); wp[i] = 6.283 * u(r2); } }
    auto wander = [&](double t) { double s = 0; for (int i = 0; i < 5; i++) s += std::sin(6.283185 * wf[i] * t + wp[i]); return su.wanderFrames * s * std::sqrt(2.0 / 5.0); };

    Outcome o;
    long long iq = 0, n = 0, k = 0;                  // IQ samples, raw analog frames, HD frames so far
    double station = 0;                              // station time, in frames, of raw analog frame n
    long long hdIdx0 = -1, hdK0 = 0;                 // where the HD run was anchored (time line index, program frame)
    double rate = 0, nextMeasure = 2.0, okTime = 0, sinceOk = 0, hdTime = 0, sinceHd = 0;
    double lockTruth = 0; bool haveLock = false, jumped = false, lost = false;
    std::vector<float> raw, fixed, mono; std::vector<int16_t> hd(4096), out(8192);
    const double blockS = (double)BLK / 1488375.0;
    long long cNow = 0;
    while ((double)iq / 1488375.0 < su.seconds) {
        double t = (double)iq / 1488375.0, e = su.ppm(t) * 1e-6;
        long long n1 = (long long)((double)(iq + BLK) / 33.75);
        if (su.lostAt >= 0 && !lost && t >= su.lostAt) { lost = true; station += su.lostFrames; }   // the samples in between never arrived
        double stationEnd = station + (double)(n1 - n) / (1 + e);
        // -- the HD pieces that come out while this block goes in (0.3 s decoder latency) --
        long long blockStart = (long long)std::llround((double)iq / 33.75 - stretch.slip());
        if (su.hdJumpAt >= 0 && !jumped && t >= su.hdJumpAt) { jumped = true; k += 2048; haveLock = false; }   // one piece lost on the way
        while ((double)(k + 2048 + 13230) <= stationEnd) {
            for (int i = 0; i < 2048; i++) { int v = (int)(prog.at((double)(k + i)) * 32767); hd[2 * i] = hd[2 * i + 1] = (int16_t)v; }
            long long idx = al.pushHd(hd.data(), 4096, blockStart, (long long)std::llround((double)BLK / 33.75));
            bl.pushHd(hd.data(), 4096, idx);
            if (hdIdx0 < 0 || (jumped && hdK0 < 0)) { hdIdx0 = idx; hdK0 = k; }
            k += 2048;
        }
        // -- the analog audio of this block, on the dongle's clock --
        raw.clear();
        for (long long i = n; i < n1; i++) {
            double tt = (double)i / FS;
            float v = prog.at(station + (double)(i - n) / (1 + e) - DELAY + wander(tt)) + 0.012f * noise(rng);
            raw.push_back(v); raw.push_back(v);
        }
        station = stationEnd; n = n1; iq += BLK;
        // -- Stretch -> aligner + blender, and the tracker's word for the next block --
        fixed.clear(); stretch.process(raw.data(), raw.size() / 2, rate, fixed);
        mono.resize(fixed.size() / 2); for (size_t i = 0; i < mono.size(); i++) mono[i] = fixed[2 * i];
        cNow = al.pushAnalog(mono.data(), mono.size());
        bl.pushAnalog(fixed.data(), mono.size());
        double offsetHz = su.haveGuess ? -(su.ppm(t) + su.guessErrPpm) * 1e-6 * STATION_HZ : 3e6;   // 3 MHz: "not usable"
        rate = tracker.update(cNow, stretch.slip(), offsetHz, STATION_HZ, blockS);
        // -- the audio thread --
        align::Status as = al.status();
        while (bl.read(out.data(), 4096, 0, as) > 0) {}
        // -- the alignment watcher, every 2 s --
        t = (double)iq / 1488375.0;
        if (t >= nextMeasure) {
            nextMeasure += 2.0;
            align::Result r;
            if (al.measure(r)) {
                o.measurements++;
                if (r.accepted) { o.accepted++; as = al.status(); tracker.addMeasurement(r.at, r.offsetFine, r.corr, r.epoch, as.state == 2, as.offsetFrames); }
                if (su.verbose) {
                    drift::Status d = tracker.status(); as = al.status();
                    printf("  t %6.1f  offset %lld (%.2f) r %.2f %s  %s median %lld  clock %+6.1f ppm (%s, %d pts, spread %.1f, restarts %d, skipped %d)  blend %d fades %d/%d\n", t, r.offsetFrames, r.offsetFine, r.corr,
                           r.accepted ? "accepted" : "REJECTED", as.state == 2 ? "OK       " : as.state == 1 ? "measuring" : as.state == 3 ? "no match " : "waiting  ", as.offsetFrames,
                           d.clockPpm, d.state == 2 ? "measured" : d.state == 1 ? "guess" : "off", d.points, d.spreadFrames, d.restarts, d.skipped, (int)bl.status().state, bl.status().toHd, bl.status().toAnalog);
                }
            }
        }
        // -- book-keeping: the truth --
        as = al.status();
        blend::Status bs = bl.status();
        if (as.state == 2 && o.firstOk < 0) o.firstOk = t;
        if (o.firstOk >= 0) { sinceOk += blockS; if (as.state == 2) okTime += blockS; }
        if (bs.state == blend::HD && o.toHdAt < 0) o.toHdAt = t;
        if (o.toHdAt >= 0) { sinceHd += blockS; if (bs.state == blend::HD) hdTime += blockS; }
        // the true offset of the corrected analog against the HD, right now: corrected frame
        // cNow holds raw frame cNow + slip, whose program frame is (station time) - DELAY;
        // the HD frame with that program frame sits at index hdIdx0 + (program frame - hdK0).
        if (hdIdx0 >= 0 && !(su.hdJumpAt >= 0 && t >= su.hdJumpAt - 1 && t < su.hdJumpAt + 25)
                        && !(su.lostAt >= 0 && t >= su.lostAt - 1 && t < su.lostAt + 40)) {
            double x = (double)cNow + stretch.slip();
            double programFrame = station - ((double)n - x) / (1 + e) - DELAY;
            double truth = (double)cNow - ((double)hdIdx0 + programFrame - (double)hdK0);
            if (jumped) truth += 2048;                                       // the HD stream lost one piece: its content is 2,048 frames on
            if (as.state == 2 && !haveLock && t > o.firstOk + 20 && bs.state == blend::HD) { haveLock = true; lockTruth = truth; }
            if (haveLock) o.worst = std::max(o.worst, std::fabs(truth - lockTruth));
        }
    }
    drift::Status d = tracker.status();
    blend::Status bs = bl.status();
    o.okShare = sinceOk > 0 ? okTime / sinceOk : 0;
    o.hdShare = sinceHd > 0 ? hdTime / sinceHd : 0;
    o.toHd = bs.toHd; o.toAnalog = bs.toAnalog;
    o.ppmErr = d.clockPpm - su.ppm(su.seconds);
    o.epochs = al.status().epoch;
    return o;
}

static int failures = 0;
static void check(const char *what, bool pass, const char *detail) {
    if (!pass) failures++;
    printf("%s %-44s %s\n", pass ? "PASS" : "FAIL", what, detail); fflush(stdout);
}

// ---- the Stretch alone: is the resampler exact enough, and does it count right? ----
static void stretchTests() {
    char d[200];
    {   // rate 0: a copy, bit for bit
        drift::Stretch st; std::vector<float> in(2 * 50000), out; std::mt19937 r(3); std::uniform_real_distribution<float> u(-1, 1);
        for (float &v : in) v = u(r);
        for (size_t i = 0; i < 50000; i += 3884) st.process(&in[2 * i], std::min<size_t>(3884, 50000 - i), 0.0, out);
        bool same = out.size() == in.size() && !memcmp(in.data(), out.data(), in.size() * sizeof(float));
        snprintf(d, sizeof(d), "%zu frames in, %zu out, slip %.3f", in.size() / 2, out.size() / 2, st.slip());
        check("Stretch at 0 ppm = a bit-for-bit copy", same && st.slip() == 0.0, d);
    }
    // a constant rate: tones up to 15 kHz must come out where the arithmetic says, to better than -80 dB
    for (double ppm : { 57.0, -100.0, 300.0 }) {
        const double rate = ppm * 1e-6, f[4] = { 400.0, 5000.0, 10000.0, 15000.0 };
        auto sig = [&](double x, int ch) { double v = 0; for (int k = 0; k < 4; k++) v += 0.2 * std::sin(6.283185307179586 * f[k] * x / FS + k + ch); return v; };
        drift::Stretch st; std::vector<float> in, out; const size_t N = 44100 * 20;
        for (size_t i = 0; i < N; i += 3884) {
            in.clear(); for (size_t j = i; j < std::min(N, i + 3884); j++) { in.push_back((float)sig((double)j, 0)); in.push_back((float)sig((double)j, 1)); }
            st.process(in.data(), in.size() / 2, rate, out);
        }
        double err = 0, pow = 0; size_t frames = out.size() / 2;
        for (size_t c = 100; c < frames; c++) for (int ch = 0; ch < 2; ch++) {
            double want = sig((double)c * (1 + rate), ch), got = out[2 * c + ch];
            err += (got - want) * (got - want); pow += want * want;
        }
        double db = 10 * std::log10(err / pow), expectFrames = (double)N / (1 + rate);
        snprintf(d, sizeof(d), "error %.1f dB, %zu frames out (arithmetic: %.0f less 12), slip %.2f (arithmetic %.2f)", db, frames, expectFrames, st.slip(), (double)frames * rate);
        char name[80]; snprintf(name, sizeof(name), "Stretch at %+.0f ppm: tones to 15 kHz", ppm);
        check(name, db < -80 && std::fabs((double)frames - (expectFrames - 12)) < 2 && std::fabs(st.slip() - (double)frames * rate) < 1e-6, d);
    }
    {   // the rate changing with every block: no click
        drift::Stretch st; std::vector<float> in, out; std::mt19937 r(9); std::uniform_real_distribution<double> u(-200e-6, 200e-6);
        double worst = 0; long long n = 0;
        for (int b = 0; b < 400; b++) {
            in.clear(); for (int j = 0; j < 3884; j++, n++) { float v = (float)(0.5 * std::sin(6.283185307179586 * 1000.0 * (double)n / FS)); in.push_back(v); in.push_back(v); }
            st.process(in.data(), 3884, b < 3 ? 0.0 : u(r), out);    // the first blocks at 0: the switch from "copy" to "interpolate" is in the test too
        }
        // a 1 kHz tone of amplitude 0.5 never steps by more than 0.5 * 2 pi 1000 / 44100 = 0.0712 from one sample to the next; a click would
        for (size_t c = 1; c < out.size() / 2; c++) worst = std::max(worst, (double)std::fabs(out[2 * c] - out[2 * (c - 1)]));
        snprintf(d, sizeof(d), "largest step %.4f (a clean 1 kHz tone: 0.0712)", worst);
        check("Stretch with the rate changing every block", worst < 0.0716, d);
    }
}

static void report(const char *name, const Outcome &o, bool pass) {
    if (!pass) failures++;
    printf("%s %-44s ok after %5.1f s (%3.0f %% of the time since)  HD at %5.1f s (%3.0f %%), fades %d/%d  offset moved <= %4.1f fr  clock figure %+5.1f ppm off  [%d of %d accepted]\n",
           pass ? "PASS" : "FAIL", name, o.firstOk, o.okShare * 100, o.toHdAt, o.hdShare * 100, o.toHd, o.toAnalog, o.worst, o.ppmErr, o.accepted, o.measurements);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc > 4 && !strcmp(argv[1], "stats")) {                     // many runs with different noise: averages
        double p = atof(argv[2]); int runs = atoi(argv[4]);
        double ok = 0, first = 0, worst = 0, worstMax = 0, fades = 0, hd = 0, perr = 0; int locked = 0;
        for (int i = 0; i < runs; i++) {
            Setup su; su.ppm = [p](double) { return p; }; su.seconds = atof(argv[3]); su.seed = 100 + (unsigned)i;
            for (int a = 5; a < argc; a++) {
                if (!strcmp(argv[a], "none")) su.haveGuess = false;
                else if (!strcmp(argv[a], "off")) su.correction = false;
                else su.guessErrPpm = atof(argv[a]);
            }
            Outcome o = run(su);
            if (o.firstOk >= 0) { locked++; first += o.firstOk; }
            ok += o.okShare; worst += o.worst; worstMax = std::max(worstMax, o.worst); fades += o.toAnalog; hd += o.hdShare; perr += std::fabs(o.ppmErr);
        }
        printf("%d runs: locked %d, first ok after %.1f s (avg), ok %.0f %% of the time, HD %.1f %%, fades back to analog %.2f per run, offset moved %.1f fr (avg) / %.1f (max), clock figure off by %.2f ppm (avg)\n",
               runs, locked, first / std::max(1, locked), ok / runs * 100, hd / runs * 100, fades / runs, worst / runs, worstMax, perr / runs);
        return 0;
    }
    if (argc > 2) {                                                  // one run
        Setup su; double p = atof(argv[1]); su.ppm = [p](double) { return p; }; su.seconds = atof(argv[2]);
        for (int a = 3; a < argc; a++) {
            if (!strcmp(argv[a], "none")) su.haveGuess = false;
            else if (!strcmp(argv[a], "off")) su.correction = false;
            else if (!strcmp(argv[a], "v")) su.verbose = true;
            else if (!strncmp(argv[a], "lost=", 5)) { su.lostAt = 70; su.lostFrames = atof(argv[a] + 5); }
            else su.guessErrPpm = atof(argv[a]);
        }
        Outcome o = run(su);
        report("this run", o, o.firstOk >= 0);
        return 0;
    }
    char name[100];
    stretchTests();
    // 1. The app before build 6 (no correction): fine with a good crystal, lost with a cheap one.
    { Setup su; su.correction = false; su.ppm = [](double) { return 1.0; }; Outcome o = run(su);
      report("no correction, 1 ppm (a good dongle)", o, o.firstOk > 0 && o.firstOk < 20 && o.toHdAt > 0); }
    { Setup su; su.correction = false; su.ppm = [](double) { return 57.0; }; Outcome o = run(su);
      report("no correction, 57 ppm: must NOT lock", o, o.toHdAt < 0 || o.hdShare < 0.3); }
    // 2. With the correction: any crystal, the first guess within a couple of ppm.
    for (double p : { 0.0, 1.0, -1.0, 30.0, -30.0, 57.0, -57.0, 100.0, -100.0, 150.0, -150.0 }) {
        Setup su; su.ppm = [p](double) { return p; }; su.guessErrPpm = p >= 0 ? 1.5 : -1.5;
        Outcome o = run(su);
        snprintf(name, sizeof(name), "%+.0f ppm", p);
        report(name, o, o.firstOk > 0 && o.firstOk < 20 && o.okShare > 0.6 && o.toHdAt > 0 && o.toHdAt < 21 && o.toAnalog == 0 && o.worst <= 3.5 && std::fabs(o.ppmErr) < 1.5);
    }
    // 3. A first guess that is off (the station's own carrier is out by 5 or 10 ppm).
    for (double ge : { 5.0, -5.0, 10.0, -10.0 }) {
        Setup su; su.ppm = [](double) { return 57.0; }; su.guessErrPpm = ge;
        Outcome o = run(su);
        snprintf(name, sizeof(name), "+57 ppm, first guess %+.0f ppm off", ge);
        report(name, o, o.firstOk > 0 && o.firstOk < 26 && o.okShare > 0.6 && o.toHdAt > 0 && o.toHdAt < 27 && o.toAnalog <= 1 && o.worst <= 6.0 && std::fabs(o.ppmErr) < 1.5);
    }
    // 4. No first guess at all, and one with the wrong sign: slower, but it must get there.
    for (double p : { 57.0, -57.0, 100.0, -100.0 }) {
        Setup su; su.ppm = [p](double) { return p; }; su.haveGuess = false;
        Outcome o = run(su);
        snprintf(name, sizeof(name), "%+.0f ppm, no first guess", p);
        report(name, o, o.firstOk > 0 && o.firstOk < 40 && o.toHdAt > 0 && o.toHdAt < 45 && o.toAnalog <= 2 && o.worst <= 6.0 && std::fabs(o.ppmErr) < 2.0);
    }
    { Setup su; su.ppm = [](double) { return 57.0; }; su.guessErrPpm = -114.0; Outcome o = run(su);
      report("+57 ppm, first guess says -57", o, o.firstOk > 0 && o.firstOk < 45 && o.toHdAt > 0 && o.toHdAt < 50 && o.toAnalog <= 2 && o.worst <= 6.0 && std::fabs(o.ppmErr) < 2.0); }
    // 5. A crystal that warms up: 40 -> 60 ppm in the first ten minutes.
    { Setup su; su.seconds = 900; su.ppm = [](double t) { return 40.0 + 20.0 * std::min(1.0, t / 600.0); };
      Outcome o = run(su);
      report("warming up, 40 -> 60 ppm in 10 minutes", o, o.firstOk > 0 && o.firstOk < 20 && o.okShare > 0.6 && o.toAnalog == 0 && o.worst <= 4.0); }
    // 6. nrsc5 re-times its output by one piece after a hiccup (seen on the air): a jump of
    //    2,048 frames. The offsets start over; the crystal's figure must survive.
    { Setup su; su.ppm = [](double) { return -57.0; }; su.hdJumpAt = 60; su.seconds = 150;
      Outcome o = run(su);
      report("-57 ppm, the HD jumps by one piece at 60 s", o, o.firstOk > 0 && o.firstOk < 20 && o.epochs >= 1 && o.toHd == 2 && o.toAnalog == 1 && std::fabs(o.ppmErr) < 1.5 && o.worst <= 3.5); }
    // 6b. A USB hiccup: the dongle loses samples worth 18 frames (seen on a Windows recording:
    //     18 and 5). The blend uses the offset it had - so the analog must be steered back to
    //     it (same offset as before: "moved" stays small), without a fade back to analog.
    //     And a big loss (120 frames): too far to steer - the new offset must be taken over.
    { Setup su; su.ppm = [](double) { return 57.0; }; su.lostAt = 70; su.lostFrames = 18; su.seconds = 180;
      Outcome o = run(su);
      report("+57 ppm, samples worth 18 frames lost at 70 s", o, o.firstOk > 0 && o.firstOk < 20 && o.toAnalog == 0 && o.epochs == 0 && std::fabs(o.ppmErr) < 1.5 && o.worst <= 3.5); }
    { Setup su; su.ppm = [](double) { return 57.0; }; su.lostAt = 70; su.lostFrames = 120; su.seconds = 180;
      Outcome o = run(su);
      report("+57 ppm, samples worth 120 frames lost at 70 s", o, o.firstOk > 0 && o.firstOk < 20 && o.toAnalog <= 1 && std::fabs(o.ppmErr) < 1.5 && o.hdShare > 0.98 && o.worst > 100 && o.worst < 125); }
    // 7. The long haul: an hour each. Without the correction 57 ppm is 9,000 frames (0.2 s) an hour.
    for (double p : { 57.0, -100.0, 0.5 }) {
        Setup su; su.seconds = 3600; su.ppm = [p](double) { return p; };
        Outcome o = run(su);
        snprintf(name, sizeof(name), "one hour at %+.1f ppm", p);
        report(name, o, o.firstOk > 0 && o.firstOk < 20 && o.okShare > 0.6 && o.hdShare > 0.999 && o.toAnalog == 0 && o.worst <= 4.0 && std::fabs(o.ppmErr) < 1.5);
    }
    printf("%d failure(s)\n", failures);
    return failures;
}
