// Harness only (build 39): gainscore.hpp alone, over a whole recording.
//   build/gaintest file.cu8 [more.cu8 ...]
// Cuts each recording into bursts as the FM auto-gain measures them (gainscore::SAMPLES samples =
// 8 ms each) and prints, per sideband: the mean and the scatter of "sideband over floor", and
// in how many of the bursts it "looks like HD". For checking the HD-or-not decision on
// recordings of stations with and without HD, and how much one burst can be trusted.
#include <cstdio>
#include <cmath>
#include <vector>
#include "gainscore.hpp"
int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { printf("%s: can't open\n", argv[a]); continue; }
        std::vector<uint8_t> buf((size_t)gainscore::SAMPLES * 2);
        double sum[2] = {0, 0}, sq[2] = {0, 0}, peak = 0; int hd[2] = {0, 0}, n = 0;
        while (fread(buf.data(), 1, buf.size(), f) == buf.size()) {
            const gainscore::Quality q = gainscore::measure(buf.data(), gainscore::SAMPLES, 1488375.0);
            const gainscore::Side *s[2] = { &q.lower, &q.upper };
            for (int i = 0; i < 2; i++) { sum[i] += s[i]->overFloorDb; sq[i] += s[i]->overFloorDb * s[i]->overFloorDb; hd[i] += s[i]->hd; }
            peak += q.peakDbfs; n++;
        }
        fclose(f);
        if (n == 0) { printf("%s: too short\n", argv[a]); continue; }
        auto sd = [&](int i) { return sqrt(fmax(0.0, sq[i] / n - (sum[i] / n) * (sum[i] / n))); };
        printf("%-28s %4d bursts  peak %6.1f dBFS  lower %5.1f +-%.1f dB over the floor, HD in %5.1f %%   upper %5.1f +-%.1f, HD in %5.1f %%\n",
               argv[a], n, peak / n, sum[0] / n, sd(0), 100.0 * hd[0] / n, sum[1] / n, sd(1), 100.0 * hd[1] / n);
    }
    return 0;
}
