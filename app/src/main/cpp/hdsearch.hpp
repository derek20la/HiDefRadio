// hdsearch.hpp - lets the HD Radio decoder REST on a station that has no HD (build 9).
//
// THE PROBLEM
// nrsc5, the HD decoder, is cheapest when it has a signal to decode. Before it has one it
// SEARCHES: about ten times a second it takes 93 ms of samples (32 OFDM symbols), filters
// out the two blocks of HD carriers beside the analog station, and looks for the repeating
// pattern every HD symbol carries (its "cyclic prefix" - the end of each symbol is sent
// again at its start). On a station without HD there is nothing to find, so it does that
// for ever. Measured on the phone (Moto G Stylus 2024): 23-34 % of a processor core while
// searching, against 4-5 % while decoding a station that has HD. In "HD Radio: Auto" - the
// setting the app starts with - that search ran for as long as the station played, on
// every analog-only station on the dial. It is as much as the analog demodulator needs, or
// more - and the demodulator makes all the sound you hear there.
//
// THE RULE
//   1. SEARCH   After a tune nrsc5 searches for SEARCH_S (30 s), as it always did.
//   2. REST     No HD in all that time: nrsc5 gets no more samples. It costs nothing.
//   3. LOOK     Every REST_S (27 s) it gets LOOK_S (3 s) of samples - one look per half
//               minute. Nothing found: rest again.
//   4. HD found, in the first search or in any look: everything is as before. The rest can
//      only begin again when nrsc5 has been without sync for a full SEARCH_S.
// "HD found" means nrsc5 reports SYNC: it has locked onto the HD carriers and reads the
// station's data. The app's status line calls that "synced".
//
// WHY THESE NUMBERS
//   - 3 s per look. On all thirteen recordings of ours with an HD signal that can be
//     decoded - ten Los Angeles locals, weak KGGI 99.1 from Riverside, KPBS and KGB from
//     San Diego received by tropo - nrsc5 was in sync within half a second of its first
//     sample (0.2 s where it was timed closely: its second attempt). A look of 3 s is
//     about thirty attempts. A signal that needs more luck than that does not give audio
//     either (94.9 from San Diego, 2026-09-29: one sync after 5 s of trying, lost again
//     at once).
//   - 30 s before the first rest. Much longer than nrsc5 needs, on purpose: the first half
//     minute after a tune is when people move the antenna, and when the auto-gain is still
//     settling. And tuning across the band - a few seconds per station - never gets as far
//     as a rest: for DXing nothing changes.
//   - One look per half minute. Searching 3 s out of every 30 costs a tenth of searching
//     all the time, which brings a station without HD down to about what a station WITH HD
//     costs. Longer rests would save little more and make the next point worse.
//
// WHAT IT COSTS THE LISTENER
// One thing: on a station that has been without HD for more than 30 s, HD that comes back
// (you drove out of a valley, tropo lifted the signal, the station switched its HD on) is
// noticed at the next look instead of at once - up to 27 s later, about 12 s on average.
// Then the blend needs its usual ten seconds or so to line the HD up with the analog, as
// after a tune. Until then the analog plays on, as it did all along.
//
// WHEN THE RULE DOES NOT APPLY (the caller says so with `applies`, and the search runs
// all the time, as before):
//   - "Digital only", and AM (which is HD only): there is nothing else to hear, and
//     somebody waiting for a station's HD wants to know the moment it is there.
//   - While an HD2 / HD3 ... program is selected: the same - silence until it is back.
//   - ("Analog only" switches nrsc5 off altogether; that is native-lib's business.)
// Every time the rule starts to apply again, the full 30 s search starts again.
//
// HOW IT IS USED (native-lib.cpp, onSamples): one call to step() per block of samples,
// just before the block would go to nrsc5. Time is the RECORDING's time - seconds of
// samples since the tune, not the wall clock - so a recording played through the desktop
// harness rests and looks at exactly the same samples every time (tools/harness).
// native-lib gives nrsc5 a fresh start whenever the samples begin again after a rest; why
// is written down there, next to g_hdSearch.
//
// Plain C++17, no dependencies, no Android. Only the streaming thread uses it.
#pragma once

namespace hdsearch {

static const double SEARCH_S = 30.0;      // search this long after a tune / after the HD was last there
static const double REST_S   = 27.0;      // then rest this long ...
static const double LOOK_S   = 3.0;       // ... and look this long, in turns

enum State {
    SEARCHING = 0,                        // nrsc5 gets every block (the HD is there, or the search is young)
    RESTING   = 1,                        // nrsc5 gets nothing
    LOOKING   = 2                         // nrsc5 gets samples for a few seconds: is there HD now?
};

class Rest {
public:
    // A new tune.
    void reset() { state_ = SEARCHING; since_ = 0; until_ = 0; rests_ = 0; looks_ = 0; }

    // One block of samples is about to be handled. Should nrsc5 get it?
    //   t        seconds of samples since the tune (the start of this block)
    //   applies  the rule applies right now (FM, Auto, HD1 selected, nrsc5 running)
    //   synced   nrsc5 is in sync, or was at some point since the last call
    bool step(double t, bool applies, bool synced) {
        if (!applies || synced) {
            // Nothing to save here, or the HD is there: search / decode all the time, and
            // count the 30 s from this moment.
            state_ = SEARCHING;
            since_ = t;
            return true;
        }
        switch (state_) {
            case SEARCHING:
                if (t - since_ < SEARCH_S) return true;
                state_ = RESTING; until_ = t + REST_S; rests_++;
                return false;
            case RESTING:
                if (t < until_) return false;
                state_ = LOOKING; until_ = t + LOOK_S; looks_++;
                return true;
            case LOOKING:
                if (t < until_) return true;
                state_ = RESTING; until_ = t + REST_S; rests_++;
                return false;
        }
        return true;
    }

    State state() const { return state_; }
    // While resting: seconds until the next look (for the signal details). Else 0.
    double lookIn(double t) const { return state_ == RESTING && until_ > t ? until_ - t : 0.0; }
    int rests() const { return rests_; }      // how often the search went to rest since the tune
    int looks() const { return looks_; }      // looks taken since the tune

private:
    State state_ = SEARCHING;
    double since_ = 0;                        // SEARCHING: the 30 s count from here
    double until_ = 0;                        // RESTING / LOOKING: when this stretch ends
    int rests_ = 0, looks_ = 0;
};

} // namespace hdsearch
