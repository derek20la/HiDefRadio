// Harness only: tests hdsearch.hpp (the rest of the HD search, build 9) by itself.
// No recording, no nrsc5: the rule gets made-up time, one step per block of samples
// (88 ms, as on the phone), and a made-up answer to "is nrsc5 in sync?".
//
//   build/searchtest        all scenarios, PASS / FAIL
//   build/searchtest v      ... and every change of state printed
#include "hdsearch.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <string>

static const double BLOCK_S = 262144.0 / 2.0 / 1488375.0;     // one block of samples: 88 ms
static bool g_verbose = false;
static int g_failed = 0;

static void check(bool ok, const std::string &what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) g_failed++;
}

// What one made-up listening session did.
struct Run {
    double fedS = 0, restS = 0;           // seconds nrsc5 got samples / got none
    double firstRestAt = -1;              // when the first rest began
    double lastRestAt = -1;               // when the latest rest began
    double firstLookAt = -1;
    int rests = 0, looks = 0;
};

// Plays `seconds` of made-up time. applies(t) and synced(t, fed) are the test's story:
// synced gets told whether nrsc5 was fed in the block before (no samples, no sync).
static Run play(double seconds, const std::function<bool(double)> &applies,
                const std::function<bool(double, bool)> &synced) {
    hdsearch::Rest rest;
    rest.reset();
    Run r;
    bool fed = true;
    hdsearch::State before = hdsearch::SEARCHING;
    for (double t = 0; t < seconds; t += BLOCK_S) {
        fed = rest.step(t, applies(t), synced(t, fed));
        if (fed) r.fedS += BLOCK_S; else r.restS += BLOCK_S;
        hdsearch::State now = rest.state();
        if (now != before) {
            if (g_verbose) printf("    %8.2f s  %s\n", t, now == hdsearch::RESTING ? "rest" : now == hdsearch::LOOKING ? "look" : "search");
            if (now == hdsearch::RESTING) { if (r.firstRestAt < 0) r.firstRestAt = t; r.lastRestAt = t; }
            if (now == hdsearch::LOOKING && r.firstLookAt < 0) r.firstLookAt = t;
            before = now;
        }
    }
    r.rests = rest.rests(); r.looks = rest.looks();
    return r;
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "v") == 0;
    auto yes = [](double) { return true; };
    auto never = [](double, bool) { return false; };

    printf("1. A station without HD, ten minutes in Auto\n");
    {
        Run r = play(600, yes, never);
        check(near(r.firstRestAt, 30.0, BLOCK_S), "searches for 30 s, then rests (first rest at " + std::to_string(r.firstRestAt) + " s)");
        check(near(r.firstLookAt, 57.0, 2 * BLOCK_S), "first look 27 s later (at " + std::to_string(r.firstLookAt) + " s)");
        check(r.looks == 19, "one look every 30 s: 19 in the 570 s after the first search (" + std::to_string(r.looks) + ")");
        double duty = (r.fedS - 30.0) / (600.0 - 30.0);
        check(duty > 0.09 && duty < 0.12, "after the first search nrsc5 works about a tenth of the time (" + std::to_string(100 * duty) + " %)");
    }

    printf("2. A station with HD from the first second, one hour\n");
    {
        Run r = play(3600, yes, [](double t, bool) { return t > 0.2; });
        check(r.rests == 0 && r.restS == 0, "never rests");
    }

    printf("3. No HD at first, then it appears at 70 s (tropo, or you drove into range)\n");
    {
        // nrsc5 needs 0.2 s of samples to sync - and it can only sync while it is fed.
        double fedSince = -1;
        Run r = play(300, yes, [&](double t, bool fed) {
            if (!fed || t < 70) { fedSince = -1; return false; }
            if (fedSince < 0) fedSince = t;
            return t - fedSince >= 0.2;
        });
        check(r.rests == 2 && r.looks == 2, "nothing in the look at 57 s, found in the look at 87 s (rests " + std::to_string(r.rests) + ", looks " + std::to_string(r.looks) + ")");
        check(near(r.restS, 2 * 27.0, 3 * BLOCK_S), "rested 2 x 27 s in all, then never again (" + std::to_string(r.restS) + " s)");
    }

    printf("4. HD for a minute, then gone for good\n");
    {
        Run r = play(300, yes, [](double t, bool fed) { return fed && t > 0.2 && t < 60; });
        check(near(r.firstRestAt, 90.0, 2 * BLOCK_S), "the rest begins 30 s after the HD was last there (at " + std::to_string(r.firstRestAt) + " s)");
    }

    printf("5. HD that drops out for 20 s now and then\n");
    {
        Run r = play(1800, yes, [](double t, bool fed) { return fed && std::fmod(t, 120.0) > 20.0; });
        check(r.rests == 0, "a gap shorter than 30 s never starts a rest");
    }

    printf("6. Digital only for two minutes, then Auto - no HD on the station\n");
    {
        Run r = play(300, [](double t) { return t >= 120; }, never);
        check(near(r.firstRestAt, 150.0, 2 * BLOCK_S), "no rest while the rule does not apply; then a full 30 s search (first rest at " + std::to_string(r.firstRestAt) + " s)");
    }

    printf("7. Resting, and the listener switches to Digital only at 100 s\n");
    {
        Run r = play(200, [](double t) { return t < 100; }, never);
        check(near(r.fedS, 30.0 + 2 * 3.0 + 100.0, 0.5), "nrsc5 gets every block again from that moment (fed " + std::to_string(r.fedS) + " s)");
    }

    printf("8. A sync that came and went inside one block\n");
    {
        bool told = false;
        Run r = play(120, yes, [&](double t, bool) { if (t >= 25 && !told) { told = true; return true; } return false; });
        check(near(r.firstRestAt, 55.0, 2 * BLOCK_S), "still counts: the 30 s start over (first rest at " + std::to_string(r.firstRestAt) + " s)");
    }

    printf("9. The countdown for the signal details\n");
    {
        hdsearch::Rest rest;
        rest.reset();
        double t = 0;
        for (; t < 40; t += BLOCK_S) rest.step(t, true, false);
        double in = rest.lookIn(t);
        check(rest.state() == hdsearch::RESTING && near(in, 30.0 + 27.0 - t, 2 * BLOCK_S), "at 40 s: resting, next look in " + std::to_string(in) + " s");
        for (; t < 58; t += BLOCK_S) rest.step(t, true, false);
        check(rest.state() == hdsearch::LOOKING && rest.lookIn(t) == 0, "at 58 s: looking");
    }

    printf(g_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
