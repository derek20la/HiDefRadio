// mkdrift - turns a recording made with a good dongle into what a dongle with a cheap
// crystal would have recorded. For testing the app's clock correction (drift.hpp).
//
//   mkdrift in.cu8 out.cu8 <ppm> <station frequency in Hz> [tuning ppm]
//   e.g.  mkdrift krth.cu8 krth+57.cu8 57 101100000
//   The optional last number moves the station by THAT many ppm instead (0 = not at all):
//   a recording no real dongle could make, for testing what the app does when the tuning
//   error and the sample clock disagree.
//
// An RTL-SDR dongle has ONE crystal (28.8 MHz). Everything is derived from it: the tuner's
// frequency AND the rate at which samples are taken. A crystal that runs `ppm` parts per
// million fast therefore does two things at once:
//   1. it tunes too high, by ppm x the frequency - the station shows up BELOW the centre
//      (57 ppm at 101.1 MHz = 5.8 kHz), and
//   2. it takes samples too fast - (1 + ppm/1e6) x 1,488,375 a second - so every second of
//      radio becomes a few samples more than a second of "dongle time".
// The first is harmless (both decoders follow the frequency). The second is what made the
// blend fail on old dongles: the analog audio is paced by the dongle's clock, the HD audio by
// the station's.
//
// This tool does both to a recording: it resamples it by (1 + ppm/1e6) with a windowed-sinc
// interpolator and shifts it down by ppm x frequency. A negative ppm = a slow crystal.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <vector>
#include <complex>

static double besselI0(double x) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { double h = x / (2.0 * k); t *= h * h; s += t; } return s; }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: mkdrift in.cu8 out.cu8 <ppm> <station Hz> [tuning ppm]\n"); return 1; }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) { fprintf(stderr, "cannot open the files\n"); return 1; }
    const double e = atof(argv[3]) * 1e-6, stationHz = atof(argv[4]), fs = 1488375.0;
    const double tune = argc > 5 ? atof(argv[5]) * 1e-6 : e;         // the tuning error (normally the same crystal error)
    std::vector<uint8_t> raw;
    { uint8_t buf[1 << 16]; size_t n; while ((n = fread(buf, 1, sizeof(buf), in)) > 0) raw.insert(raw.end(), buf, buf + n); }
    const long long N = (long long)(raw.size() / 2);
    // The recording dongle's own DC offset sits at 0 Hz and would be shifted along with the
    // station - take it out first (a real dongle's DC stays at 0 Hz).
    double mi = 0, mq = 0;
    for (long long k = 0; k < N; k++) { mi += raw[2 * k]; mq += raw[2 * k + 1]; }
    mi /= N; mq /= N;
    // Interpolator: 32 taps, 1,024 positions between two samples, Kaiser window (beta 9).
    const int T = 32, P = 1024;
    std::vector<float> tab((size_t)(P + 1) * T);
    for (int p = 0; p <= P; p++) {
        double frac = (double)p / P, sum = 0;
        for (int j = 0; j < T; j++) {
            double x = (j - (T / 2 - 1)) - frac;                 // distance (in samples) from the point we want
            double r = x / (T / 2), w = std::fabs(r) < 1 ? besselI0(9.0 * std::sqrt(1 - r * r)) / besselI0(9.0) : 0;
            double sinc = std::fabs(x) < 1e-9 ? 1 : std::sin(M_PI * x) / (M_PI * x);
            tab[(size_t)p * T + j] = (float)(sinc * w); sum += sinc * w;
        }
        for (int j = 0; j < T; j++) tab[(size_t)p * T + j] = (float)(tab[(size_t)p * T + j] / sum);
    }
    const long long M = (long long)((N - T) * (1 + e));           // output samples for the same stretch of time
    const double shift = -2 * M_PI * tune * stationHz / (fs * (1 + e)); // radians per OUTPUT sample
    std::vector<uint8_t> o(1 << 16); size_t fill = 0; long long clipped = 0;
    for (long long k = 0; k < M; k++) {
        double pos = k / (1 + e) + (T / 2 - 1);                   // where output sample k falls in the input
        long long i0 = (long long)pos; int p = (int)((pos - i0) * P);
        const float *h = &tab[(size_t)p * T];
        const uint8_t *x = &raw[2 * (size_t)(i0 - (T / 2 - 1))];
        float si = 0, sq = 0;
        for (int j = 0; j < T; j++) { si += h[j] * (float)(x[2 * j] - mi); sq += h[j] * (float)(x[2 * j + 1] - mq); }
        double ph = std::fmod(shift * (double)k, 2 * M_PI);
        float c = (float)std::cos(ph), s = (float)std::sin(ph);
        float oi = si * c - sq * s + 127.5f, oq = si * s + sq * c + 127.5f;
        int vi = (int)std::lround(oi), vq = (int)std::lround(oq);
        if (vi < 0 || vi > 255 || vq < 0 || vq > 255) clipped++;
        o[fill++] = (uint8_t)(vi < 0 ? 0 : vi > 255 ? 255 : vi);
        o[fill++] = (uint8_t)(vq < 0 ? 0 : vq > 255 ? 255 : vq);
        if (fill == o.size()) { fwrite(o.data(), 1, fill, out); fill = 0; }
    }
    fwrite(o.data(), 1, fill, out); fclose(out);
    fprintf(stderr, "%lld samples in, %lld out (sample clock %+.1f ppm, station moved %+.0f Hz at %.1f MHz), %lld clipped\n",
            N, M, e * 1e6, -tune * stationHz, stationHz / 1e6, clipped);
    return 0;
}
