// amdemod.hpp - the analog AM receiver of HiDef Radio: IQ samples in, audio out (mono, or
// C-QUAM stereo).
//
// WHAT IT DOES
// The RTL-SDR dongle hands us 1,488,375 complex samples a second ("IQ"), centred on the AM
// station we are tuned to. The same stream goes to libnrsc5, which decodes the station's HD
// Radio (digital) part if it has one. This file decodes the analog AM - the part every radio
// since the 1920s can hear - so the app can play it at once, blend to the HD when that is
// there and good (blend.hpp, exactly as on FM), and give every AM station a sound, not only
// the two or three with HD. It also decodes C-QUAM AM stereo (Motorola, 1980s), which a few
// dozen US stations still send.
//
//   Input:  cu8 IQ (bytes I, Q, I, Q ... - 128 = zero), 1,488,375 samples/s, station near 0 Hz.
//   Output: audio at 44,100 Hz, float, STEREO interleaved L R L R ...; 1.0 = 100 % modulation
//           (the carrier fully modulated). Left = right while the station is mono (nearly all
//           of them) or stereo is not wanted. In C-QUAM stereo the two channels are
//           L = (L+R) + (L-R) and R = (L+R) - (L-R) - twice each side's own share - so that a
//           centre image plays at the same level in stereo as it did in mono a moment before
//           (no 6 dB jump when the pilot is found); a hard-panned sound at full modulation
//           then reaches 2.0, which is why the app scales the output by 0.5.
//
// THE SIGNAL PATH (every rate is an exact ratio of the input rate - see fmdemod.hpp for why
// that matters, and drift.hpp for the station's clock)
//
//   bytes -> floats (I, Q)
//   stage 1: low-pass (pass 12 kHz, stop 153 kHz) + keep every 9th sample   -> 165,375 S/s
//   stage 2: low-pass (pass 12 kHz, stop 43 kHz)  + keep every 3rd sample   ->  55,125 S/s
//       An AM channel is 10 kHz wide in the Americas (9 kHz elsewhere) and a station's audio
//       may reach 10 kHz, so +-12 kHz around the carrier is kept flat; everything beyond is
//       filtered before it is thrown away, or the other stations in the band would fold back
//       onto ours (aliasing). 55,125 is exactly 44,100 x 5/4, so the audio resampler at the
//       end is a plain 4/5. (nrsc5 decimates AM by 32 to 46,512 S/s; a different ratio is fine
//       - the two decoders only share the input stream.)
//   DC blocker: the dongle adds a spike at exactly 0 Hz (its own leakage; large on an RTL-SDR
//       Blog V3 in direct sampling). A slow running average of I and Q is subtracted. The
//       station's carrier is never at exactly 0 Hz (the dongle's crystal and the station's own
//       error put it a few Hz off), so it survives; see "carrier" below for the one case that
//       would not.
//   carrier acquisition: a 8,192-point FFT of the first 0.15 s (and again whenever the lock is
//       lost for 2 s) finds the carrier within +-1.5 kHz of the centre to ~1 Hz. The PLL below
//       could not pull in from 100 Hz away with a loop this slow; with the FFT's figure it locks
//       in a fraction of a second.
//   carrier PLL: an oscillator of our own, locked to the station's carrier (phase AND
//       frequency). What it gives is the complex baseband z = I' + jQ' with the carrier turned
//       into a positive real number: I' = the carrier and whatever modulates its amplitude,
//       Q' = whatever is in QUADRATURE (90 degrees off) to it. The loop is slow (natural
//       frequency 5 Hz): it must follow the dongle's and the station's drift, and must NOT
//       follow the phase modulation C-QUAM puts on the carrier at audio rates, nor its 25 Hz
//       pilot.
//   synchronous detector: audio = (I' - A) / A, where A = the carrier's amplitude, a slow
//       (0.5 s) average of I'. That is the station's modulation, 1.0 = 100 %, and dividing by A
//       is the AGC every AM radio has: the loudness does not change with the signal strength.
//       WHY SYNCHRONOUS AND NOT A SIMPLE ENVELOPE DETECTOR (|z|, what a diode does): a hybrid
//       HD station (mode MA1) puts its lowest digital carriers UNDER the analog audio, within
//       +-5 kHz of the carrier, in QUADRATURE to it - so that an in-phase detector rejects
//       them. An envelope detector hears them as hiss; that is also why HD-ready AM radios are
//       synchronous. (Measured on KMZT 1260: the quadrature holds 7 % of the carrier's
//       amplitude, mostly below 1 kHz - small on a strong signal, a hiss on a weak one.)
//       A synchronous detector also stays clean through selective fading (night skywave)
//       where an envelope detector distorts, and a carrier of the adjacent station at 10 kHz
//       becomes a 10 kHz tone that the audio low-pass removes, not a mess of beats.
//   signal meter (Meter): the carrier's power against the noise in the quietest 1 kHz of the
//       channel's own band between 5.5 and 9.5 kHz either side of the carrier, as a
//       carrier-to-noise ratio in 5 kHz (snrDb()). On an HD station that band holds the
//       (weak) secondary digital sidebands, so the figure reads a little low there - it is an
//       S-meter, not a lab instrument. It drives the screen's bars and the stereo -> mono blend.
//   C-QUAM stereo (see "C-QUAM" below): L+R from the envelope |z|, L-R from Q' - only when a
//       25 Hz pilot is found in L-R and the HD decoder is NOT in sync (hybrid HD uses the
//       quadrature for its own carriers; the two cannot coexist, and a C-QUAM station has no HD).
//   audio low-pass + resampler: 55,125 -> 44,100 S/s (x4 / 5, exactly), with the audio
//       bandwidth built into the resampler's filter: 5 kHz by default (what an AM station in
//       hybrid HD mode sends, and what most radios pass; the HD's secondary sidebands start
//       at 5 kHz), settable (setBandwidth) up to 10 kHz for a non-HD station with wide audio.
//       Two channels (L+R and L-R) go through the same filter in lock-step, as on FM.
//   de-emphasis (setDeemphasis, build 21): the receiver half of NRSC-1. US AM stations boost
//       their treble before transmission ("modified 75 us": +6.9 dB at 5 kHz, +10 dB at
//       10 kHz) and the standard asks receivers to take it out again; without it AM sounds
//       bright and hissy, and unlike the HD audio, which is not pre-emphasized. See "NRSC-1".
//
// C-QUAM (Motorola's "compatible quadrature AM", the AM stereo system the FCC chose in 1993):
//   The station sends (1 + L + R) x cos(wt + phi) with phi = atan((L - R) / (1 + L + R)), plus a
//   25 Hz pilot tone in L - R (about 4 % modulation) that says "this is stereo". The envelope
//   is still 1 + L + R, so an ordinary radio hears mono exactly as before - that is the
//   "compatible". With our carrier-locked z:  |z| = A (1 + L + R),  Q'/I' = tan(phi), so
//       L + R = |z| / A - 1          L - R = |z| x Q' / I' / A
//   - a few lines of arithmetic. The judgement calls are elsewhere: the carrier PLL must be
//   slow enough not to follow phi (the FCC's own receiver notes say the same); the division by
//   I' blows up when the carrier is modulated to a deep trough, so I' is held at >= 20 % of A
//   (what Motorola's decoder chips did too); L - R is blended toward mono as the signal gets
//   noisy, as FM stereo is (fmdemod.hpp), since the quadrature carries most of the noise; the
//   pilot is taken out of the audio by a notch. The pilot is "there" when the 25 Hz band of
//   L - R holds several times the power of the 40 Hz band next to it, for a second (a noise
//   floor is flat, music in L - R is nearly flat between 25 and 40 Hz; the pilot is a line).
//   WHICH SIDE IS LEFT? L = M + S by the standard's definition (S = L - R). It has NOT been
//   checked against a real C-QUAM station - none is on the air within reach of Los Angeles
//   (the FM lesson of 2026-10-02: only a real station proves which channel is which). It IS
//   checked against a synthetic C-QUAM signal built from the formula above (tools/harness).
//
// Everything is plain C++17 with no dependencies (no Android): it reuses the filter design,
// the block FIR decimator and the polyphase resampler of fmdemod.hpp, and can be lifted into
// any program that has RTL-SDR samples.
//
// SPEED: about a quarter of the FM demodulator's work. The two decimating filters run on the
// whole block through vec4.hpp (NEON on arm64); the PLL, detector and C-QUAM arithmetic go
// sample by sample at 55,125 S/s (feedback loops), the resampler's filters are vectorised.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <complex>
#include "fmdemod.hpp"                              // designLowpass, ComplexFirDecimator, Resampler, vec4

namespace am {

using fm::designLowpass; using fm::ComplexFirDecimator; using fm::Resampler;   // the FM file's building blocks, reused

static constexpr double IQ_RATE = 1488375.0;        // NRSC5_SAMPLE_RATE_CU8: one stream feeds both decoders
static constexpr int    DECIM1 = 9, DECIM2 = 3;
static constexpr double RATE1 = IQ_RATE / DECIM1;    // 165,375 S/s after stage 1
static constexpr double CH_RATE = RATE1 / DECIM2;    // 55,125 S/s = 44,100 x 5/4: the "channel" rate
static constexpr double AUDIO_RATE = 44100.0;
static constexpr int    RESAMPLE_L = 4, RESAMPLE_M = 5; // 55,125 x 4 / 5 = 44,100 exactly
static constexpr double CH_PASS_HZ = 12000.0;        // kept flat around the carrier (an AM channel is 10 kHz)
static const int DEFAULT_BANDWIDTH_HZ = 5000;        // audio low-pass (setBandwidth): 5 kHz = hybrid HD's analog
static const int MIN_BANDWIDTH_HZ = 2000, MAX_BANDWIDTH_HZ = 10000;

// ---- NRSC-1 de-emphasis (build 21; setDeemphasis) ----
// NRSC-1-A (the NRSC's AM preemphasis / deemphasis standard), section 6 / Table 2: the receiver
// "shall complement the recommended transmission preemphasis": one POLE at 2122 Hz (75 us) and
// one ZERO at 8700 Hz (~18.3 us), unity at low frequencies:
//       H(f) = (1 + j f / 8700) / (1 + j f / 2122)       -> -6.92 dB at 5 kHz, -10.00 dB at 10 kHz
// At 44,100 S/s that is a one-pole / one-zero filter,  y[n] = g (x[n] - q x[n-1]) + p y[n-1],
// g = (1 - p) / (1 - q) for unity at DC. Why not the textbook bilinear transform? With 8.7 kHz
// so close to a quarter of the sample rate it bends the curve: 0.46 dB too much cut at 5 kHz,
// 1.3 dB at 10 kHz. So p and q were FITTED (least squares) to the standard's own curve between
// 50 Hz and 10 kHz: the result is within 0.01 dB of Table 2 everywhere in that range
// (100 Hz -0.009, 1 kHz -0.81, 2122 Hz -2.76, 5 kHz -6.93, 10 kHz -9.99 dB). p is almost the
// "matched" exp(-2 pi 2122 / 44100) = 0.7392, q a little below exp(-2 pi 8700 / 44100) = 0.2896.
// Above 10 kHz nothing is left anyway (the audio low-pass stops at the bandwidth + 1.5 kHz).
static constexpr float DEEMPH_P = 0.738747605f;     // the pole  (2122 Hz)
static constexpr float DEEMPH_Q = 0.285136516f;     // the zero  (8700 Hz)
static constexpr float DEEMPH_G = (1.0f - DEEMPH_P) / (1.0f - DEEMPH_Q);   // unity gain at DC

// ---- the stereo setting (setStereoMode) - the same numbers as fm:: so one setting serves both ----
static const int STEREO_AUTO = 0;                // C-QUAM stereo when a pilot is there, blended to mono when noisy
static const int STEREO_MONO = 1;                // always mono (skips the C-QUAM work)
static const int STEREO_FULL = 2;                // stereo whenever there is a pilot, never blended

// ---- the carrier PLL ----
static const double PLL_BW_HZ = 5.0;             // natural frequency: follows drift, not C-QUAM's phase modulation or its 25 Hz pilot
static const double PLL_DAMPING = 0.707;
static const double PLL_MAX_HZ = 2000.0;         // the oscillator never runs further from the centre than this
static const double ACQ_RANGE_HZ = 1500.0;       // the FFT looks for the carrier within this of the centre
static const int    ACQ_N = 8192;                // FFT size at 55,125 S/s: 6.7 Hz bins, 0.15 s of samples
static const double REACQUIRE_S = 2.0;           // unlocked this long -> look for the carrier again
static const double LEVEL_TAU_S = 0.5;           // the AGC: how fast the carrier amplitude A follows the signal ...
static const double LEVEL_TAU_START_S = 0.02;    // ... starting fast (the first second would otherwise be far too loud)
static const float  LEVEL_FLOOR = 0.002f;        // A is never taken smaller than this (-54 dBFS): no division by ~0
static const float  LOCK_ON = 2.5f, LOCK_OFF = 1.8f;   // "locked" = A > LOCK_ON x the rms of the quadrature (hysteresis)
static const double LOCK_TAU_S = 0.3;            // ... both averaged this long

// ---- the signal meter and the stereo -> mono blend ----
static const float SNR_STEREO_DB = 45.0f;        // C/N (5 kHz) >= this: full C-QUAM stereo
static const float SNR_MONO_DB   = 30.0f;        // C/N <= this: mono (the quadrature is where the noise is)
static const float BLEND_ATTACK_S = 0.05f, BLEND_RELEASE_S = 4.0f, BLEND_FAST_UNTIL_S = 1.5f;   // as on FM

// ---- C-QUAM pilot detection ----
static const double PILOT_HZ = 25.0, PILOT_REF_HZ = 40.0;   // the pilot, and the band next to it that stands for "no pilot"
static const double PILOT_Q = 8.0;               // band-pass width: 3 Hz at 25 Hz, 5 Hz at 40 Hz
static const float  PILOT_ON_RATIO = 4.0f;       // 25 Hz power > this x the 40 Hz power (6 dB) -> pilot (hysteresis: off below PILOT_OFF_RATIO)
static const float  PILOT_OFF_RATIO = 2.0f;
static const float  PILOT_MIN_AMPL = 0.005f;     // ... and the pilot at least 0.5 % modulation (nominal 4 %)
static const double PILOT_TAU_S = 1.0;           // the two powers averaged this long
static const float  IQUAD_MIN = 0.2f;            // C-QUAM: I' is held at >= this x A in the division (deep troughs)
static const float  DIFF_MAX = 1.5f;             // ... and L-R never beyond this (100 % = 1.0)
static const float  OUT_MAX = 2.0f;              // the detector's output is held within this (noise with no carrier)

// ---- Tiny radix-2 FFT with its tables, for the acquisition and the meter ----------------------
class SmallFft {
public:
    void init(int n_) {
        n = n_; tw.resize((size_t)n / 2); rev.resize((size_t)n);
        for (int i = 0; i < n / 2; i++) tw[(size_t)i] = std::polar(1.f, (float)(-2 * M_PI * i / n));
        int bits = 0; while ((1 << bits) < n) bits++;
        for (int i = 0; i < n; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[(size_t)i] = r; }
    }
    void run(std::complex<float> *buf) const {               // in place, decimation in time
        for (int i = 0; i < n; i++) if (i < rev[(size_t)i]) std::swap(buf[i], buf[rev[(size_t)i]]);
        for (int len = 2; len <= n; len <<= 1) {
            int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; j++) {
                    std::complex<float> t = buf[i + j + half] * tw[(size_t)(j * step)];
                    buf[i + j + half] = buf[i + j] - t;
                    buf[i + j] += t;
                }
        }
    }
    int size() const { return n; }
private:
    int n = 0; std::vector<std::complex<float>> tw; std::vector<int> rev;
};

// ---- A biquad (two-pole) filter: band-pass or notch, RBJ cookbook -------------------------
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void bandpass(double fs, double f0, double q) {           // unity gain at f0
        double w0 = 2 * M_PI * f0 / fs, alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        b0 = (float)(alpha / a0); b1 = 0; b2 = (float)(-alpha / a0);
        a1 = (float)(-2 * std::cos(w0) / a0); a2 = (float)((1 - alpha) / a0);
        reset();
    }
    void notch(double fs, double f0, double q) {
        double w0 = 2 * M_PI * f0 / fs, alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        b0 = (float)(1 / a0); b1 = (float)(-2 * std::cos(w0) / a0); b2 = b0;
        a1 = (float)(-2 * std::cos(w0) / a0); a2 = (float)((1 - alpha) / a0);
        reset();
    }
    void reset() { x1 = x2 = y1 = y2 = 0; }
    inline float push(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// ---- The signal meter: carrier against the quietest part of the channel ----------------------
// One 2,048-point FFT (26.9 Hz bins) of the carrier-locked baseband z on every 8th block of
// 2,048 samples (3.4 a second), Hann window. Eight 1 kHz bands, 5.5 .. 9.5 kHz above and below
// the carrier; the QUIETEST one is taken as the noise floor (an adjacent station's spill or an
// HD secondary sideband can fill some bands, as an SCA can on FM). Density per Hz = mean
// |X|^2 over the band / sum(w^2) / fs; the carrier's power = A^2 (from the detector); C/N in
// 5 kHz = A^2 / (density x 5000). A 1 s average for the screen, a 0.3 s one for the blend.
class Meter {
public:
    static const int N = 2048, EVERY = 8, BANDS = 8;
    void reset() {
        if (win.empty()) init();
        fill = 0; blockNo = 0; ffts = 0;
        slow = fast = 0; result = -99.f; fastResult = -99.f;
    }
    // One baseband sample and the current carrier amplitude A.
    inline void push(const std::complex<float> &z, float a) {
        if (blockNo != 0) { if (++fill == N) { fill = 0; blockNo = (blockNo + 1) % EVERY; } return; }
        buf[fill] = z * win[(size_t)fill];
        if (++fill < N) return;
        fill = 0; blockNo = 1 % EVERY;
        fft.run(buf);
        double best = 1e30;
        for (int b = 0; b < BANDS; b++) {
            double p = 0;
            for (int k = kLo[b]; k < kHi[b]; k++) p += std::norm(buf[(k + N) % N]);   // negative bins wrap
            best = std::min(best, p / (kHi[b] - kLo[b]));
        }
        double density = best / winPow / CH_RATE;                        // per Hz
        double cn = (double)a * a / std::max(density * 5000.0, 1e-20);    // carrier / noise in 5 kHz
        double db = 10.0 * std::log10(std::max(cn, 1e-9));
        if (ffts == 0) { slow = db; fast = db; }
        else { fast += (db - fast) * (1.0 / FAST_FFTS); slow += (db - slow) * (1.0 / SLOW_FFTS); }
        ffts++;
        fastResult = (float)fast;
        if (ffts >= 2) result = (float)slow;                              // the first reading alone is too fresh
    }
    float db() const { return result; }                                   // -99 = not yet
    float fastDb() const { return fastResult; }
private:
    static const int FAST_FFTS = 1, SLOW_FFTS = 3;                        // 0.3 s / 0.9 s
    void init() {
        fft.init(N);
        win.resize(N); winPow = 0;
        for (int i = 0; i < N; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / N)); winPow += (double)win[(size_t)i] * win[(size_t)i]; }
        double binHz = CH_RATE / N;
        for (int b = 0; b < BANDS / 2; b++) {                             // above the carrier ...
            kLo[b] = (int)std::ceil((5500.0 + 1000.0 * b) / binHz);
            kHi[b] = (int)std::ceil((6500.0 + 1000.0 * b) / binHz);
            kLo[b + BANDS / 2] = -kHi[b] + 1;                            // ... and the mirror band below it
            kHi[b + BANDS / 2] = -kLo[b] + 1;
        }
    }
    SmallFft fft;
    std::vector<float> win; std::complex<float> buf[N];
    double winPow = 0, slow = 0, fast = 0;
    int kLo[BANDS] = {}, kHi[BANDS] = {}, fill = 0, blockNo = 0, ffts = 0;
    float result = -99.f, fastResult = -99.f;
};

// ---- The demodulator ------------------------------------------------------------------------
class Demod {
public:
    Demod() { reset(); }
    void reset() {
        stage1.init(designLowpass(IQ_RATE, CH_PASS_HZ, RATE1 - CH_PASS_HZ, 60), DECIM1);
        stage2.init(designLowpass(RATE1, CH_PASS_HZ, CH_RATE - CH_PASS_HZ, 60), DECIM2);
        initAudio();
        meter.reset();
        if (acqFft.size() == 0) {
            acqFft.init(ACQ_N);
            acqWin.resize(ACQ_N);
            for (int i = 0; i < ACQ_N; i++) acqWin[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / ACQ_N));
            sinTab.resize(TABLE);
            for (int i = 0; i < TABLE; i++) sinTab[(size_t)i] = (float)std::sin(2 * M_PI * i / TABLE);
        }
        acqBuf.assign(ACQ_N, std::complex<float>(0, 0)); acqFill = 0; acquired = false; unlockedS = 0;
        phase = 0; freq = 0; integ = 0;
        level = 0; quadRms = 0; locked_ = false;
        pilotBp.bandpass(CH_RATE, PILOT_HZ, PILOT_Q); refBp.bandpass(CH_RATE, PILOT_REF_HZ, PILOT_Q);
        pilotNotch.notch(CH_RATE, PILOT_HZ, 2.0);
        pilotPow = refPow = 0; pilot_ = false;
        stereo = 0; stereoTarget = 0; framesOut = 0;
        dcI = dcQ = 0;
        dcAlpha = (float)(1 - std::exp(-2 * M_PI * 10.0 / RATE1));          // 10 Hz corner at first ...
        dcAlphaSlow = (float)(1 - std::exp(-2 * M_PI * 0.05 / RATE1));      // ... 0.05 Hz once settled (a carrier
        dcDecay = (float)std::exp(-std::log(200.0) / (1.0 * RATE1));         // 1 Hz off centre loses 0.01 dB)
        levAlphaSlow = (float)(1 - std::exp(-1.0 / (LEVEL_TAU_S * CH_RATE)));
        levAlpha = (float)(1 - std::exp(-1.0 / (LEVEL_TAU_START_S * CH_RATE)));          // fast start ...
        levDecay = (float)std::exp(-std::log(LEVEL_TAU_S / LEVEL_TAU_START_S) / (1.0 * CH_RATE));   // ... slow after ~1 s
        lockAlpha = (float)(1 - std::exp(-1.0 / (LOCK_TAU_S * CH_RATE)));
        pilotAlpha = (float)(1 - std::exp(-1.0 / (PILOT_TAU_S * CH_RATE)));
        // PLL loop constants (detector gain 1: the error is Q'/A = sin of the phase error)
        double wn = 2 * M_PI * PLL_BW_HZ;
        kp = (float)(2 * PLL_DAMPING * wn / CH_RATE);                       // radians per sample per unit error
        ki = (float)(wn * wn / (CH_RATE * CH_RATE));
        maxFreq = (float)(2 * M_PI * PLL_MAX_HZ / CH_RATE);
        attackStep = (float)(1.0 / (BLEND_ATTACK_S * AUDIO_RATE));
        releaseStep = (float)(1.0 / (BLEND_RELEASE_S * AUDIO_RATE));
        fastUntil = (long long)(BLEND_FAST_UNTIL_S * AUDIO_RATE);
        resetDeemphasis();
        audioOut.clear();
    }
    // STEREO_AUTO (default), STEREO_MONO or STEREO_FULL. Between blocks (streaming thread).
    void setStereoMode(int m) { mode = m == STEREO_MONO ? STEREO_MONO : m == STEREO_FULL ? STEREO_FULL : STEREO_AUTO; }
    int stereoMode() const { return mode; }
    // Is the HD decoder in sync on this station? Then it is a hybrid HD station: the quadrature
    // carries its digital carriers, and C-QUAM decoding is off. Between blocks.
    void setHdSynced(bool yes) { hdSynced = yes; }
    // The audio bandwidth (Hz, 2,000 .. 10,000; default 5,000). A change redesigns the
    // resampler's filter (a few ms, once) - the audio history restarts (a tiny gap).
    void setBandwidth(int hz) {
        hz = std::max(MIN_BANDWIDTH_HZ, std::min(MAX_BANDWIDTH_HZ, hz));
        if (hz == bandwidthHz) return;
        bandwidthHz = hz;
        initAudio();
    }
    int bandwidth() const { return bandwidthHz; }
    // Build 21: NRSC-1 de-emphasis on (the app's default - Settings -> "AM de-emphasis") or off
    // (flat audio, as the station sends it). Survives reset(), like the bandwidth. Switching it
    // on starts the filter from rest (no click: the detector's audio has no DC in it).
    // Between blocks (streaming thread). Off = the output is exactly what it was before build 21.
    void setDeemphasis(bool on) {
        if (on == deemph) return;
        deemph = on;
        resetDeemphasis();
    }
    bool deemphasis() const { return deemph; }

    // Feed cu8 bytes (I,Q,I,Q,...). Audio appears in audioOut (append-only; caller clears).
    void processCu8(const uint8_t *buf, size_t len) {
        const size_t n = len / 2;
        if (n == 0) return;
        // 1. bytes -> floats, into stage 1's input area
        vec::cu8ToFloat(buf, stage1.inI(n), stage1.inQ(n), n);
        // 2. stage 1 (÷9), into stage 2's input area
        const size_t m1 = stage1.outputs(n);
        float *s2i = stage2.inI(m1), *s2q = stage2.inQ(m1);
        const size_t m = stage1.process(n, s2i, s2q);
        // 3. DC blocker at 165 kS/s (a feedback loop: sample by sample)
        for (size_t j = 0; j < m; j++) {
            dcI += dcAlpha * (s2i[j] - dcI); dcQ += dcAlpha * (s2q[j] - dcQ);
            if (dcAlpha > dcAlphaSlow) dcAlpha *= dcDecay;
            s2i[j] -= dcI; s2q[j] -= dcQ;
        }
        // 4. stage 2 (÷3) -> the channel at 55,125 S/s
        if (chI.size() < m + 8) { chI.resize(m + 8, 0.f); chQ.resize(m + 8, 0.f); }
        const size_t c = stage2.process(m, chI.data(), chQ.data());
        // 5. the rest, sample by sample: acquisition, PLL, detector, C-QUAM, meter, resampler
        for (size_t j = 0; j < c; j++) {
            const std::complex<float> x(chI[j], chQ[j]);
            // acquisition: collect ACQ_N samples, then find the carrier (at the start, and
            // again after REACQUIRE_S without a lock)
            if (!acquired || (!locked_ && unlockedS >= REACQUIRE_S)) {
                acqBuf[(size_t)acqFill] = x * acqWin[(size_t)acqFill];
                if (++acqFill == ACQ_N) { acquire(); acqFill = 0; unlockedS = 0; }
                if (!acquired) continue;                                   // silence until the first fix (0.15 s)
            }
            // the PLL's oscillator, and the carrier-locked baseband z = x * conj(e^{j phase})
            const uint32_t idx = phase >> SHIFT;
            const float s = sinTab[idx], co = sinTab[(idx + TABLE / 4) & (TABLE - 1)];
            const float zr = x.real() * co + x.imag() * s, zi = x.imag() * co - x.real() * s;
            // the carrier's amplitude A: the slow average of I' (the modulation averages to zero)
            level += levAlpha * (zr - level);
            if (levAlpha > levAlphaSlow) levAlpha *= levDecay;
            const float a = std::max(level, LEVEL_FLOOR);
            // loop: error = Q'/A ~ sin(phase error); proportional + integral
            float e = zi / a;
            if (e > 1.f) e = 1.f; else if (e < -1.f) e = -1.f;
            integ += ki * e;
            if (integ > maxFreq) integ = maxFreq; else if (integ < -maxFreq) integ = -maxFreq;
            freq = integ;
            phase += (uint32_t)(nominal + (int32_t)((freq + kp * e) * (4294967296.0f / (2.f * (float)M_PI))));
            // lock: the carrier well above the quadrature's rms (noise, or C-QUAM's L-R)
            quadRms += lockAlpha * (zi * zi - quadRms);
            const float q = std::sqrt(std::max(quadRms, 0.f));
            if (locked_) { if (level < LOCK_OFF * q) locked_ = false; }
            else if (level > LOCK_ON * q && level > LEVEL_FLOOR) locked_ = true;
            if (!locked_) unlockedS += (float)(1.0 / CH_RATE); else unlockedS = 0;
            meter.push(std::complex<float>(zr, zi), a);
            // the synchronous detector: mono = the in-phase modulation
            float mono = (zr - level) / a;
            if (mono > OUT_MAX) mono = OUT_MAX; else if (mono < -OUT_MAX) mono = -OUT_MAX;   // noise with no carrier: bounded
            // C-QUAM: the pilot detector always listens to L-R (cheap), so "ST" is known even in
            // the mono setting; the stereo path itself only runs when wanted.
            const float ir = std::max(zr, IQUAD_MIN * a);                 // I' held away from zero for the division
            const float env = std::sqrt(zr * zr + zi * zi);
            float diff = env * zi / ir / a;                                // L - R (with the pilot)
            if (diff > DIFF_MAX) diff = DIFF_MAX; else if (diff < -DIFF_MAX) diff = -DIFF_MAX;
            const float pb = pilotBp.push(diff), rb = refBp.push(diff);
            pilotPow += pilotAlpha * (pb * pb - pilotPow);
            refPow += pilotAlpha * (rb * rb - refPow);
            const float minPow = 0.5f * PILOT_MIN_AMPL * PILOT_MIN_AMPL;    // a sine's power = ampl^2 / 2
            if (pilot_) { if (pilotPow < PILOT_OFF_RATIO * refPow || pilotPow < 0.25f * minPow || hdSynced) pilot_ = false; }
            else if (pilotPow > PILOT_ON_RATIO * refPow && pilotPow > minPow && locked_ && !hdSynced) pilot_ = true;
            const bool wantD = mode != STEREO_MONO && pilot_;
            if (wantD != audio.secondEnabled()) {
                if (wantD) { audio.enableSecond(true); pilotNotch.reset(); }
                else if (stereo <= 0.f) audio.enableSecond(false);         // keep it until the fade to mono is done
            }
            float mSig = mono, dSig = 0.f;
            if (audio.secondEnabled()) {
                mSig = env / a - 1.f;                                      // C-QUAM: L+R is the envelope
                if (mSig > OUT_MAX) mSig = OUT_MAX;
                dSig = pilotNotch.push(diff);                              // L-R without the 25 Hz pilot
            }
            audio.push(mSig, dSig, [this](float mm, float dd) {
                if (stereoTarget < stereo) stereo = std::max(stereoTarget, stereo - attackStep);
                else if (stereoTarget > stereo) stereo = framesOut < fastUntil ? stereoTarget : std::min(stereoTarget, stereo + releaseStep);
                float l = mm + stereo * dd, r = mm - stereo * dd;          // L = (L+R) + (L-R), R = (L+R) - (L-R): see the header
                if (deemph) {                                              // build 21: NRSC-1 (see DEEMPH_P), per channel
                    const float yl = DEEMPH_G * (l - DEEMPH_Q * deXl) + DEEMPH_P * deYl;
                    const float yr = DEEMPH_G * (r - DEEMPH_Q * deXr) + DEEMPH_P * deYr;
                    deXl = l; deYl = yl; deXr = r; deYr = yr;
                    l = yl; r = yr;
                }
                if (l > OUT_MAX) l = OUT_MAX; else if (l < -OUT_MAX) l = -OUT_MAX;   // (the filter can overshoot a clamped input)
                if (r > OUT_MAX) r = OUT_MAX; else if (r < -OUT_MAX) r = -OUT_MAX;
                audioOut.push_back(l);
                audioOut.push_back(r);
                if ((++framesOut & 63) == 0) updateTarget();
            });
        }
    }
    std::vector<float> audioOut;                 // STEREO interleaved (L, R), 44,100 Hz, 1.0 = 100 % modulation

    // For the screen and the clock correction:
    float freqOffsetHz() const { return (float)(acqHz + freq * CH_RATE / (2 * M_PI)); }   // the carrier's distance from the centre
    float carrierDbfs() const { return level > 1e-6f ? 20.f * std::log10(level) : -99.f; } // carrier amplitude, 0 dB = a full-scale sine
    float snrDb() const { return meter.db(); }                                             // carrier / noise in 5 kHz (-99 = not yet)
    float snrDbFast() const { return meter.fastDb(); }
    bool  locked() const { return locked_; }                                               // a carrier is there and the PLL holds it
    bool  pilot() const { return pilot_; }                                                 // a C-QUAM pilot is there
    float pilotLevel() const { return std::sqrt(std::max(2.f * pilotPow, 0.f)); }          // its amplitude, 1.0 = 100 % modulation (nominal 0.04)
    float stereoAmount() const { return stereo; }                                          // 0 = mono ... 1 = full stereo
    bool  hdSyncedNow() const { return hdSynced; }
    // Total delay of the analog path in seconds (filters only; not counting output buffering).
    double latencySeconds() const {
        return stage1.groupDelay() / IQ_RATE + stage2.groupDelay() / RATE1 + audio.delaySamplesIn / CH_RATE;
    }
private:
    static const int TABLE = 4096, SHIFT = 32 - 12;
    void resetDeemphasis() { deXl = deYl = deXr = deYr = 0; }
    void initAudio() {
        // pass bandwidth, stop 1.5 kHz above it (10 kHz: stop 11.5 - still inside the 12 kHz channel)
        audio.init(CH_RATE, RESAMPLE_L, RESAMPLE_M, bandwidthHz, bandwidthHz + 1500, 60);
        if (mode != STEREO_MONO && pilot_) audio.enableSecond(true);
    }
    // The FFT fix on the carrier: the strongest line within +-ACQ_RANGE_HZ of the centre,
    // refined to a fraction of a bin by a parabola through the three log-magnitudes.
    void acquire() {
        acqFft.run(acqBuf.data());
        const double binHz = CH_RATE / ACQ_N;
        const int kMax = (int)(ACQ_RANGE_HZ / binHz);
        int best = 0; float bestP = -1;
        for (int k = -kMax; k <= kMax; k++) {
            float p = std::norm(acqBuf[(size_t)((k + ACQ_N) % ACQ_N)]);
            if (p > bestP) { bestP = p; best = k; }
        }
        auto lp = [&](int k) { return std::log10(std::norm(acqBuf[(size_t)((k + ACQ_N) % ACQ_N)]) + 1e-20f); };
        const float y0 = lp(best - 1), y1 = lp(best), y2 = lp(best + 1);
        const float den = y0 - 2 * y1 + y2;
        const float frac = std::fabs(den) > 1e-9f ? 0.5f * (y0 - y2) / den : 0.f;
        acqHz = ((double)best + (double)std::max(-0.5f, std::min(0.5f, frac))) * binHz;
        // the oscillator starts at the fix; the loop only has the last Hz to pull in
        phase = 0; integ = 0; freq = 0;
        nominal = (int32_t)std::llround(acqHz / CH_RATE * 4294967296.0);
        acquired = true;
    }
    void updateTarget() {
        if (mode == STEREO_MONO || !pilot_ || hdSynced) { stereoTarget = 0; return; }
        if (mode == STEREO_FULL) { stereoTarget = 1; return; }
        const float q = meter.fastDb();
        if (q < -98.f) { stereoTarget = 0; return; }
        stereoTarget = std::max(0.f, std::min(1.f, (q - SNR_MONO_DB) / (SNR_STEREO_DB - SNR_MONO_DB)));
    }
    ComplexFirDecimator stage1, stage2; Resampler audio;
    std::vector<float> chI, chQ;
    Meter meter;
    SmallFft acqFft; std::vector<float> acqWin; std::vector<std::complex<float>> acqBuf; int acqFill = 0; bool acquired = false;
    double acqHz = 0; float unlockedS = 0;
    std::vector<float> sinTab;
    uint32_t phase = 0; int32_t nominal = 0;
    float freq = 0, integ = 0, kp = 0, ki = 0, maxFreq = 0;
    float level = 0, quadRms = 0, levAlpha = 0, levAlphaSlow = 0, levDecay = 0, lockAlpha = 0; bool locked_ = false;
    Biquad pilotBp, refBp, pilotNotch; float pilotPow = 0, refPow = 0, pilotAlpha = 0; bool pilot_ = false;
    int mode = STEREO_AUTO; bool hdSynced = false; int bandwidthHz = DEFAULT_BANDWIDTH_HZ;
    float stereo = 0, stereoTarget = 0, attackStep = 0, releaseStep = 0; long long framesOut = 0, fastUntil = 0;
    float dcI = 0, dcQ = 0, dcAlpha = 0, dcAlphaSlow = 0, dcDecay = 0;
    bool deemph = false; float deXl = 0, deYl = 0, deXr = 0, deYr = 0;   // build 21: NRSC-1 de-emphasis and its state
};

} // namespace am
