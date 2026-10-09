// Harness only: takes the HD Radio signal OUT of a recording for a while.
//
//   mkhdgap in.cu8 out.cu8              the whole file without HD
//   mkhdgap in.cu8 out.cu8 14:58        no HD from 14 s to 58 s, the rest untouched
//   mkhdgap in.cu8 out.cu8 0:45 60:70   several stretches
//
// Why: the app treats "this station has no HD" and "the HD went away" differently from
// "the HD is there" (hdsearch.hpp rests the HD search, the blend stays on the analog), and
// no recording of a real station does those things at a moment of our choosing. This tool
// makes one that does - a station that keeps its analog programme while its HD disappears
// and comes back to the second.
//
// How: in FM the HD Radio signal sits in two blocks of carriers BESIDE the analog signal,
// 129 to 198 kHz above and below the centre (from 101 kHz in the widest mode, MP11). The
// analog signal lives inside about +-100 kHz. So a low-pass filter on the IQ samples -
// everything up to 100 kHz passes, everything from 125 kHz is removed - deletes the HD and
// leaves the analog station as it was. (Loud peaks of the analog reach a little beyond
// 100 kHz; they lose a trace of their outermost edges. For a test that does not matter.)
// Outside the stretches the samples are copied as they are, only delayed by the filter's
// own delay (half its length), so the file has no jump where the filter starts or stops.
//
// The filter is a "windowed sinc": the ideal low-pass (a sinc) cut to 255 points and
// rounded off with a Blackman window, which puts everything beyond the transition more
// than 70 dB down - less than the 8-bit samples can show.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <vector>

static const double RATE = 1488375.0;       // samples per second of a .cu8 file made by nrsc5 -w
static const double PASS_HZ = 100000.0;     // up to here: untouched (the analog station)
static const double STOP_HZ = 125000.0;     // from here: gone (the HD carriers start at 129 kHz)
static const int TAPS = 255;                // odd -> the delay is a whole number of samples (127)

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mkhdgap in.cu8 out.cu8 [from:to ...]   (seconds; none = the whole file)\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) { fprintf(stderr, "cannot open the files\n"); return 1; }
    struct Gap { double from, to; };
    std::vector<Gap> gaps;
    for (int a = 3; a < argc; a++) {
        double from = 0, to = 0;
        if (sscanf(argv[a], "%lf:%lf", &from, &to) != 2 || to <= from) { fprintf(stderr, "bad stretch '%s'\n", argv[a]); return 1; }
        gaps.push_back({ from, to });
    }
    if (gaps.empty()) gaps.push_back({ 0, 1e12 });

    // The low-pass: cut-off half-way between PASS and STOP.
    std::vector<float> h(TAPS);
    const double fc = 0.5 * (PASS_HZ + STOP_HZ) / RATE;       // as a fraction of the sample rate
    double sum = 0;
    for (int k = 0; k < TAPS; k++) {
        double m = k - (TAPS - 1) / 2.0;
        double sinc = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
        double window = 0.42 - 0.5 * std::cos(2 * M_PI * k / (TAPS - 1)) + 0.08 * std::cos(4 * M_PI * k / (TAPS - 1));
        h[k] = (float)(sinc * window);
        sum += h[k];
    }
    for (float &v : h) v = (float)(v / sum);                  // gain exactly 1 at the centre

    // The last TAPS samples, I and Q apart, each stored twice so the filter can read them
    // in one straight run (the same trick as in fmdemod.hpp).
    std::vector<float> hi(2 * TAPS, 0.f), hq(2 * TAPS, 0.f);
    int pos = 0;
    const int delay = (TAPS - 1) / 2;
    const size_t CHUNK = 1 << 16;
    std::vector<uint8_t> buf(2 * CHUNK), obuf(2 * CHUNK);
    long long n = 0, filtered = 0;
    for (;;) {
        size_t got = fread(buf.data(), 2, CHUNK, in);
        if (got == 0) break;
        for (size_t s = 0; s < got; s++, n++) {
            float i = buf[2 * s] - 127.5f, q = buf[2 * s + 1] - 127.5f;
            hi[pos] = hi[pos + TAPS] = i;
            hq[pos] = hq[pos + TAPS] = q;
            pos = pos + 1 == TAPS ? 0 : pos + 1;
            // The sample that comes OUT now is the one that went in `delay` samples ago.
            double t = (double)(n - delay) / RATE;
            bool cut = false;
            for (const Gap &g : gaps) if (t >= g.from && t < g.to) cut = true;
            float oi, oq;
            if (cut) {
                const float *wi = &hi[pos], *wq = &hq[pos];   // oldest first; the filter is symmetric
                float ai = 0, aq = 0;
                for (int k = 0; k < TAPS; k++) { ai += h[k] * wi[k]; aq += h[k] * wq[k]; }
                oi = ai; oq = aq; filtered++;
            } else {
                int at = pos + TAPS - 1 - delay;              // the unfiltered sample of the same moment
                oi = hi[at]; oq = hq[at];
            }
            float ri = std::floor(oi + 127.5f + 0.5f), rq = std::floor(oq + 127.5f + 0.5f);
            obuf[2 * s]     = (uint8_t)(ri < 0 ? 0 : ri > 255 ? 255 : ri);
            obuf[2 * s + 1] = (uint8_t)(rq < 0 ? 0 : rq > 255 ? 255 : rq);
        }
        fwrite(obuf.data(), 2, got, out);
    }
    fclose(in); fclose(out);
    fprintf(stderr, "mkhdgap: %.1f s written, HD removed from %.1f s of it\n", n / RATE, filtered / RATE);
    return 0;
}
