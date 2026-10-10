// Build 10 test: stationid.hpp alone - "whose station is this?" - against the app's REAL
// list of stations (app/src/main/assets/stations/fm.txt), with the cases that were seen on
// the air. No recording needed; runs in a blink.
//
//   build/identtest [path/to/fm.txt]        (default: the app's own list, found from build/)
//
// Each case feeds the judge what the engine would feed it: the calls licensed on the
// frequency, the HD station's name, the RDS decoder's status, which station's RDS blocks
// came in lately, and the aligner's attempts.
#include <cstdio>
#include <fstream>
#include <sstream>
#include "stationid.hpp"

static std::string listPath = "../../../app/src/main/assets/stations/fm.txt";
static int fails = 0, checks = 0;
static void check(const char *what, bool ok) { checks++; if (!ok) fails++; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }

// The calls licensed on a frequency, as Stations.callsOn gives them to the engine.
static std::vector<std::string> callsOn(double mhz) {
    std::ifstream in(listPath);
    if (!in) { fprintf(stderr, "cannot read %s\n", listPath.c_str()); exit(2); }
    char want[16]; snprintf(want, sizeof(want), "@%d", (int)(mhz * 100 + 0.5));
    std::vector<std::string> out; std::string line; bool on = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '@') { on = line == want; continue; }
        if (!on) continue;
        std::string c = line.substr(0, line.find('|'));
        c = c.substr(0, c.find('-'));
        bool ok = c.size() >= 3 && c.size() <= 4 && (c[0] == 'K' || c[0] == 'W');
        for (char ch : c) if (ch < 'A' || ch > 'Z') ok = false;
        if (ok && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}
static bool has(const std::vector<std::string> &v, const char *c) { return std::find(v.begin(), v.end(), c) != v.end(); }
// The RDS code the US formula gives a call (the reverse of rds::callFromPi), by trying them all.
static uint16_t piOf(const char *call) {
    for (int p = 0x2000; p <= 0xFFFF; p++) if (rds::callFromPi((uint16_t)p) == call) return (uint16_t)p;
    fprintf(stderr, "no code for %s\n", call); exit(2);
}

static ident::Judge j;
static double now = 0;
static rds::Status last;
static void tune(double mhz, bool list = true) { j.setChannel(list ? callsOn(mhz) : std::vector<std::string>(), list); j.newTune(); now = 0; last = rds::Status(); }
// The RDS decoder reports: code, PS (and how long it stood still), RadioText. Returns the settled call.
static std::string rdsNow(uint16_t pi, const char *ps = "", float psSecs = 0, const char *rt = "", const char *decoderCall = nullptr) {
    rds::Status s; s.synced = true; s.piOk = true; s.pi = pi; s.ps = ps; s.psSecs = psSecs; s.rt = rt;
    s.call = decoderCall ? decoderCall : (pi >> 12) == 1 ? "" : rds::callFromPi(pi);   // what rds.hpp itself would put there
    j.onRds(s, now); last = s; return s.call;
}
// Time passes (seconds), with this many clean blocks of the current / another code in the last 3 s.
static void air(double secs, int cur, int other = 0) {
    for (double t = 0; t < secs; t += 0.1) {
        now += 0.1;
        rds::Lately l; l.cur = cur; l.other = other; l.ageCur = cur > 0 ? 0.1 : 1e9;
        j.tick(now, l);
    }
}
static void attempt(bool accepted, float r, float cover) { j.onAlign(accepted, r, cover, now); }
static const ident::Status &st() { return j.status(); }

int main(int argc, char **argv) {
    if (argc > 1) listPath = argv[1];
    using namespace ident;

    // ---- 1. 90.3: XHITZ-FM Tijuana sends 692B, which the formula turns into "WHTZ" (Newark NJ, 100.3)
    check("the list: WHTZ is licensed on 100.3, not on 90.3; XHITZ's code 692B spells WHTZ",
          has(callsOn(100.3), "WHTZ") && !has(callsOn(90.3), "WHTZ") && rds::callFromPi(0x692B) == "WHTZ");
    tune(90.3);
    std::string call = rdsNow(0x692B, "Z90     ", 3, "");
    air(20, 10);                                              // (build 9 showed "WHTZ" after 10 s)
    call = rdsNow(0x692B, "Z90     ", 23, "LA MAS PERRONA");
    check("90.3 / 692B: NO call for the title - 'WHTZ' is only a maybe", call.empty() && st().call.empty() && st().callMaybe == "WHTZ" && !st().callRelay);
    check("... no verdict, no contest (there is no HD)", st().same == SAME_UNKNOWN && st().contest == CONTEST_NONE);

    // ---- 2. a relay that says so: a translator frequency, the parent's code, the parent's call in the text
    tune(90.3);
    uint16_t kusc = piOf("KUSC");
    check("the list: KUSC is not licensed on 90.3", !has(callsOn(90.3), "KUSC"));
    call = rdsNow(kusc, "CLASSICL", 2, "");
    check("a call from elsewhere, not named in the text: a maybe", call.empty() && st().callMaybe == "KUSC");
    call = rdsNow(kusc, "CLASSICL", 4, "Classical KUSC - Mozart: Symphony No. 40");
    check("... once the station's own text names it: shown, as a relay", call == "KUSC" && st().callRelay && !st().callSure && st().callMaybe.empty());
    call = rdsNow(kusc, "MOZART  ", 1, "Symphony No. 40");
    check("... and it stays when the text scrolls on", call == "KUSC" && st().callRelay);

    // ---- 3. a 1xxx code (iHeart): KMYI 94.1 San Diego sends 1228 - the list picks the one of nine
    tune(94.1);
    call = rdsNow(0x1228, "Star    ", 2, "San Diego's Best Variety");
    check("94.1 / 1228: KMYI, sure (the one candidate licensed there)", call == "KMYI" && st().callSure);

    // ---- 4. 91.1: KDSC's HD, XETRA-FM's analog (F000 - not a US code). First meeting, nothing remembered.
    check("the list: KDSC is licensed on 91.1 and its code is 19C2", has(callsOn(91.1), "KDSC") && rds::callFromPi(0x19C2) == "KDSC");
    tune(91.1);
    rdsNow(0xF000, "91X     ", 1, "LISTEN TO 91X ANYTIME");
    air(1, 20);
    check("91.1, F000, no HD name yet: nothing to suspect", st().contest == CONTEST_NONE && st().same == SAME_UNKNOWN);
    j.onHdName("KDSC", now); air(0.5, 20);
    check("HD says KDSC, the analog sends F000: SUSPECTED - and nothing is claimed yet", st().contest == CONTEST_SUSPECTED && st().same == SAME_UNKNOWN);
    for (int i = 0; i < 6; i++) { air(2, 20); attempt(false, 0.0f, 0.0f); }    // HD in sync but no audio (BER 18 %): zero-filled
    check("six 'no match' against MISSING HD audio: still only suspected (the 10-09 first screenshot)", st().contest == CONTEST_SUSPECTED && st().same == SAME_UNKNOWN);
    air(2, 20); attempt(false, 0.07f, 0.9f); air(2, 20); attempt(false, 0.08f, 0.95f);
    check("two attempts with the HD audio there and no likeness: not yet", st().contest == CONTEST_SUSPECTED && st().disagree == 2);
    rdsNow(0xF000, "91X     ", 20, "LISTEN TO 91X ANYTIME");
    air(2, 20); attempt(false, 0.05f, 1.0f);
    check("the third: CONFIRMED, 'another station'", st().contest == CONTEST_CONFIRMED && st().same == SAME_NO);
    check("... named by its PS that stood still: 91X", st().otherName == "91X");
    check("... the own code is still unknown (nothing is stored on a guess)", st().ownPi < 0);

    // ---- 5. 91.1, the 10-09 evening sequence: F000, then KDSC's own code gets through, then F000 again
    tune(91.1);
    j.onHdName("KDSC", now);
    rdsNow(0xF000, "", 0, ""); air(3, 6);
    check("F000 against HD KDSC: suspected", st().contest == CONTEST_SUSPECTED);
    call = rdsNow(0x19C2, "KDSC    ", 1, ""); air(2, 6);
    check("19C2 gets through: KDSC, and 19C2 is now known as the HD station's OWN code", call == "KDSC" && st().ownPi == 0x19C2 && st().ownFrom == OWN_CALL);
    check("... but the suspicion is NOT dropped (build 9: one reception latched 'same station' for good)", st().contest == CONTEST_SUSPECTED);
    rdsNow(0xF000, "", 0, ""); air(0.2, 1);
    check("F000 again, one block: still only suspected", st().contest == CONTEST_SUSPECTED);
    air(1, 4);
    check("F000 steadily: CONFIRMED without waiting for the audio (two codes, one proven the HD's)", st().contest == CONTEST_CONFIRMED && st().same == SAME_NO);
    rdsNow(0x19C2, "KDSC    ", 1, ""); air(20, 6);
    check("KDSC's analog on top for 20 s: the verdict holds (no flipping with the capture)", st().contest == CONTEST_CONFIRMED && st().same == SAME_NO);
    air(45, 6);
    check("65 s without the other station: over - same station again", st().contest == CONTEST_NONE && st().same == SAME_YES);

    // ---- 6. the same with memory: the saved logo says KDSC / 19C2
    tune(91.1); j.setOwner("KDSC", 0x19C2); j.newTune();
    rdsNow(0xF000, "91X     ", 30, ""); air(1, 20);
    j.onHdName("KDSC", now); air(1, 20);
    check("remembered owner KDSC / 19C2, F000 on the air: suspected at once", st().contest == CONTEST_SUSPECTED && st().ownPi == 0x19C2 && st().ownFrom == OWN_MEMORY);
    air(30, 20);
    check("... but memory alone never confirms (a station can change its code)", st().contest == CONTEST_SUSPECTED);
    for (int i = 0; i < 3; i++) { air(2, 20); attempt(false, 0.06f, 0.8f); }
    check("... the audio does", st().contest == CONTEST_CONFIRMED);
    j.setOwner("", -1);

    // ---- 7. a station that changed its code: memory says 4321, the analog sends 5678, the audio matches
    tune(88.5); j.setOwner("ROCK", 0x4321); j.newTune();
    j.onHdName("ROCK", now);
    rdsNow(0x5678, "", 0, ""); air(3, 20);
    check("memory 4321, the analog sends 5678: suspected", st().contest == CONTEST_SUSPECTED && st().ownPi == 0x4321);
    air(2, 20); attempt(true, 0.95f, 1.0f);
    check("one close match: not confirmed, not relearned yet", st().contest == CONTEST_SUSPECTED && st().ownPi == 0x4321);
    air(1, 20); attempt(true, 0.96f, 1.0f);
    check("two in a row: the own code is relearned (5678), the suspicion is gone", st().contest == CONTEST_NONE && st().ownPi == 0x5678 && st().ownFrom == OWN_AUDIO);
    j.setOwner("", -1);

    // ---- 8. KGGI 99.1: its code 69D8 spells "WIAQ"; its text names it; its HD was weak (10-02)
    tune(99.1);
    j.onHdName("KGGI", now);
    call = rdsNow(0x69D8, "99.1    ", 1, ""); air(2, 20);
    check("KGGI before its text: no call shown, WIAQ a maybe; suspected, nothing claimed", call.empty() && st().callMaybe == "WIAQ" && st().contest == CONTEST_SUSPECTED && st().same == SAME_UNKNOWN);
    for (int i = 0; i < 8; i++) { air(2, 20); attempt(false, 0.0f, 0.2f); }    // BER 17-21 %: hardly any HD audio
    check("... eight 'no match' against a weak HD: NEVER 'another station' (the bug of 10-02)", st().contest == CONTEST_SUSPECTED && st().same == SAME_UNKNOWN);
    call = rdsNow(0x69D8, "Bunny - ", 1, "99.1 KGGI - Bad Bunny - Dtmf"); air(1, 20);
    check("its text says KGGI: KGGI, same station, 69D8 is its own code, no contest", call == "KGGI" && st().callSure && st().same == SAME_YES && st().contest == CONTEST_NONE && st().ownPi == 0x69D8);
    for (int i = 0; i < 8; i++) { air(2, 20); attempt(false, 0.05f, 1.0f); }
    check("... and whatever the audio does afterwards, never contested", st().contest == CONTEST_NONE && st().same == SAME_YES);

    // ---- 9. a wrong code that is never explained, on a station processed differently on analog and HD
    tune(99.1);
    j.onHdName("KGGI", now); rdsNow(0x69D8, "TOP 40  ", 30, ""); air(2, 20);
    for (int i = 0; i < 10; i++) { air(2, 20); attempt(false, 0.38f, 1.0f); }   // KKLQ-like: r 0.3 - 0.5
    check("wrong code + a faint likeness in the audio (r 0.38): suspected for ever, never confirmed", st().contest == CONTEST_SUSPECTED && st().same == SAME_UNKNOWN);
    air(2, 20); attempt(true, 0.93f, 1.0f); air(1, 20); attempt(true, 0.94f, 1.0f);
    check("... and the first good matches settle it: its own code after all", st().contest == CONTEST_NONE && st().ownPi == 0x69D8);

    // ---- 10. KTWV 94.7: the HD names itself "WAVE" - a name, not a call licensed there
    tune(94.7);
    call = rdsNow(piOf("KTWV"), "OZERAN  ", 1, ""); j.onHdName("WAVE", now); air(5, 20);
    check("KTWV + HD 'WAVE': KTWV, no verdict, no contest", call == "KTWV" && st().same == SAME_UNKNOWN && st().contest == CONTEST_NONE);
    air(2, 20); attempt(true, 0.94f, 1.0f); air(1, 20); attempt(true, 0.94f, 1.0f);
    check("... the audio teaches the own code (for the saved logo)", st().ownPi == piOf("KTWV") && st().ownFrom == OWN_AUDIO);

    // ---- 11. 98.9 Bakersfield: KHHT's analog, KSOF's HD - two sure calls (the rule since M12)
    check("the list: KHHT and KSOF are both licensed on 98.9", has(callsOn(98.9), "KHHT") && has(callsOn(98.9), "KSOF"));
    tune(98.9);
    j.onHdName("KSOF", now); call = rdsNow(piOf("KHHT"), "", 0, ""); air(1, 20);
    check("KHHT vs KSOF: 'another station' said at once (as before), contest open, strong", call == "KHHT" && st().same == SAME_NO && st().contest == CONTEST_SUSPECTED && st().strong && st().otherName == "KHHT");
    for (int i = 0; i < 3; i++) { air(2, 20); attempt(false, 0.1f, 1.0f); }
    check("... confirmed by the audio", st().contest == CONTEST_CONFIRMED);

    // ---- 12. ordinary stations: nothing new happens
    tune(101.1);
    call = rdsNow(piOf("KRTH"), "Madonna ", 1, ""); j.onHdName("KRTH", now); air(5, 20);
    for (int i = 0; i < 5; i++) { air(3, 20); attempt(true, 0.96f, 1.0f); }
    check("KRTH: same station, own code by its call, no contest", call == "KRTH" && st().same == SAME_YES && st().contest == CONTEST_NONE && st().ownPi == piOf("KRTH") && st().ownFrom == OWN_CALL);
    tune(100.3);
    rdsNow(piOf("KKLQ"), "K-LOVE  ", 1, ""); j.onHdName("KKLQ", now);
    for (int i = 0; i < 10; i++) { air(2, 20); attempt(false, 0.4f, 1.0f); }
    check("KKLQ (audio never matches, r 0.4): same station by RDS, no contest", st().same == SAME_YES && st().contest == CONTEST_NONE);
    for (int i = 0; i < 10; i++) { air(2, 20); attempt(false, 0.1f, 1.0f); }
    check("... even with no likeness at all: the code is its own, nothing to suspect", st().same == SAME_YES && st().contest == CONTEST_NONE);
    tune(103.5);
    rdsNow(piOf("KOST"), "", 0, ""); j.onHdName("KOST 103.5", now); air(5, 20);
    check("an HD name with the call in it ('KOST 103.5'): same station", st().same == SAME_YES && st().ownPi == piOf("KOST"));
    tune(99.5);
    call = rdsNow(piOf("KKLA"), "HOPE    ", 1, "99.5 Find Hope Here! KKLA"); air(60, 20);
    check("KKLA (no HD): KKLA, no verdict, no contest", call == "KKLA" && st().same == SAME_UNKNOWN && st().contest == CONTEST_NONE && st().ownPi < 0);

    // ---- 13. no list for the frequency (another region): the rules from before the list
    tune(101.1, false);
    call = rdsNow(0x692B, "", 0, ""); j.onHdName("KRTH", now); air(3, 20);
    check("no list: the formula's call is shown as before, and a different exact call = NO", call == "WHTZ" && st().callMaybe.empty() && st().same == SAME_NO);
    tune(101.1, false);
    rdsNow(piOf("KRTH"), "", 0, ""); j.onHdName("KRTH", now); air(3, 20);
    check("no list: KRTH = KRTH, yes", st().same == SAME_YES && st().contest == CONTEST_NONE);

    // ---- 14. a contest ends 60 s after the other station was last heard - also when RDS just fades away
    tune(91.1); j.onHdName("KDSC", now);
    rdsNow(0xF000, "", 0, ""); air(3, 20);
    air(50, 0);                                              // no clean block at all for 50 s
    check("50 s after the last block of the other station: still suspected", st().contest == CONTEST_SUSPECTED);
    air(15, 0);
    check("65 s after: over", st().contest == CONTEST_NONE);

    printf(fails ? "%d of %d FAILED\n" : "all %d passed\n", fails ? fails : checks, checks);
    return fails;
}
