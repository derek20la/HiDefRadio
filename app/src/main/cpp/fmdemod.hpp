// fmdemod.hpp - analog FM demodulator for HiDef Radio (M11).
//
// Input:  the same cu8 IQ stream that libnrsc5 gets (1,488,375 samples/s, station at 0 Hz).
// Output: analog audio at 44,100 Hz, float, where 1.0 = 75 kHz deviation (100 % modulation).
//         11b: STEREO, interleaved L R L R ... (left = right while the station or the setting is mono).
//
// Signal path (all rates exact, no drift against the HD audio):
//   cu8 -> float I/Q
//   stage 1: low-pass (pass 100 kHz, stop 197.7 kHz) + decimate by 5   -> 297,675 S/s
//   DC blocker (removes the dongle's DC spike, ~10 Hz corner)
//   stage 2: low-pass (pass 100 kHz, stop 125 kHz, 70 dB)               <- rejects the HD sidebands (start at 129.4 kHz for MP1)
//   discriminator: angle(x[n] * conj(x[n-1])) -> instantaneous frequency -> MPX (composite) signal
//   carrier-offset tracker (running mean of the MPX, ~0.5 s) subtracted -> also reported as freqOffsetHz()
//   9k: quieting meter on the MPX (noise in 60-100 kHz, where a station sends nothing) -> quietingDb()
//   11b: pilot PLL (19 kHz) -> is there a pilot (stereo station)? -> 38 kHz from the PLL's phase
//   M = L+R: MPX          low-pass 15 kHz (pilot at 19 kHz rejected 60 dB) + resample 297,675 -> 44,100 (x4/27)
//   D = L-R: MPX x 38 kHz low-pass 15 kHz + the same resampler (only while a pilot is there and stereo is wanted)
//   L = M + s*D, R = M - s*D, where s = the stereo amount (0 = mono ... 1 = full stereo) - the
//   stereo -> mono blend on noisy signals (see StereoBlend below), then de-emphasis 75 us on each channel.
//
// Everything is plain C++17 with no dependencies; filter coefficients are computed at start-up
// (Kaiser-windowed sinc), so nothing has to be tuned by hand when a rate changes.
//
// Build 14 (the NEON step): the signal path is the same, but the work is done a block at a
// time instead of a sample at a time, and the three filters, the byte-to-float conversion
// and the discriminator's arc tangent run four values per instruction (vec4.hpp, NEON on
// arm64, plain C++ elsewhere). Builds 1-13 pushed every one of the 1.5 million samples a
// second through five function calls with a ring buffer each; now a block of ~130,000
// samples goes through stage 1 in one loop, then stage 2 in one loop, and so on, and only
// the parts that are a feedback loop by nature (DC blocker, offset tracker, pilot PLL, RDS,
// the stereo blend) still go sample by sample. The arithmetic order inside each filter was
// kept, so on a machine without NEON the audio is bit for bit what build 13 made; on NEON
// the fused multiply-add and the polynomial atan2 (FM_FAST_ATAN2) change it at the 1e-7
// level - see vec4.hpp and tools/harness/README.md, "The NEON step".
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <complex>
#include "vec4.hpp"                                // build 14: four floats at a time
#include "rds.hpp"                                 // M12: the RDS / RBDS decoder

// The discriminator's arc tangent: 1 = the polynomial in vec4.hpp (four at a time), 0 = the C
// library's atan2f, one sample at a time. Default: the polynomial wherever NEON is, atan2f
// elsewhere, so the desktop harness still reproduces builds 1-13 bit for bit unless told
// otherwise (-DFM_FAST_ATAN2=1). The difference is below 1.5e-7 of full scale (vec4.hpp).
#ifndef FM_FAST_ATAN2
#define FM_FAST_ATAN2 VEC4_NEON
#endif

namespace fm {

static constexpr double IQ_RATE = 1488375.0;        // NRSC5_SAMPLE_RATE_CU8
static constexpr int    DECIM1 = 5;
static constexpr double MPX_RATE = IQ_RATE / DECIM1; // 297,675
static constexpr double AUDIO_RATE = 44100.0;
static constexpr int    RESAMPLE_L = 4, RESAMPLE_M = 27; // 297,675 * 4 / 27 = 44,100 exactly
static constexpr double MAX_DEVIATION_HZ = 75000.0;
static constexpr double PILOT_HZ = 19000.0;          // 11b: the stereo pilot; the L-R subcarrier is at 2 x 19 kHz

// ---- 11b: the stereo setting (setStereoMode) ----
static const int STEREO_AUTO = 0;                // stereo when the station sends it, blended to mono when noisy
static const int STEREO_MONO = 1;                // always mono ("with DX, cut out the noisy stereo") - skips the L-R work
static const int STEREO_FULL = 2;                // stereo whenever there is a pilot, never blended (hiss and all)

// ---- 11b: stereo -> mono blend, the way a car radio's chip does it ----
// The Si4684/Si4689 (the FM/HD chip in most car radios) reduces the stereo
// separation gradually between two signal-quality limits (its defaults: SNR
// 24 dB = full stereo ... 15 dB = full mono), quickly toward mono (16 ms) and
// slowly back to stereo (4 s), so a fade never clicks. Here the "signal
// quality" is the 9k quieting meter: full stereo from QUIET_STEREO_DB up, full
// mono from QUIET_MONO_DB down, a straight line in between (in the stereo
// amount s: L = M + s*D). No pilot = mono, whatever the quieting. Calibrated on
// the 2026-09-28 recordings (see the plan, 11b).
static const float QUIET_STEREO_DB = 36.0f;      // quieting >= this: full stereo (4 bars on the S-meter)
static const float QUIET_MONO_DB   = 24.0f;      // quieting <= this: mono ("weak" on the S-meter starts at 25)
static const float BLEND_ATTACK_S  = 0.05f;      // full stereo -> mono in this long (the meter itself adds ~0.1 s)
static const float BLEND_RELEASE_S = 4.0f;       // mono -> full stereo in this long (Si468x: 4 s)
static const float BLEND_FAST_UNTIL_S = 1.5f;    // ... except in the first moments after a tune: straight to the target
static const float PILOT_ON  = 0.025f;           // pilot amplitude (1.0 = 75 kHz) to call it stereo (nominal 0.08-0.10)
static const float PILOT_OFF = 0.012f;           // ... and to drop it again (hysteresis)

// ---- Kaiser-window low-pass design -------------------------------------------------------
inline double besselI0(double x) {
    double sum = 1, term = 1, k = 1;
    while (term > 1e-12 * sum) { double h = x / (2 * k); term *= h * h; sum += term; k += 1; }
    return sum;
}
// Low-pass FIR: passband edge fPass, stopband edge fStop (Hz) at sample rate fs, stopband attenuation attenDb.
inline std::vector<float> designLowpass(double fs, double fPass, double fStop, double attenDb) {
    double dw = 2 * M_PI * (fStop - fPass) / fs;                // transition width (rad/sample)
    double beta = attenDb > 50 ? 0.1102 * (attenDb - 8.7)
                : attenDb > 21 ? 0.5842 * std::pow(attenDb - 21, 0.4) + 0.07886 * (attenDb - 21) : 0.0;
    int n = (int)std::ceil((attenDb - 8) / (2.285 * dw)) + 1;
    if (n % 2 == 0) n++;                                          // odd length -> integer group delay
    double fc = (fPass + fStop) / 2 / fs;                         // cutoff (cycles/sample), middle of the transition
    std::vector<float> h(n);
    double mid = (n - 1) / 2.0, i0 = besselI0(beta), sum = 0;
    for (int i = 0; i < n; i++) {
        double t = i - mid;
        double sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        double r = 2 * t / (n - 1);
        double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0;
        h[i] = (float)(sinc * w); sum += h[i];
    }
    for (auto &v : h) v = (float)(v / sum);                       // unity DC gain
    return h;
}

// ---- Real-coefficient FIR on a complex (I/Q) stream with integer decimation ----------------
// Works a block at a time (build 14). The caller writes the block's I and Q samples straight
// into this filter's input area (inI() / inQ(), right behind the taps-1 samples of history it
// keeps), calls process(), and reads the outputs. Each output is a dot product of the padded,
// reversed taps with a window of the input - four taps per instruction (vec4.hpp) in the same
// four-lane order the one-sample-at-a-time code of builds 1-13 used, so the sums come out
// the same (bit for bit without a fused multiply-add). With decimation only every m-th
// window is computed - the inputs in between are never multiplied at all.
class ComplexFirDecimator {
public:
    void init(const std::vector<float> &taps, int decim) {
        h = taps; m = decim; taps_ = (int)h.size();
        n = (taps_ + 3) / 4 * 4;                                  // padded length (multiple of 4)
        hrev.assign(n, 0.f);                                      // reversed taps, zero padded at the START
        for (int k = 0; k < taps_; k++) hrev[k + (n - taps_)] = h[taps_ - 1 - k];
        H = n - 1;                                                // samples of history in front of each block
        bufI.assign(H, 0.f); bufQ.assign(H, 0.f); phase = 0;      // starts from silence
    }
    // Room for a block of cnt samples; returns where the caller writes them.
    float *inI(size_t cnt) { reserve(cnt); return bufI.data() + H; }
    float *inQ(size_t cnt) { reserve(cnt); return bufQ.data() + H; }
    // How many outputs a block of cnt inputs will give (so the caller can size its buffers).
    size_t outputs(size_t cnt) const {
        size_t first = (size_t)(m - 1 - phase);                   // index of the first output's input sample
        return cnt <= first ? 0 : (cnt - first + (size_t)m - 1) / (size_t)m;
    }
    // Filters the cnt samples written at inI()/inQ(); writes the outputs; keeps the history.
    size_t process(size_t cnt, float *oi, float *oq) {
        const float *hr = hrev.data(), *bi = bufI.data(), *bq = bufQ.data();
        size_t out = 0;
        // The one-sample code gave an output on the m-th sample pushed (phase counted the
        // samples since the last output); the window ends on that sample, hr[0] multiplies
        // the oldest of the n. In the buffer the block's sample k sits at index H + k, so
        // its window is [k, k + n).
        for (size_t k = (size_t)(m - 1 - phase); k < cnt; k += (size_t)m, out++)
            vec::dot2(hr, bi + k, bq + k, n, oi[out], oq[out]);
        phase = (int)((cnt + (size_t)phase) % (size_t)m);
        // keep the last H samples as the history for the next block
        std::memmove(bufI.data(), bufI.data() + cnt, H * sizeof(float));
        std::memmove(bufQ.data(), bufQ.data() + cnt, H * sizeof(float));
        return out;
    }
    int groupDelay() const { return (taps_ - 1) / 2; }            // in input samples
private:
    void reserve(size_t cnt) {
        if (bufI.size() < H + cnt + 4) { bufI.resize(H + cnt + 4, 0.f); bufQ.resize(H + cnt + 4, 0.f); }
    }
    std::vector<float> h, hrev, bufI, bufQ; int n = 0, taps_ = 0, m = 1, phase = 0; size_t H = 0;
};

// ---- Rational resampler (L up, M down) with a built-in low-pass, real signal ----------------
// 11b: TWO channels through the same filter, in lock-step (M = L+R and D = L-R),
// so both audio channels come out of the same output sample times. The second
// channel can be switched off (mono: its FIR isn't computed at all).
class Resampler {
public:
    // Filter designed at the virtual rate fs*L; cutoff/stop are in Hz of the real signal.
    void init(double fsIn, int L_, int M_, double fPass, double fStop, double attenDb) {
        L = L_; M = M_;
        std::vector<float> proto = designLowpass(fsIn * L, fPass, fStop, attenDb);
        K = ((int)proto.size() + L - 1) / L;                      // taps per phase
        K = (K + 3) / 4 * 4;                                      // padded to a multiple of 4 (zeros)
        poly.assign((size_t)L * K, 0.f);
        // phase p, tap k multiplies x[n-k]; stored reversed (index K-1-k) so the window can be read forwards
        for (int i = 0; i < (int)proto.size(); i++) poly[(i % L) * K + (K - 1 - i / L)] = proto[i] * L; // gain L for zero stuffing
        hist.assign(2 * K, 0.f); hist2.assign(2 * K, 0.f); pos = 0; count = 0; second = false;
        // Start on the filter's centre so output sample m sits at input time m*M/L exactly
        // (the filter's group delay is absorbed; the first outputs see zeros = silence).
        nextT = ((long long)proto.size() - 1) / 2;
        phase = (int)(nextT % L); countL = 0;
        delaySamplesIn = 0;
    }
    // 11b: switch the second channel on (its history starts from silence) or off.
    void enableSecond(bool on) {
        if (on && !second) std::fill(hist2.begin(), hist2.end(), 0.f);
        second = on;
    }
    bool secondEnabled() const { return second; }
    // Pushes one input sample of each channel; emit(a, b) is called for every output sample
    // (b = 0 while the second channel is off).
    template <class Out> inline void push(float x, float y, Out &&emit) {
        hist[pos] = hist[pos + K] = x;
        if (second) hist2[pos] = hist2[pos + K] = y;
        int start = pos + 1; pos = (pos + 1 == K) ? 0 : pos + 1; count++; countL += L;
        // newest sample has index count-1; window = samples count-K .. count-1 (oldest first) = hist[start..start+K).
        // An output is due while nextT / L <= count - 1, i.e. nextT < L * count (countL). The phase
        // p = nextT % L is carried along instead of divided out (build 14: the two 64-bit
        // divisions here were a measurable part of the demodulator's cost, 300,000 times a second).
        while (nextT < countL) {
            const float *hp = &poly[(size_t)phase * K];             // hp is stored reversed: hp[j] multiplies x[n-(K-1-j)]
            float a, b = 0;
            if (second) vec::dot2(hp, &hist[start], &hist2[start], K, a, b);   // build 14: four taps per instruction
            else a = vec::dot(hp, &hist[start], K);
            emit(a, b); nextT += M;
            phase += M % L; if (phase >= L) phase -= L;            // = nextT % L
        }
    }
    double delaySamplesIn = 0;
private:
    std::vector<float> poly, hist, hist2; int L = 1, M = 1, K = 0, pos = 0, phase = 0; long long count = 0, countL = 0, nextT = 0;
    bool second = false;
};

// ---- FM quieting meter (9k) -----------------------------------------------------------------
// "Is there a station, and how clean is it?" - judged the way an FM receiver's noise squelch does:
// by the NOISE in the part of the MPX (discriminator output) where a station sends nothing.
// A station "quiets" the discriminator; with no station (or a weak one) the noise comes up.
// Unlike the carrier level, this does not depend on the tuner gain or on other stations in the band.
//
// Measured in 60-100 kHz (above RDS at 57 kHz, below the demod's 100 kHz channel edge), split into
// eight 5 kHz bands. SCA subcarriers (67 / 92 kHz) can fill some bands, so the QUIETEST band counts.
// Result: dB of noise relative to 75 kHz deviation, scaled to a 25 kHz band (the 70-95 kHz reference
// of the 2026-09-28 prototype). Measured on recordings: pure noise -8 dB, Z90 fading out -11,
// weak KGGI -29..-31, KZNO/KDAY -38..-39, KKLQ/KKLA -43..-46, KLOS -50 (lower = quieter = better).
// One 1024-point FFT on every 4th block of 1024 MPX samples, averaged over ~1 s (~3 Mflop/s).
// 11b: also a FAST reading (~0.1 s average) for the stereo blend, which must react quickly.
class QuietMeter {
public:
    static const int N = 1024;              // FFT size: 290.7 Hz bins at 297,675 S/s
    static const int EVERY = 4;             // analyse 1 block in 4 (plenty for a 1 s average)
    static const int BANDS = 8;             // 60-65, 65-70, ... 95-100 kHz
    void reset() {
        if (win.empty()) init();
        fill = 0; blockNo = 0; ffts = 0; skip = (long)(0.2 * MPX_RATE);   // skip the filters' start-up
        std::fill(std::begin(sum), std::end(sum), 0.0);
        std::fill(std::begin(fast), std::end(fast), 0.0);
        fastCount = 0;
        result = -99.f; fastResult = -99.f;
    }
    inline void push(float x) {
        if (skip > 0) { skip--; return; }
        if (blockNo != 0) {                              // a block we don't analyse: just count it
            if (++fill == N) { fill = 0; blockNo = (blockNo + 1) % EVERY; }
            return;
        }
        buf[fill] = std::complex<float>(x * win[fill], 0.f);
        if (++fill < N) return;
        fill = 0; blockNo = 1 % EVERY;
        fft();
        double fastBest = 1e30;
        for (int b = 0; b < BANDS; b++) {
            double p = 0;
            for (int k = kLo[b]; k < kHi[b]; k++) p += std::norm(buf[k]);
            sum[b] += p;
            // 11b: the fast reading - each band's density smoothed over FAST_FFTS (~0.11 s)
            double d = 2.0 * p / ((double)N * winPow) / ((kHi[b] - kLo[b]) * binHz);
            if (fastCount == 0) fast[b] = d; else fast[b] += (d - fast[b]) * (1.0 / FAST_FFTS);
            fastBest = std::min(fastBest, fast[b]);
        }
        if (fastCount < FAST_FFTS) fastCount++;
        double f25 = fastBest * 25000.0;
        fastResult = f25 > 1e-12 ? (float)(10.0 * std::log10(f25)) : -99.f;
        if (++ffts >= FFTS_PER_RESULT) {
            // density (per Hz) of each band, one-sided: 2 * sum|X|^2 / (N * sum w^2) / bandwidth
            double best = 1e30;
            for (int b = 0; b < BANDS; b++) {
                double d = 2.0 * sum[b] / ((double)N * winPow) / ((double)ffts * (kHi[b] - kLo[b]) * binHz);
                best = std::min(best, d);
                sum[b] = 0;
            }
            ffts = 0;
            double p25 = best * 25000.0;                 // as if measured over 25 kHz (70-95 kHz)
            result = p25 > 1e-12 ? (float)(10.0 * std::log10(p25)) : -99.f;
        }
    }
    float db() const { return result; }                  // -99 = not measured yet (1 s average, for the S-meter)
    float fastDb() const { return fastResult; }          // 11b: the same over ~0.1 s (for the stereo blend)
private:
    static const int FFTS_PER_RESULT = (int)(MPX_RATE / (N * EVERY));   // 72 -> one result per ~0.99 s
    static const int FAST_FFTS = 8;                                     // 8 x 13.8 ms = 0.11 s
    void init() {
        win.resize(N); winPow = 0;
        for (int i = 0; i < N; i++) { win[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); winPow += (double)win[i] * win[i]; }
        tw.resize(N / 2);
        for (int i = 0; i < N / 2; i++) tw[i] = std::polar(1.f, (float)(-2 * M_PI * i / N));
        rev.resize(N);
        int bits = 0; while ((1 << bits) < N) bits++;
        for (int i = 0; i < N; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[i] = r; }
        binHz = MPX_RATE / N;
        for (int b = 0; b < BANDS; b++) {
            kLo[b] = (int)std::ceil((60000.0 + 5000.0 * b) / binHz);
            kHi[b] = (int)std::ceil((65000.0 + 5000.0 * b) / binHz);
        }
    }
    void fft() {                                         // in-place radix-2, decimation in time
        for (int i = 0; i < N; i++) if (i < rev[i]) std::swap(buf[i], buf[rev[i]]);
        for (int len = 2; len <= N; len <<= 1) {
            int half = len / 2, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; j++) {
                    std::complex<float> t = buf[i + j + half] * tw[j * step];
                    buf[i + j + half] = buf[i + j] - t;
                    buf[i + j] += t;
                }
        }
    }
    std::vector<float> win; std::vector<std::complex<float>> tw; std::vector<int> rev;
    std::complex<float> buf[N];
    double sum[BANDS] = {}, fast[BANDS] = {}, winPow = 0, binHz = 0;
    int kLo[BANDS] = {}, kHi[BANDS] = {}, fill = 0, blockNo = 0, ffts = 0, fastCount = 0;
    long skip = 0;
    float result = -99.f, fastResult = -99.f;
};

// ---- 11b: pilot PLL ------------------------------------------------------------------------
// Locks a 19 kHz oscillator to the station's stereo pilot and hands out sin(2*phase),
// the 38 kHz subcarrier the L-R signal rides on - "coherent" demodulation, as SDR#
// does it (and unlike a free-running 38 kHz, which would let the separation wander).
//
// The MPX first goes through a narrow band-pass at 19 kHz (a biquad, Q 20: 950 Hz wide),
// so the loop sees mostly the pilot. Phase detector: bandpassed x (-sin phase) -> ~(A/2) sin(error).
// Loop filter: proportional + integral, natural frequency 25 Hz, damping 0.7 -> locks in
// ~40 ms; the error is normalised by the pilot amplitude, so a weak pilot locks as fast.
// The oscillator is a 32-bit phase accumulator (one turn = 2^32) and a 4,096-entry sine
// table - no sin() calls per sample. ~15 operations per MPX sample.
//
// Pilot amplitude (in-phase, 1.0 = 75 kHz deviation; a station sends 0.08-0.10) and the
// quadrature (~0 when locked) are averaged over 20 ms; "locked" has a hysteresis
// (PILOT_ON / PILOT_OFF), so a pilot on the edge doesn't make the stereo flicker.
class PilotPll {
public:
    static const int TABLE = 4096;
    void reset() {
        if (sinTab.empty()) init();
        phase = 0; freqOff = 0; ip = qp = 0; locked_ = false; s2 = 0;
        bx1 = bx2 = by1 = by2 = 0;
    }
    inline void push(float mpx) {
        // band-pass at 19 kHz (direct form II transposed would save a state; this is clear enough)
        float y = bB0 * (mpx - bx2) - bA1 * by1 - bA2 * by2;
        bx2 = bx1; bx1 = mpx; by2 = by1; by1 = y;
        // oscillator: sin/cos of the phase, and sin(2*phase) for the 38 kHz subcarrier
        uint32_t idx = phase >> SHIFT;
        float s = sinTab[idx], c = sinTab[(idx + TABLE / 4) & (TABLE - 1)];
        s2 = sinTab[(phase >> (SHIFT - 1)) & (TABLE - 1)];
        // phase detector, normalised by the pilot amplitude (>= 0.02 so noise can't blow it up)
        float e = -y * s;                                       // ~ (A/2) sin(pilot phase - our phase)
        ip += ipAlpha * (2.f * y * c - ip);                     // in-phase: ~A when locked
        qp += ipAlpha * (2.f * e - qp);                         // quadrature: ~0 when locked
        float en = e / std::max(ip, 0.02f);
        freqOff += ki * en;
        if (freqOff > maxOff) freqOff = maxOff; else if (freqOff < -maxOff) freqOff = -maxOff;
        phase += nominal + (uint32_t)(int32_t)((freqOff + kp * en) * 4294967296.0f);
        // lock with hysteresis: enough pilot, and mostly in phase
        if (locked_) { if (ip < PILOT_OFF) locked_ = false; }
        else if (ip > PILOT_ON && std::fabs(qp) < ip) locked_ = true;
    }
    inline float sin2() const { return s2; }                    // sin(2 x phase); the 38 kHz subcarrier is MINUS this (see Demod)
    bool locked() const { return locked_; }
    float pilotLevel() const { return ip; }                     // 1.0 = 75 kHz deviation
    float freqErrorHz() const { return freqOff * (float)MPX_RATE; }
private:
    static const int SHIFT = 32 - 12;                            // 2^32 / 4096
    void init() {
        sinTab.resize(TABLE);
        for (int i = 0; i < TABLE; i++) sinTab[i] = (float)std::sin(2 * M_PI * i / TABLE);
        nominal = (uint32_t)std::llround(PILOT_HZ / MPX_RATE * 4294967296.0);
        // band-pass biquad (RBJ cookbook): centre 19 kHz, Q 20, unity gain at the centre
        double w0 = 2 * M_PI * PILOT_HZ / MPX_RATE, q = 20.0, alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        bB0 = (float)(alpha / a0); bA1 = (float)(-2 * std::cos(w0) / a0); bA2 = (float)((1 - alpha) / a0);
        // loop: detector gain 0.5 (normalised), natural frequency 25 Hz, damping 0.707, in cycles/sample
        double wn = 2 * M_PI * 25.0, zeta = 0.707, kd = 0.5;
        kp = (float)(2 * zeta * wn / (kd * MPX_RATE) / (2 * M_PI));
        ki = (float)(wn * wn / (kd * MPX_RATE * MPX_RATE) / (2 * M_PI));
        maxOff = (float)(60.0 / MPX_RATE);                       // the pilot is 19 kHz +- 2 Hz; the dongle adds a few ppm
        ipAlpha = (float)(1 - std::exp(-1.0 / (0.02 * MPX_RATE)));
    }
    std::vector<float> sinTab;
    uint32_t phase = 0, nominal = 0;
    float freqOff = 0, ip = 0, qp = 0, s2 = 0, kp = 0, ki = 0, maxOff = 0, ipAlpha = 0;
    float bB0 = 0, bA1 = 0, bA2 = 0, bx1 = 0, bx2 = 0, by1 = 0, by2 = 0;
    bool locked_ = false;
};

// ---- The demodulator ------------------------------------------------------------------------
class Demod {
public:
    Demod() { reset(); }
    void reset() {
        stage1.init(designLowpass(IQ_RATE, 100000, MPX_RATE - 100000, 60), DECIM1);
        stage2.init(designLowpass(MPX_RATE, 100000, 125000, 70), 1);
        audio.init(MPX_RATE, RESAMPLE_L, RESAMPLE_M, 15000, 19000, 60);
        quiet.reset();                                                     // 9k
        pll.reset();                                                       // 11b
        rdsDec.reset();                                                    // M12
        dcI = dcQ = 0; prevI = 1; prevQ = 0; offset = 0; level = 0; deemphL = deemphR = 0; audioOut.clear();
        dcAlpha  = (float)(1 - std::exp(-2 * M_PI * 10.0 / MPX_RATE));    // starts at a 10 Hz corner...
        dcAlphaSlow = (float)(1 - std::exp(-2 * M_PI * 0.1 / MPX_RATE)); // ...settles to 0.1 Hz (a 10 Hz notch
        dcDecay  = (float)std::exp(-std::log(100.0) / (1.0 * MPX_RATE));  // in the FM signal costs ~5 dB SNR)
        offAlpha = (float)(1 - std::exp(-1.0 / (0.5 * MPX_RATE)));        // 0.5 s
        levAlpha = (float)(1 - std::exp(-1.0 / (0.1 * MPX_RATE)));        // 0.1 s
        deemphA  = (float)(1 - std::exp(-1.0 / (deemphUs * 1e-6 * AUDIO_RATE)));  // 75 us (11b step 2: or 50)
        discScale = (float)(MPX_RATE / (2 * M_PI * MAX_DEVIATION_HZ));    // rad/sample -> fraction of 75 kHz
        // 11b: the blend
        stereo = 0; stereoTarget = 0; framesOut = 0;
        attackStep  = (float)(1.0 / (BLEND_ATTACK_S * AUDIO_RATE));
        releaseStep = (float)(1.0 / (BLEND_RELEASE_S * AUDIO_RATE));
        fastUntil = (long long)(BLEND_FAST_UNTIL_S * AUDIO_RATE);
    }
    // 11b: STEREO_AUTO (default), STEREO_MONO or STEREO_FULL. Called between blocks (streaming thread).
    void setStereoMode(int m) { mode = m == STEREO_MONO ? STEREO_MONO : m == STEREO_FULL ? STEREO_FULL : STEREO_AUTO; }
    int stereoMode() const { return mode; }
    // 11b step 2: de-emphasis time constant in microseconds (75 Americas, 50 elsewhere).
    // Cheap to call every block: only recomputes when the value changes.
    void setDeemphasis(int us) {
        if (us != 50) us = 75;
        if (us == deemphUs) return;
        deemphUs = us;
        deemphA = (float)(1 - std::exp(-1.0 / (deemphUs * 1e-6 * AUDIO_RATE)));
    }

    // Feed cu8 bytes (I,Q,I,Q,...). Audio appears in audioOut (append-only; caller clears).
    // Build 14: a block at a time - each stage runs over the whole block before the next one
    // starts (the stages that are feedback loops still go sample by sample, inside their loop).
    void processCu8(const uint8_t *buf, size_t len) {
        const size_t n = len / 2;                                   // IQ samples in this block
        if (n == 0) return;
        // 1. bytes -> floats, straight into stage 1's input area (behind its history)
        vec::cu8ToFloat(buf, stage1.inI(n), stage1.inQ(n), n);
        // 2. stage 1: low-pass + decimate by 5, into stage 2's input area
        const size_t m1 = stage1.outputs(n);
        float *s2i = stage2.inI(m1), *s2q = stage2.inQ(m1);
        const size_t m = stage1.process(n, s2i, s2q);
        // 3. DC blocker on the decimated signal (a feedback loop: sample by sample)
        for (size_t j = 0; j < m; j++) {
            dcI += dcAlpha * (s2i[j] - dcI); dcQ += dcAlpha * (s2q[j] - dcQ);
            if (dcAlpha > dcAlphaSlow) dcAlpha *= dcDecay;              // fast start, slow tracking
            s2i[j] -= dcI; s2q[j] -= dcQ;
        }
        // 4. stage 2: the channel filter (rejects the HD sidebands). Its outputs go behind one
        //    slot of history (the last sample of the previous block), and the arrays have a few
        //    spare slots at the end, so the discriminator can run over whole groups of four.
        if (chI.size() < m + 1 + 8) { chI.resize(m + 1 + 8, 0.f); chQ.resize(m + 1 + 8, 0.f); mag.resize(m + 8); mpxBuf.resize(m + 8); }
        chI[0] = prevI; chQ[0] = prevQ;
        stage2.process(m, chI.data() + 1, chQ.data() + 1);
        prevI = chI[m]; prevQ = chQ[m];
        // 5. discriminator: angle of x[n] * conj(x[n-1]), four samples at a time
        //    (the spare slots beyond m hold whatever they hold - those results are not used)
        {
#if FM_FAST_ATAN2
            const vec::f4 scale = vec::set1(discScale);
#endif
            for (size_t j = 0; j < m; j += 4) {
                vec::f4 ci = vec::load(&chI[j + 1]), cq = vec::load(&chQ[j + 1]);
                vec::f4 pI = vec::load(&chI[j]),     pQ = vec::load(&chQ[j]);
                vec::f4 re = vec::fma(vec::mul(ci, pI), cq, pQ);              // ci*pI + cq*pQ
                vec::f4 im = vec::fms(vec::mul(cq, pI), ci, pQ);              // cq*pI - ci*pQ
                vec::store(&mag[j], vec::sqrt(vec::fma(vec::mul(re, re), im, im)));
#if FM_FAST_ATAN2
                vec::store(&mpxBuf[j], vec::mul(vec::atan2(im, re), scale)); // 1.0 = 75 kHz
#else
                for (int i = 0; i < 4; i++) mpxBuf[j + i] = std::atan2(vec::lane(im, i), vec::lane(re, i)) * discScale;
#endif
            }
        }
        // 6. the rest, sample by sample (feedback loops and the audio path)
        for (size_t j = 0; j < m; j++) {
            float mpx = mpxBuf[j];
            level += levAlpha * (mag[j] - level);                    // |x| smoothed -> carrier level
            offset += offAlpha * (mpx - offset);                     // carrier offset (fraction of 75 kHz)
            mpx -= offset;
            quiet.push(mpx);                                         // 9k: noise in 60-100 kHz
            pll.push(mpx);                                           // 11b: always tracks the pilot (cheap)
            rdsDec.push(mpx);                                        // M12: RDS (57 kHz subcarrier)
            // 11b: the L-R path runs while the pilot is there, stereo is wanted, and the blend
            // hasn't reached mono (then it's switched off - nothing to compute in mono).
            bool wantD = mode != STEREO_MONO && pll.locked();
            if (wantD != audio.secondEnabled()) {
                if (wantD) audio.enableSecond(true);
                else if (stereo <= 0.f) audio.enableSecond(false);   // keep it until the fade to mono is done
            }
            // MPX x (2 x the 38 kHz subcarrier), low-passed, = (L-R)/2 - the same scale as M = (L+R)/2.
            //
            // WHICH SIGN? It decides which channel is the left one, so it is worth the long note.
            // The standard (ITU-R BS.450 "pilot-tone system"; 47 CFR 73.322 is the same system):
            // S = (left - right) / 2, and with only S positive the 38 kHz signal crosses zero
            // going UP at every zero crossing of the pilot. In a formula:
            //     MPX = M + S * sin(2wt) + pilot * sin(wt)          (w = 2 pi 19 kHz)
            // Our PLL locks with the pilot as cos(phase) = sin(phase + 90 deg). So wt = phase + 90 deg
            // and the subcarrier sin(2wt) = sin(2 phase + 180 deg) = -sin(2 phase).  ->  MINUS.
            //
            // History: 11b (2026-09-29) used PLUS, because with minus the analog came out
            // left/right swapped against the HD1 audio from nrsc5 (KRTH, KLOS, KGGI). On 2026-10-02
            // that was turned around by three checks that don't involve nrsc5:
            //   1. a left-only test signal built from the formula above came out on the RIGHT;
            //   2. ngsoftfm (the SoftFM family) puts that signal on the left, and on a real KLOS
            //      recording its left was our right (L-R correlation -0.996);
            //   3. Gqrx / GNU Radio uses the same sign as ngsoftfm.
            // So the analog was wrong here, and it is nrsc5's HD audio that arrives with the
            // channels swapped - that is now corrected where the HD audio comes in
            // (fixHdAudio() in native-lib.cpp), and the analog follows the standard.
            // Lesson: a sign convention needs a reference that is known to be right, not just
            // "the other decoder in the same program".
            float d = audio.secondEnabled() ? mpx * (-2.f * pll.sin2()) : 0.f;
            audio.push(mpx, d, [this](float m, float dd) {
                // the stereo amount s: toward the target quickly (mono) or slowly (stereo)
                if (stereoTarget < stereo) stereo = std::max(stereoTarget, stereo - attackStep);
                else if (stereoTarget > stereo) stereo = framesOut < fastUntil ? stereoTarget : std::min(stereoTarget, stereo + releaseStep);
                float l = m + stereo * dd, r = m - stereo * dd;
                deemphL += deemphA * (l - deemphL);
                deemphR += deemphA * (r - deemphR);
                audioOut.push_back(deemphL);
                audioOut.push_back(deemphR);
                if ((++framesOut & 63) == 0) updateTarget();          // every 1.5 ms is plenty
            });
        }
    }
    std::vector<float> audioOut;                 // 11b: STEREO interleaved (L, R), 44,100 Hz, 1.0 = 75 kHz deviation
    float freqOffsetHz() const { return offset * (float)MAX_DEVIATION_HZ; }
    float carrierDbfs() const { return level > 0 ? 10.f * std::log10(level) : -99.f; } // |x|^2 of the filtered channel, 0 dB = full-scale
    // 9k: FM quieting - noise where the station sends nothing, dB re 75 kHz deviation in 25 kHz.
    // Lower = better: ~-8 pure noise, ~-30 weak but listenable, ~-50 a strong local. -99 = not yet.
    float quietingDb() const { return quiet.db(); }
    float quietingDbFast() const { return quiet.fastDb(); }             // 11b: ~0.1 s average (drives the blend)
    // 11b: the stereo state for the UI.
    bool pilot() const { return pll.locked(); }                          // a stereo pilot is there
    float pilotLevel() const { return pll.pilotLevel(); }                 // its amplitude, 1.0 = 75 kHz (nominal 0.08-0.10)
    float stereoAmount() const { return stereo; }                         // 0 = mono ... 1 = full stereo (the blend)
    // M12: the RDS decoder (PI / call letters, PS, RadioText ...) - see rds.hpp.
    const rds::Decoder &rds() const { return rdsDec; }
    // Total delay of the analog path in seconds (filters only; not counting output buffering).
    double latencySeconds() const {
        return (stage1.groupDelay() / IQ_RATE) + (stage2.groupDelay() + 0.5) / MPX_RATE + audio.delaySamplesIn / MPX_RATE;
    }
private:
    // 11b: where should the blend go? Full stereo above QUIET_STEREO_DB of quieting, mono
    // below QUIET_MONO_DB, a straight line in between; mono without a pilot or in the mono setting.
    void updateTarget() {
        if (mode == STEREO_MONO || !pll.locked()) { stereoTarget = 0; return; }
        if (mode == STEREO_FULL) { stereoTarget = 1; return; }
        float q = -quiet.fastDb();                                  // positive: higher = cleaner
        if (q > 98.f) { stereoTarget = 0; return; }                 // not measured yet
        stereoTarget = std::max(0.f, std::min(1.f, (q - QUIET_MONO_DB) / (QUIET_STEREO_DB - QUIET_MONO_DB)));
    }
    ComplexFirDecimator stage1, stage2; Resampler audio;
    std::vector<float> chI, chQ, mag, mpxBuf;                             // build 14: one block's channel output, |x|, MPX
    QuietMeter quiet;                                                     // 9k
    PilotPll pll;                                                         // 11b
    rds::Decoder rdsDec{MPX_RATE};                                        // M12
    int mode = STEREO_AUTO;
    float stereo = 0, stereoTarget = 0, attackStep = 0, releaseStep = 0;
    long long framesOut = 0, fastUntil = 0;
    float dcI, dcQ, prevI, prevQ, offset, level, deemphL, deemphR;
    float dcAlpha, dcAlphaSlow, dcDecay, offAlpha, levAlpha, deemphA, discScale;
    int deemphUs = 75;                                                 // 11b step 2
};

} // namespace fm
