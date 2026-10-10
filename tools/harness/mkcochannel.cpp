// Harness only: makes a recording of TWO stations on one frequency - the HD Radio signal of
// one station with the analog FM of another on top, the two analogs trading places.
//
//   mkcochannel hd.cu8 other.cu8 out.cu8 [seconds] [from:to ...]
//
//   hd.cu8     a recording of a station with HD (its HD carriers and its analog are used)
//   other.cu8  a recording of any other station (only its analog is used; if the file is
//              shorter it is played round and round)
//   seconds    how much to write (default: the whole of hd.cu8)
//   from:to    stretches (seconds) in which the HD station's OWN analog is the strong one.
//              Everywhere else the other station's analog wins.
//
// Why: 91.1 MHz west of Los Angeles, 2026-10-09 - KDSC Thousand Oaks (HD) and XETRA-FM
// Tijuana ("91X", by tropo) on one frequency. The HD decoded as KDSC's, the analog heard
// was mostly 91X's, and the app swapped between classical music and rock five times a
// minute. No recording of that evening exists, and it could not be repeated on demand
// anyway. This tool builds the same situation out of two ordinary recordings.
//
// How: in FM the HD carriers sit BESIDE the analog signal (129-198 kHz from the centre),
// the analog inside about +-100 kHz. One low-pass filter (the one mkhdgap uses: passes
// up to 100 kHz, removes everything from 125 kHz) splits each recording in two:
//     low  = the analog station          high = recording - low = the HD carriers
// The output is   high(hd)  +  a * low(hd)  +  b * low(other)
// with a and b trading 1.0 and WEAK (-14 dB): an FM receiver hears only the stronger of two
// signals on one frequency (the "capture effect"), so whichever has the 1.0 is the station
// heard, its RDS included. The other station's analog is first brought to the same level
// as the HD station's, and the two change places over 50 ms, not in one jump.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <vector>

static const double RATE = 1488375.0;       // samples per second of a .cu8 file made by nrsc5 -w
static const double PASS_HZ = 100000.0, STOP_HZ = 125000.0;
static const int TAPS = 255;                // odd: the filter's delay is a whole number of samples (127)
static const float WEAK = 0.2f;             // the losing analog: 14 dB down
static const double XFADE_S = 0.05;

struct LowPass {                            // I and Q, the last TAPS samples stored twice (see fmdemod.hpp)
    std::vector<float> h, hi, hq; int pos = 0;
    LowPass() : h(TAPS), hi(2 * TAPS, 0.f), hq(2 * TAPS, 0.f) {
        const double fc = 0.5 * (PASS_HZ + STOP_HZ) / RATE;
        double sum = 0;
        for (int k = 0; k < TAPS; k++) {
            double m = k - (TAPS - 1) / 2.0;
            double sinc = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            double window = 0.42 - 0.5 * std::cos(2 * M_PI * k / (TAPS - 1)) + 0.08 * std::cos(4 * M_PI * k / (TAPS - 1));
            h[k] = (float)(sinc * window); sum += h[k];
        }
        for (float &v : h) v = (float)(v / sum);
    }
    // In: one sample. Out: the filtered sample AND the unfiltered one of the same moment
    // (both belong to the instant TAPS/2 samples ago).
    inline void push(float i, float q, float &li, float &lq, float &ri, float &rq) {
        hi[pos] = hi[pos + TAPS] = i; hq[pos] = hq[pos + TAPS] = q;
        pos = pos + 1 == TAPS ? 0 : pos + 1;
        const float *wi = &hi[pos], *wq = &hq[pos];
        float ai = 0, aq = 0;
        for (int k = 0; k < TAPS; k++) { ai += h[k] * wi[k]; aq += h[k] * wq[k]; }
        li = ai; lq = aq;
        int at = pos + TAPS - 1 - (TAPS - 1) / 2;
        ri = hi[at]; rq = hq[at];
    }
};

// The power of a recording's analog part (its first 2 seconds).
static double analogPower(const char *name) {
    FILE *f = fopen(name, "rb"); if (!f) return 0;
    LowPass lp; std::vector<uint8_t> buf(2 << 16); double p = 0; long long n = 0;
    while (n < (long long)(2 * RATE)) {
        size_t got = fread(buf.data(), 2, 1 << 16, f); if (got == 0) break;
        for (size_t s = 0; s < got; s++, n++) {
            float li, lq, ri, rq; lp.push(buf[2 * s] - 127.5f, buf[2 * s + 1] - 127.5f, li, lq, ri, rq);
            if (n > TAPS) p += (double)li * li + (double)lq * lq;
        }
    }
    fclose(f);
    return n > TAPS ? p / (double)(n - TAPS) : 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: mkcochannel hd.cu8 other.cu8 out.cu8 [seconds] [from:to ...]\n"); return 1; }
    double limit = 1e12;
    struct Own { double from, to; }; std::vector<Own> own;
    for (int a = 4; a < argc; a++) {
        double from = 0, to = 0;
        if (sscanf(argv[a], "%lf:%lf", &from, &to) == 2 && to > from) own.push_back({ from, to });
        else limit = atof(argv[a]);
    }
    double pa = analogPower(argv[1]), pb = analogPower(argv[2]);
    if (pa <= 0 || pb <= 0) { fprintf(stderr, "cannot read the recordings\n"); return 1; }
    const float level = (float)std::sqrt(pa / pb);          // brings the other analog to the HD station's level
    FILE *fa = fopen(argv[1], "rb"), *fb = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (!fa || !fb || !out) { fprintf(stderr, "cannot open the files\n"); return 1; }
    LowPass la, lb;
    const size_t CHUNK = 1 << 16;
    std::vector<uint8_t> ba(2 * CHUNK), bb(2 * CHUNK), ob(2 * CHUNK);
    float mix = 0;                                           // 0 = the other station wins, 1 = the HD station's own analog
    const float step = (float)(1.0 / (XFADE_S * RATE));
    long long n = 0, clipped = 0;
    for (;;) {
        size_t got = fread(ba.data(), 2, CHUNK, fa);
        if (got == 0 || n / RATE >= limit) break;
        size_t have = 0;                                     // the other file: round and round
        while (have < got) {
            size_t g = fread(bb.data() + 2 * have, 2, got - have, fb);
            if (g == 0) { rewind(fb); continue; }
            have += g;
        }
        for (size_t s = 0; s < got; s++, n++) {
            float ali, alq, ari, arq, bli, blq, bri, brq;
            la.push(ba[2 * s] - 127.5f, ba[2 * s + 1] - 127.5f, ali, alq, ari, arq);
            lb.push(bb[2 * s] - 127.5f, bb[2 * s + 1] - 127.5f, bli, blq, bri, brq);
            double t = (double)n / RATE;
            bool ownNow = false;
            for (const Own &o : own) if (t >= o.from && t < o.to) ownNow = true;
            if (ownNow && mix < 1) mix = std::min(1.f, mix + step); else if (!ownNow && mix > 0) mix = std::max(0.f, mix - step);
            const float a = WEAK + (1 - WEAK) * mix, b = (WEAK + (1 - WEAK) * (1 - mix)) * level;
            float oi = (ari - ali) + a * ali + b * bli, oq = (arq - alq) + a * alq + b * blq;
            float ri = std::floor(oi + 127.5f + 0.5f), rq = std::floor(oq + 127.5f + 0.5f);
            if (ri < 0 || ri > 255 || rq < 0 || rq > 255) clipped++;
            ob[2 * s]     = (uint8_t)(ri < 0 ? 0 : ri > 255 ? 255 : ri);
            ob[2 * s + 1] = (uint8_t)(rq < 0 ? 0 : rq > 255 ? 255 : rq);
        }
        fwrite(ob.data(), 2, got, out);
    }
    fclose(fa); fclose(fb); fclose(out);
    fprintf(stderr, "mkcochannel: %.1f s written; the other analog scaled by %.2f; %lld samples clipped (%.3f %%)\n",
            n / RATE, level, clipped, n ? 100.0 * clipped / (2.0 * n) : 0.0);
    return 0;
}
