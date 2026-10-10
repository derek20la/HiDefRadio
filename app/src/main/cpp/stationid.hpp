// stationid.hpp - whose station is this? (build 10)
//
// A frequency is not a station. Three things were found in two days of listening away from
// downtown Los Angeles (2026-10-09 / 10-10), each of them the app stating something as a
// fact that was only a guess:
//
//  1. 90.3 MHz: the title said "WHTZ", "Newark, NJ - relayed here". It was XHITZ-FM in
//     Tijuana. XHITZ sends the RDS code 692B, and the US formula that turns a code into
//     call letters makes W-H-T-Z out of it (a nod to "HITZ"). WHTZ exists - in New Jersey,
//     on 100.3. A call that comes ONLY from the formula, is not licensed on the tuned
//     frequency and is not named in the station's own text proves nothing.
//
//  2. 91.1 MHz: KDSC Thousand Oaks (HD) and XETRA-FM Tijuana, "91X" (by tropo), on one
//     frequency. The HD decoded as KDSC's (classical), the analog heard was mostly 91X's
//     (rock), and the blend swapped between the two five times a minute: every HD dropout
//     fell back on "the analog" as if it were the same programme. ONE reception of KDSC's
//     own code had settled "same station" for the whole tune.
//
//  3. 91.1 again, next morning: 91X playing under KDSC's saved logo. (That one is fixed in
//     Kotlin - LogoCache - with the "own code" this file works out.)
//
// So this file keeps the evidence apart from the conclusion. It answers three questions:
//
//  A. WHAT MAY THE ANALOG STATION BE CALLED?  (settle)
//     call       - call letters fit to be shown as the station's name: licensed on this
//                  frequency (FCC list), or named by the station in its own text.
//     callMaybe  - what the formula makes of the code when nobody vouches for it ("WHTZ").
//                  Shown in the details as "unconfirmed", never as the title.
//
//  B. WHICH RDS CODE IS THE HD STATION'S OWN?  (ownPi - three sources, strongest first)
//     - the analog's call is SURE and equals the HD station's call: this code is it;
//     - audio: twice in a row the analog matched the HD audio closely (r >= 0.9) while
//       this code - and no other - was coming in. Covers HD stations whose name is not a
//       call ("ROCK", "WAVE") and stations that send a wrong or stale code (KGGI: 69D8);
//     - memory: the code stored with this frequency's saved logo, once the HD names
//       itself as it did then.
//
//  C. IS THE ANALOG ON THE AIR RIGHT NOW ANOTHER STATION THAN THE HD?  (the "contest")
//     Suspected when a code is coming in that is not the HD station's:
//       - the own code is known and this is a different one, or
//       - the HD's name is a call licensed here and the analog's SURE call is a different
//         call licensed here (KHHT analog / KSOF HD on 98.9 - the rule since M12), or
//       - the HD's name is a call licensed here and this code is not that call's.
//         (Weak on its own - wrong codes are common - so it only counts with the audio.)
//     Confirmed when the AUDIO agrees: three alignment attempts in a row that found no
//     likeness (r < 0.25) although the HD audio was there (at least half of the stretch
//     searched). An attempt against missing or zero-filled HD audio says nothing - both
//     real sightings of "no match (r 0.00)" were exactly that (KGGI 10-02, its own station
//     with a weak HD; 91.1 before its first HD audio).
//     Or without the audio: the own code was established during THIS tune and now another
//     code is on the air. Two codes, one of them proven the HD station's = two stations.
//     A contest lasts until 60 s after the foreign code was last seen, so the receiver
//     flipping between the two stations every few seconds does not flip the verdict.
//
// Only "confirmed" changes what is heard (blend.hpp: the HD plays alone). "Suspected" shows
// "checking the audio..." in the details and nothing else. A false "another station" is
// worse than a missed one (the KGGI bug of 2026-10-02: logo hidden, no blend), so every
// path to "confirmed" needs evidence of identity AND either the audio or a proven own code.
// With no evidence of identity at all - the HD's name is not a call and no own code is
// known, or the analog has no RDS - nothing here fires and the app behaves as it always did.
//
// Plain C++17. Needs rds.hpp (the call-letter formula). No locks of its own: the caller
// (native-lib.cpp, under g_rdsMutex) serialises the calls. Time is passed in as seconds
// since the tune, counted in samples - so a recording gives the same answers every run.
// tools/harness/identtest.cpp runs the cases above against the app's real FCC list.
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include "rds.hpp"

namespace ident {

// = blend::SameStation = Signal.HD_SAME_* in Kotlin
enum { SAME_UNKNOWN = 0, SAME_YES = 1, SAME_NO = 2 };
enum { CONTEST_NONE = 0, CONTEST_SUSPECTED = 1, CONTEST_CONFIRMED = 2 };
// where the own code came from (higher = stronger)
enum { OWN_NONE = 0, OWN_MEMORY = 1, OWN_AUDIO = 2, OWN_CALL = 3 };

static const double PI_FRESH_S = 1.5;        // "on the air right now": a clean block of this code this recently
static const double CONTEST_HOLD_S = 60.0;   // a contest ends this long after the foreign code was last seen
static const int    DISAGREE_NEEDED = 3;     // alignment attempts in a row that say "different programme"
static const float  DISAGREE_CORR = 0.25f;   // ... = a best likeness below this (a station processed differently
                                             //     on its analog and HD, like KKLQ, still shows 0.3 - 0.5)
static const float  HD_COVER_NEEDED = 0.5f;  // ... with at least this much of the HD stretch really there
static const float  OWN_CORR = 0.9f;         // audio proof of the own code: this close a match ...
static const int    OWN_BLOCKS = 12;         // ... with this many clean blocks of the code in the last 3 s (of ~34
                                             //     sent), none of another. Why so many: when two stations fight, the
                                             //     weaker one's audio does get through for seconds at a time (seen in
                                             //     the test recording: accepted at r 0.54 and 0.74 while the OTHER
                                             //     station's code was still the confirmed one). A match of 0.9 needs
                                             //     this station on top for nearly all of the 3 s - and then a third
                                             //     of the blocks cannot have been the other station's.
static const int    OWN_RUN = 2;             // ... twice in a row
static const int    FOREIGN_BLOCKS = 3;      // "another code is coming in steadily": this many clean blocks in 3 s
static const float  PS_NAME_AFTER_S = 15.0f; // a PS that stood still this long is a name ("91X"), not scrolling text

// The call letters in an HD station name. Stations send "KKLQ", "KGB-FM", but also
// "101.5 KGB" (seen 09-30), so look at each word: the first one that is K/W + 2-3 letters
// (a "-FM" / "-HD1" ending dropped) counts. exact = the whole name is just the call (+ ending).
// "" when there is none - Mexican XH..., "MPS", an empty name: can't be judged.
inline std::string usCall(const std::string &name, bool *exact = nullptr) {
    std::vector<std::string> words;
    std::string w;
    for (char ch : name + " ") {
        if (ch == ' ' || ch == '/' || ch == ',') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    for (const std::string &word : words) {
        std::string c = word.substr(0, word.find('-'));
        if (c.size() < 3 || c.size() > 4 || (c[0] != 'K' && c[0] != 'W')) continue;
        bool letters = true;
        for (char ch : c) if (ch < 'A' || ch > 'Z') letters = false;
        if (!letters) continue;
        if (exact) *exact = words.size() == 1;
        return c;
    }
    return "";
}

// A station name as it is stored in a saved logo's file name (LogoCache.kt does the same):
// upper case, letters and digits only, at most 12. "KDSC-FM" -> "KDSCFM", "Go Country" -> "GOCOUNTRY".
inline std::string ownerKey(const std::string &name) {
    std::string k;
    for (char c : name) {
        const unsigned char ch = (unsigned char)c;
        if (ch < 128 && std::isalnum(ch) && k.size() < 12) k.push_back((char)std::toupper(ch));
    }
    return k;
}

struct Status {
    std::string call;            // the analog's call, fit to show ("" = none)
    bool callSure = false;       // ... licensed on this frequency (or there is no list to check against)
    bool callRelay = false;      // ... licensed elsewhere, but the station names it in its own text: a relay
    std::string callMaybe;       // the formula's call that nobody vouches for
    int same = SAME_UNKNOWN;     // is the HD the same station as the analog?
    int contest = CONTEST_NONE;
    bool strong = false;         // the contest rests on two different SURE calls (the M12 rule)
    int ownPi = -1;              // the HD station's own RDS code (-1 = not known)
    int ownFrom = OWN_NONE;
    std::string otherName;       // the analog station's name for the screen: in a contest the OTHER
                                 // station's (call, or a PS that stood still: "91X"), else the analog's call
    int disagree = 0;            // attempts in a row that said "different programme" (for the details / tests)
};

class Judge {
public:
    // ---- before a tune (kept across newTune) ----

    // The call letters licensed on the frequency about to be tuned (Stations.kt); known =
    // false: no list for this frequency (AM, another region) - the rules from before the list.
    void setChannel(std::vector<std::string> calls, bool known) { calls_ = std::move(calls); known_ = known; }

    // Memory: the owner written into this frequency's saved HD1 logo file name - the HD
    // station's name as stored (see ownerKey) and its own code (-1 = not known).
    void setOwner(const std::string &name, int pi) { memName_ = ownerKey(name); memPi_ = pi < 0 ? -1 : (pi & 0xFFFF); }

    // A new tune: forget the station, keep the list and the memory.
    void newTune() {
        hdName_.clear(); hdCall_.clear(); hdExact_ = false;
        piOk_ = false; pi_ = 0; call_.clear(); callSure_ = callRelay_ = false; callMaybe_.clear();
        ps_.clear(); psSecs_ = 0;
        textPi_ = -1; textCall_.clear(); relayNamed_ = false;
        air_ = rds::Lately();
        ownPi_ = -1; ownFrom_ = OWN_NONE; ownRun_ = 0; ownRunPi_ = -1;
        classic_ = SAME_UNKNOWN; classicCall_.clear(); classicNo_ = false; weak_ = false;
        contestPi_ = -1; lastForeign_ = 0; disagree_ = 0; confirmed_ = false; strong_ = false; otherName_.clear();
        out_ = Status();
    }

    // ---- while tuned ----

    // The HD station named itself (SIS).
    void onHdName(const std::string &name, double t) {
        hdName_ = name;
        hdCall_ = usCall(name, &hdExact_);
        if (hdCall_.empty()) hdExact_ = false;
        // Memory applies once the HD says it is who it was when the logo was saved.
        if (memPi_ >= 0 && !memName_.empty() && ownerKey(name) == memName_) setOwn(memPi_, OWN_MEMORY);
        settle(); judge(t);
    }

    // The RDS decoder's status changed. Settles the call letters IN `now` (question A): on
    // return now.call is what may be shown as the station's name.
    void onRds(rds::Status &now, double t) {
        // A new code: what "came in lately" was counted for the old one - forget it until the
        // next tick brings the count for this one.
        if (!now.piOk || !piOk_ || now.pi != pi_) air_ = rds::Lately();
        piOk_ = now.piOk; pi_ = now.pi; ps_ = now.ps; psSecs_ = now.psSecs;
        call_ = now.call; callSure_ = callRelay_ = false; callMaybe_.clear();
        if (!piOk_) { call_.clear(); }
        else {
            // A 1xxx code (iHeart & co. replace the first digit by 1, which leaves nine possible
            // calls): the one of the nine licensed on this frequency, if there is exactly one.
            // (The decoder's own rule comes first: a candidate the station's text mentions.)
            if (call_.empty()) call_ = tableCall(pi_);
            if (!known_) callSure_ = true;                                   // no list: as before the list
            else if (!call_.empty() && licensedHere(call_)) callSure_ = true;
            else {
                // The code's call is not licensed here. Does the station say who it is? A call
                // that IS licensed here and appears as a word in the PS or the RadioText is the
                // station speaking for itself ("99.1 KGGI - Bad Bunny - Dtmf"; KGGI's code 69D8
                // spells "WIAQ"). Found once per code, then kept - the text scrolls on.
                if (textPi_ != (int)pi_) { textPi_ = pi_; textCall_.clear(); relayNamed_ = false; }
                const std::string text = now.ps + " " + now.rt;
                if (textCall_.empty()) textCall_ = callInText(text, "");
                if (!textCall_.empty()) { call_ = textCall_; callSure_ = true; }
                else if (!call_.empty()) {
                    // Licensed on ANOTHER frequency, or nowhere. A translator relaying its parent
                    // station sends the parent's code - and usually names the parent in its text.
                    // If the text names this very call, the relay is confirmed. (A 1xxx call got
                    // here through the station's text already - rds.hpp only takes one of the
                    // nine that the text mentions.) Otherwise it is only what the formula says.
                    if ((pi_ >> 12) == 1 || relayNamed_ || !callInText(text, call_).empty()) { relayNamed_ = true; callRelay_ = true; }
                    else { callMaybe_ = call_; call_.clear(); }
                }
            }
        }
        now.call = call_;
        settle(); judge(t);
    }

    // Every block of samples (about 11 times a second) while the demodulator runs: what the
    // RDS decoder received lately. This is what makes time pass for the contest.
    void tick(double t, const rds::Lately &air) { air_ = air; judge(t); }

    // One alignment attempt (aligner.hpp): accepted or not, its best likeness r, and how much
    // of the HD stretch it searched was real audio.
    void onAlign(bool accepted, float corr, float hdCover, double t) {
        // "This code, and only this code, has been coming in": the proof is about the audio of
        // the last 3 s, so the code must have been on the air for those same seconds.
        const bool alone = piOk_ && air_.cur >= OWN_BLOCKS && air_.other == 0 && air_.ageCur <= PI_FRESH_S;
        if (accepted) {
            disagree_ = 0;                                   // the audio found a likeness: not "three in a row"
            if (corr >= OWN_CORR && alone) {
                if (ownRunPi_ == (int)pi_) ownRun_++; else { ownRunPi_ = pi_; ownRun_ = 1; }
                if (ownRun_ >= OWN_RUN) setOwn(pi_, OWN_AUDIO);
            } else ownRun_ = 0;
        } else {
            ownRun_ = 0;
            const bool foreignOnAir = contestPi_ >= 0 && piOk_ && contestPi_ == (int)pi_ && air_.cur >= 1;
            if (hdCover >= HD_COVER_NEEDED && corr < DISAGREE_CORR) {
                if (foreignOnAir && ++disagree_ >= DISAGREE_NEEDED) confirmed_ = true;
            } else if (hdCover >= HD_COVER_NEEDED) disagree_ = 0;   // some likeness after all
            // (an attempt against missing HD audio neither counts nor clears the count)
        }
        judge(t);
    }

    const Status &status() const { return out_; }

private:
    bool licensedHere(const std::string &call) const {
        return std::find(calls_.begin(), calls_.end(), call) != calls_.end();
    }

    // The call letters for a 1xxx code from the FCC list: the ONE of its nine possible calls
    // licensed on this frequency. "" when the list has none or several of them here (about 3
    // in 100 stations share their frequency with another of "their" nine somewhere in the
    // country - then it stays open), or when there is no list.
    std::string tableCall(uint16_t pi) const {
        if (!known_ || (pi >> 12) != 1) return "";
        std::string found;
        for (const std::string &c : rds::callCandidates(pi)) {
            if (!licensedHere(c)) continue;
            if (!found.empty()) return "";
            found = c;
        }
        return found;
    }

    // A call that appears as a whole word in `text`: with want = "" the first one that is
    // licensed on this frequency, else `want` itself. "" = none.
    std::string callInText(const std::string &text, const std::string &want) const {
        std::string word;
        for (size_t i = 0; i <= text.size(); i++) {
            unsigned char ch = i < text.size() ? (unsigned char)text[i] : ' ';
            if (std::isalnum(ch)) { word.push_back((char)std::toupper(ch)); continue; }
            if (want.empty()) {
                if (word.size() >= 3 && word.size() <= 4 && (word[0] == 'K' || word[0] == 'W') && licensedHere(word)) return word;
            } else if (word == want) return word;
            word.clear();
        }
        return "";
    }

    void setOwn(int pi, int from) {
        // The call beats the audio, the audio beats (and corrects) the memory, the memory
        // only fills an empty place.
        const bool take = from == OWN_CALL || (from == OWN_AUDIO && ownFrom_ != OWN_CALL) || (from == OWN_MEMORY && ownFrom_ == OWN_NONE);
        if (!take) return;
        ownPi_ = pi; ownFrom_ = from;
        if (contestPi_ == pi) endContest();                  // the code under suspicion turned out to be the station's own
    }

    void endContest() { contestPi_ = -1; disagree_ = 0; confirmed_ = false; strong_ = false; otherName_.clear(); }

    // The parts of the verdict that only change when the HD name or the RDS data change.
    void settle() {
        // B, first source: the analog's sure call is the HD station's call -> this code is its own.
        const bool callIsHd = piOk_ && !hdCall_.empty() && callSure_ && !call_.empty() && call_ == hdCall_;
        if (callIsHd) setOwn(pi_, OWN_CALL);

        // "Same station?" by the calls alone - the rules since M12 / 13b / 14b, unchanged:
        // kept until something says otherwise (a code without a US call, or no HD name yet,
        // says nothing either way).
        classicNo_ = false; weak_ = false;
        if (!piOk_ || hdCall_.empty()) return;
        // The weak hint: the HD's name is a call licensed here, and this code is not that
        // call's (nor its 1xxx form), and the analog does not name itself as that call.
        weak_ = known_ && hdExact_ && licensedHere(hdCall_) && !rds::piMatchesCall(pi_, hdCall_) && !callIsHd;
        const std::vector<std::string> cands = rds::callCandidates(pi_);
        if (cands.empty()) return;                           // a code that isn't a US call (Mexico, 0000...)
        const bool sure = known_ && callSure_ && !call_.empty();
        const bool same = (sure || ((pi_ >> 12) == 1 && !call_.empty())) ? call_ == hdCall_ : rds::piMatchesCall(pi_, hdCall_);
        if (same) classic_ = SAME_YES;
        // "Another station" only from a name that IS a call licensed here, against an analog
        // call that is licensed here too: two different stations on one frequency. A call
        // from elsewhere (a relay, or a wrong code) proves nothing by itself.
        else if (hdExact_ && (!known_ || (licensedHere(hdCall_) && sure))) { classic_ = SAME_NO; classicNo_ = true; }
        else return;                                         // "101.5 KGB"-style name without a match: can't tell
        classicCall_ = !call_.empty() ? call_ : !callMaybe_.empty() ? callMaybe_ : (!known_ && cands.size() == 1) ? cands[0] : "";
    }

    // Question C, and the answers. Runs on every event and every tick.
    void judge(double t) {
        // Is a code on the air right now that is not the HD station's?
        const bool fresh = piOk_ && air_.cur >= 1 && air_.ageCur <= PI_FRESH_S;
        const bool foreign = fresh && (ownPi_ >= 0 ? (int)pi_ != ownPi_ : (classicNo_ || weak_));
        if (foreign) {
            if (contestPi_ != (int)pi_) { endContest(); contestPi_ = pi_; }
            lastForeign_ = t;
            strong_ = classicNo_;
            // The other station's name for the notice: its call if it has one we may show,
            // else a PS that stood still ("91X"). Kept while the contest lasts.
            if (!call_.empty()) otherName_ = call_;
            else if (psSecs_ >= PS_NAME_AFTER_S) {
                std::string n = ps_;
                while (!n.empty() && n.back() == ' ') n.pop_back();
                while (!n.empty() && n.front() == ' ') n.erase(n.begin());
                if (!n.empty()) otherName_ = n;
            }
            // Confirmed without the audio: the own code was established in THIS tune (by the
            // call or by the audio - not merely remembered, a station can change its code) and
            // now another one is coming in steadily.
            if (ownPi_ >= 0 && ownFrom_ >= OWN_AUDIO && air_.cur >= FOREIGN_BLOCKS) confirmed_ = true;
        }
        if (contestPi_ >= 0 && t - lastForeign_ >= CONTEST_HOLD_S) endContest();
        const bool in = contestPi_ >= 0;

        out_.call = call_; out_.callSure = callSure_; out_.callRelay = callRelay_; out_.callMaybe = callMaybe_;
        // "Another station" is only SAID when it is confirmed, or when it rests on two sure
        // calls (then Kotlin still waits for the aligner's "no match", as it has since 12a).
        // A mere suspicion leaves the verdict where the calls put it.
        out_.same = in && (confirmed_ || strong_) ? SAME_NO : classic_;
        out_.contest = !in ? CONTEST_NONE : confirmed_ ? CONTEST_CONFIRMED : CONTEST_SUSPECTED;
        out_.strong = in && strong_;
        out_.ownPi = ownPi_; out_.ownFrom = ownFrom_;
        out_.otherName = in ? otherName_ : classicCall_;
        out_.disagree = disagree_;
    }

    // before the tune
    std::vector<std::string> calls_; bool known_ = false;
    std::string memName_; int memPi_ = -1;
    // the HD side
    std::string hdName_, hdCall_; bool hdExact_ = false;
    // the analog side (RDS)
    bool piOk_ = false; uint16_t pi_ = 0;
    std::string call_, callMaybe_, ps_; bool callSure_ = false, callRelay_ = false; float psSecs_ = 0;
    int textPi_ = -1; std::string textCall_; bool relayNamed_ = false;
    rds::Lately air_;
    // the own code
    int ownPi_ = -1, ownFrom_ = OWN_NONE, ownRun_ = 0, ownRunPi_ = -1;
    // the verdict by the calls alone
    int classic_ = SAME_UNKNOWN; std::string classicCall_; bool classicNo_ = false, weak_ = false;
    // the contest
    int contestPi_ = -1; double lastForeign_ = 0; int disagree_ = 0; bool confirmed_ = false, strong_ = false;
    std::string otherName_;
    Status out_;
};

} // namespace ident
