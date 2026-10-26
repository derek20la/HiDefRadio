// pstest.cpp - build 37: checks psstitch.hpp (the scrolling PS put back together) on made-up
// frame sequences: the cases the 10-26 recordings don't have. Prints each case and PASS / FAIL.
//
//   cd tools/harness && sh build.sh && build/pstest
//
// The real recordings are checked with apptest (its log prints `RDS: PS text "..."`).
#include "psstitch.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

struct Feed {
    rds::PsStitch st; double t = 0;
    explicit Feed(int khz) { st.setFreqKhz(khz); }
    // one PS frame, padded to 8 characters, shown for `hold` seconds
    void ps(const char *s, double hold = 2.0) {
        uint8_t f[8]; std::string p(s); p.resize(8, ' ');
        for (int i = 0; i < 8; i++) f[i] = (uint8_t)p[i];
        st.onFrame(f, t); t += hold;
    }
    void rt(const char *s) { std::string r(s); st.onRt((const uint8_t *)r.data(), (int)r.size()); }
    std::string text() const { return std::string(st.text().begin(), st.text().end()); }
    std::string other() const { return std::string(st.rtOther().begin(), st.rtOther().end()); }
};

static void check(const char *what, const std::string &got, const std::string &want) {
    bool ok = got == want;
    if (!ok) failures++;
    std::printf("%s  %-52s \"%s\"%s\n", ok ? "PASS" : "FAIL", what, got.c_str(),
                ok ? "" : (std::string("  (wanted \"") + want + "\")").c_str());
}

int main() {
    {   // Derek's 10-26 case on 91X: only two pieces, no frame naming the station; RT = the title
        Feed f(91100);
        f.rt("ROSES");
        for (int k = 0; k < 3; k++) { f.ps("ROSES by", 30); f.ps("CHURCHES", 30); }
        check("two pieces, RadioText gives the start", f.text(), "ROSES by CHURCHES");
        check("  ...under it: nothing (RT is a piece of it)", f.other(), "");
    }
    {   // The same, heard from the middle: still starts at the title
        Feed f(91100);
        f.rt("ROSES");
        f.ps("CHURCHES", 30);
        for (int k = 0; k < 3; k++) { f.ps("ROSES by", 30); f.ps("CHURCHES", 30); }
        check("tuned in mid-message", f.text(), "ROSES by CHURCHES");
    }
    {   // 91X as recorded, with a changeover frame (0.5 s) and one garbled frame in round 2
        Feed f(91100);
        const char *m[] = {"  91X", "  LOVECATS", "by CURE", "on 91X"};
        f.rt("CURE");
        f.ps("  91X"); f.ps("LOVECATS"); f.ps("by CU", 0.5); f.ps("by CURE"); f.ps("on 91X");
        f.ps("  91X"); f.ps("LOVECATS"); f.ps("bF CURE"); f.ps("on 91X");    // garbled, held 2 s
        f.ps("  91X"); f.ps("LOVECATS"); f.ps("by CURE"); f.ps("on 91X");
        check("  (one round after the garbled one: not yet)", f.text(), "LOVECATS bF CURE on 91X");
        f.ps("  91X"); f.ps("LOVECATS"); f.ps("by CURE"); f.ps("on 91X");
        f.ps("  91X");
        (void)m;
        check("garbled frame repaired, changeover dropped", f.text(), "LOVECATS by CURE on 91X");
        f.rt("LISTEN TO 91X ANYTIME DOWNLOAD THE 91X APP");
        f.rt("CURE");
        check("  ...under it: the ad, not \"CURE\"", f.other(), "LISTEN TO 91X ANYTIME DOWNLOAD THE 91X APP");
        // A new song: two frames that aren't in the message -> forgotten, then the new one
        f.ps("CRAZY IN"); f.ps("LOVE by"); f.ps("BEYONCE");
        check("new song: the old line is forgotten", f.text(), "");
        for (int k = 0; k < 2; k++) { f.ps("on 91X"); f.ps("  91X"); f.ps("CRAZY IN"); f.ps("LOVE by"); f.ps("BEYONCE"); }
        f.ps("on 91X"); f.ps("  91X");
        check("  ...then the new one", f.text(), "CRAZY IN LOVE by BEYONCE on 91X");
    }
    {   // One garbled frame while the line is up doesn't clear it
        Feed f(90300);
        for (int k = 0; k < 2; k++) { f.ps(" ROSES"); f.ps("by SAINT"); f.ps(" JHN ON"); f.ps("  Z90"); }
        f.ps(" ROSES"); f.ps("by SAINT");
        std::string before = f.text();
        f.ps(" JHN 0N"); f.ps("  Z90"); f.ps(" ROSES");
        check("one bad frame keeps the line", f.text(), before);
        check("  (the line)", before, "ROSES by SAINT JHN ON Z90");
    }
    {   // Alt 98.7: a sliding 8-letter window
        Feed f(98700);
        for (int k = 0; k < 2; k++) { f.ps("Alt 98.7", 4); f.ps("Justice", 4); f.ps("/ Tame", 4); f.ps("Impala -", 4); f.ps("Neverend", 4); f.ps("verender", 4); }
        f.ps("Alt 98.7", 4); f.ps("Justice", 4); f.ps("/ Tame", 4);
        check("sliding window joined, starts after \"Alt 98.7\"", f.text(), "Justice / Tame Impala - Neverender Alt 98.7");
    }
    {   // A station whose PS is just its name never makes a line
        Feed f(104500);
        for (int k = 0; k < 10; k++) f.ps("XHLTN-FM", 30);
        check("steady name: no line", f.text(), "");
    }
    {   // "1991" in a title is not the station on 99.1 ...
        Feed f(99100);
        for (int k = 0; k < 2; k++) { f.ps("SONGS OF"); f.ps("  1991"); f.ps("by SOMEO"); f.ps("NE"); }
        f.ps("SONGS OF"); f.ps("  1991"); f.ps("by SOMEO");
        check("\"1991\" is not 99.1 (heard order kept)", f.text(), "SONGS OF 1991 by SOMEO NE");
    }
    std::printf("%s\n", failures ? "SOME FAILED" : "all passed");
    return failures ? 1 : 0;
}
