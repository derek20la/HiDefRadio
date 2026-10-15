// blend.hpp - the analog <-> HD blend for the "HD Radio: Auto" setting.
//
// What a car radio does: play the analog FM at once, and switch to the HD audio
// when it's there and good - without a skip, because the station delays its
// analog by the same ~2.5 s the HD decoder needs (the "diversity delay").
// nrsc5 is faster than a car radio, so here the HD audio comes out ~2.5 s
// BEFORE the matching analog audio (the exact figure and the loudness
// difference are measured by aligner.hpp - they differ per station:
// KRTH 2485 ms, KLOS 2432 ms and +4.2 dB).
//
// THE TRICK THIS FILE IS BUILT ON - a radio that can see the future:
//
//     time line ->              now (what you hear)
//     analog  ......................|............
//     HD      .........|.......................XXXX......   (XXXX = a dropout)
//                      |<- 2.5 s ->|
//                 same sound as "now"         ^ known 2.5 s before it would be heard
//
// Because the HD audio for any moment arrives ~2.5 s before the analog audio of that
// moment, we always hold 2.5 s of HD audio "from the future". A car radio only learns
// about a dropout when it happens and must cover it in a hurry; here every dropout is
// seen coming, so the blend fades to the analog BEFORE the gap, smoothly, and the
// listener never hears the gap at all.
//
// So this class keeps
//   - the analog audio (from fmdemod via native-lib, stereo) in a ring, indexed
//     by analog frame number since the tune (frame n = IQ time n * 33.75), and
//   - the HD1 audio (stereo, from nrsc5) in a bigger ring, indexed by the
//     aligner's time line (the same frame numbers, see Aligner::pushHd),
//     with one flag per frame: good, or "bad" (a 2,048-frame piece that is all
//     zero = a dropout, a damaged packet or a sync loss - nrsc5 fills those
//     with silence) or not received (yet).
// The play position `play_` walks along the ANALOG time line. The HD frame
// that carries the same sound is play_ - offset (offset = the aligner's
// offsetFrames, > 0 = HD ahead). Because the HD is ~2.5 s ahead, we can look
// AHEAD in the HD: a dropout is known 2.5 s before it would be heard, so the
// blend fades to analog before the gap and comes back once the next 2 s of HD
// are clean. Output = (1 - g) * analog * gainMatch + g * HD (per channel), where g ramps
// with a raised cosine over FADE_FRAMES (0.5 s, approved by ear on the first blends; a
// raised cosine - half a cosine wave from 0 to 1 - starts and ends gently, so the fade has
// no corner you could hear) and
// gainMatch brings the analog to the HD's loudness (so the HD sounds exactly
// as it does in "Digital only").
//
// States: ANALOG -> TO_HD -> HD -> TO_ANALOG -> ANALOG. A fade can reverse
// half-way (the ramp position just runs the other way).
//
// "Unaligned" HD: if the aligner never finds a match (the HD on this frequency
// is another station's - see Radio World, "An unintended consequence of digital
// radio" - or the station processes its HD so differently that the two don't
// correlate), the "unmatched HD" setting decides: play the HD anyway (offset chosen
// so the HD plays ~0.3 s behind live, like Digital only: a one-time jump of
// ~2.5 s) or stay on the analog.
//
// "The HD plays alone": two stations can share a frequency (KDSC's HD and the
// analog of XETRA-FM, "91X", on 91.1 west of Los Angeles, 2026-10-09). Then the analog is
// NOT this HD's programme, and falling back on it in every dropout swaps between two
// stations - five times a minute that evening. stationid.hpp works out when that is the
// case (setAnalogForeign), and while it is, the HD is treated like an HD2 sub-channel,
// which has no analog either:
//   - the HD comes in once it has been good for 2 s (a normal 0.5 s crossfade from the
//     other station, which is what was playing), unaligned, 0.6 s behind live;
//   - a dropout ahead -> the HD fades out to SILENCE (fast: the look-ahead is only 0.2 s);
//     the HD back -> the HD again at once. No waiting times in this mode;
//   - safety net: 10 s of silence -> the other station's analog is given back (someone who
//     drives out of the HD's range must not sit in silence for ever), and the HD returns
//     only after it has been good for 5 s;
//   - with the setting "stay on analog" the HD is never put on;
//   - the contest over -> a fade back to the analog and the ordinary, aligned blend.
// The silence is made by a second gain on the analog leg (aGain_, 1 = heard, 0 = muted)
// that is only applied when it is not 1 - so on every ordinary station the samples come
// out exactly as before.
//
// Threads: pushAnalog/pushHd on the streaming thread, read() on the audio
// thread, status()/reset() from wherever. One mutex (m_), a LEAF lock: it is
// taken while native-lib holds g_statusMutex (pushHd) - never the other way.
// Plain C++17, no dependencies except aligner.hpp's Status struct.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>       // std::llabs
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "aligner.hpp"

namespace blend {

static const int RATE = 44100;
static const size_t ANALOG_RING = (size_t)RATE * 4;       // 4 s of analog (stereo frames; only ~0.3 s is ever waiting)
static const size_t HD_RING = (size_t)RATE * 6;           // 6 s of stereo: holds the ~2.5 s delay + look-ahead
static const long long FADE_FRAMES = RATE / 2;            // 0.5 s crossfade (raised cosine)
static const long long GUARD_FRAMES = RATE * 3 / 20;      // the fade must END this long (0.15 s) before a bad frame
static const long long CLEAN_FRAMES = RATE * 2;           // HD must be good this far ahead before we fade to it
static const long long RESTART_LATENCY = RATE * 3 / 10;   // play 0.3 s behind the newest analog after a (re)start
static const long long UNALIGNED_LATENCY = RATE * 3 / 10; // unaligned HD plays this far behind live (like Digital only)
static const long long UNALIGNED_CLEAN = RATE / 5;        // ... so only this much look-ahead exists then
static const long long NOMATCH_FRAMES = RATE * 20;        // "no match" counts from this long after tuning
static const long long HD_HELD_RESET = RATE * 10;         // HD held this long = the flapping counter starts over
static const int OFFSET_TOLERANCE = 3;                    // the aligner's median may wobble this much (frames)
static const float GAIN_SLEW_DB_PER_S = 2.0f;             // how fast the analog level follows the measured match
static const float MAX_MATCH_DB = 12.0f;                  // never scale the analog by more than this either way
// After a fade back to analog, wait this long before trying HD again - longer
// each time it happens again soon (a signal on the edge would otherwise flap).
// (Tuned on 20 recordings with the antenna moved every ~10 s, 2026-09-28: 5/15/30 s and a
//  60 s reset kept the analog playing far longer than the dropouts themselves.)
static const long long DWELL_FRAMES[] = { 0, RATE * 3, RATE * 8, RATE * 15 };
// "The HD plays alone" (see the top of the file):
static const long long ALONE_FIRST_GOOD = RATE * 2;       // the HD must have been good this long before it comes in
static const long long ALONE_NET_FRAMES = RATE * 10;      // this much silence -> the other station's analog is given back
static const long long ALONE_NET_GOOD = RATE * 5;         // ... and the HD must then be good this long to return
static const long long ALONE_LATENCY = RATE * 6 / 10;      // the HD alone plays this far behind live. More than the 0.3 s
                                                          // of UNALIGNED_LATENCY: nrsc5 hands its audio over in steps
                                                          // of ~0.09 s, and with only 0.1 s to spare a late step looked
                                                          // like a dropout (seen in the harness: one false fade). There
                                                          // is nothing to keep in time with here, so the room is free.
static const long long ALONE_FAST = 4;                    // fade to / from silence 4x as fast as a crossfade: 0.125 s
                                                          // (0.125 s + one decision every 0.05 s < the 0.2 s look-ahead)

enum State { ANALOG = 0, TO_HD = 1, HD = 2, TO_ANALOG = 3 };

// "Is the HD the same station as the analog?" (setSameStation)
enum SameStation { SAME_UNKNOWN = 0, SAME_YES = 1, SAME_NO = 2 };

struct Status {
    State state = ANALOG;
    bool aligned = false;         // HD (when playing) is time-aligned with the analog
    bool mismatch = false;        // the aligner says the HD1 and the analog don't match (another station?)
    int toHd = 0, toAnalog = 0;   // fades started, since the tune
    long long hdFrames = 0;       // frames played from the HD (blended) since the tune
    long long hdSince = 0;        // frames since the current HD stretch began (0 when not on HD)
    float matchDb = 0;            // the analog level correction in use (dB, negative = turned down)
    std::string reason;           // why the last fade happened (for the details)
    // The analog on this frequency is another station's (setAnalogForeign)
    bool alone = false;           // ... and the HD stands alone: it plays, or the silence waits for it
    bool silent = false;          // ... the silence is waiting right now
    long long silentFrames = 0;   // ... for this long
    int silences = 0;             // fades to silence, since the tune
};

class Blender {
public:
    Blender() { reset(); }

    // A new tune: everything starts over. (Not while read() may be running with
    // stale indices - native-lib stops the audio player before it retunes.)
    void reset() {
        std::lock_guard<std::mutex> lock(m_);
        analog_.assign(ANALOG_RING * 2, 0);
        hd_.assign(HD_RING * 2, 0);
        hdGood_.assign(HD_RING, 0);
        aWritten_ = 0; hdFrontier_ = 0; hdOldest_ = 0;
        play_ = 0; restart_ = true;
        state_ = ANALOG; ramp_ = 0; offset_ = 0; aligned_ = false;
        returns_ = 0; dwellUntil_ = 0; hdStreak_ = 0;
        matchDb_ = 0; matchTargetDb_ = 0;
        alone_ = false; heardAnalog_ = true; aGain_ = 1.0f; rampStep_ = 1;     // "the HD plays alone" 
        hdGoodRun_ = 0; silentFrames_ = 0; needGood_ = ALONE_FIRST_GOOD;
        st_ = Status();
    }

    // The "unmatched HD" setting: play the HD even when it doesn't match the analog.
    void setUnmatchedPlaysHd(bool yes) {
        std::lock_guard<std::mutex> lock(m_);
        unmatchedPlaysHd_ = yes;
    }

    // Is the HD the same station as the analog? Judged by native-lib (stationid.hpp) from the
    // analog's RDS PI code and the HD's call sign: SAME_UNKNOWN / SAME_YES / SAME_NO. Only matters
    // when the aligner finds NO match: SAME_YES = it's this station's HD, just processed so
    // differently that the audio doesn't correlate (KKLQ) -> play it whatever the setting
    // says; otherwise the "unmatched HD" setting decides, as before. A match always wins
    // (a PI can be out of date). Not cleared by reset() - native-lib sets it on every tune.
    void setSameStation(int verdict) {
        std::lock_guard<std::mutex> lock(m_);
        sameStation_ = verdict;
    }

    // The analog on this frequency is ANOTHER station's than the HD (judged by
    // stationid.hpp from the RDS codes and the audio) - see "the HD plays alone" at the top.
    // Not cleared by reset() - native-lib sets it on every tune, like setSameStation.
    void setAnalogForeign(bool yes) {
        std::lock_guard<std::mutex> lock(m_);
        foreign_ = yes;
    }

    // Streaming thread: analog audio as it comes out of the demodulator (stereo,
    // interleaved L R, `frames` frames; already scaled and clamped to -1..1 - the
    // aligner gets the same audio as mono).
    void pushAnalog(const float *stereo, size_t frames) {
        {
            std::lock_guard<std::mutex> lock(m_);
            for (size_t i = 0; i < frames; i++) {
                size_t slot = (size_t)(aWritten_ % (long long)ANALOG_RING);
                analog_[slot * 2] = (int16_t)(stereo[2 * i] * 32767.0f);
                analog_[slot * 2 + 1] = (int16_t)(stereo[2 * i + 1] * 32767.0f);
                aWritten_++;
            }
        }
        cv_.notify_one();
    }

    // Streaming thread: one HD1 audio piece from nrsc5 (16-bit stereo interleaved,
    // `count` values), placed at frame `index` of the aligner's time line.
    void pushHd(const int16_t *stereo, size_t count, long long index) {
        std::lock_guard<std::mutex> lock(m_);
        size_t frames = count / 2;
        if (index < hdFrontier_) return;                 // never goes backwards (the aligner's anchor rule)
        // A gap (first audio, or a new run after a sync loss): those frames were never received.
        for (long long f = std::max(hdFrontier_, index - (long long)HD_RING); f < index; f++)
            hdGood_[(size_t)(f % (long long)HD_RING)] = 0;
        if (index > hdFrontier_) hdGoodRun_ = 0;          // a gap ends the run of good HD
        // A piece that is all zero is one nrsc5 filled in for a missing or damaged
        // packet (or a sync loss): a dropout, not music.
        bool silent = true;
        for (size_t i = 0; i < count; i++) if (stereo[i] != 0) { silent = false; break; }
        for (size_t i = 0; i < frames; i++) {
            size_t slot = (size_t)((index + (long long)i) % (long long)HD_RING);
            hd_[slot * 2] = stereo[2 * i];
            hd_[slot * 2 + 1] = stereo[2 * i + 1];
            hdGood_[slot] = silent ? 0 : 1;
        }
        hdFrontier_ = index + (long long)frames;
        hdOldest_ = std::max(hdOldest_, hdFrontier_ - (long long)HD_RING);
        hdGoodRun_ = silent ? 0 : hdGoodRun_ + (long long)frames;   // how long the HD has been good without a break
    }

    // The demodulator was switched on while the stream was already running
    // (Digital only -> Auto): the analog time line must stay in step with the
    // IQ time line, so jump the analog frame counter forward to `frame`.
    void skipAnalogTo(long long frame) {
        std::lock_guard<std::mutex> lock(m_);
        if (frame > aWritten_) { aWritten_ = frame; restart_ = true; }
    }

    // The next read() starts afresh near the newest analog (after a program
    // switch back to HD1, or a change of the audio-source setting).
    void restart() {
        std::lock_guard<std::mutex> lock(m_);
        restart_ = true;
    }

    // Analog frames ready to play (for the audio player's pre-buffer).
    size_t available() const {
        std::lock_guard<std::mutex> lock(m_);
        long long p = restart_ ? std::max(0LL, aWritten_ - RESTART_LATENCY) : play_;
        return (size_t)std::max(0LL, aWritten_ - p);
    }

    // Audio thread: up to `frames` stereo frames into `out` (interleaved). Waits up
    // to timeoutMs for analog audio if there is none. `al` = the aligner's status
    // right now (fetched by the caller BEFORE calling, so this lock stays a leaf).
    // Returns the number of frames written (0 = nothing arrived in time).
    size_t read(int16_t *out, size_t frames, int timeoutMs, const align::Status &al) {
        std::unique_lock<std::mutex> lock(m_);
        if (restart_) {
            play_ = std::max(0LL, aWritten_ - RESTART_LATENCY);
            restart_ = false;
            if (state_ != ANALOG) { state_ = ANALOG; ramp_ = 0; }   // start on the analog...
            hdStreak_ = 0; dwellUntil_ = 0;
            // ...unless the HD is ready this very moment (see decide): coming back
            // to HD1 from a sub-channel, the aligner still has its measurement and the HD ring
            // kept filling, so the first decide() below puts us straight on the HD - no detour
            // through 0.5 s of analog and a crossfade. After a tune nothing is measured yet,
            // so there this changes nothing.
            resume_ = true;
        }
        if (play_ < aWritten_ - (long long)ANALOG_RING + RATE / 10) {
            // The reader fell far behind (it was paused?): skip forward, like the rings do.
            play_ = aWritten_ - RESTART_LATENCY;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return aWritten_ > play_; });
        size_t n = (size_t)std::min<long long>((long long)frames, aWritten_ - play_);
        if (n == 0) return 0;

        // Level match (analog -> HD loudness), aimed at the aligner's measurement.
        if (al.accepted > 0) matchTargetDb_ = std::max(-MAX_MATCH_DB, std::min(MAX_MATCH_DB, -al.gainDb));
        else matchTargetDb_ = 0;
        if (foreign_) matchTargetDb_ = 0;                 // that measurement belongs to another programme
        decide(al);                                       // state changes, once per read (every ~46 ms)
        const float aTarget = heardAnalog_ ? 1.0f : 0.0f; // the analog leg, heard or muted
        const float aStep = 1.0f / (float)FADE_FRAMES;

        const float slew = GAIN_SLEW_DB_PER_S / RATE;
        for (size_t i = 0; i < n; i++) {
            // analog sample, level-matched
            if (matchDb_ < matchTargetDb_) matchDb_ = std::min(matchTargetDb_, matchDb_ + slew);
            else if (matchDb_ > matchTargetDb_) matchDb_ = std::max(matchTargetDb_, matchDb_ - slew);
            if (i % 441 == 0) matchLin_ = std::pow(10.0f, matchDb_ / 20.0f);   // dB -> factor, every 10 ms is plenty
            size_t aslot = (size_t)(play_ % (long long)ANALOG_RING);
            float l = analog_[aslot * 2] * matchLin_, r = analog_[aslot * 2 + 1] * matchLin_;   // stereo
            if (aGain_ != 1.0f || aTarget != 1.0f) {      // only when the analog is (being) muted
                if (aGain_ < aTarget) aGain_ = std::min(aTarget, aGain_ + aStep);
                else if (aGain_ > aTarget) aGain_ = std::max(aTarget, aGain_ - aStep);
                l *= aGain_; r *= aGain_;
            }
            if (state_ != ANALOG) {
                // the blend ramp: 0 = analog, 1 = HD (rampStep_ is 1, or 4 to and from silence)
                if (state_ == TO_HD && ramp_ < FADE_FRAMES) ramp_ = std::min(FADE_FRAMES, ramp_ + rampStep_);
                else if (state_ == TO_ANALOG && ramp_ > 0) ramp_ = std::max(0LL, ramp_ - rampStep_);
                float g = 0.5f - 0.5f * std::cos((float)M_PI * (float)ramp_ / (float)FADE_FRAMES);
                long long h = play_ - offset_;
                float hl = 0, hr = 0;
                if (h >= hdOldest_ && h < hdFrontier_) {
                    size_t slot = (size_t)(h % (long long)HD_RING);
                    hl = hd_[slot * 2]; hr = hd_[slot * 2 + 1];
                }
                l = (1 - g) * l + g * hl;
                r = (1 - g) * r + g * hr;
                if (state_ == TO_HD && ramp_ == FADE_FRAMES) { state_ = HD; }
                if (state_ == TO_ANALOG && ramp_ == 0) {
                    state_ = ANALOG;
                    // Came back to analog: wait before trying HD again (longer each time it
                    // happens again soon - a signal on the edge would otherwise flap).
                    if (alone_) { silentFrames_ = 0; }    // no waiting times while the HD stands alone
                    else {
                    if (hdStreak_ >= HD_HELD_RESET) returns_ = 0;
                    if (reversible_) {                    // came back because of a dropout
                        dwellUntil_ = play_ + DWELL_FRAMES[std::min(returns_, 3)];
                        returns_++;
                    }
                    }
                    hdStreak_ = 0;
                }
                if (state_ == HD || state_ == TO_HD) { st_.hdFrames++; hdStreak_++; }
            }
            out[2 * i] = clip(l);
            out[2 * i + 1] = clip(r);
            play_++;
        }
        // The silence is counted while it lasts (decideAlone's safety net reads it).
        const bool silentNow = alone_ && !heardAnalog_ && state_ == ANALOG;
        if (silentNow) silentFrames_ += (long long)n;
        st_.alone = alone_ && !heardAnalog_;
        st_.silent = silentNow;
        st_.silentFrames = silentNow ? silentFrames_ : 0;
        st_.state = state_;
        st_.aligned = aligned_;
        st_.matchDb = matchDb_;
        st_.hdSince = (state_ == HD || state_ == TO_HD) ? hdStreak_ : 0;
        return n;
    }

    Status status() const {
        std::lock_guard<std::mutex> lock(m_);
        return st_;
    }

private:
    static int16_t clip(float v) {
        if (v > 32767.f) return 32767; if (v < -32768.f) return -32768;
        return (int16_t)v;
    }

    // Are the HD frames [from, from + len) all received and good?
    bool hdClean(long long from, long long len) const {
        if (from < hdOldest_ || from + len > hdFrontier_) return false;
        for (long long f = from; f < from + len; f++)
            if (!hdGood_[(size_t)(f % (long long)HD_RING)]) return false;
        return true;
    }

    void startFade(State to, bool aligned, long long offset, const char *why) {
        if (to == TO_HD) { offset_ = offset; aligned_ = aligned; st_.toHd++; }
        else st_.toAnalog++;
        state_ = to;
        st_.reason = why;
    }

    // From the analog to the HD: a crossfade - or, on the first decision after a (re)start
    // (`resume`: back on HD1 from a sub-channel), straight onto the HD. The listener was
    // hearing digital audio a moment ago and the HD1 is good, so there is nothing to blend
    // from; native-lib's short fade-in covers the switch. Not counted as a fade ("to HD 3x").
    void goToHd(bool resume, bool aligned, long long offset, const char *why) {
        if (!resume) { startFade(TO_HD, aligned, offset, why); return; }
        offset_ = offset; aligned_ = aligned;
        state_ = HD; ramp_ = FADE_FRAMES;
        st_.reason = why;
    }

    // Called with m_ held, once per read(): should the blend change direction?
    void decide(const align::Status &al) {
        bool alignedOk = al.state == 2;
        bool noMatch = al.state == 3 && play_ >= NOMATCH_FRAMES;
        st_.mismatch = noMatch;
        bool resume = resume_;                            // the first decision after a (re)start
        resume_ = false;
        if (foreign_ || alone_) { decideAlone(resume); return; }   // the analog is another station's
        if (state_ == ANALOG) {
            if (play_ < dwellUntil_) return;
            if (alignedOk) {
                if (hdClean(play_ - al.offsetFrames, CLEAN_FRAMES))
                    goToHd(resume, true, al.offsetFrames, "HD is good");
            } else if (noMatch && (unmatchedPlaysHd_ || sameStation_ == SAME_YES)) {
                // No time alignment possible: play the HD ~0.3 s behind live (a one-time jump).
                long long off = play_ - (hdFrontier_ - UNALIGNED_LATENCY);
                if (hdClean(play_ - off, UNALIGNED_CLEAN))
                    goToHd(resume, false, off, sameStation_ == SAME_YES
                           ? "same station (RDS), audio doesn't line up - playing the HD"
                           : "HD doesn't match the analog - playing it anyway");
            }
            return;
        }
        // On HD, or fading either way: anything wrong ahead -> (keep) fading to analog.
        // "Ahead" = the next 0.65 s of HD (the 0.5 s fade + 0.15 s guard), so the fade is
        // finished before the gap arrives. "Re-aligning": the aligner's figure moved (after a
        // sync loss nrsc5 can come back a whole 2,048-frame piece later) - fade out, take the
        // new figure, fade back in, instead of jumping in the middle of the HD.
        long long look = aligned_ ? FADE_FRAMES + GUARD_FRAMES : UNALIGNED_CLEAN;
        bool bad = !hdClean(play_ - offset_, look);
        bool realign = aligned_ && alignedOk && std::llabs(al.offsetFrames - offset_) > OFFSET_TOLERANCE;
        if (bad || realign) {
            if (state_ != TO_ANALOG) {
                startFade(TO_ANALOG, aligned_, offset_, bad ? "dropout ahead" : "re-aligning");
                reversible_ = bad;                        // a re-alignment must finish (new offset on the way back)
            }
        } else if (state_ == TO_ANALOG && reversible_ && hdClean(play_ - offset_, CLEAN_FRAMES)) {
            // The problem ahead went away (a piece arrived late?) and the next 2 s are
            // clean: go straight back up instead of finishing the fade.
            state_ = TO_HD; st_.reason = "HD is good again";
        }
    }

    // The decisions while the analog on this frequency is another station's
    // ("the HD plays alone", top of the file). Called with m_ held, once per read().
    // heardAnalog_ says what fills the place of the HD when it is not there: the other
    // station's analog (true - at the start, after the safety net, with "stay on analog",
    // and on the way out) or silence (false).
    void decideAlone(bool resume) {
        if (!alone_) {                                    // coming in: keep what is playing, change the rules
            alone_ = true; heardAnalog_ = true; aGain_ = 1.0f; rampStep_ = 1;
            needGood_ = ALONE_FIRST_GOOD; silentFrames_ = 0;
            dwellUntil_ = 0; returns_ = 0;
        }
        const bool onHd = state_ == HD || state_ == TO_HD;
        if (!foreign_ || !unmatchedPlaysHd_) {
            // The contest is over - or the setting says "stay on analog": the analog plays.
            heardAnalog_ = true; rampStep_ = 1;
            if (state_ == HD) aGain_ = 1.0f;              // inaudible here (the HD has all the weight)
            if (onHd) {
                startFade(TO_ANALOG, aligned_, offset_, foreign_ ? "the analog here is another station - staying on the analog"
                                                                   : "the analog is this station's again");
                reversible_ = false;
            }
            // Back to the ordinary blend once the analog is fully there.
            if (!foreign_ && state_ == ANALOG && aGain_ == 1.0f) { alone_ = false; needGood_ = ALONE_FIRST_GOOD; }
            return;
        }
        if (state_ == HD && heardAnalog_) {               // the crossfade from the other station is done:
            heardAnalog_ = false; aGain_ = 0.0f;          // from here the HD stands alone (inaudible switch, see above)
        }
        if (onHd) {
            // A dropout ahead? The fast fade needs 0.125 s; we look 0.2 s ahead.
            if (!hdClean(play_ - offset_, UNALIGNED_CLEAN)) {
                state_ = TO_ANALOG; reversible_ = false;
                if (heardAnalog_) { rampStep_ = 1; st_.toAnalog++; st_.reason = "dropout ahead"; }   // still coming from the other station
                else { rampStep_ = ALONE_FAST; st_.silences++; st_.reason = "HD dropout - silence (the analog here is another station)"; }
            }
            return;
        }
        if (state_ == TO_ANALOG) return;                  // a fade (to silence, or back to the analog) finishes first
        // state_ == ANALOG: the other station's analog is heard, or the silence waits.
        if (!heardAnalog_ && silentFrames_ >= ALONE_NET_FRAMES) {
            heardAnalog_ = true; needGood_ = ALONE_NET_GOOD;   // the safety net: aGain_ comes up over 0.5 s
            st_.toAnalog++;
            st_.reason = "no HD for 10 s - back to the analog (another station)";
            return;
        }
        // The HD (again)? Only if it is arriving NOW: while the HD decoder is off ("Analog only",
        // or the rest of the HD search) nothing new comes, and the newest audio in the ring is
        // seconds old - good, clean, and not to be played. (Both streams are counted on one
        // time line, so the HD is live when its newest frame is close to the newest analog one.)
        if (hdFrontier_ < aWritten_ - RATE) return;
        // It plays 0.6 s behind live (see ALONE_LATENCY).
        const long long off = play_ - (hdFrontier_ - ALONE_LATENCY);
        if (!hdClean(play_ - off, UNALIGNED_CLEAN)) return;
        if (heardAnalog_) {
            if (hdGoodRun_ < needGood_) return;
            rampStep_ = 1;                                // from the other station: an ordinary crossfade
            goToHd(false, false, off, "the analog here is another station - the HD plays alone");
        } else {
            rampStep_ = ALONE_FAST;                       // from silence: at once
            goToHd(resume, false, off, "the HD is back (it plays alone)");
            if (!resume) st_.toHd--;                      // not a blend: counted as a silence, not as "to HD"
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<int16_t> analog_, hd_;
    std::vector<uint8_t> hdGood_;
    long long aWritten_ = 0, hdFrontier_ = 0, hdOldest_ = 0;
    long long play_ = 0; bool restart_ = true;
    State state_ = ANALOG; long long ramp_ = 0; long long offset_ = 0; bool aligned_ = false;
    int returns_ = 0; long long dwellUntil_ = 0, hdStreak_ = 0;
    float matchDb_ = 0, matchTargetDb_ = 0, matchLin_ = 1.0f;
    bool unmatchedPlaysHd_ = true; bool reversible_ = false;
    bool resume_ = false;                                 // a (re)start is waiting for its first decide()
    int sameStation_ = SAME_UNKNOWN;                      // setSameStation
    // "The HD plays alone"
    bool foreign_ = false;                                // the analog is another station's (setAnalogForeign)
    bool alone_ = false;                                  // ... and decideAlone() is in charge
    bool heardAnalog_ = true;                             // what stands in for the HD: the analog (true) or silence
    float aGain_ = 1.0f;                                  // the analog leg's gain, slewing to 1 (heard) or 0 (muted)
    long long rampStep_ = 1;                              // fade speed: 1 = 0.5 s, ALONE_FAST = 0.125 s
    long long hdGoodRun_ = 0;                             // frames of good HD in one piece, up to the newest
    long long silentFrames_ = 0, needGood_ = ALONE_FIRST_GOOD;
    Status st_;
};

} // namespace blend
