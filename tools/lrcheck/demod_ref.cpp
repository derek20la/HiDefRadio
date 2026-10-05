// The REFERENCE: ngsoftfm's FmDecoder (github.com/f4exb/ngsoftfm, the SoftFM family, GPL) on a .cu8 file
// -> raw float32 stereo, 44.1 kHz.   usage: demod_ref in.cu8 out.f32
#include "SoftFM.h"
#include "FmDecode.h"
#include <cstdio>
#include <cstdint>
int main(int, char **argv) {
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    FmDecoder fm(1488375.0, 0.0, 44100.0, true, 75.0, 100000.0, 75000.0, 15000.0, 5);
    static uint8_t buf[65530 * 4]; size_t n; IQSampleVector iq; SampleVector audio; std::vector<float> out;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        iq.resize(n / 2);
        for (size_t k = 0; k < n / 2; k++) iq[k] = IQSample((buf[2*k] - 127.5f) / 127.5f, (buf[2*k+1] - 127.5f) / 127.5f);
        fm.process(iq, audio);
        out.assign(audio.begin(), audio.end());
        fwrite(out.data(), sizeof(float), out.size(), o);
    }
    fprintf(stderr, "ngsoftfm: stereo detected %d pilot level %.3f\n", fm.stereo_detected(), fm.get_pilot_level());
}
