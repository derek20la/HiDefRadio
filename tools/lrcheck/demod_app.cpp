// The app's own FM demodulator (app/src/main/cpp/fmdemod.hpp) on a .cu8 file
// -> raw float32 stereo, 44.1 kHz.   usage: demod_app in.cu8 out.f32
#include "fmdemod.hpp"
#include <cstdio>
int main(int, char **argv) {
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    fm::Demod d; d.setStereoMode(fm::STEREO_FULL);
    static uint8_t buf[1 << 16]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        d.processCu8(buf, n);
        fwrite(d.audioOut.data(), sizeof(float), d.audioOut.size(), o); d.audioOut.clear();
    }
    fprintf(stderr, "app: pilot %d level %.3f stereo %.2f quiet %.1f\n", d.pilot(), d.pilotLevel(), d.stereoAmount(), d.quietingDb());
}
