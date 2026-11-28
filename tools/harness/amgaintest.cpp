// Harness only (build 42): gainscore::measureAm() alone, over a whole recording - the AM twin of
// gaintest.cpp.
//   build/amgaintest file.cu8 [more.cu8 ...]
// Cuts each recording into bursts as the AM auto-gain reads them (AM_BURST_BYTES, the first
// quarter skipped - see doAutoGainAm() in native-lib.cpp), one burst every 0.25 s, and prints:
// the HD figure (median, the 10th and 90th percentile over the bursts), each side alone, in how
// many bursts it "looks like HD", and the peak (median and loudest). For checking the HD-or-not
// decision and how much one burst can be trusted, and for comparing recordings made at
// different gains.
//   AMEVERY=1.0 = one burst per second instead.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "gainscore.hpp"
static const int AM_BURST_BYTES = 160 * 1024;     // = native-lib.cpp
int main(int argc, char **argv) {
    const double every = getenv("AMEVERY") ? atof(getenv("AMEVERY")) : 0.25;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { printf("%s: can't open\n", argv[a]); continue; }
        std::vector<uint8_t> buf(AM_BURST_BYTES);
        std::vector<float> fig, lo, up, peak;
        int hd = 0;
        const long stepBytes = 2 * (long)(every * 1488375.0);
        for (long pos = 0; ; pos += stepBytes) {
            if (fseek(f, pos, SEEK_SET) != 0 || fread(buf.data(), 1, buf.size(), f) != buf.size()) break;
            const int skip = (AM_BURST_BYTES / 4) & ~1;
            const gainscore::AmQuality q = gainscore::measureAm(buf.data() + skip, (AM_BURST_BYTES - skip) / 2, 1488375.0);
            fig.push_back(q.overFloorDb); lo.push_back(q.lowerDb); up.push_back(q.upperDb); peak.push_back(q.peakDbfs);
            hd += q.hd;
        }
        fclose(f);
        if (fig.empty()) { printf("%s: too short\n", argv[a]); continue; }
        auto pct = [](std::vector<float> v, double p) { std::sort(v.begin(), v.end()); return v[(size_t)std::lround(p * (double)(v.size() - 1))]; };
        printf("%-34s %4zu bursts  HD %5.1f dB (p10 %5.1f p90 %5.1f)  lower %5.1f upper %5.1f  HD in %5.1f %%  peak %5.1f (max %5.1f) dBFS\n",
               argv[a], fig.size(), pct(fig, 0.5), pct(fig, 0.1), pct(fig, 0.9), pct(lo, 0.5), pct(up, 0.5),
               100.0 * hd / (double)fig.size(), pct(peak, 0.5), pct(peak, 1.0));
    }
    return 0;
}
