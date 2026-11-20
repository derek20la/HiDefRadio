// Build 39: "how good is this tuner gain for HD?" - measured from a short burst of samples,
// without the HD decoder. Used by the FM auto-gain (doAutoGainFm() in native-lib.cpp).
//
// WHY. Until build 38 the auto-gain looked at ONE thing: how much of the dongle's 0..255
// range the signal uses (the "peak"), and took the highest gain that stayed below -4 dBFS.
// That is right for most stations and badly wrong for some: on 103.5 KOST in Long Beach
// (2026-11-16) 20.7 dB gave a peak of -1.8 dBFS - nothing clipped - and no HD at all, while
// 14.4 dB (peak -8.9) gave solid HD. The tuner itself was overloaded by strong signals
// (some of them outside our 1.5 MHz slice, so the peak can't see them); an overloaded tuner
// smears those signals all over the band, the noise floor comes up, and the HD sidebands -
// which sit only 10-25 dB above the floor to begin with - drown.
//
// WHAT WE MEASURE INSTEAD. An FM HD Radio station sends its digital part in two blocks of
// carriers, 129 to 198 kHz below and above its centre (the "sidebands"). They are flat-topped,
// like a table. We take the spectrum of the burst and compare
//   - the level INSIDE each sideband (135 ... 195 kHz from the centre, a little in from the
//     edges) with
//   - the NOISE FLOOR: the level of the quietest parts of the slice (the 5th percentile of
//     all the spectrum bins within +-650 kHz - there is always a gap between stations
//     somewhere).
// "Sideband over floor" in dB is how far the HD carriers stand out of the noise. It follows
// the decoder's own quality figure (MER) closely: too little gain and the floor is the
// dongle's rounding noise (8 bits), so the figure rises dB for dB with the gain; too much
// and the floor comes up faster than the signal, so it FALLS. The best gain is the top of
// that hill. (Measured on gain-step recordings of KOST, KDAY, KKLQ and KGGI: the gain with
// the best sideband-over-floor was the gain with the best MER, or within 0.3 dB of it, on
// all four - claude/Build_39_autogain_fable_brief.md.)
//
// IS IT HD AT ALL? A station without HD has nothing in those 60 kHz ... unless its neighbour
// 200 kHz away is strong: that neighbour's ANALOG signal lies right across our "sideband".
// Three things tell them apart:
//   - an HD sideband is FLAT: its strongest and weakest spectrum bin are 2-5 dB apart (the
//     scatter of the measurement itself is 2 dB). A neighbour's analog signal is a hill that
//     moves with its audio: 7-35 dB from its strongest to its weakest bin.
//   - an HD sideband is always WEAKER than the station's own analog signal (at most a tenth
//     of its power, by the rules; measured -5 ... -30 dB). A "sideband" with more power than
//     everything within +-100 kHz of the centre belongs to somebody else.
//   - an HD sideband less than 5 dB over the floor doesn't give audio anyway.
// A side that passes all three tests "looks like HD" (Side::hd). If neither side does, the
// auto-gain keeps the peak rule, exactly as before build 39.
//
// Plain C++ (no Android, no other file of the app), so the harness can test it alone.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace gainscore {

const int FFT = 512;                    // spectrum bins: 1,488,375 / 512 = 2.9 kHz each
const int HOP = FFT / 2;                // each spectrum starts half a spectrum after the last (see measure())
const int SAMPLES = 12288;              // IQ samples one measurement uses (x 2 bytes): 8 ms = the auto-gain's burst
                                        //   without its first quarter -> 47 spectra
const double SIDEBAND_LO_HZ = 135000;   // the part of each HD sideband we look at (it spans 129.4 ... 198.4 kHz)
const double SIDEBAND_HI_HZ = 195000;
const double FLOOR_SPAN_HZ = 650000;    // the floor is looked for within +- this (beyond it the dongle's own filter rolls off)
const float FLOOR_PERCENTILE = 0.05f;   // "the quietest 5 %"
const float HD_MIN_DB = 5.0f;           // looks like HD: at least this far over the floor ...
const float HD_MAX_RIPPLE_DB = 6.0f;    // ... and this flat (strongest bin vs weakest bin) ...
const double HOST_SPAN_HZ = 100000;     // ... and weaker than the station's analog signal (all within +- this)

struct Side {
    float overFloorDb = 0.0f;           // the sideband's mean level over the noise floor
    float rippleDb = 99.0f;             // its strongest bin over its weakest
    float overHostDb = 0.0f;            // its power against the station's analog signal (HD: well below 0)
    bool hd = false;                    // looks like an HD sideband (see the top)
};
struct Quality {
    float peakDbfs = -99.0f;            // how much of 0..255 the samples use (0 dBFS = all of it), as measurePeak()
    Side lower, upper;
};

// A small FFT (radix 2, in place). 512 points a few dozen times is a fraction of a millisecond, so
// there is no need for anything faster (or for FFTW, which belongs to the HD decoder).
inline void fft(std::complex<float> *x) {
    static std::vector<std::complex<float>> twiddle;        // e^(-2 pi i k / FFT), built once
    if (twiddle.empty()) {
        twiddle.resize(FFT / 2);
        for (int k = 0; k < FFT / 2; k++)
            twiddle[k] = std::polar(1.0f, (float)(-2.0 * M_PI * k / FFT));
    }
    for (int i = 1, j = 0; i < FFT; i++) {                  // put the samples in bit-reversed order
        int bit = FFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= FFT; len <<= 1) {              // then combine: 2, 4, 8 ... 512
        const int step = FFT / len;
        for (int i = 0; i < FFT; i += len)
            for (int k = 0; k < len / 2; k++) {
                const std::complex<float> a = x[i + k], b = x[i + k + len / 2] * twiddle[k * step];
                x[i + k] = a + b;
                x[i + k + len / 2] = a - b;
            }
    }
}

// [iq] = unsigned 8-bit I, Q, I, Q ... as the dongle sends them; [samples] IQ pairs (uses up
// to SAMPLES of them); [sampleRate] in samples per second.
// Build 39a: the spectra overlap by half. With the Hann window that nearly doubles what a burst
// tells (the window throws away the ends of each 512), so 8 ms measure as steadily as 22 ms did
// without the overlap (checked on the build 39 recordings: same scatter, same flatness figures).
inline Quality measure(const uint8_t *iq, int samples, double sampleRate) {
    Quality q;
    samples = std::min(samples, SAMPLES);
    const int frames = samples >= FFT ? (samples - FFT) / HOP + 1 : 0;
    if (frames <= 0) return q;

    // The peak, the same way measurePeak() works it out.
    uint8_t minS = 255, maxS = 0;
    for (int j = 0; j < samples * 2; j++) {
        if (iq[j] > maxS) maxS = iq[j];
        if (iq[j] < minS) minS = iq[j];
    }
    q.peakDbfs = 20.0f * log10f((maxS - minS + 1) / 256.0f);

    // The averaged power spectrum. power[k]: k = 0 is the centre (the station's carrier),
    // k = 1 ... 255 above it, k = 511 ... 257 below it (bin -1 ... -255).
    static std::vector<float> window;                       // Hann: keeps a strong carrier from leaking into far bins
    if (window.empty()) {
        window.resize(FFT);
        for (int n = 0; n < FFT; n++) window[n] = 0.5f - 0.5f * cosf((float)(2.0 * M_PI * n / FFT));
    }
    std::vector<float> power(FFT, 0.0f);
    std::complex<float> x[FFT];
    for (int f = 0; f < frames; f++) {
        const uint8_t *p = iq + (size_t)f * HOP * 2;
        for (int n = 0; n < FFT; n++)
            x[n] = std::complex<float>((p[2 * n] - 127.5f) * window[n], (p[2 * n + 1] - 127.5f) * window[n]);
        fft(x);
        for (int k = 0; k < FFT; k++) power[k] += std::norm(x[k]);
    }

    // The floor: the 5th percentile of the bins within +-FLOOR_SPAN_HZ.
    const double binHz = sampleRate / FFT;
    const int span = std::min((int)(FLOOR_SPAN_HZ / binHz), FFT / 2 - 1);
    std::vector<float> sorted;
    sorted.reserve((size_t)(2 * span + 1));
    for (int k = -span; k <= span; k++) sorted.push_back(power[(k + FFT) % FFT]);
    const size_t nth = (size_t)(FLOOR_PERCENTILE * (float)(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + (std::ptrdiff_t)nth, sorted.end());
    const float floorP = std::max(sorted[nth], 1e-9f);

    // The station's own analog signal: everything within +-HOST_SPAN_HZ.
    const int hostSpan = (int)(HOST_SPAN_HZ / binHz);
    float hostP = 0.0f;
    for (int k = -hostSpan; k <= hostSpan; k++) hostP += power[(k + FFT) % FFT];

    // The two sidebands.
    const int k1 = (int)std::ceil(SIDEBAND_LO_HZ / binHz), k2 = (int)std::floor(SIDEBAND_HI_HZ / binHz);
    auto side = [&](int sign) {
        Side s;
        float sum = 0.0f, lo = 1e30f, hi = 0.0f;
        for (int k = k1; k <= k2; k++) {
            const float v = power[(sign * k + FFT) % FFT];
            sum += v; lo = std::min(lo, v); hi = std::max(hi, v);
        }
        s.overFloorDb = 10.0f * log10f(std::max(sum / (float)(k2 - k1 + 1), 1e-9f) / floorP);
        s.rippleDb = 10.0f * log10f(std::max(hi, 1e-9f) / std::max(lo, 1e-9f));
        s.overHostDb = 10.0f * log10f(std::max(sum, 1e-9f) / std::max(hostP, 1e-9f));
        s.hd = s.overFloorDb >= HD_MIN_DB && s.rippleDb <= HD_MAX_RIPPLE_DB && s.overHostDb < 0.0f;
        return s;
    };
    q.lower = side(-1);
    q.upper = side(+1);
    return q;
}

} // namespace gainscore
