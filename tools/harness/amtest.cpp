// amtest.cpp - the AM demodulator (amdemod.hpp) alone, on made-up signals and on a recording.
//
//   ./amtest                      all the checks on made-up signals (exit code = failures)
//   ./amtest file.cu8 out.wav [seconds] [bw]   demodulate a recording (mono / C-QUAM as it comes),
//                                 print the meter once a second, write the audio as a WAV
//   ./amtest gen plain|cquam out.cu8 [seconds]  write a made-up AM station (no HD) as a .cu8 recording
//                                 for the whole engine (apptest): plain AM, or C-QUAM stereo
//
// The made-up signals are built from the standards' own formulas, not from the decoder's idea
// of them (the FM lesson of 2026-10-02): plain AM = (1 + m(t)) cos(wt); C-QUAM (Motorola) =
// (1 + L + R) cos(wt + atan((L - R) / (1 + L + R))) with a 25 Hz pilot at 4 % in L - R.
// What is checked:
//   1. a 1 kHz tone at 50 % on a carrier 300 Hz off centre: the tone comes out at 0.5, the
//      carrier offset reads 300 Hz, left = right, no pilot, the meter reads high
//   2. the same with white noise added at a known level: the meter reads the C/N it should
//      (within 2 dB) and the lock is held
//   3. the carrier 1.2 kHz off and drifting: the PLL follows (offset tracks, tone still clean)
//   4. an HD-like quadrature signal (noise in quadrature to the carrier): the synchronous
//      detector rejects it (> 30 dB below what an envelope detector would hear)
//   5. C-QUAM with a tone on the LEFT only: the tone is on the left output, >= 25 dB
//      separation, pilot found, stereo 100 %; the same tone on the right -> right (the level is
//      2 x the tone: the stereo channels are (L+R) +- (L-R), see amdemod.hpp)
//   6. the same C-QUAM signal with hdSynced = true: mono (no stereo decode), pilot "off"
//   7. C-QUAM with the noise of a 28 dB C/N: the blend goes toward mono
//   8. no carrier (noise alone): not locked, the output does not explode (|out| <= 1)
//   9. build 21, NRSC-1 de-emphasis: tones 100 Hz .. 9.5 kHz with the 10 kHz bandwidth, the
//      level with de-emphasis on against off = the standard's receiver curve (Table 2 of
//      NRSC-1-A: -6.92 dB at 5 kHz, -10.00 dB at 10 kHz) within 0.1 dB; off = bit for bit a
//      demodulator that was never switched; C-QUAM's separation is kept with it on
#include "amdemod.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <functional>

static const double FS = am::IQ_RATE;
static int failures = 0;
static void check(bool ok, const char *what) { printf("  [%s] %s\n", ok ? " ok " : "FAIL", what); if (!ok) failures++; }

// A made-up signal: cu8 bytes from a complex baseband function. amplitude = the carrier's
// amplitude in the 8-bit range (1.0 = full scale).
struct Gen {
    std::vector<uint8_t> bytes;
    void make(double seconds, double carrierHz, double amplitude, double noiseRms, double drift,
              const std::function<void(double, float &, float &)> &mod /* t -> (I, Q) modulation */) {
        size_t n = (size_t)(seconds * FS);
        bytes.resize(n * 2);
        std::mt19937 rng(42); std::normal_distribution<float> g(0.f, 1.f);
        double ph = 0;
        for (size_t i = 0; i < n; i++) {
            double t = (double)i / FS;
            float mi, mq; mod(t, mi, mq);
            ph += 2 * M_PI * (carrierHz + drift * t) / FS;
            std::complex<double> c = std::polar(amplitude, ph) * std::complex<double>(mi, mq);
            double re = c.real() + noiseRms * g(rng), im = c.imag() + noiseRms * g(rng);
            bytes[2 * i] = (uint8_t)std::max(0.0, std::min(255.0, std::round(re * 127.5 + 127.5)));
            bytes[2 * i + 1] = (uint8_t)std::max(0.0, std::min(255.0, std::round(im * 127.5 + 127.5)));
        }
    }
};

// Runs the demodulator over the bytes in app-sized blocks; returns the stereo audio.
static std::vector<float> run(am::Demod &d, const std::vector<uint8_t> &bytes) {
    std::vector<float> out;
    const size_t block = 262144;
    for (size_t at = 0; at < bytes.size(); at += block) {
        d.processCu8(bytes.data() + at, std::min(block, bytes.size() - at));
        out.insert(out.end(), d.audioOut.begin(), d.audioOut.end());
        d.audioOut.clear();
    }
    return out;
}
// rms of one channel over [from, to) seconds; and the rms of its component at f Hz
static double rms(const std::vector<float> &st, int ch, double from, double to) {
    size_t a = (size_t)(from * 44100), b = std::min(st.size() / 2, (size_t)(to * 44100));
    double s = 0; for (size_t i = a; i < b; i++) s += (double)st[2 * i + ch] * st[2 * i + ch];
    return b > a ? std::sqrt(s / (double)(b - a)) : 0;
}
static double toneRms(const std::vector<float> &st, int ch, double from, double to, double f) {
    size_t a = (size_t)(from * 44100), b = std::min(st.size() / 2, (size_t)(to * 44100));
    std::complex<double> acc = 0;
    for (size_t i = a; i < b; i++) acc += (double)st[2 * i + ch] * std::polar(1.0, -2 * M_PI * f * (double)i / 44100);
    return b > a ? std::abs(acc) / (double)(b - a) * std::sqrt(2.0) : 0;    // amplitude -> rms
}
static double db(double x) { return 20 * std::log10(std::max(x, 1e-12)); }

static void writeWav(const char *name, const std::vector<float> &st) {
    FILE *f = fopen(name, "wb"); if (!f) return;
    uint32_t n = (uint32_t)st.size(), bytes = n * 2, rate = 44100, byteRate = rate * 4, fmtLen = 16, riffLen = 36 + bytes;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riffLen, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (float v : st) { int16_t s = (int16_t)std::lrintf(std::max(-1.f, std::min(1.f, v)) * 32767.f); fwrite(&s, 2, 1, f); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "gen") == 0) {
        // ./amtest gen plain|cquam out.cu8 [seconds]: a made-up AM station as a recording the whole
        // engine can play (apptest): a -20 dBFS carrier 300 Hz off centre, no HD. "plain": a 1 kHz
        // tone at 50 % that steps to 600 Hz every 2 s (something for the aligner's envelope);
        // "cquam": C-QUAM, the tone on the LEFT for 2 s, then on the RIGHT, and so on, pilot at 4 %.
        const bool cquam = strcmp(argv[2], "cquam") == 0;
        const double secs = argc > 4 ? atof(argv[4]) : 40;
        Gen g;
        g.make(secs, 300.0, 0.1, 0.002, 0.0, [cquam](double t, float &i, float &q) {
            const int step = (int)(t / 2);
            const double tone = 0.4 * std::sin(2 * M_PI * (step % 2 ? 600 : 1000) * t) * (0.6 + 0.4 * std::sin(2 * M_PI * 1.3 * t));
            if (!cquam) { i = (float)(1 + tone); q = 0; return; }
            const double L = step % 2 ? 0 : tone, R = step % 2 ? tone : 0, pilot = 0.04 * std::sin(2 * M_PI * 25 * t);
            const double phi = std::atan2(L - R + pilot, 1 + L + R);
            i = (float)((1 + L + R) * std::cos(phi)); q = (float)((1 + L + R) * std::sin(phi)); });
        FILE *f = fopen(argv[3], "wb"); if (!f) { perror(argv[3]); return 1; }
        fwrite(g.bytes.data(), 1, g.bytes.size(), f); fclose(f);
        printf("wrote %s: %.0f s of %s AM\n", argv[3], secs, cquam ? "C-QUAM" : "plain");
        return 0;
    }
    if (argc >= 3) {
        // a recording
        FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
        double secs = argc > 3 ? atof(argv[3]) : 1e9;
        am::Demod d;
        if (argc > 4) d.setBandwidth(atoi(argv[4]));
        if (argc > 5) d.setDeemphasis(strcmp(argv[5], "on") == 0);     // build 21: ... [bw] [on|off]
        std::vector<uint8_t> buf(262144); std::vector<float> out; double t = 0, nextPrint = 1;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0 && t < secs) {
            d.processCu8(buf.data(), n);
            out.insert(out.end(), d.audioOut.begin(), d.audioOut.end()); d.audioOut.clear();
            t += (double)n / 2 / FS;
            if (t >= nextPrint) {
                nextPrint += 1;
                printf("%6.1f s  carrier %6.1f dBFS  offset %+7.1f Hz  C/N %5.1f dB  locked %d  pilot %d (%.1f %%)  stereo %3.0f %%  audio rms %.3f\n",
                       t, d.carrierDbfs(), d.freqOffsetHz(), d.snrDb(), d.locked() ? 1 : 0, d.pilot() ? 1 : 0, d.pilotLevel() * 100,
                       d.stereoAmount() * 100, rms(out, 0, t - 1, t));
            }
        }
        fclose(f);
        writeWav(argv[2], out);
        printf("wrote %s (%.1f s); path delay %.1f ms\n", argv[2], (double)out.size() / 2 / 44100, d.latencySeconds() * 1000);
        return 0;
    }

    Gen g; am::Demod d; std::vector<float> out;
    // 1. a 1 kHz tone at 50 %, carrier 300 Hz off centre, -20 dBFS carrier
    printf("1. plain AM, 1 kHz tone at 50 %%, carrier +300 Hz\n");
    g.make(4.0, 300.0, 0.1, 0.0, 0.0, [](double t, float &i, float &q) { i = (float)(1 + 0.5 * std::sin(2 * M_PI * 1000 * t)); q = 0; });
    d.reset(); out = run(d, g.bytes);
    {
        double tl = toneRms(out, 0, 2, 4, 1000), tr = toneRms(out, 1, 2, 4, 1000), all = rms(out, 0, 2, 4);
        printf("   tone L %.4f R %.4f (want 0.3536 rms) total %.4f  offset %.1f Hz  carrier %.1f dBFS  C/N %.1f dB  locked %d pilot %d\n",
               tl, tr, all, d.freqOffsetHz(), d.carrierDbfs(), d.snrDb(), d.locked(), d.pilot());
        check(std::fabs(tl - 0.3536) < 0.02 && std::fabs(tr - 0.3536) < 0.02, "tone at 50 % on both channels (within 0.5 dB)");
        check(std::fabs(all - tl) < 0.01, "nothing but the tone (distortion + noise < -30 dB)");
        check(std::fabs(d.freqOffsetHz() - 300) < 2, "carrier offset reads +300 Hz (within 2 Hz)");
        check(std::fabs(d.carrierDbfs() + 20) < 0.5, "carrier level reads -20 dBFS");
        check(d.locked() && !d.pilot(), "locked, no pilot");
        check(d.snrDb() > 45, "meter reads > 45 dB C/N on a clean signal (8-bit rounding sets ~50 dB at -20 dBFS)");
    }
    // 2. the same with noise: C/N in 5 kHz = A^2 / (2 sigma^2 / FS * 5000)  (complex noise: sigma per component)
    for (double cn : { 40.0, 25.0 }) {
        double sigma = std::sqrt(0.1 * 0.1 / std::pow(10, cn / 10) * FS / 5000 / 2);
        printf("2. plain AM with noise: C/N %.0f dB in 5 kHz (sigma %.5f)\n", cn, sigma);
        g.make(6.0, 300.0, 0.1, sigma, 0.0, [](double t, float &i, float &q) { i = (float)(1 + 0.5 * std::sin(2 * M_PI * 1000 * t)); q = 0; });
        d.reset(); out = run(d, g.bytes);
        double tl = toneRms(out, 0, 3, 6, 1000);
        printf("   meter %.1f dB  tone %.4f  locked %d  pilot %d  stereo %.0f %%\n", d.snrDb(), tl, d.locked(), d.pilot(), d.stereoAmount() * 100);
        check(std::fabs(d.snrDb() - cn) < 2.0, "meter within 2 dB of the true C/N");
        check(d.locked(), "still locked");
        check(!d.pilot(), "no false pilot in noise");
    }
    // 3. drifting carrier: 1.2 kHz off, moving 50 Hz/s
    printf("3. carrier -1200 Hz, drifting +50 Hz/s\n");
    g.make(5.0, -1200.0, 0.1, 0.0, 50.0, [](double t, float &i, float &q) { i = (float)(1 + 0.5 * std::sin(2 * M_PI * 1000 * t)); q = 0; });
    d.reset(); out = run(d, g.bytes);
    {
        double tl = toneRms(out, 0, 3, 5, 1000), all = rms(out, 0, 3, 5);
        printf("   offset at the end %.1f Hz (true -950)  tone %.4f total %.4f\n", d.freqOffsetHz(), tl, all);
        check(std::fabs(d.freqOffsetHz() + 950) < 5, "PLL followed the drift (offset within 5 Hz of the truth)");
        check(std::fabs(tl - 0.3536) < 0.02 && std::fabs(all - tl) < 0.02, "tone clean while drifting");
    }
    // 4. HD-like quadrature: noise at 20 % rms in quadrature (the tertiary carriers), tone in phase
    printf("4. a quadrature signal (like HD's tertiary carriers) at 20 %% rms\n");
    {
        std::mt19937 rng(7); std::normal_distribution<float> gn(0.f, 1.f); float lp = 0;
        g.make(4.0, 300.0, 0.1, 0.0, 0.0, [&](double t, float &i, float &q) {
            lp += 0.02f * (gn(rng) - lp);                                  // band-limited-ish noise, ~0-5 kHz
            i = (float)(1 + 0.5 * std::sin(2 * M_PI * 1000 * t)); q = 0.2f * lp * 7.f; });
        d.reset(); out = run(d, g.bytes);
        double tl = toneRms(out, 0, 2, 4, 1000), all = rms(out, 0, 2, 4);
        double rest = std::sqrt(std::max(all * all - tl * tl, 0.0));
        printf("   tone %.4f  everything else %.4f (%.1f dB below the tone)\n", tl, rest, db(rest / tl));
        check(db(rest / tl) < -25, "quadrature rejected: the rest is > 25 dB under the tone");
    }
    // 5. C-QUAM, tone on the LEFT only (then on the right)
    for (int side = 0; side < 2; side++) {
        printf("5. C-QUAM, 1 kHz tone at 40 %% on the %s only, 25 Hz pilot at 4 %%\n", side == 0 ? "LEFT" : "RIGHT");
        g.make(8.0, 300.0, 0.1, 0.0, 0.0, [side](double t, float &i, float &q) {
            double tone = 0.4 * std::sin(2 * M_PI * 1000 * t);
            double L = side == 0 ? tone : 0, R = side == 1 ? tone : 0;
            double pilot = 0.04 * std::sin(2 * M_PI * 25 * t);
            double sum = L + R, diff = L - R + pilot;
            double phi = std::atan2(diff, 1 + sum);
            i = (float)((1 + sum) * std::cos(phi)); q = (float)((1 + sum) * std::sin(phi)); });
        d.reset(); out = run(d, g.bytes);
        double tl = toneRms(out, 0, 6, 8, 1000), tr = toneRms(out, 1, 6, 8, 1000);
        double pl = toneRms(out, 0, 6, 8, 25);
        printf("   tone L %.4f R %.4f -> separation %.1f dB; pilot found %d (%.1f %%), stereo %.0f %%, pilot left in the audio %.4f\n",
               tl, tr, side == 0 ? db(tl / tr) : db(tr / tl), d.pilot(), d.pilotLevel() * 100, d.stereoAmount() * 100, pl);
        check(d.pilot() && d.stereoAmount() > 0.99, "pilot found, full stereo");
        check(side == 0 ? tl > tr : tr > tl, side == 0 ? "the tone is on the LEFT" : "the tone is on the RIGHT");
        check((side == 0 ? db(tl / tr) : db(tr / tl)) > 25, "separation >= 25 dB");
        check(std::fabs((side == 0 ? tl : tr) - 2 * 0.4 / std::sqrt(2.0)) < 0.035, "the tone's level is right (2 x 40 %, see the header: L = (L+R) + (L-R); within 0.5 dB)");
        check(pl < 0.004, "the 25 Hz pilot is notched out of the audio (< 0.4 %)");
    }
    // 6. the same C-QUAM signal, but the HD decoder says it is in sync: mono
    printf("6. C-QUAM signal with hdSynced = true\n");
    {
        d.reset(); d.setHdSynced(true); out = run(d, g.bytes); d.setHdSynced(false);
        double tl = toneRms(out, 0, 6, 8, 1000), tr = toneRms(out, 1, 6, 8, 1000);
        printf("   L %.4f R %.4f pilot %d stereo %.0f %%\n", tl, tr, d.pilot(), d.stereoAmount() * 100);
        check(!d.pilot() && d.stereoAmount() == 0 && std::fabs(tl - tr) < 0.001, "mono, pilot off");
    }
    // 7. C-QUAM in noise at 28 dB C/N: the blend narrows the stereo
    printf("7. C-QUAM (left-only tone) at 28 dB C/N\n");
    {
        double sigma = std::sqrt(0.1 * 0.1 / std::pow(10, 28.0 / 10) * FS / 5000 / 2);
        g.make(8.0, 300.0, 0.1, sigma, 0.0, [](double t, float &i, float &q) {
            double L = 0.4 * std::sin(2 * M_PI * 1000 * t), pilot = 0.04 * std::sin(2 * M_PI * 25 * t);
            double phi = std::atan2(L + pilot, 1 + L);
            i = (float)((1 + L) * std::cos(phi)); q = (float)((1 + L) * std::sin(phi)); });
        d.reset(); out = run(d, g.bytes);
        printf("   meter %.1f dB  pilot %d  stereo %.0f %%\n", d.snrDb(), d.pilot(), d.stereoAmount() * 100);
        check(d.pilot(), "pilot still found at 28 dB");
        check(d.stereoAmount() < 0.5, "stereo narrowed below 50 % at 28 dB C/N");
    }
    // 8. noise alone
    printf("8. noise alone (no carrier)\n");
    g.make(4.0, 0.0, 0.0, 0.02, 0.0, [](double, float &i, float &q) { i = 0; q = 0; });
    d.reset(); out = run(d, g.bytes);
    {
        float peak = 0; for (float v : out) peak = std::max(peak, std::fabs(v));
        printf("   locked %d  pilot %d  peak |out| %.3f  meter %.1f\n", d.locked(), d.pilot(), peak, d.snrDb());
        check(!d.locked() && !d.pilot(), "not locked, no pilot");
        check(peak <= 2.0f, "output stays bounded (<= 2.0; the app clamps at 1.0 after its 0.5 gain)");
    }
    // 9. build 21: NRSC-1 de-emphasis
    printf("9. NRSC-1 de-emphasis (bandwidth 10 kHz)\n");
    {
        auto nrsc = [](double f) {                         // the receiver curve, NRSC-1-A Table 2
            return 10 * std::log10(1 + (f / 8700) * (f / 8700)) - 10 * std::log10(1 + (f / 2122) * (f / 2122)); };
        double worst = 0;
        for (double f : {100.0, 1000.0, 2122.0, 3000.0, 5000.0, 7000.0, 8700.0, 9500.0}) {
            g.make(3.0, 300.0, 0.1, 0.0, 0.0, [f](double t, float &i, float &q) { i = (float)(1 + 0.3 * std::sin(2 * M_PI * f * t)); q = 0; });
            am::Demod off, on; off.setBandwidth(10000); on.setBandwidth(10000); on.setDeemphasis(true);
            std::vector<float> a = run(off, g.bytes), b = run(on, g.bytes);
            double got = db(toneRms(b, 0, 1.5, 3, f) / toneRms(a, 0, 1.5, 3, f)), want = nrsc(f);
            worst = std::max(worst, std::fabs(got - want));
            printf("   %6.0f Hz  on/off %+7.2f dB  NRSC-1 %+7.2f dB\n", f, got, want);
        }
        check(worst < 0.1, "de-emphasis follows NRSC-1 Table 2 within 0.1 dB, 100 Hz .. 9.5 kHz");
        // off = untouched: a demodulator switched on and off again against one never switched
        g.make(3.0, 300.0, 0.1, 0.002, 0.0, [](double t, float &i, float &q) { i = (float)(1 + 0.4 * std::sin(2 * M_PI * 3000 * t)); q = 0; });
        am::Demod plain, flipped; flipped.setDeemphasis(true); flipped.setDeemphasis(false);
        std::vector<float> a = run(plain, g.bytes), b = run(flipped, g.bytes);
        check(a == b, "de-emphasis off = bit for bit the output without it");
        // C-QUAM (test 5's signal and window): a tone on the left stays on the left - the filter
        // is the same on both channels, so the separation must not change
        g.make(8.0, 300.0, 0.1, 0.0, 0.0, [](double t, float &i, float &q) {
            double L = 0.4 * std::sin(2 * M_PI * 1000 * t), pilot = 0.04 * std::sin(2 * M_PI * 25 * t);
            double phi = std::atan2(L + pilot, 1 + L);
            i = (float)((1 + L) * std::cos(phi)); q = (float)((1 + L) * std::sin(phi)); });
        am::Demod st0, st; st.setDeemphasis(true);
        std::vector<float> c0 = run(st0, g.bytes), c = run(st, g.bytes);
        const double sep0 = db(toneRms(c0, 0, 6, 8, 1000) / toneRms(c0, 1, 6, 8, 1000));
        const double sep = db(toneRms(c, 0, 6, 8, 1000) / toneRms(c, 1, 6, 8, 1000));
        printf("   C-QUAM left-only: separation %.1f dB off, %.1f dB on\n", sep0, sep);
        check(sep >= 25 && std::fabs(sep - sep0) < 0.5, "C-QUAM separation kept with de-emphasis on (>= 25 dB, within 0.5 dB of off)");
    }
    printf("%d failure(s)\n", failures);
    return failures;
}
