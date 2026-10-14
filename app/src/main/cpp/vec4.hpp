// vec4.hpp - four floats at a time, for the filters in HiDef Radio (build 14, the NEON step).
//
// WHY THIS FILE EXISTS
// Every arm64 Android phone has NEON: 128-bit registers that hold four floats and do four
// multiply-adds in one instruction (the ABI requires it, so no phone is left out - a 2015
// Cortex-A53 has it exactly like a 2024 Cortex-A78). The FM demodulator's cost is almost all
// multiply-adds (three FIR filters at 1.5 M, 0.3 M and 0.3 M samples/s), so running them four
// at a time is where the speed is. The compiler sometimes manages that by itself and sometimes
// not; writing it out makes it certain, and makes the arithmetic the SAME on every phone.
//
// HOW IT IS USED
// A filter output is a dot product: sum over k of h[k] * x[n-k]. Here it is computed as four
// running sums - lane 0 adds the taps 0, 4, 8, ..., lane 1 the taps 1, 5, 9, ... - and the
// four sums are added at the end as (lane0 + lane1) + (lane2 + lane3). That is exactly the
// order the scalar code of builds 1-13 used (four accumulators a0..a3), so on a machine
// without NEON (the desktop harness) the results are bit for bit the same as before. On NEON
// the multiply-add is "fused" (one rounding instead of two), which changes the last bit of a
// float now and then - about 1e-7 of full scale, far below anything audible or measurable in
// the radio (the quieting meter's floor is at 1e-5). The tolerance tests in tools/harness
// (README, "The NEON step") show the actual numbers.
//
// WHAT IS IN HERE
//   f4        four floats; load / store / set1 / add / sub / mul / fma / fms / min / max /
//             abs / sqrt / div / hsum, and a mask type for "which lanes" (cmp + select).
//   cu8ToFloat  the dongle's bytes (I, Q, I, Q, ... unsigned, 127.5 = zero) into two float
//             arrays, -1 .. +1, sixteen bytes per instruction.
//   atan2     a polynomial arc tangent, four at a time (see the note there).
//
// Two implementations of the same functions: NEON intrinsics when the compiler says the
// target has them (__ARM_NEON), plain C++ otherwise. Both are plain C++17, no dependencies.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VEC4_NEON 1
#else
#define VEC4_NEON 0
#endif

namespace vec {

#if VEC4_NEON

// ---- NEON -----------------------------------------------------------------------------------
typedef float32x4_t f4;
typedef uint32x4_t  m4;                                   // a mask: each lane all ones or all zeros

static inline f4 load(const float *p)            { return vld1q_f32(p); }          // any alignment
static inline void store(float *p, f4 a)         { vst1q_f32(p, a); }
static inline f4 set1(float x)                   { return vdupq_n_f32(x); }
static inline f4 zero()                          { return vdupq_n_f32(0.f); }
static inline f4 add(f4 a, f4 b)                 { return vaddq_f32(a, b); }
static inline f4 sub(f4 a, f4 b)                 { return vsubq_f32(a, b); }
static inline f4 mul(f4 a, f4 b)                 { return vmulq_f32(a, b); }
static inline f4 fma(f4 acc, f4 a, f4 b)         { return vfmaq_f32(acc, a, b); }  // acc + a*b, one rounding
static inline f4 fms(f4 acc, f4 a, f4 b)         { return vfmsq_f32(acc, a, b); }  // acc - a*b
static inline f4 min(f4 a, f4 b)                 { return vminq_f32(a, b); }
static inline f4 max(f4 a, f4 b)                 { return vmaxq_f32(a, b); }
static inline f4 abs(f4 a)                       { return vabsq_f32(a); }
static inline f4 sqrt(f4 a)                      { return vsqrtq_f32(a); }         // IEEE sqrt, like sqrtf
static inline f4 div(f4 a, f4 b)                 { return vdivq_f32(a, b); }       // IEEE division
static inline m4 cmpgt(f4 a, f4 b)               { return vcgtq_f32(a, b); }       // a > b
static inline m4 cmplt(f4 a, f4 b)               { return vcltq_f32(a, b); }       // a < b
static inline f4 select(m4 m, f4 a, f4 b)        { return vbslq_f32(m, a, b); }    // m ? a : b, per lane
static inline float lane(f4 a, int i)            { return a[i]; }
// (lane0 + lane1) + (lane2 + lane3) - the order the scalar code used.
static inline float hsum(f4 a) {
    float32x2_t s = vpadd_f32(vget_low_f32(a), vget_high_f32(a));   // (l0+l1, l2+l3)
    return vget_lane_f32(s, 0) + vget_lane_f32(s, 1);
}

// The dongle's bytes to floats: sixteen I/Q pairs per round. (b - 127.5) * (1/127.5), the
// same two operations as the scalar conversion, so the floats are identical to the byte.
static inline void cu8ToFloat(const uint8_t *in, float *I, float *Q, size_t n) {
    const float32x4_t half = vdupq_n_f32(127.5f), scale = vdupq_n_f32(1.f / 127.5f);
    size_t k = 0;
    for (; k + 16 <= n; k += 16) {
        uint8x16x2_t iq = vld2q_u8(in + 2 * k);                       // de-interleaves: [0] = I bytes, [1] = Q bytes
        for (int c = 0; c < 2; c++) {
            uint16x8_t lo = vmovl_u8(vget_low_u8(iq.val[c])), hi = vmovl_u8(vget_high_u8(iq.val[c]));
            float *out = (c == 0 ? I : Q) + k;
            vst1q_f32(out,      vmulq_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(lo))),  half), scale));
            vst1q_f32(out + 4,  vmulq_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(lo))), half), scale));
            vst1q_f32(out + 8,  vmulq_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(hi))),  half), scale));
            vst1q_f32(out + 12, vmulq_f32(vsubq_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(hi))), half), scale));
        }
    }
    for (; k < n; k++) {                                              // the last few (n not a multiple of 16)
        I[k] = (in[2 * k] - 127.5f) * (1.f / 127.5f);
        Q[k] = (in[2 * k + 1] - 127.5f) * (1.f / 127.5f);
    }
}

#else

// ---- plain C++ (the desktop harness, any other machine) ------------------------------------
// The compiler may turn these loops into SSE on a PC; the arithmetic order is fixed either way.
struct f4 { float v[4]; };
struct m4 { uint32_t v[4]; };

static inline f4 load(const float *p)            { f4 r; for (int i = 0; i < 4; i++) r.v[i] = p[i]; return r; }
static inline void store(float *p, f4 a)         { for (int i = 0; i < 4; i++) p[i] = a.v[i]; }
static inline f4 set1(float x)                   { f4 r; for (int i = 0; i < 4; i++) r.v[i] = x; return r; }
static inline f4 zero()                          { return set1(0.f); }
static inline f4 add(f4 a, f4 b)                 { f4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i]; return r; }
static inline f4 sub(f4 a, f4 b)                 { f4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] - b.v[i]; return r; }
static inline f4 mul(f4 a, f4 b)                 { f4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; return r; }
static inline f4 fma(f4 acc, f4 a, f4 b)         { f4 r; for (int i = 0; i < 4; i++) r.v[i] = acc.v[i] + a.v[i] * b.v[i]; return r; }
static inline f4 fms(f4 acc, f4 a, f4 b)         { f4 r; for (int i = 0; i < 4; i++) r.v[i] = acc.v[i] - a.v[i] * b.v[i]; return r; }
static inline f4 min(f4 a, f4 b)                 { f4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return r; }
static inline f4 max(f4 a, f4 b)                 { f4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r; }
static inline f4 abs(f4 a)                       { f4 r; for (int i = 0; i < 4; i++) r.v[i] = std::fabs(a.v[i]); return r; }
static inline f4 sqrt(f4 a)                      { f4 r; for (int i = 0; i < 4; i++) r.v[i] = std::sqrt(a.v[i]); return r; }
static inline f4 div(f4 a, f4 b)                 { f4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] / b.v[i]; return r; }
static inline m4 cmpgt(f4 a, f4 b)               { m4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i] ? 0xFFFFFFFFu : 0u; return r; }
static inline m4 cmplt(f4 a, f4 b)               { m4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 0xFFFFFFFFu : 0u; return r; }
static inline f4 select(m4 m, f4 a, f4 b)        { f4 r; for (int i = 0; i < 4; i++) r.v[i] = m.v[i] ? a.v[i] : b.v[i]; return r; }
static inline float lane(f4 a, int i)            { return a.v[i]; }
static inline float hsum(f4 a)                   { return (a.v[0] + a.v[1]) + (a.v[2] + a.v[3]); }

static inline void cu8ToFloat(const uint8_t *in, float *I, float *Q, size_t n) {
    for (size_t k = 0; k < n; k++) {
        I[k] = (in[2 * k] - 127.5f) * (1.f / 127.5f);
        Q[k] = (in[2 * k + 1] - 127.5f) * (1.f / 127.5f);
    }
}

#endif

// ---- the same on both ----------------------------------------------------------------------

// A dot product of n floats (n a multiple of 4) in the four-lane order described at the top.
static inline float dot(const float *h, const float *x, int n) {
    f4 acc = zero();
    for (int k = 0; k < n; k += 4) acc = fma(acc, load(h + k), load(x + k));
    return hsum(acc);
}
// Two dot products with the same taps (I and Q of a complex signal through a real filter).
static inline void dot2(const float *h, const float *x, const float *y, int n, float &outX, float &outY) {
    f4 ax = zero(), ay = zero();
    for (int k = 0; k < n; k += 4) { f4 hk = load(h + k); ax = fma(ax, hk, load(x + k)); ay = fma(ay, hk, load(y + k)); }
    outX = hsum(ax); outY = hsum(ay);
}

// atan2, four at a time, by a polynomial. The discriminator of an FM demodulator needs one
// arc tangent per sample (300,000 a second here), and the C library's atan2f is slow and
// cannot be vectorised. This one brings the angle into 0..45 degrees (ratio of the smaller
// to the larger of |x| and |y|), evaluates an odd polynomial of degree 15, and puts the
// octant back. The coefficients were fitted for the smallest worst-case error on 0..1
// (iteratively reweighted least squares, 20,001 points); in float arithmetic the largest
// error against the C library over a million random angles is 1.2e-7 radians - the size of
// one float rounding step at pi. In the demodulator 1e-7 rad is 1e-7 x 297,675 / (2 pi) =
// 0.005 Hz of deviation, 1.4e-7 of full scale: -137 dB. The old atan2f and this one give
// the same radio within the stated tolerance; the desktop harness keeps atan2f unless
// FM_FAST_ATAN2 is set, so that the old proofs "bit for bit" can still be run.
static inline f4 atan2(f4 y, f4 x) {
    const f4 ax = abs(x), ay = abs(y);
    const f4 mn = min(ax, ay), mx = max(ax, ay);
    const f4 t = div(mn, max(mx, set1(1e-30f)));                 // 0..1; 0 when both are 0 (atan2(0,0) = 0, like the C library)
    const f4 r = mul(t, t);
    // atan(t) ~ t * (c0 + r*(c1 + r*(c2 + ... ))) , Horner from the highest coefficient down
    f4 p = set1(-4.054363351e-03f);
    p = fma(set1( 2.186219394e-02f), p, r);
    p = fma(set1(-5.591116846e-02f), p, r);
    p = fma(set1( 9.642106295e-02f), p, r);
    p = fma(set1(-1.390859038e-01f), p, r);
    p = fma(set1( 1.994655728e-01f), p, r);
    p = fma(set1(-3.332985938e-01f), p, r);
    p = fma(set1( 9.999993443e-01f), p, r);
    f4 a = mul(p, t);                                                // 0 .. pi/4
    a = select(cmpgt(ay, ax), sub(set1(1.57079637f), a), a);        // |y| > |x|: pi/2 - a
    a = select(cmplt(x, zero()), sub(set1(3.14159274f), a), a);     // x < 0: pi - a
    return select(cmplt(y, zero()), sub(zero(), a), a);             // y < 0: -a
}

} // namespace vec
