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
//                                   "wander=12 noise=0.05" = a station whose analog and HD audio are
//                                   processed differently (the timing wanders by 12 frames rms, r ~0.65),
//                                   "seed=3" = other noise, "v" = print every measurement.
//   drifttest tracker [v]           only the tests of the Tracker alone (build 8): measurements written
//                                   down by hand, among them the fault a tester's video showed.
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
    double noise = 0.012;                             // noise on the analog audio (the program itself is ~0.04 rms)
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
    double clockWorst = 0;        // ... and the furthest it ever was from the truth (from 6 s after the tune), ppm
    double rAvg = 0;              // average correlation of the accepted measurements
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
            float v = prog.at(station + (double)(i - n) / (1 + e) - DELAY + wander(tt)) + (float)su.noise * noise(rng);
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
        if (t > 6) o.clockWorst = std::max(o.clockWorst, std::fabs(tracker.status().clockPpm - su.ppm(t)));
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
                if (r.accepted) { o.accepted++; o.rAvg += r.corr; as = al.status(); tracker.addMeasurement(r.at, r.offsetFine, r.corr, r.epoch, as.state == 2, as.offsetFrames); }
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
    if (o.accepted > 0) o.rAvg /= o.accepted;
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

// ---- the Tracker alone, fed with measurements we make up --------------------------------------
// No audio at all here: the tracker gets the tuning offset block by block, as in the app, and
// alignment measurements we write down by hand - the true offset of a dongle `truePpm` fast,
// plus the error we want each measurement to have. That is how a fault seen once in the field
// becomes a test that runs in a millisecond.
struct Feed { double t; double errFrames; float r; };     // when (s after the tune), how wrong (frames), how well matched
struct Script {
    double truePpm = 58, tuningErrPpm = 0;                // the crystal, and how far the tuning offset is off it
    double warmPpm = 0;                                   // the crystal moves by this much over the run (it warms up)
    bool haveGuess = true;                                // false: the tuning offset is unusable (reads 3 MHz)
    std::vector<Feed> feeds;
    double seconds = 30;
    bool verbose = false;
};
struct ScriptOutcome {
    double worst = 0, last = 0, lastErr = 0;              // the clock figure: furthest from the truth (ppm), at the end, and its error then
    int state = 0;                                        // drift::Status::state at the end
    int unused = 0; double unusedPpm = 0;                 // ... and which witness was not believed then (0 = none)
    bool audioRefused = false;                            // at some point the audio's figure was refused (unused == 1)
    double measuredAt = -1;                               // when the figure was first called "measured"
};
static ScriptOutcome play(const Script &sc) {
    drift::Tracker tr;
    const double F = (double)BLK / 33.75, blockS = (double)BLK / 1488375.0;
    struct Then { long long frame; double slip, raw; };
    std::vector<Then> hist;                               // every block: the measurements refer to the past
    double slip = 0, rate = 0, raw = 100000.0; long long c = 0; size_t next = 0;
    std::vector<long long> results;
    ScriptOutcome o;
    for (double t = 0; t < sc.seconds; t += blockS) {
        const double ppm = sc.truePpm + sc.warmPpm * t / sc.seconds;
        c += (long long)F; slip += rate * F;
        raw += ppm * 1e-6 * F;                            // without the correction the offset grows by that many frames per frame
        hist.push_back({ c, slip, raw });
        rate = tr.update(c, slip, sc.haveGuess ? -(ppm + sc.tuningErrPpm) * 1e-6 * STATION_HZ : 3e6, STATION_HZ, blockS);
        while (next < sc.feeds.size() && t >= sc.feeds[next].t) {
            const Feed &f = sc.feeds[next++];
            long long at = c - (long long)(1.5 * FS);     // the aligner compares the last 3 s: the middle lies 1.5 s back
            Then then = hist.back();
            for (size_t i = hist.size(); i-- > 0;) if (hist[i].frame <= at) { then = hist[i]; break; }
            // ... and the Stretch has taken `slip` of it out again
            double offset = then.raw - then.slip + f.errFrames;
            results.push_back((long long)std::llround(offset)); if (results.size() > 9) results.erase(results.begin());
            size_t n = results.size();
            bool ok = n >= 2 && std::llabs(results[n - 1] - results[n - 2]) <= 3;
            std::vector<long long> sorted = results; std::sort(sorted.begin(), sorted.end());
            tr.addMeasurement(at, offset, f.r, 0, ok, sorted[n / 2]);
            drift::Status d = tr.status();
            if (d.unused == 1) o.audioRefused = true;
            if (d.state == 2 && o.measuredAt < 0) o.measuredAt = f.t;
            if (sc.verbose) {
                printf("    t %5.1f  measurement %+5.1f fr off, r %.2f -> clock %+7.1f ppm (%s, %d points, scatter x%.1f)", f.t, f.errFrames, f.r, d.clockPpm,
                       d.state == 2 ? "measured" : d.state == 1 ? "tuning offset" : "nothing", d.points, d.scatter);
                if (d.unused) printf("   [%s says %+.1f ppm - not used]", d.unused == 1 ? "the audio" : "the tuning offset", d.unusedPpm);
                printf("\n");
            }
        }
        drift::Status d = tr.status();
        if (t > 4) o.worst = std::max(o.worst, std::fabs(d.clockPpm - ppm));
        o.last = d.clockPpm; o.lastErr = d.clockPpm - ppm; o.state = d.state;
        o.unused = d.unused; o.unusedPpm = d.unusedPpm;
    }
    return o;
}
// measurements at the times the app makes them: its watcher asks every 2 s, the aligner
// answers when 3 s of both streams are there, again at the next tick, then every third tick
// (it wants 5 s between two measurements) - each with the error given
static std::vector<Feed> feedsAt(double first, std::initializer_list<double> errs, float r) {
    std::vector<Feed> f; double t = first; int i = 0;
    for (double e : errs) { f.push_back({ t, e, r }); t += i++ == 0 ? 2.0 : 6.0; }
    return f;
}
static void trackerTests(bool verbose) {
    char d[240];
    {   // The fault a tester's screen video showed (2026-10-08, a 2014 Nooelec dongle +58 ppm fast,
        // WWHT 107.9 Syracuse, r 0.71): the first two measurements agreed, the blend went to HD,
        // the third came out ~14 frames off - and build 7 took that for the crystal: "+103.2 ppm
        // (measured)". One noisy measurement must not outvote the tuning offset.
        Script sc; sc.truePpm = 58.2; sc.feeds = feedsAt(8.0, { 0.0, 1.0, 14.0 }, 0.71f); sc.seconds = 20; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "clock figure %+.1f ppm after the third measurement, never more than %.1f ppm from the true +58.2", o.last, o.worst);
        check("the tester's fault: 3rd measurement 14 frames off", o.worst < 9.0, d);
    }
    {   // The same thing found on a recording of Derek's own (KKLQ 100.3, 09-28, his V4 at +1 ppm):
        // analog and HD processed differently, r 0.52-0.54, offsets 109627 / 109651 / 109613 ->
        // build 7: "-44.2 ppm (measured)" on a dongle that is 1 ppm off.
        Script sc; sc.truePpm = 1.0;
        sc.feeds = { { 8.0, 0, 0.54f }, { 10.0, 24, 0.52f }, { 20.0, -14, 0.53f } }; sc.seconds = 24; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "clock figure %+.1f ppm at the end, never more than %.1f ppm from the true +1.0", o.last, o.worst);
        check("KKLQ's numbers: offsets 24 and 38 frames apart", o.worst < 9.0, d);
    }
    {   // A station where every measurement is off by 10 frames or so, for five minutes
        Script sc; sc.truePpm = -100; sc.seconds = 300; sc.verbose = verbose;
        std::mt19937 r(21); std::normal_distribution<double> g(0, 10);
        double t = 8; for (int i = 0; t < 295; i++) { sc.feeds.push_back({ t, g(r), 0.65f }); t += i == 0 ? 2.0 : 6.0; }
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "clock figure %+.1f ppm at the end, never more than %.1f ppm from the true -100.0", o.last, o.worst);
        check("five minutes of measurements 10 frames off", o.worst < 9.0, d);
    }
    // measurements as a normal station gives them: within a frame or so, r 0.95
    auto normal = [](double seconds, unsigned seed) {
        std::vector<Feed> f; std::mt19937 r(seed); std::normal_distribution<double> g(0, 1.2);
        double t = 8; for (int i = 0; t < seconds - 2; i++) { f.push_back({ t, g(r), 0.95f }); t += i == 0 ? 2.0 : 6.0; }
        return f;
    };
    {   // ... the same station, and the dongle is cold: the crystal moves 20 ppm in five minutes.
        // The tuning offset moves with it (it is read all the time), so the figure must too.
        Script sc; sc.truePpm = 40; sc.warmPpm = 20; sc.seconds = 300; sc.verbose = verbose;
        std::mt19937 r(22); std::normal_distribution<double> g(0, 10);
        double t = 8; for (int i = 0; t < 295; i++) { sc.feeds.push_back({ t, g(r), 0.65f }); t += i == 0 ? 2.0 : 6.0; }
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "clock figure %+.1f ppm at the end (true: +60.0), never more than %.1f ppm from the truth", o.last, o.worst);
        check("... while the crystal warms up 40 -> 60 ppm", o.worst < 9.0, d);
    }
    {   // A normal station: nothing of the above may get in its way. "Measured" within half a
        // minute, and then right to a ppm.
        Script sc; sc.truePpm = 58.2; sc.tuningErrPpm = 1.5; sc.feeds = normal(90, 31); sc.seconds = 90; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "\"measured\" from %.0f s, clock figure %+.1f ppm at the end, never more than %.1f ppm from the true +58.2", o.measuredAt, o.last, o.worst);
        check("a normal station: measured, and right", o.state == 2 && o.measuredAt > 0 && o.measuredAt <= 32 && std::fabs(o.lastErr) < 1.0 && o.worst < 4.0 && o.unused == 0, d);
    }
    {   // A normal station and ONE wild measurement, the fourth (60 frames off: under the 200
        // the aligner itself would call a jump). It must not carry the figure away.
        Script sc; sc.truePpm = 58.2; sc.feeds = normal(60, 32); sc.feeds[3].errFrames = 60; sc.seconds = 60; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "clock figure %+.1f ppm at the end, never more than %.1f ppm from the true +58.2", o.last, o.worst);
        check("a normal station, 4th measurement 60 frames off", o.worst < 9.0 && std::fabs(o.lastErr) < 3.0, d);
    }
    {   // The other way round: the TUNING OFFSET is the witness that is wrong (it says -57, the
        // crystal is +57 - a station far off its frequency, a dongle with two crystals) and the
        // audio is as clean as a normal station's. First the fence holds the figure near the
        // tuning offset (and says the audio was refused); half a minute of measurements that
        // agree with each other later the audio wins, and the status says so.
        Script sc; sc.truePpm = 57; sc.tuningErrPpm = -114; sc.feeds = normal(120, 33); sc.seconds = 120; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "audio refused at first: %s; \"measured\" from %.0f s; clock figure %+.1f ppm at the end (true +57.0); the tuning offset's %+.0f ppm marked as not used: %s",
                 o.audioRefused ? "yes" : "NO", o.measuredAt, o.last, o.unusedPpm, o.unused == 2 ? "yes" : "NO");
        check("a tuning offset that is wrong altogether", o.audioRefused && o.measuredAt > 0 && o.measuredAt <= 60 && std::fabs(o.lastErr) < 1.5 && o.unused == 2, d);
    }
    {   // ... and one that is only 12 ppm out (a station 1.1 kHz off its frequency): the figure
        // waits at the fence, 4 ppm short, and is let through once the audio has made its case.
        Script sc; sc.truePpm = 57; sc.tuningErrPpm = 12; sc.feeds = normal(120, 34); sc.seconds = 120; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "\"measured\" from %.0f s; clock figure %+.1f ppm at the end (true +57.0), never more than %.1f ppm off", o.measuredAt, o.last, o.worst);
        check("a tuning offset 12 ppm out", o.measuredAt > 0 && o.measuredAt <= 60 && std::fabs(o.lastErr) < 1.5 && o.worst < 12.5, d);
    }
    {   // No tuning offset at all (it reads nonsense): as before build 8, the audio alone.
        Script sc; sc.truePpm = -57; sc.haveGuess = false; sc.feeds = normal(90, 35); sc.seconds = 90; sc.verbose = verbose;
        ScriptOutcome o = play(sc);
        snprintf(d, sizeof(d), "\"measured\" from %.0f s; clock figure %+.1f ppm at the end (true -57.0)", o.measuredAt, o.last);
        check("no tuning offset: the audio alone", o.measuredAt > 0 && o.measuredAt <= 32 && std::fabs(o.lastErr) < 1.0, d);
    }
}

static void report(const char *name, const Outcome &o, bool pass) {
    if (!pass) failures++;
    printf("%s %-44s ok after %5.1f s (%3.0f %% of the time since)  HD at %5.1f s (%3.0f %%), fades %d/%d  offset moved <= %4.1f fr  clock figure %+5.1f ppm off (never more than %4.1f)  [%d of %d accepted, r %.2f]\n",
           pass ? "PASS" : "FAIL", name, o.firstOk, o.okShare * 100, o.toHdAt, o.hdShare * 100, o.toHd, o.toAnalog, o.worst, o.ppmErr, o.clockWorst, o.accepted, o.measurements, o.rAvg);
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
    if (argc > 1 && !strcmp(argv[1], "tracker")) { trackerTests(argc > 2); printf("%d failure(s)\n", failures); return failures; }
    if (argc > 2) {                                                  // one run
        Setup su; double p = atof(argv[1]); su.ppm = [p](double) { return p; }; su.seconds = atof(argv[2]);
        for (int a = 3; a < argc; a++) {
            if (!strcmp(argv[a], "none")) su.haveGuess = false;
            else if (!strcmp(argv[a], "off")) su.correction = false;
            else if (!strcmp(argv[a], "v")) su.verbose = true;
            else if (!strncmp(argv[a], "lost=", 5)) { su.lostAt = 70; su.lostFrames = atof(argv[a] + 5); }
            else if (!strncmp(argv[a], "wander=", 7)) su.wanderFrames = atof(argv[a] + 7);
            else if (!strncmp(argv[a], "noise=", 6)) su.noise = atof(argv[a] + 6);
            else if (!strncmp(argv[a], "seed=", 5)) su.seed = (unsigned)atoi(argv[a] + 5);
            else su.guessErrPpm = atof(argv[a]);
        }
        Outcome o = run(su);
        report("this run", o, o.firstOk >= 0);
        return 0;
    }
    char name[100];
    stretchTests();
    trackerTests(false);
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
        report(name, o, o.firstOk > 0 && o.firstOk < 20 && o.okShare > 0.6 && o.toHdAt > 0 && o.toHdAt < 21 && o.toAnalog == 0 && o.worst <= 3.5 && std::fabs(o.ppmErr) < 1.5
                        && o.clockWorst < 5.0);
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
    // (Build 8: a tuning offset that is wrong altogether has to be PROVED wrong now - six
    // measurements on a tight line, three times running - so this takes about a minute where
    // build 6 took 25 s, and because the analog drifted all that time the aligner's "middle
    // one of the last nine" is stale when it finally says ok: a few detours through the
    // analog follow while it catches up, then it is steady (from ~160 s here). The price for
    // not believing three measurements over the tuning offset - which is the fault that was
    // seen in the field; this one never has been.)
    { Setup su; su.ppm = [](double) { return 57.0; }; su.guessErrPpm = -114.0; su.seconds = 300; Outcome o = run(su);
      report("+57 ppm, first guess says -57", o, o.firstOk > 0 && o.firstOk < 80 && o.toHdAt > 0 && o.toHdAt < 85 && o.toAnalog <= 5 && o.hdShare > 0.95 && std::fabs(o.ppmErr) < 2.0); }
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
    // 6c. Build 8: a station whose analog and HD audio are processed differently. The two
    //     match only loosely (r 0.6-0.7) and single measurements land 10-20 frames off - with
    //     build 7 one or two of those were taken for the crystal ("+103 ppm" on a +58 ppm
    //     dongle, seen in a tester's video; the clock figure here was up to 50 ppm out in five
    //     of these eight runs). The figure must stay with the tuning offset: never further
    //     than the fence from it. (How often the BLEND goes to HD on such a station is the
    //     aligner's business - it wants two measurements in a row within 3 frames - and not
    //     judged here.)
    for (unsigned seed : { 1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u }) {
        Setup su; su.ppm = [](double) { return 58.0; }; su.wanderFrames = 12; su.noise = 0.05; su.seconds = 180; su.seed = seed;
        Outcome o = run(su);
        snprintf(name, sizeof(name), "+58 ppm, processed differently (run %u)", seed);
        report(name, o, o.clockWorst < 9.5 && o.rAvg < 0.8);
    }
    { Setup su; su.ppm = [](double) { return -100.0; }; su.guessErrPpm = -1.5; su.wanderFrames = 12; su.noise = 0.05; su.seconds = 600; su.seed = 4;
      Outcome o = run(su);
      report("-100 ppm, processed differently, 10 minutes", o, o.clockWorst < 9.5 && o.rAvg < 0.8); }
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
