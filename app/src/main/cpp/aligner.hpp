// aligner.hpp - measures how far the analog FM audio lags the HD1 audio (M11, 11c).
//
// Background (decision 33): the station delays its analog audio so that a car
// radio's HD decoder (which is slow) comes out in step with it. nrsc5 is faster
// than a car radio, so its HD audio comes out ~2.5 s BEFORE our analog audio.
// The figure is the station's choice (2485 ms on most LA stations, KLOS 2432 ms),
// and so is the loudness difference (KLOS's analog is 4 dB louder). 11d needs both
// to blend without a skip.
//
// How it works
//   Both streams are put on ONE time line, counted in audio frames (44,100 Hz)
//   since the tune:
//     - analog: frame n came out of the demodulator at IQ time n * 33.75 (the
//       demodulator is time-true), so its index IS its position. (Since drift.hpp:
//       the analog audio passes through its clock correction first, which takes
//       out or adds a frame now and then so that the analog keeps the STATION's
//       time like the HD audio does; the IQ positions are corrected by the same
//       amount before they get here. With a good dongle that amount is ~0.)
//     - HD: nrsc5 hands out 2,048-frame pieces at points locked to the IQ stream
//       (output.c: 2 pieces per acquire block of 138,240 IQ samples). We know the
//       IQ position of the block we were pushing when a piece arrived, so the
//       piece's position is that block's start (exact to within one block, ~88 ms
//       with librtlsdr's default blocks); later pieces continue from there in
//       exact steps. anchorCorrection() narrows the block uncertainty to a few ms
//       from the way the pieces fall into successive blocks.
//   Both streams are kept as MONO copies at 11,025 Hz (decimated by 4) for the
//   last HIST_SECONDS, plus 10 ms energy envelopes for the loudness.
//   measure() (call it from a background thread every couple of seconds):
//     1. coarse: cross-correlate the newest 3 s of analog against the HD of the
//        same period shifted by -1 .. +6 s, at 2,756 Hz, with an FFT
//        (normalised, so the peak value is a real correlation coefficient)
//     2. fine: direct correlation at 11,025 Hz around the coarse peak, then a
//        parabola through the top 3 points -> better than one 44.1 kHz frame
//     3. loudness: energy of both streams over the aligned stretch
//   Results are collected; the reported offset is the median of the accepted
//   ones. offsetFrames = analog frame index - HD frame index for the same sound
//   (> 0 = HD is ahead). dMs = the same in real time (block uncertainty removed).
//
// Plain C++17, no dependencies except fmdemod.hpp's designLowpass().
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <mutex>
#include <complex>
#include <climits>
#include "fmdemod.hpp"

namespace align {

static const int    FRAME_RATE   = 44100;
static const int    DECIM        = 4;                  // 44,100 -> 11,025 Hz history
static const int    HIST_RATE    = FRAME_RATE / DECIM;
static const int    COARSE_DECIM = 4;                  // 11,025 -> 2,756 Hz for the coarse search
static const double HIST_SECONDS = 14.0;
static const int    ENV_FRAMES   = 441;                // 10 ms energy envelope
static const double IQ_PER_FRAME = 1488375.0 / 44100.0; // 33.75 exactly
static const long long RUN_SLACK_FRAMES = 8192;        // an HD piece this far off its expected place = new run
static const double TEMPLATE_SECONDS = 3.0;            // analog stretch we look for in the HD
static const double D_MIN_SECONDS = -1.0;              // search range for the offset (real time)...
static const double D_MAX_SECONDS =  6.0;              // ... analog may lag HD by up to 6 s
static const double HINT_BEFORE_SECONDS = 0.25;        // search window around the expected value
static const double HINT_AFTER_SECONDS  = 0.35;        //   (a bit more after: the block uncertainty adds)
// Measured 2026-09-27 on KRTH, KBIG, KKGO, KYSR and KLLI: 2485 ms on every one of them
// (real time) - but KLOS (Meruelo) runs 2432 ms, so it IS the station's choice after
// all (most of them just use the same figure). Used as the first guess; the full
// -1..+6 s search is the fallback, and the hint window (-0.25/+0.35 s) covers KLOS.
static const double EXPECTED_D_SECONDS = 2.485;
static const float  ACCEPT_CORR = 0.5f;                // below this = no match
static const int    KEEP_RESULTS = 9;                  // median over the last accepted results
static const int    AGREE_FRAMES = 3;                  // two results this close = "ok"
static const int    JUMP_FRAMES  = 200;                // a result this far from the median = the alignment changed
static const int    NO_MATCH_AFTER = 3;                // failed attempts before we say "no match"

// ---- Tiny radix-2 FFT (complex, in place) --------------------------------------------------
inline void fft(std::vector<std::complex<float>> &x, bool inverse) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; i++) {               // bit-reversal permutation
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * M_PI / (double)len * (inverse ? 1 : -1);
        std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<float> u = x[i + j], v = x[i + j + len / 2] * w;
                x[i + j] = u + v; x[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto &v : x) v /= (float)n;
}

// ---- FIR low-pass + decimate, real signal, computes only the outputs it needs ---------------
class Decimator {
public:
    void init(const std::vector<float> &taps, int decim) {
        h = taps; m = decim; n = (int)h.size();
        hist.assign(2 * n, 0.f); pos = 0; phase = 0;
    }
    template <class Out> inline void push(float x, Out &&emit) {
        hist[pos] = hist[pos + n] = x;
        int start = pos + 1;
        pos = (pos + 1 == n) ? 0 : pos + 1;
        if (++phase < m) return;
        phase = 0;
        const float *w = &hist[start];                    // oldest first; h is symmetric so no reversal needed
        float a = 0;
        for (int k = 0; k < n; k++) a += h[k] * w[k];
        emit(a);
    }
private:
    std::vector<float> h, hist; int n = 0, m = 1, pos = 0, phase = 0;
};

struct Result {
    bool  accepted = false;
    long long offsetFrames = 0;   // analog index - HD index (44.1 kHz frames), > 0 = HD ahead
    double offsetFine = 0;        // the same with its fraction of a frame (for drift.hpp's line fit)
    long long at = 0;             // time line position of the MIDDLE of the analog stretch that was compared
    int   epoch = 0;              // Status::epoch when this was measured
    float corr = 0;               // correlation coefficient at the peak (1 = identical)
    float gainDb = 0;             // analog loudness relative to HD (dB, > 0 = analog louder)
    float dMs = 0;                // offset in real time, ms (block uncertainty removed)
    bool  usedHint = false;       // searched only around the remembered value
};

struct Status {
    int   state = 0;              // 0 waiting for audio, 1 measuring, 2 ok, 3 no match
    int   attempts = 0, accepted = 0;
    int   jumps = 0;              // times the alignment changed (after sync losses)
    int   epoch = 0;              // counts every fresh start of the offsets (HD re-anchored, or a jump):
                                  // offsets of different epochs can't be compared (drift.hpp)
    long long offsetFrames = 0;   // median of the accepted results (content offset)
    float dMs = 0;                // ... in real time, ms
    float corr = 0;               // latest accepted correlation
    float gainDb = 0;             // median analog-vs-HD loudness, dB
    long long hdFrames = 0;       // HD1 frames received since the tune
    long long analogFrames = 0;   // analog frames received since the tune
};

class Aligner {
public:
    Aligner() { reset(); }

    void reset() {
        std::lock_guard<std::mutex> lock(m_);
        histLen_ = (size_t)(HIST_SECONDS * HIST_RATE);
        histA_.assign(histLen_, 0.f); histH_.assign(histLen_, 0.f);
        envLen_ = (size_t)(HIST_SECONDS * FRAME_RATE / ENV_FRAMES) + 2;
        envA_.assign(envLen_, 0.f); envH_.assign(envLen_, 0.f);
        decA_.init(histTaps(), DECIM); decH_.init(histTaps(), DECIM);
        aNext_ = 0; hdNext_ = 0; hdValidFrom_ = 0; hdEver_ = false;
        hdRun_ = false; hdRunFrames_ = 0; hdAnchor_ = 0; hdLower_ = LLONG_MIN; hdUpper_ = LLONG_MAX;
        hdFirstBlockLen_ = 0;
        envAccA_ = envAccH_ = 0; envCntA_ = envCntH_ = 0;
        results_.clear(); status_ = Status(); nextTryFrame_ = 0; failures_ = 0;
        hintFrames_ = (long long)(EXPECTED_D_SECONDS * FRAME_RATE); hintFailed_ = false;
    }

    // Expected real-time offset (frames) to search around first, or LLONG_MIN = full search.
    void setHint(long long dFrames) {
        std::lock_guard<std::mutex> lock(m_);
        hintFrames_ = dFrames; hintFailed_ = false;
    }

    // Streaming thread: the analog audio as it goes into the analog ring (mono, 44.1 kHz,
    // same scale as the HD audio: 1.0 = full scale).
    // Returns the time line position of the NEXT analog frame (= frames received so far).
    long long pushAnalog(const float *mono, size_t n) {
        std::lock_guard<std::mutex> lock(m_);
        for (size_t i = 0; i < n; i++) {
            float v = mono[i];
            decA_.push(v, [this](float d) { histA_[(size_t)(aNext_ / DECIM) % histLen_] = d; });
            envAccA_ += v * v;
            if (++envCntA_ == ENV_FRAMES) {
                envA_[(size_t)(aNext_ / ENV_FRAMES) % envLen_] = envAccA_ / ENV_FRAMES;
                envAccA_ = 0; envCntA_ = 0;
            }
            aNext_++;
        }
        status_.analogFrames = aNext_;
        return aNext_;
    }

    // Streaming thread: one HD1 audio piece from nrsc5 (16-bit stereo interleaved, `count`
    // values), received while we were pushing the IQ block that starts at time line
    // position `blockStart` and is `blockLen` frames long. (The caller converts: IQ
    // samples since the tune / 33.75, minus the frames drift.hpp's clock correction has
    // taken out of the analog audio so far - so the HD pieces are anchored on the same
    // time line the analog frames are counted on.)
    // 11d: returns the piece's position on the time line (the frame index of its first
    // frame) - the blend keeps the HD audio in a ring indexed by the same numbers.
    long long pushHd(const int16_t *stereo, size_t count, long long blockStart, long long blockLen) {
        std::lock_guard<std::mutex> lock(m_);
        // Where should this piece sit on the time line? The true position of the run's
        // first piece is somewhere in its block [anchor, anchor + firstBlockLen), so the
        // expected position of this piece is within (blockStart - firstBlockLen, blockStart + blockLen).
        long long expected = hdAnchor_ + hdRunFrames_;
        bool fits = hdRun_ && expected > blockStart - hdFirstBlockLen_ - RUN_SLACK_FRAMES
                            && expected < blockStart + blockLen + RUN_SLACK_FRAMES;
        if (!fits) {
            // A new run (first audio, or audio back after a sync loss): anchor it at the
            // start of the current block, rounded to the decimation grid.
            hdAnchor_ = std::max(blockStart / DECIM * DECIM, hdNext_ / DECIM * DECIM);
            hdFirstBlockLen_ = blockLen;
            hdRunFrames_ = 0; hdRun_ = true;
            hdLower_ = LLONG_MIN; hdUpper_ = LLONG_MAX;
            // Zero-fill the gap so no old audio is mistaken for current audio.
            long long from = std::max(hdNext_, hdAnchor_ - (long long)(histLen_ * DECIM));
            for (long long f = from; f < hdAnchor_; f += DECIM) histH_[(size_t)(f / DECIM) % histLen_] = 0.f;
            for (long long f = from / ENV_FRAMES * ENV_FRAMES; f < hdAnchor_; f += ENV_FRAMES)
                envH_[(size_t)(f / ENV_FRAMES) % envLen_] = 0.f;
            hdNext_ = hdAnchor_;
            // Only THIS run's audio can be compared with the analog: what came before the
            // anchor is the gap just filled with zeros, and before that a run that sat
            // somewhere else on the time line. (Build 9. Until then this was done for the
            // very first run only, and measure() counted its tries against the zeros as
            // failures - three of them and the verdict was "no match", seconds before the
            // new run had enough audio to be measured at all. It rarely mattered, because
            // a new run was rare. Since hdsearch.hpp rests the HD search, every look after
            // a rest starts one.)
            hdValidFrom_ = hdAnchor_;
            if (!hdEver_) hdEver_ = true;
            else {
                // Audio is back after an outage: the HD stream is anchored afresh, so the
                // content offset changes - old results no longer apply. (The real-time
                // figure stays the same, and the expected value is kept as the hint.)
                results_.clear(); failures_ = 0; status_.epoch++;
                if (status_.state == 2) status_.state = 1;
                nextTryFrame_ = aNext_;
            }
            envAccH_ = 0; envCntH_ = 0;
            decH_.init(histTaps(), DECIM);
        }
        // Narrow down the block uncertainty: the run's true start E0 = E_k - framesSoFar lies in
        // [blockStart_k - framesSoFar, blockStart_k + blockLen_k - framesSoFar) for every piece k.
        hdLower_ = std::max(hdLower_, blockStart - hdRunFrames_);
        hdUpper_ = std::min(hdUpper_, blockStart + blockLen - hdRunFrames_);
        long long index = hdNext_;
        size_t frames = count / 2;
        for (size_t i = 0; i < frames; i++) {
            float v = (stereo[2 * i] + stereo[2 * i + 1]) * (0.5f / 32768.0f);
            decH_.push(v, [this](float d) { histH_[(size_t)(hdNext_ / DECIM) % histLen_] = d; });
            envAccH_ += v * v;
            if (++envCntH_ == ENV_FRAMES) {
                envH_[(size_t)(hdNext_ / ENV_FRAMES) % envLen_] = envAccH_ / ENV_FRAMES;
                envAccH_ = 0; envCntH_ = 0;
            }
            hdNext_++;
        }
        hdRunFrames_ += (long long)frames;
        long long oldest = hdNext_ - (long long)(histLen_ * DECIM) + DECIM;
        if (hdValidFrom_ < oldest) hdValidFrom_ = oldest;
        status_.hdFrames += (long long)frames;
        return index;
    }

    // 11d: the demodulator was switched on while the stream was already running
    // (Digital only -> Auto): the analog time line must stay in step with the IQ
    // time line (frame n = IQ time n * 33.75), so jump the analog counter to `frame`.
    // The history in between is zero (nothing was received) - measure() only uses
    // the newest 3 s of analog, so 3 s later everything is fresh again.
    void skipAnalogTo(long long frame) {
        std::lock_guard<std::mutex> lock(m_);
        if (frame <= aNext_) return;
        long long from = std::max(aNext_, frame - (long long)(histLen_ * DECIM));
        for (long long f = from; f < frame; f += DECIM) histA_[(size_t)(f / DECIM) % histLen_] = 0.f;
        for (long long f = from / ENV_FRAMES * ENV_FRAMES; f < frame; f += ENV_FRAMES)
            envA_[(size_t)(f / ENV_FRAMES) % envLen_] = 0.f;
        aNext_ = frame / DECIM * DECIM;
        envAccA_ = 0; envCntA_ = 0;
        decA_.init(histTaps(), DECIM);
        nextTryFrame_ = aNext_ + (long long)(TEMPLATE_SECONDS * FRAME_RATE);
        status_.analogFrames = aNext_;
        // The analog frames are numbered afresh from here (the demodulator's own delay, ~20
        // frames, is no longer in the numbers), so offsets measured before the gap can't be
        // compared with the ones to come: start over, like after a re-anchored HD run.
        results_.clear(); failures_ = 0; status_.epoch++;
        if (status_.state == 2) status_.state = 1;
    }

    Status status() const {
        std::lock_guard<std::mutex> lock(m_);
        return status_;
    }

    // Background thread: try a measurement if it's time and there is enough audio.
    // Returns true when a measurement was made (accepted or not) -> caller may log it.
    bool measure(Result &out) {
        std::vector<float> tmpl, region, envA, envH;
        long long a0, s0, corr;
        bool usedHint = false;
        {
            std::lock_guard<std::mutex> lock(m_);
            const long long La = (long long)(TEMPLATE_SECONDS * FRAME_RATE);
            if (aNext_ < La || !hdEver_) { status_.state = 0; return false; }
            if (aNext_ < nextTryFrame_) return false;
            if (status_.state == 0) status_.state = 1;
            a0 = aNext_ - La;
            double dMinFr, dMaxFr;
            if (hintFrames_ != LLONG_MIN && !hintFailed_) {
                dMinFr = hintFrames_ - HINT_BEFORE_SECONDS * FRAME_RATE;
                dMaxFr = hintFrames_ + HINT_AFTER_SECONDS * FRAME_RATE;
                usedHint = true;
            } else {
                dMinFr = D_MIN_SECONDS * FRAME_RATE; dMaxFr = D_MAX_SECONDS * FRAME_RATE;
            }
            // HD region that could hold the template's sound: [a0 - dMax, a0 + La - dMin)
            long long r0 = (long long)(a0 - dMaxFr), r1 = (long long)(a0 + La - dMinFr);
            long long v0 = std::max(r0, hdValidFrom_), v1 = std::min(r1, hdNext_);
            v0 = (v0 + DECIM - 1) / DECIM * DECIM; v1 = v1 / DECIM * DECIM;
            if (v1 - v0 < La + (long long)(0.3 * FRAME_RATE)) {          // not enough HD yet: try again in 1 s
                nextTryFrame_ = aNext_ + FRAME_RATE;
                return false;
            }
            s0 = v0;
            tmpl.resize((size_t)(La / DECIM)); region.resize((size_t)((v1 - v0) / DECIM));
            for (size_t i = 0; i < tmpl.size(); i++) tmpl[i] = histA_[(size_t)(a0 / DECIM + i) % histLen_];
            for (size_t i = 0; i < region.size(); i++) region[i] = histH_[(size_t)(s0 / DECIM + i) % histLen_];
            envA = envA_; envH = envH_;
            corr = anchorCorrection();
        }
        // ---- 1. coarse: 2,756 Hz, FFT cross-correlation, normalised ----
        std::vector<float> ct = decimate(tmpl, COARSE_DECIM), cr = decimate(region, COARSE_DECIM);
        float rCoarse = 0; long long kCoarse = 0;
        if (!bestLag(ct, cr, 0, (long long)cr.size() - (long long)ct.size(), rCoarse, kCoarse, true))
            return finish(out, 0, 0.0, a0, 0.f, corr, false, usedHint, 0.f);
        // ---- 2. fine: 11,025 Hz, direct, +-6 coarse steps around the peak ----
        long long kMid = kCoarse * COARSE_DECIM, span = 6 * COARSE_DECIM;
        long long kMax = (long long)region.size() - (long long)tmpl.size();
        long long kLo = std::max(0LL, kMid - span), kHi = std::min(kMax, kMid + span);
        float rFine = 0; long long kFine = 0;
        if (!bestLag(tmpl, region, kLo, kHi, rFine, kFine, false))
            return finish(out, 0, 0.0, a0, 0.f, corr, false, usedHint, 0.f);
        // Parabola through the three points around the peak -> fraction of a sample.
        double frac = 0;
        if (kFine > kLo && kFine < kHi) {
            double ym = corrAt(tmpl, region, kFine - 1), y0 = rFine, yp = corrAt(tmpl, region, kFine + 1);
            double den = ym - 2 * y0 + yp;
            if (den < 0) frac = 0.5 * (ym - yp) / den;
            frac = std::max(-0.5, std::min(0.5, frac));
        }
        // template sample i (frame a0 + i*DECIM) matches HD sample kFine + i (frame s0 + (kFine + i)*DECIM)
        double offsetFine = (double)(a0 - s0) - (kFine + frac) * DECIM;
        long long offsetFrames = a0 - s0 - (long long)std::llround((kFine + frac) * DECIM);
        // ---- 3. loudness over the aligned stretch (10 ms energies, full band) ----
        // (10 ms bins where the HD is exactly silent are dropouts nrsc5 filled with
        // zeros - they'd make the analog look louder than it is, so skip them.)
        double ea = 0, eh = 0;
        long long envN = (long long)envH.size();
        for (long long f = a0; f + ENV_FRAMES <= a0 + (long long)tmpl.size() * DECIM; f += ENV_FRAMES) {
            long long fh = f - offsetFrames;
            float h = envH[(size_t)((((fh / ENV_FRAMES) % envN) + envN) % envN)];
            if (h <= 0.f) continue;
            ea += envA[(size_t)((f / ENV_FRAMES) % envN)];
            eh += h;
        }
        float gainDb = (ea > 0 && eh > 0) ? (float)(10.0 * std::log10(ea / eh)) : 0.f;
        return finish(out, offsetFrames, offsetFine, a0, rFine, corr, rFine >= ACCEPT_CORR, usedHint, gainDb);
    }

private:
    static std::vector<float> histTaps() { return fm::designLowpass(FRAME_RATE, 4200, 5400, 45); }

    // Estimated error of the HD anchor (frames): true run start = anchor + correction.
    long long anchorCorrection() const {   // call with m_ held
        if (!hdRun_ || hdLower_ == LLONG_MIN || hdUpper_ == LLONG_MAX) return 0;
        long long lo = std::max(hdLower_, hdAnchor_), hi = std::max(lo, hdUpper_);
        return (lo + hi) / 2 - hdAnchor_;
    }

    bool finish(Result &out, long long offsetFrames, double offsetFine, long long a0, float r, long long corr,
                bool accepted, bool usedHint, float gainDb) {
        std::lock_guard<std::mutex> lock(m_);
        status_.attempts++;
        out = Result();
        out.accepted = accepted; out.corr = r; out.offsetFrames = offsetFrames; out.gainDb = gainDb;
        out.offsetFine = offsetFine;
        out.at = a0 + (long long)(TEMPLATE_SECONDS * FRAME_RATE) / 2;
        out.dMs = (float)((offsetFrames - corr) * 1000.0 / FRAME_RATE);
        out.usedHint = usedHint;
        if (accepted) {
            status_.accepted++;
            status_.corr = r;
            // After a sync loss nrsc5 can line its output up differently (seen: exactly
            // one 2,048-frame piece later) -> the old results are wrong now, start over.
            if (!results_.empty() && std::llabs(offsetFrames - status_.offsetFrames) > JUMP_FRAMES) {
                results_.clear();
                status_.jumps++; status_.epoch++;
            }
            out.epoch = status_.epoch;
            results_.push_back(out);
            if (results_.size() > (size_t)KEEP_RESULTS) results_.erase(results_.begin());
            std::vector<long long> offs; std::vector<float> gains;
            for (const Result &q : results_) { offs.push_back(q.offsetFrames); gains.push_back(q.gainDb); }
            std::sort(offs.begin(), offs.end()); std::sort(gains.begin(), gains.end());
            status_.offsetFrames = offs[offs.size() / 2];
            status_.gainDb = gains[gains.size() / 2];
            status_.dMs = (float)((status_.offsetFrames - corr) * 1000.0 / FRAME_RATE);
            size_t n = results_.size();
            bool agree = n >= 2 && std::llabs(results_[n - 1].offsetFrames - results_[n - 2].offsetFrames) <= AGREE_FRAMES;
            status_.state = agree ? 2 : 1;
            // Re-measure every 5 s - but confirm a first result after 1 s already (11d:
            // the blend waits for two agreeing results, so this gets the HD in ~1 s sooner).
            nextTryFrame_ = aNext_ + (results_.size() < 2 ? 1 : 5) * FRAME_RATE;
            hintFrames_ = (long long)std::llround(status_.dMs * FRAME_RATE / 1000.0); // search near it next time
            hintFailed_ = false; failures_ = 0;
        } else {
            if (usedHint) hintFailed_ = true;                        // next time: the full search
            failures_++;
            if (status_.state != 2) status_.state = (results_.empty() && failures_ >= NO_MATCH_AFTER) ? 3 : 1;
            nextTryFrame_ = aNext_ + 2 * FRAME_RATE;                 // keep trying every 2 s
        }
        return true;
    }

    static std::vector<float> decimate(const std::vector<float> &x, int m) {
        Decimator d; d.init(fm::designLowpass(HIST_RATE, 1000, 1350, 40), m);
        std::vector<float> out; out.reserve(x.size() / m + 1);
        for (float v : x) d.push(v, [&out](float y) { out.push_back(y); });
        return out;
    }

    // Normalised correlation of template t at lag k inside s.
    static double corrAt(const std::vector<float> &t, const std::vector<float> &s, long long k) {
        double num = 0, et = 0, es = 0;
        for (size_t i = 0; i < t.size(); i++) {
            double a = t[i], b = s[(size_t)k + i];
            num += a * b; et += a * a; es += b * b;
        }
        return (et > 0 && es > 0) ? num / std::sqrt(et * es) : 0.0;
    }

    // Best lag in [kLo, kHi]; FFT-based when `useFft`, direct otherwise. Returns false if impossible.
    static bool bestLag(const std::vector<float> &t, const std::vector<float> &s, long long kLo, long long kHi,
                        float &rBest, long long &kBest, bool useFft) {
        if (t.empty() || kHi < kLo || (long long)s.size() < (long long)t.size() + kHi) return false;
        rBest = -2.f; kBest = kLo;
        if (!useFft) {
            for (long long k = kLo; k <= kHi; k++) {
                double r = corrAt(t, s, k);
                if (r > rBest) { rBest = (float)r; kBest = k; }
            }
            return true;
        }
        size_t n = 1; while (n < t.size() + s.size()) n <<= 1;
        std::vector<std::complex<float>> ft(n), fs(n);
        double et = 0;
        for (size_t i = 0; i < t.size(); i++) { ft[i] = t[i]; et += (double)t[i] * t[i]; }
        for (size_t i = 0; i < s.size(); i++) fs[i] = s[i];
        fft(ft, false); fft(fs, false);
        for (size_t i = 0; i < n; i++) ft[i] = std::conj(ft[i]) * fs[i];   // corr[k] = sum_i t[i] s[i+k]
        fft(ft, true);
        // running energy of s over windows of t.size()
        std::vector<double> ps(s.size() + 1, 0.0);
        for (size_t i = 0; i < s.size(); i++) ps[i + 1] = ps[i] + (double)s[i] * s[i];
        for (long long k = kLo; k <= kHi; k++) {
            double es = ps[(size_t)k + t.size()] - ps[(size_t)k];
            double r = (et > 0 && es > 1e-6 * et) ? ft[(size_t)k].real() / std::sqrt(et * es) : 0.0;
            if (r > rBest) { rBest = (float)r; kBest = k; }
        }
        return true;
    }

    mutable std::mutex m_;
    size_t histLen_ = 0, envLen_ = 0;
    std::vector<float> histA_, histH_, envA_, envH_;
    Decimator decA_, decH_;
    long long aNext_ = 0;                  // analog frames received (= next frame's time line position)
    long long hdNext_ = 0, hdValidFrom_ = 0; bool hdEver_ = false;
    bool hdRun_ = false; long long hdRunFrames_ = 0, hdAnchor_ = 0, hdLower_ = 0, hdUpper_ = 0, hdFirstBlockLen_ = 0;
    float envAccA_ = 0, envAccH_ = 0; int envCntA_ = 0, envCntH_ = 0;
    std::vector<Result> results_; Status status_;
    long long nextTryFrame_ = 0; int failures_ = 0;
    long long hintFrames_ = LLONG_MIN; bool hintFailed_ = false;
};

} // namespace align
