// A tiny stand-in for FFTW (the harness only): complex float FFT, power-of-two sizes, forward.
#pragma once
#include <stdlib.h>
#include <math.h>
#include <complex.h>
typedef float complex fftwf_complex;
typedef struct { int n; fftwf_complex *in, *out; float *cs, *sn; int *rev; } fftwf_plan_s;
typedef fftwf_plan_s *fftwf_plan;
#define FFTW_FORWARD (-1)
#define FFTW_ESTIMATE 0
static inline fftwf_complex *fftwf_alloc_complex(size_t n) { return (fftwf_complex *)malloc(sizeof(fftwf_complex) * n); }
static inline void fftwf_free(void *p) { free(p); }
static inline fftwf_plan fftwf_plan_dft_1d(int n, fftwf_complex *in, fftwf_complex *out, int sign, unsigned flags) {
    (void)sign; (void)flags;
    fftwf_plan p = (fftwf_plan)malloc(sizeof(fftwf_plan_s));
    p->n = n; p->in = in; p->out = out;
    p->cs = (float *)malloc(sizeof(float) * n / 2); p->sn = (float *)malloc(sizeof(float) * n / 2);
    p->rev = (int *)malloc(sizeof(int) * n);
    for (int i = 0; i < n / 2; i++) { p->cs[i] = (float)cos(2 * M_PI * i / n); p->sn[i] = (float)-sin(2 * M_PI * i / n); }
    int bits = 0; while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); p->rev[i] = r; }
    return p;
}
static inline void fftwf_execute(fftwf_plan p) {
    int n = p->n;
    for (int i = 0; i < n; i++) p->out[p->rev[i]] = p->in[i];
    for (int len = 2; len <= n; len <<= 1) {
        int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k++) {
                float complex w = p->cs[k * step] + I * p->sn[k * step];
                float complex t = p->out[i + k + half] * w, a = p->out[i + k];
                p->out[i + k] = a + t; p->out[i + k + half] = a - t;
            }
    }
}
static inline void fftwf_destroy_plan(fftwf_plan p) { free(p->cs); free(p->sn); free(p->rev); free(p); }
