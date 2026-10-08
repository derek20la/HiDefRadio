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
//   Which of the two to believe (build 8)
//   The tuning error and the audio are two witnesses to the same crystal, and they are
//   good at different things. The tuning error is there within a second or two and is
//   never far out - a ppm or two. The audio ends up ten times better, but it needs half a
//   minute of measurements that agree with each other; from two or three of them it can be
//   wildly wrong, and on a station whose analog and HD audio are processed differently
//   (they match only loosely: r 0.5-0.7) measurements keep landing 10-20 frames off.
//   A tester's screen video showed what that does (2026-10-08, a Nooelec dongle +58 ppm
//   fast, a station with r 0.71): two measurements agreed, the third was ~14 frames off,
//   the line through the three said "+103 ppm" - and that was applied. So:
//     - The audio may REFINE what the tuning error says, but not contradict it: the clock
//       figure stays within FENCE_PPM of the tuning error ...
//     - ... unless the audio has a strong case: STRONG_POINTS measurements over
//       STRONG_SPAN_S that lie on a straight line as tightly as a normal station's do, and
//       that OVERRULE_FITS times running. Then it is the tuning error that is wrong (a
//       station off its frequency, a dongle that isn't built the usual way) and the audio
//       takes over. That costs such a dongle half a minute more; nothing else can tell
//       the two cases apart.
//     - Before the audio gets a say at all, its measurements are checked against each
//       other: there must be four of them, and they must lie on a straight line about as
//       tightly as a normal station's do. On a station where they scatter, the tuning
//       error alone sets the correction - and the odd ones out are not mistaken for
//       lost samples.
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
    int    state = 0;        // 0 = nothing known (no correction), 1 = from the tuning error, 2 = measured from the audio
    double ppm = 0;          // the correction in use right now, parts per million (> 0 = the dongle's clock is fast)
    double clockPpm = 0;     // the dongle's clock error as best known (the fitted slope, or the tuning error's figure)
    double guessPpm = 0;     // what the tuning error says (0 until it has settled)
    bool   haveGuess = false;
    int    points = 0;       // measurements in the fit
    double spreadFrames = 0; // rms distance of those measurements from the fitted line
    double errorFrames = 0;  // how far the analog is from where it should sit right now (the Stretch is closing this)
    int    restarts = 0;     // times the fit started over because the offset really moved (not counting re-anchored HD)
    int    skipped = 0;      // single measurements left out because they were far off the line
    int    unused = 0;       // one of the two witnesses was not believed: 1 = the audio (too far from the tuning error,
                             // and no strong case), 2 = the tuning error (the audio proved it wrong)
    double unusedPpm = 0;    // ... and what that witness says
    double scatter = 1;      // how much more the measurements scatter than a normal station's (1 = as usual)
};

class Tracker {
public:
    // -- the first guess --
    static constexpr double GUESS_AFTER_S   = 2.0;    // the demodulator's offset reading needs this long to settle
    static constexpr double GUESS_SMOOTH_S  = 4.0;    // ... and wobbles with the music (+-100 Hz and more): average it over
    static constexpr double GUESS_SMOOTH_MAX_S = 20.0; // this long at first, and longer as time goes by, up to this (build 8:
                                                      // on a station whose audio can't be used, this reading IS the correction)
    static constexpr double GUESS_MAX_PPM   = 160.0;  // more than this is no crystal error (the worst dongles: ~100) - an
                                                      // empty channel can read anything (seen: "+171 ppm" on 87.75)
    static constexpr double GUESS_SIGMA_PPM = 3.0;    // how far we trust it (1 sigma): our reading wobbles ~1 ppm, an HD
                                                      // station's carrier is good to ~1 ppm (seen: guess within 1.5 ppm)
    static constexpr double KEPT_SIGMA_PPM  = 2.0;    // trust in OUR last measurement after the HD audio was re-anchored
    static constexpr double BLIND_SIGMA_PPM = 150.0;  // no guess at all: let the measurements decide alone
    // -- the fit --
    static constexpr double WINDOW_S        = 60.0;   // fit the measurements of the last minute (one every 5 s) ...
    static const int        MAX_POINTS      = 13;     // ... at most this many
    static constexpr double POINT_SIGMA     = 1.0;    // scatter of one good measurement, frames (measured: 1-1.5 at r 0.96)
    static constexpr double POINT_SIGMA_R   = 4.0;    // ... plus this x (1 - r): weaker matches scatter more
    static constexpr double MEASURED_SPAN_S = 6.0;    // a line through the audio needs 3 points over at least this long
    static constexpr double DOUBT_SIGMAS    = 3.0;    // measurements this clearly against the expected slope win outright
    static const int        SETTLED_POINTS  = 5;      // from this many points on, the line is trusted enough to ...
    static constexpr double MISS_SIGMAS     = 3.5;    // ... leave out a single point this far off it
    static constexpr double HEAL_FRAMES     = 30.0;   // a real step up to this size is steered away again (see addMeasurement)
    // -- which witness to believe (see "Which of the two to believe" at the top) --
    static const int        AUDIO_POINTS    = 4;      // fewer measurements than this can't be checked against each other (see fit)
    static constexpr double FENCE_PPM       = 8.0;    // without a strong case the clock figure stays this close to the tuning
                                                      // error (which is good to 1-3 ppm: this is its "can't be further out").
                                                      // 8 ppm is 2 frames between two measurements - the most that still
                                                      // lets the aligner call them "the same" (its AGREE_FRAMES is 3).
    static const int        STRONG_POINTS   = 6;      // a strong case: at least this many measurements ...
    static constexpr double STRONG_SPAN_S   = 20.0;   // ... over at least this long ...
    static constexpr double STRONG_SURE_PPM = 2.0;    // ... which pin the slope down to this (1 sigma), scattering no more than usual
    static const int        OVERRULE_FITS   = 3;      // ... and against the tuning error this many measurements running
    static constexpr double SCATTER_DOUBT   = 2.0;    // measurements scattering more than this x what we assume: no say on the slope
    static constexpr double SCATTER_FADE    = 0.85;   // ... and that is forgotten only this fast (per measurement) once they stop
    static constexpr double GOOD_SURE_PPM   = 3.0;    // "measured": the audio alone knows the slope at least as well as the
                                                      // tuning error does (a normal station: after 4-5 measurements, ~20 s)
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
        overruled_ = false; against_ = 0; scatter_ = 1;
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
                // averaged over about half the time since it became usable: quick to settle,
                // steadier later, and still following a crystal that warms up (20 ppm in ten
                // minutes is 0.03 ppm a second)
                const double smooth = std::min(GUESS_SMOOTH_MAX_S, std::max(GUESS_SMOOTH_S, 0.5 * (t_ - GUESS_AFTER_S)));
                if (!haveGuess_) { guess_ = g; haveGuess_ = true; }
                else guess_ += (g - guess_) * std::min(1.0, blockSeconds / smooth);
            }
        }
        // b) the measured line, if there is one, and the pull toward it
        double clock = slope();
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
        if (good_ && pts_.size() >= (size_t)SETTLED_POINTS) {
            // The line is well known by now (good_: it is the audio's own line, and a tight one -
            // not a line held at the fence, which the audio is busy contradicting, and not one on
            // a station whose measurements scatter: there 10 frames off is nothing special, and
            // must not be mistaken for lost samples). A point far off it is a bad measurement - leave
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
        return !enabled_ ? 0.0 : slope();
    }
    Status status() const { std::lock_guard<std::mutex> lock(m_); return st_; }

private:
    struct Point { long long c = 0; double y = 0, sigma = 1; };
    struct Checkpoint { long long frame = 0; double slip = 0; };
    static const int CP = 256;                        // ~22 s of blocks

    // What status() shows (call with m_ held).
    void publish() {
        // "measured" = the audio's line is in use and good enough to deserve the word (fit()) -
        // also while a fresh run of points still leans on the slope measured before
        st_.state = !enabled_ ? 0 : (good_ || (useKept_ && !fenced_)) ? 2 : (haveGuess_ || !pts_.empty()) ? 1 : 0;
        st_.clockPpm = slope() * 1e6;
        st_.points = (int)pts_.size();
        st_.scatter = scatter_;
        st_.unused = (fenced_ && haveAudio_) ? 1 : (overruled_ && haveGuess_) ? 2 : 0;
        st_.unusedPpm = (st_.unused == 1 ? audio_ : st_.unused == 2 ? guess_ : 0.0) * 1e6;
    }

    // The slope we expect before the audio has had its say: our own result from before the
    // offsets started over (same crystal), else the tuning error's (unless the audio has
    // proved that one wrong), else nothing.
    bool trustGuess() const { return haveGuess_ && !overruled_; }
    // The slope in use: the fitted line's - or, while the audio has no say on it (leaning_,
    // see fit), the expectation as it is NOW: the tuning error is read all the time and
    // may have moved on since the last measurement.
    double slope() const { return pts_.empty() || leaning_ ? expected() : fitB_; }
    double expected() const { return useKept_ ? keptThen_ : trustGuess() ? guess_ : 0.0; }
    double expectedSigma() const { return (useKept_ ? KEPT_SIGMA_PPM : trustGuess() ? GUESS_SIGMA_PPM : BLIND_SIGMA_PPM) * 1e-6; }

    // The offsets start over (call with m_ held); what we know about the crystal stays.
    void restart() {
        pts_.clear(); good_ = false; fenced_ = false; leaning_ = false; haveAudio_ = false; against_ = 0;
        held_ = false; misses_ = 0; healing_ = false;
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
    // we expect, with its uncertainty. With a few points the audio says little about the
    // slope and the expectation rules; after half a minute the audio knows it to a fraction
    // of a ppm and the expectation no longer matters.
    //
    // Build 8 - who may overrule whom (the reasons are at the top of the file):
    //   1. The audio has a say on the SLOPE only when its points can be checked against each
    //      other and pass: at least AUDIO_POINTS of them (three - two of them 2 s apart, the
    //      third 6 s later - always "lie on a line": if one is a bad one, nothing in them
    //      shows it), and lying on their own best line about as tightly as a normal
    //      station's do (not more than SCATTER_DOUBT times what we assume for them). Until
    //      then, and on a station that scatters, the slope is the expected one and the
    //      points only say where the line sits.
    //   2. Only a STRONG case - more measurements, over longer - may throw the expectation
    //      out altogether. (Build 6 let three points do it.)
    //   3. The fence: unless the audio has made its strong case OVERRULE_FITS times running,
    //      the result stays within FENCE_PPM of what the tuning error says.
    //   With no expectation at all (no usable tuning error, nothing measured before) the
    //   audio decides alone from the second point on, as it always did.
    void fit() {
        const size_t n = pts_.size();
        fitC0_ = pts_.back().c;
        double b0 = expected(), s0 = expectedSigma();
        good_ = false; fenced_ = false; leaning_ = false; haveAudio_ = false;
        if (n == 1) { fitA_ = pts_[0].y; fitB_ = b0; leaning_ = true; against_ = 0; st_.spreadFrames = 0; return; }
        // work in seconds so the numbers stay tame: slope in frames per second
        double sw = 0, swt = 0, swtt = 0, swy = 0, swty = 0;
        for (const Point &p : pts_) {
            double w = 1.0 / (p.sigma * p.sigma), t = (double)(p.c - fitC0_) / RATE;
            sw += w; swt += w * t; swtt += w * t * t; swy += w * p.y; swty += w * t * p.y;
        }
        const double span = (double)(pts_.back().c - pts_.front().c) / RATE;
        double det = sw * swtt - swt * swt;
        double sure = 1e9;                              // how well the audio ALONE knows the slope (1 sigma, frames/s)
        bool strong = false;
        haveAudio_ = n >= 3 && span >= MEASURED_SPAN_S && det > 0;    // enough points for a line of the audio's own
        if (haveAudio_) {
            const double alone = (sw * swty - swt * swy) / det;
            audio_ = alone / RATE;                                    // (kept for the status)
            // 1. The scatter: how far the points lie from their OWN best line, in units of
            //    what we assume for them (1 = as assumed; a normal station: 1 to 1.5). A
            //    station that has scattered stays under suspicion for a while - the figure
            //    comes down only slowly (SCATTER_FADE per measurement: from 3 back to 1 takes
            //    ~40 s of good ones) - or one on the border would be trusted every other time.
            double now = 1.0;
            if (n >= (size_t)AUDIO_POINTS) {
                const double a0 = (swy * swtt - swt * swty) / det;
                double chi = 0;
                for (const Point &p : pts_) {
                    double d = (p.y - a0 - alone * (double)(p.c - fitC0_) / RATE) / p.sigma;
                    chi += d * d;
                }
                const double s = std::sqrt(chi / (double)(n - 2));
                if (s > SCATTER_DOUBT) now = s;
            }
            scatter_ = std::max(now, std::max(1.0, scatter_ * SCATTER_FADE));
            sure = std::sqrt(sw / det);
            // 2. a strong case?
            strong = n >= (size_t)STRONG_POINTS && span >= STRONG_SPAN_S && scatter_ == 1.0
                     && sure <= STRONG_SURE_PPM * 1e-6 * RATE;
            // 3. ... against the tuning error, and for how long now?
            if (trustGuess()) {
                against_ = strong && std::fabs(alone / RATE - guess_) > FENCE_PPM * 1e-6 ? against_ + 1 : 0;
                if (against_ >= OVERRULE_FITS) {        // the tuning error is out, for the rest of this tune
                    overruled_ = true;
                    b0 = expected(); s0 = expectedSigma();
                }
            }
            // ... or against our own earlier result (after the offsets started over)
            if (strong && std::fabs(alone - b0 * RATE) > DOUBT_SIGMAS * (sure + s0 * RATE)) s0 = BLIND_SIGMA_PPM * 1e-6;
        } else against_ = 0;
        const bool checked = n >= (size_t)AUDIO_POINTS && scatter_ == 1.0;   // 1.: the audio has a say on the slope
        const bool lean = useKept_ || trustGuess();                          // there is an expectation to lean on
        if (lean && !checked) {
            fitB_ = b0;                                               // the expected slope ...
            fitA_ = (swy - fitB_ * RATE * swt) / sw;                  // ... and the best line WITH that slope
            leaning_ = true;
        } else {
            // the expectation as one more measurement
            const double lam = 1.0 / ((s0 * RATE) * (s0 * RATE));
            const double swttE = swtt + lam, swtyE = swty + lam * (b0 * RATE);
            det = sw * swttE - swt * swt;
            if (det <= 0) return;
            fitA_ = (swy * swttE - swt * swtyE) / det;
            fitB_ = (sw * swtyE - swt * swy) / det / RATE;            // back to frames per frame
        }
        // 3. the fence
        if (trustGuess() && std::fabs(fitB_ - guess_) > FENCE_PPM * 1e-6) {
            fitB_ = guess_ + (fitB_ > guess_ ? FENCE_PPM : -FENCE_PPM) * 1e-6;
            fitA_ = (swy - fitB_ * RATE * swt) / sw;
            fenced_ = true; leaning_ = false;
        }
        double ss = 0;
        for (const Point &p : pts_) { double d = p.y - fitA_ - fitB_ * (double)(p.c - fitC0_); ss += d * d; }
        st_.spreadFrames = std::sqrt(ss / (double)n);
        // "measured": the audio's own line is in use (checked, or there was nothing else),
        // not held back at the fence, and it knows the slope well enough
        good_ = (checked || !lean) && scatter_ == 1.0 && !fenced_ && sure <= GOOD_SURE_PPM * 1e-6 * RATE;
        if (good_) { kept_ = fitB_; haveKept_ = true; }               // for the next time the offsets start over
    }

    mutable std::mutex m_;
    bool enabled_ = true;
    double t_ = 0;                                   // seconds since the tune
    double guess_ = 0; bool haveGuess_ = false;      // a) from the tuning error
    std::vector<Point> pts_;                         // b) the measurements (raw offsets) since the offsets last started over
    int epoch_ = -1;
    long long target_ = 0; bool held_ = false;       // the offset the analog is held at (held_: fixed, the aligner said ok)
    double fitA_ = 0, fitB_ = 0; long long fitC0_ = 0;
    bool good_ = false;                              // "measured": the audio's line is in use and knows the slope well enough
    bool fenced_ = false;                            // the fit wanted further from the tuning error than it may go
    bool leaning_ = false;                           // the slope is the expectation's: the audio has no say on it (yet)
    double audio_ = 0; bool haveAudio_ = false;      // what the audio alone says (for the status), once it has a line of its own
    double scatter_ = 1;                             // how much more the measurements scatter than assumed (1 = as assumed)
    int against_ = 0; bool overruled_ = false;       // the audio's strong case against the tuning error: fits running / won
    double kept_ = 0; bool haveKept_ = false;        // the slope we measured, kept across restarts ...
    bool useKept_ = false; double keptThen_ = 0;     // ... and whether THIS run of points started with it (and its value then)
    int misses_ = 0; Point lastMiss_;
    bool healing_ = false;                           // a small step is being steered away
    Checkpoint cp_[CP]; int cpFill_ = 0, cpNext_ = 0;
    Status st_;
};

} // namespace drift
