// drift.hpp - keeps the analog audio on the STATION's clock, whatever the dongle's crystal does.
//
// The problem
//   The blend (blend.hpp) needs the analog FM audio and the HD audio of a station lined up
//   to a fraction of a millisecond, and it needs them to STAY lined up. But the two are
//   paced by different clocks:
//     - the HD audio by the STATION's clock: nrsc5 follows the timing of the digital signal
//       and hands out exactly 44,100 audio frames per second of station time;
//     - the analog audio by the DONGLE's clock: fmdemod.hpp turns every 33.75 samples into
//       one audio frame, so its "second" is as long as the dongle's crystal says it is.
//   A dongle with a temperature-compensated crystal (RTL-SDR Blog V3 / V4: 1 part per
//   million) agrees with the station to within 0.05 frames a second - nobody notices. A
//   plain crystal (most older and cheaper dongles) is off by 30 to 100 ppm. At 57 ppm the
//   analog audio gains 2.5 frames on the HD audio EVERY SECOND: 13 frames between two
//   alignment measurements, 3.4 ms a minute. The aligner (aligner.hpp) then never sees two
//   measurements that agree, never says "ok", and the blend stays on analog for ever. That
//   was the first bug report from a tester (2026-10-05, a 2014 Nooelec R820T dongle).
//
// The cure: two small parts, both in this file
//   1. Stretch - a resampler in the analog audio path, right behind the demodulator. It
//      plays the analog audio a few millionths faster or slower (a 57 ppm change is 1/1000
//      of a semitone - far below anything a person or a pitch meter can hear) so that it
//      comes out at 44,100 frames per STATION second. Behind it, both audio streams run on
//      one clock and the aligner and the blend work exactly as they do with a good dongle.
//      The HD audio is never touched.
//   2. Tracker - works out how much to stretch. It has two sources:
//      a) A first guess, a second or two after tuning, from the TUNING ERROR. An RTL
//         dongle has one crystal for everything: the tuner's frequency and the sample
//         clock both come from the same 28.8 MHz. A crystal that runs 57 ppm fast tunes
//         57 ppm too high - the station shows up 57 ppm x 94.5 MHz = 5.4 kHz BELOW the
//         centre (the demodulator measures that offset anyway) - and it takes samples
//         57 ppm too fast. So:  clock error = -(frequency offset) / (station frequency).
//         Good to a ppm or two (the station's own carrier isn't perfect either, and the
//         offset reading wobbles with the music).
//      b) The real thing, from the AUDIO: every accepted measurement of the aligner says
//         how far the (corrected) analog is from the HD. The tracker undoes its own
//         correction to get the raw figures, fits a straight line through the last minute
//         of them - its slope is the clock error, to a fraction of a ppm - and steers the
//         Stretch so the analog sits still at the offset the aligner reports, which is the
//         one the blend lines the two streams up with. This is what counts once it
//         exists; it also follows a crystal that warms up.
//   Neither source can make things worse than before: with no usable guess and no
//   measurement the correction is exactly zero, and the Stretch then copies its input
//   bit for bit.
//
//   A bonus: when the dongle LOSES a few samples on the way (a USB hiccup), the analog
//   jumps against the HD by a few frames - each lost sample is 1/33.75 of a frame. The
//   tracker sees two measurements in a row land off its line by the same amount, and
//   steers the analog back to where it was within two or three seconds. Before this file
//   existed the blend went on with the old figure for 20 s and then took a detour through
//   the analog to re-align. This helps every dongle, also the good ones.
//
//   Tests: tools/harness/drifttest (made-up audio, an hour in half a minute) and mkdrift
//   (turns a real recording into one from a dongle with a cheap crystal).
//
// Words used below
//   frame   one audio sample pair (left, right) at 44,100 Hz.
//   rate    the correction as a plain fraction: 0.000057 = 57 ppm. The Stretch reads
//           (1 + rate) input frames for every frame it puts out; > 0 = the dongle is fast.
//   slip    how many frames the Stretch has taken out so far (the sum of all the rates):
//           output frame c holds the sound of input frame c + slip.
//   offset  the aligner's figure: analog frame number - HD frame number of the same sound.
//
// Threads: Stretch belongs to the streaming thread alone. The Tracker is called from the
// streaming thread (update, once per block) and from the engine thread (addMeasurement,
// after each alignment measurement); it has its own small lock, a leaf (it never calls out).
// Plain C++17, no dependencies.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <mutex>

namespace drift {

static const int RATE = 44100;                       // audio frames per second

// ---- Stretch: the resampler -------------------------------------------------------------------
// Input and output are stereo, interleaved, float. For every output frame it moves
// (1 + rate) frames forward in the input and works out the sound at that point - which
// usually lies BETWEEN two input frames - from the 24 frames around it: a "windowed sinc"
// interpolator, the textbook way to read a sampled signal between its samples. The weights
// for 256 positions between two frames are computed once at start-up (Kaiser window, beta 9);
// positions in between use a straight-line mix of the two nearest sets.
//   Accuracy: within -86 dB of the ideal up to 15 kHz, the top of FM audio (worst case,
//   computed from the weights; -99 dB measured on test tones, see drifttest).
//   Cost: ~100 multiply-adds per frame = a fraction of a percent of what the demodulator needs.
//   Delay: 12 frames (0.27 ms) while it interpolates - it has to see 12 frames past the
//   point it reads. While the rate is exactly 0 and it sits exactly on a frame, it just
//   copies (no arithmetic, no delay): the output IS the input.
class Stretch {
public:
    static const int TAPS = 24;                      // input frames used for one output frame
    static const int HALF = TAPS / 2;
    static const int PHASES = 256;                   // tabulated positions between two frames

    Stretch() { init(); reset(); }

    void reset() {
        hist_.assign((size_t)(HALF + 1) * 2, 0.f);   // silence "before the beginning"
        base_ = -(long long)(HALF + 1);
        ipos_ = 0; frac_ = 0; out_ = 0;
    }

    // Takes `n` stereo frames, appends the corrected frames to `out` (which is NOT cleared).
    // `rate` applies to this call; change it between calls as often as you like - a step of
    // a few ppm is inaudible.
    void process(const float *in, size_t n, double rate, std::vector<float> &out) {
        hist_.insert(hist_.end(), in, in + 2 * n);
        const long long newest = base_ + (long long)(hist_.size() / 2) - 1;   // input index of the last frame we have
        for (;;) {
            const bool copy = rate == 0.0 && frac_ == 0.0;
            if (ipos_ + (copy ? 0 : HALF) > newest) break;                    // not enough input yet
            if (copy) {
                const float *x = &hist_[(size_t)(ipos_ - base_) * 2];
                out.push_back(x[0]); out.push_back(x[1]);
            } else {
                // weights for this position: a straight-line mix of the two nearest tabulated sets
                double fp = frac_ * PHASES;
                int p = (int)fp; if (p >= PHASES) p = PHASES - 1;
                const float mix = (float)(fp - p);
                const float *h0 = &tab_[(size_t)p * TAPS], *h1 = h0 + TAPS;
                const float *x = &hist_[(size_t)(ipos_ - (HALF - 1) - base_) * 2];   // oldest of the 24 frames
                float l = 0, r = 0;
                for (int j = 0; j < TAPS; j++) {
                    float h = h0[j] + mix * (h1[j] - h0[j]);
                    l += h * x[2 * j]; r += h * x[2 * j + 1];
                }
                // the input was clamped to -1..1; between two clamped peaks the true curve
                // can poke a little above - keep the result in range too
                out.push_back(l > 1.f ? 1.f : l < -1.f ? -1.f : l);
                out.push_back(r > 1.f ? 1.f : r < -1.f ? -1.f : r);
            }
            out_++;
            ipos_++; frac_ += rate;                                           // (1 + rate) frames on
            if (frac_ >= 1.0) { frac_ -= 1.0; ipos_++; }
            else if (frac_ < 0.0) { frac_ += 1.0; ipos_--; }
        }
        // forget the input we can't need again (one spare frame: the position may step back by one)
        long long keepFrom = ipos_ - HALF - 1;
        if (keepFrom > base_) {
            hist_.erase(hist_.begin(), hist_.begin() + (size_t)(keepFrom - base_) * 2);
            base_ = keepFrom;
        }
    }

    // Frames taken out so far (input position - output position); negative = frames added.
    double slip() const { return (double)(ipos_ - out_) + frac_; }
    long long framesOut() const { return out_; }

private:
    void init() {
        tab_.assign((size_t)(PHASES + 1) * TAPS, 0.f);
        const double beta = 9.0, i0b = besselI0(beta);
        for (int p = 0; p <= PHASES; p++) {
            double frac = (double)p / PHASES, sum = 0, h[TAPS];
            for (int j = 0; j < TAPS; j++) {
                double x = (j - (HALF - 1)) - frac;                   // distance (in frames) from the point we want
                double r = x / HALF;
                double w = std::fabs(r) < 1 ? besselI0(beta * std::sqrt(1 - r * r)) / i0b : 0;
                double sinc = std::fabs(x) < 1e-12 ? 1 : std::sin(M_PI * x) / (M_PI * x);
                h[j] = sinc * w; sum += h[j];
            }
            for (int j = 0; j < TAPS; j++) tab_[(size_t)p * TAPS + j] = (float)(h[j] / sum);   // no change of level
        }
    }
    static double besselI0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 60; k++) { double h = x / (2.0 * k); term *= h * h; sum += term; }
        return sum;
    }
    std::vector<float> tab_, hist_;
    long long base_ = 0;                             // input index of hist_'s first frame
    long long ipos_ = 0; double frac_ = 0;           // where the next output frame is read: input frame ipos_ + frac_
    long long out_ = 0;                              // frames put out
};

// ---- Tracker: how much to stretch ------------------------------------------------------------
struct Status {
    int    state = 0;        // 0 = nothing known (no correction), 1 = first guess (tuning error), 2 = measured from the audio
    double ppm = 0;          // the correction in use right now, parts per million (> 0 = the dongle's clock is fast)
    double clockPpm = 0;     // the dongle's clock error as best known (the fitted slope, or the first guess)
    double guessPpm = 0;     // what the tuning error says (0 until it has settled)
    bool   haveGuess = false;
    int    points = 0;       // measurements in the fit
    double spreadFrames = 0; // rms distance of those measurements from the fitted line
    double errorFrames = 0;  // how far the analog is from where it should sit right now (the Stretch is closing this)
    int    restarts = 0;     // times the fit started over because the offset really moved (not counting re-anchored HD)
    int    skipped = 0;      // single measurements left out because they were far off the line
};

class Tracker {
public:
    // -- the first guess --
    static constexpr double GUESS_AFTER_S   = 2.0;    // the demodulator's offset reading needs this long to settle
    static constexpr double GUESS_SMOOTH_S  = 4.0;    // ... and wobbles with the music (+-100 Hz and more): average it
    static constexpr double GUESS_MAX_PPM   = 200.0;  // more than this is no crystal error (the worst dongles: ~100)
    static constexpr double GUESS_SIGMA_PPM = 3.0;    // how far we trust it (1 sigma): our reading wobbles ~1 ppm, an HD
                                                      // station's carrier is good to ~1 ppm (seen: guess within 1.5 ppm)
    static constexpr double KEPT_SIGMA_PPM  = 2.0;    // trust in OUR last measurement after the HD audio was re-anchored
    static constexpr double BLIND_SIGMA_PPM = 150.0;  // no guess at all: let the measurements decide alone
    // -- the fit --
    static constexpr double WINDOW_S        = 60.0;   // fit the measurements of the last minute (one every 5 s) ...
    static const int        MAX_POINTS      = 13;     // ... at most this many
    static constexpr double POINT_SIGMA     = 1.0;    // scatter of one good measurement, frames (measured: 1-1.5 at r 0.96)
    static constexpr double POINT_SIGMA_R   = 4.0;    // ... plus this x (1 - r): weaker matches scatter more
    static constexpr double MEASURED_SPAN_S = 6.0;    // "measured" needs 3 points over at least this long
    static constexpr double DOUBT_SIGMAS    = 3.0;    // measurements this clearly against the expected slope win outright
    static const int        SETTLED_POINTS  = 5;      // from this many points on, the line is trusted enough to ...
    static constexpr double MISS_SIGMAS     = 3.5;    // ... leave out a single point this far off it
    static constexpr double HEAL_FRAMES     = 30.0;   // a real step up to this size is steered away again (see addMeasurement)
    // -- the steering --
    static constexpr double PULL_FAST_S     = 4.0;    // close a leftover distance in about this long while the line is young ...
    static constexpr double PULL_S          = 15.0;   // ... and this gently once it is settled (see update)
    static constexpr double PULL_MAX_PPM    = 40.0;   // ... but never faster than this
    static constexpr double HEAL_S          = 1.0;    // steering a small step away (see addMeasurement): much quicker ...
    static constexpr double HEAL_MAX_PPM    = 250.0;  // ... 11 frames a second; 250 ppm is 1/200 of a semitone, for a second or two
    static constexpr double RATE_MAX_PPM    = 300.0;  // total correction limit

    Tracker() { reset(); }

    // A new tune: everything starts over (another station, another carrier; the crystal may
    // have warmed up meanwhile).
    void reset() {
        std::lock_guard<std::mutex> lock(m_);
        t_ = 0; guess_ = 0; haveGuess_ = false;
        kept_ = 0; haveKept_ = false;
        epoch_ = -1; restart();
        cpFill_ = 0; cpNext_ = 0;
        st_ = Status();
    }

    // false = no correction at all (the rate is exactly 0, as before this file existed).
    // For tests: it shows what the correction is worth.
    void setEnabled(bool on) {
        std::lock_guard<std::mutex> lock(m_);
        enabled_ = on;
    }

    // Streaming thread, once per block, AFTER the block's audio went through the Stretch:
    //   frame       the time line position (the aligner's frame count) of the next analog frame
    //   slip        Stretch::slip() now
    //   offsetHz    the station's distance from the centre as the demodulator sees it; stationHz
    //               = the tuned frequency; blockSeconds = how much time this block covers
    // Returns the rate for the next block.
    double update(long long frame, double slip, double offsetHz, double stationHz, double blockSeconds) {
        std::lock_guard<std::mutex> lock(m_);
        // where the Stretch was when: the measurements arrive a moment later and refer to the past
        cp_[cpNext_] = { frame, slip };
        cpNext_ = (cpNext_ + 1) % CP; if (cpFill_ < CP) cpFill_++;
        // a) the first guess from the tuning error
        t_ += blockSeconds;
        if (stationHz > 1e6 && t_ >= GUESS_AFTER_S) {
            double g = -offsetHz / stationHz;
            if (std::fabs(g) <= GUESS_MAX_PPM * 1e-6) {
                if (!haveGuess_) { guess_ = g; haveGuess_ = true; }
                else guess_ += (g - guess_) * std::min(1.0, blockSeconds / GUESS_SMOOTH_S);
            }
        }
        // b) the measured line, if there is one, and the pull toward it
        double clock = pts_.empty() ? expected() : fitB_;
        double pull = 0, err = 0;
        if (!pts_.empty()) {
            // the slip that would put the analog exactly `target_` frames behind the HD right now
            double want = fitA_ + clock * (double)(frame - fitC0_) - (double)target_;
            err = want - slip;
            // How fast to close the distance? Quickly while the slope is still being found (a
            // wrong slope must not carry the analog away). Gently afterwards: every new
            // measurement nudges the line by a fraction of a frame, and chasing each nudge at
            // once would make the very next measurement look "moved" to the aligner.
            pull = err / ((pts_.size() >= (size_t)SETTLED_POINTS ? PULL_S : PULL_FAST_S) * RATE);
            pull = std::max(-PULL_MAX_PPM * 1e-6, std::min(PULL_MAX_PPM * 1e-6, pull));
            if (healing_) {                       // a small step is being steered away: get it over with
                pull = std::max(-HEAL_MAX_PPM * 1e-6, std::min(HEAL_MAX_PPM * 1e-6, err / (HEAL_S * RATE)));
                if (std::fabs(err) < 0.5) healing_ = false;
            }
        }
        double rate = std::max(-RATE_MAX_PPM * 1e-6, std::min(RATE_MAX_PPM * 1e-6, clock + pull));
        if (!enabled_) rate = 0;
        st_.ppm = rate * 1e6; st_.errorFrames = err;
        st_.guessPpm = guess_ * 1e6; st_.haveGuess = haveGuess_;
        publish();
        return rate;
    }

    // Engine thread: one ACCEPTED measurement of the aligner (see align::Result):
    //   at       the time line position of the middle of the analog stretch it compared
    //   offset   analog frame - HD frame of the same sound, with its fraction
    //   corr     how well the two matched (0.5 .. 1)
    //   epoch    the aligner's counter of "the HD audio was anchored afresh / the offset
    //            jumped": figures from different epochs can't be compared
    //   agreed   the aligner now calls the alignment "ok" (its last two results agree) ...
    //   settled  ... and this is the offset it reports (the middle one of its last results) -
    //            the one the blend will use.
    void addMeasurement(long long at, double offset, float corr, int epoch, bool agreed, long long settled) {
        std::lock_guard<std::mutex> lock(m_);
        if (epoch != epoch_) { epoch_ = epoch; restart(); }   // a new start for the offsets (the slope is kept: same crystal)
        // Undo our own correction: the offset the aligner WOULD have seen without the Stretch.
        Point p;
        p.c = at; p.y = offset + slipAt(at);
        p.sigma = POINT_SIGMA + POINT_SIGMA_R * (1.0 - std::min(1.f, std::max(0.f, corr)));
        bool stepped = false; double step = 0;
        if (measured_ && pts_.size() >= (size_t)SETTLED_POINTS) {
            // The line is well known by now. A point far off it is a bad measurement - leave
            // it out. But two in a row that agree with EACH OTHER are a real step: the dongle
            // lost a few samples on the way (a USB hiccup: every lost sample moves the analog
            // 1/33.75 of a frame against the HD), or the station's delay moved. Then the line
            // starts over from those two points - with the slope we had, it is the same crystal.
            double expect = fitA_ + fitB_ * (double)(p.c - fitC0_);
            if (std::fabs(p.y - expect) > MISS_SIGMAS * p.sigma + 1.0) {
                bool second = misses_ > 0 && std::fabs((p.y - lastMiss_.y) - fitB_ * (double)(p.c - lastMiss_.c)) <= MISS_SIGMAS * p.sigma + 1.0;
                misses_++;
                if (!second) { lastMiss_ = p; st_.skipped++; publish(); return; }
                Point first = lastMiss_;
                const long long targetWas = target_; const bool heldWas = held_;
                restart();
                st_.restarts++;
                pts_.push_back(first);
                stepped = true; step = p.y - expect;
                target_ = targetWas; held_ = heldWas;
            }
        }
        misses_ = 0;
        pts_.push_back(p);
        while (pts_.size() > (size_t)MAX_POINTS || (double)(pts_.back().c - pts_.front().c) > WINDOW_S * RATE)
            pts_.erase(pts_.begin());
        fit();
        // Where shall the analog sit? Once the aligner says "ok", exactly at the offset it
        // reports - that is the figure the blend lines the two streams up with, so from then
        // on the Stretch holds the analog THERE. Before that (slope not known yet): where it
        // is now - pulling it back to an earlier place would only delay the "ok".
        if (stepped) {
            // After a step: the aligner's "settled" figure is still the OLD offset (it is the
            // middle one of its last nine results and takes half a minute to follow), and that
            // is what the blend keeps using. A small step is therefore simply steered away:
            // the old place stays the target, and the Stretch moves the analog back there in
            // two or three seconds (30 frames = 0.7 ms, at a rate nobody can hear). A big one
            // would take too long - there the new place is the target at once, and the aligner
            // and the blend follow in their own time.
            if (!held_ || std::fabs(step) > HEAL_FRAMES) { target_ = (long long)std::llround(fitA_ - slipAt(fitC0_)); held_ = true; }
            else healing_ = true;
        } else if (!held_) {
            if (agreed) { target_ = settled; held_ = true; }
            else target_ = (long long)std::llround(fitA_ - slipAt(fitC0_));
        }
        publish();
    }

    // The dongle's clock error as best known (the slope alone, without the steering) - what
    // to assume while the analog audio is switched off and the Stretch stands still.
    double clock() const {
        std::lock_guard<std::mutex> lock(m_);
        return !enabled_ ? 0.0 : pts_.empty() ? expected() : fitB_;
    }
    Status status() const { std::lock_guard<std::mutex> lock(m_); return st_; }

private:
    struct Point { long long c = 0; double y = 0, sigma = 1; };
    struct Checkpoint { long long frame = 0; double slip = 0; };
    static const int CP = 256;                        // ~22 s of blocks

    // What status() shows (call with m_ held).
    void publish() {
        // "measured" also while a fresh run of points still leans on the slope measured before
        st_.state = !enabled_ ? 0 : (measured_ || useKept_) ? 2 : (haveGuess_ || !pts_.empty()) ? 1 : 0;
        st_.clockPpm = (pts_.empty() ? expected() : fitB_) * 1e6;
        st_.points = (int)pts_.size();
    }

    // The slope we expect before the audio has had its say: our own result from before the
    // offsets started over (same crystal), else the first guess, else nothing.
    double expected() const { return useKept_ ? keptThen_ : haveGuess_ ? guess_ : 0.0; }
    double expectedSigma() const { return (useKept_ ? KEPT_SIGMA_PPM : haveGuess_ ? GUESS_SIGMA_PPM : BLIND_SIGMA_PPM) * 1e-6; }

    // The offsets start over (call with m_ held); what we know about the crystal stays.
    void restart() {
        pts_.clear(); measured_ = false; held_ = false; misses_ = 0; healing_ = false;
        useKept_ = haveKept_; keptThen_ = kept_;       // (a copy: kept_ itself moves on with every new fit)
        fitA_ = 0; fitB_ = expected(); fitC0_ = 0; target_ = 0;
    }

    // The Stretch's slip at time line position `frame` (straight line between two checkpoints).
    double slipAt(long long frame) const {
        if (cpFill_ == 0) return 0;
        int newest = (cpNext_ + CP - 1) % CP, oldest = (cpNext_ + CP - cpFill_) % CP;
        if (frame >= cp_[newest].frame) return cp_[newest].slip;
        if (frame <= cp_[oldest].frame) return cp_[oldest].slip;
        for (int k = 0, i = newest; k < cpFill_ - 1; k++) {
            int prev = (i + CP - 1) % CP;
            if (frame >= cp_[prev].frame) {
                double span = (double)(cp_[i].frame - cp_[prev].frame);
                double f = span > 0 ? (double)(frame - cp_[prev].frame) / span : 0;
                return cp_[prev].slip + f * (cp_[i].slip - cp_[prev].slip);
            }
            i = prev;
        }
        return cp_[oldest].slip;
    }

    // Straight line through the points: y = A + B * (c - c0), c0 = the newest point. Least
    // squares, each point weighted by its scatter, plus ONE extra "measurement": the slope
    // we expect, with its uncertainty. With two points 2 s apart the audio says little about
    // the slope (+-20 ppm) and the expectation rules; after half a minute the audio knows it
    // to a fraction of a ppm and the expectation no longer matters. And if the audio clearly
    // contradicts the expectation (the guess was wrong - a station far off its frequency, a
    // dongle that isn't built the usual way), the expectation is dropped at once.
    void fit() {
        const size_t n = pts_.size();
        fitC0_ = pts_.back().c;
        double b0 = expected(), s0 = expectedSigma();
        if (n == 1) { fitA_ = pts_[0].y; fitB_ = b0; measured_ = false; st_.spreadFrames = 0; return; }
        // work in seconds so the numbers stay tame: slope in frames per second
        double sw = 0, swt = 0, swtt = 0, swy = 0, swty = 0;
        for (const Point &p : pts_) {
            double w = 1.0 / (p.sigma * p.sigma), t = (double)(p.c - fitC0_) / RATE;
            sw += w; swt += w * t; swtt += w * t * t; swy += w * p.y; swty += w * t * p.y;
        }
        const double span = (double)(pts_.back().c - pts_.front().c) / RATE;
        double det = sw * swtt - swt * swt;
        if (n >= 3 && span >= MEASURED_SPAN_S && det > 0) {
            // what the audio alone says, and how sure it is
            double alone = (sw * swty - swt * swy) / det, sure = std::sqrt(sw / det);
            if (std::fabs(alone - b0 * RATE) > DOUBT_SIGMAS * (sure + s0 * RATE)) s0 = BLIND_SIGMA_PPM * 1e-6;
        }
        double lam = 1.0 / ((s0 * RATE) * (s0 * RATE));
        swtt += lam; swty += lam * (b0 * RATE);
        det = sw * swtt - swt * swt;
        if (det <= 0) return;
        fitA_ = (swy * swtt - swt * swty) / det;
        fitB_ = (sw * swty - swt * swy) / det / RATE;                 // back to frames per frame
        double ss = 0;
        for (const Point &p : pts_) { double d = p.y - fitA_ - fitB_ * (double)(p.c - fitC0_); ss += d * d; }
        st_.spreadFrames = std::sqrt(ss / (double)n);
        measured_ = n >= 3 && span >= MEASURED_SPAN_S;
        if (measured_) { kept_ = fitB_; haveKept_ = true; }           // for the next time the offsets start over
    }

    mutable std::mutex m_;
    bool enabled_ = true;
    double t_ = 0;                                   // seconds since the tune
    double guess_ = 0; bool haveGuess_ = false;      // a) from the tuning error
    std::vector<Point> pts_;                         // b) the measurements (raw offsets) since the offsets last started over
    int epoch_ = -1;
    long long target_ = 0; bool held_ = false;       // the offset the analog is held at (held_: fixed, the aligner said ok)
    double fitA_ = 0, fitB_ = 0; long long fitC0_ = 0;
    bool measured_ = false;
    double kept_ = 0; bool haveKept_ = false;        // the slope we measured, kept across restarts ...
    bool useKept_ = false; double keptThen_ = 0;     // ... and whether THIS run of points started with it (and its value then)
    int misses_ = 0; Point lastMiss_;
    bool healing_ = false;                           // a small step is being steered away
    Checkpoint cp_[CP]; int cpFill_ = 0, cpNext_ = 0;
    Status st_;
};

} // namespace drift
