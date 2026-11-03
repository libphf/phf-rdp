/*
 * NHF3 SIMD Acceleration Layer
 *
 * Provides bit-exact vector implementations of spatial prediction (MED, LEFT)
 * and color decorrelation for:
 *   - x86/x64: SSE2
 *   - ARM/ARM64: NEON
 *   - WebAssembly: SIMD128
 *   - Portable: clean branchless scalar fallback (ESP32, RISC-V, etc.)
 */
#ifndef NHF3_SIMD_H
#define NHF3_SIMD_H

#include <stdint.h>
#include <stddef.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define NHF3_SIMD_NEON 1
#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define NHF3_SIMD_WASM 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NHF3_SIMD_SSE2 1
#endif

static inline uint8_t nhf3_med_scalar(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a > b ? b : a;
    if (c >= mx) return (uint8_t)mn;
    if (c <= mn) return (uint8_t)mx;
    return (uint8_t)(a + b - c);
}

/* ------------------------------------------------------------------------- */
/* Spatial Forward Prediction                                                */
/* ------------------------------------------------------------------------- */
static inline void nhf3_spatial_forward_med(const uint8_t *src, uint8_t *res, int w, int h) {
    if (w <= 0 || h <= 0) return;
    res[0] = src[0];
    for (int x = 1; x < w; x++) res[x] = (uint8_t)(src[x] - src[x - 1]);

#if defined(NHF3_SIMD_SSE2)
    const __m128i z = _mm_setzero_si128();
    for (int y = 1; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w;
        const uint8_t *sp = s - w;
        uint8_t *r = res + (size_t)y * w;
        r[0] = (uint8_t)(s[0] - sp[0]);
        int x = 1;
        for (; x + 16 <= w; x += 16) {
            __m128i A = _mm_loadu_si128((const __m128i *)(s + x - 1));
            __m128i B = _mm_loadu_si128((const __m128i *)(sp + x));
            __m128i C = _mm_loadu_si128((const __m128i *)(sp + x - 1));
            __m128i X = _mm_loadu_si128((const __m128i *)(s + x));

            __m128i mn = _mm_min_epu8(A, B);
            __m128i mx = _mm_max_epu8(A, B);

            __m128i lo = _mm_sub_epi16(_mm_add_epi16(_mm_unpacklo_epi8(A, z), _mm_unpacklo_epi8(B, z)), _mm_unpacklo_epi8(C, z));
            __m128i hi = _mm_sub_epi16(_mm_add_epi16(_mm_unpackhi_epi8(A, z), _mm_unpackhi_epi8(B, z)), _mm_unpackhi_epi8(C, z));
            __m128i g  = _mm_packus_epi16(lo, hi);

            __m128i p = _mm_max_epu8(mn, _mm_min_epu8(mx, g));
            _mm_storeu_si128((__m128i *)(r + x), _mm_sub_epi8(X, p));
        }
        for (; x < w; x++) {
            r[x] = (uint8_t)(s[x] - nhf3_med_scalar(s[x - 1], sp[x], sp[x - 1]));
        }
    }
#elif defined(NHF3_SIMD_NEON)
    for (int y = 1; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w;
        const uint8_t *sp = s - w;
        uint8_t *r = res + (size_t)y * w;
        r[0] = (uint8_t)(s[0] - sp[0]);
        int x = 1;
        for (; x + 16 <= w; x += 16) {
            uint8x16_t A = vld1q_u8(s + x - 1);
            uint8x16_t B = vld1q_u8(sp + x);
            uint8x16_t C = vld1q_u8(sp + x - 1);
            uint8x16_t X = vld1q_u8(s + x);

            uint8x16_t mn = vminq_u8(A, B);
            uint8x16_t mx = vmaxq_u8(A, B);

            int16x8_t lo = vsubq_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(A))),
                                               vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(B)))),
                                     vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(C))));
            int16x8_t hi = vsubq_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(A))),
                                               vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(B)))),
                                     vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(C))));

            uint8x16_t g = vcombine_u8(vqmovun_s16(lo), vqmovun_s16(hi));
            uint8x16_t p = vmaxq_u8(mn, vminq_u8(mx, g));
            vst1q_u8(r + x, vsubq_u8(X, p));
        }
        for (; x < w; x++) {
            r[x] = (uint8_t)(s[x] - nhf3_med_scalar(s[x - 1], sp[x], sp[x - 1]));
        }
    }
#elif defined(NHF3_SIMD_WASM)
    const v128_t z = wasm_i8x16_splat(0);
    for (int y = 1; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w;
        const uint8_t *sp = s - w;
        uint8_t *r = res + (size_t)y * w;
        r[0] = (uint8_t)(s[0] - sp[0]);
        int x = 1;
        for (; x + 16 <= w; x += 16) {
            v128_t A = wasm_v128_load(s + x - 1);
            v128_t B = wasm_v128_load(sp + x);
            v128_t C = wasm_v128_load(sp + x - 1);
            v128_t X = wasm_v128_load(s + x);

            v128_t mn = wasm_u8x16_min(A, B);
            v128_t mx = wasm_u8x16_max(A, B);

            v128_t lo = wasm_i16x8_sub(wasm_i16x8_add(wasm_u16x8_extend_low_u8x16(A), wasm_u16x8_extend_low_u8x16(B)), wasm_u16x8_extend_low_u8x16(C));
            v128_t hi = wasm_i16x8_sub(wasm_i16x8_add(wasm_u16x8_extend_high_u8x16(A), wasm_u16x8_extend_high_u8x16(B)), wasm_u16x8_extend_high_u8x16(C));
            v128_t g = wasm_u8x16_narrow_i16x8(lo, hi);

            v128_t p = wasm_u8x16_max(mn, wasm_u8x16_min(mx, g));
            wasm_v128_store(r + x, wasm_i8x16_sub(X, p));
        }
        for (; x < w; x++) {
            r[x] = (uint8_t)(s[x] - nhf3_med_scalar(s[x - 1], sp[x], sp[x - 1]));
        }
    }
#else
    for (int y = 1; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w;
        const uint8_t *sp = s - w;
        uint8_t *r = res + (size_t)y * w;
        r[0] = (uint8_t)(s[0] - sp[0]);
        for (int x = 1; x < w; x++) {
            r[x] = (uint8_t)(s[x] - nhf3_med_scalar(s[x - 1], sp[x], sp[x - 1]));
        }
    }
#endif
}

static inline void nhf3_spatial_forward_left(const uint8_t *src, uint8_t *res, int w, int h) {
    if (w <= 0 || h <= 0) return;
    res[0] = src[0];
    for (int x = 1; x < w; x++) res[x] = (uint8_t)(src[x] - src[x - 1]);
    for (int y = 1; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w;
        uint8_t *r = res + (size_t)y * w;
        r[0] = (uint8_t)(s[0] - s[-w]);
        for (int x = 1; x < w; x++) r[x] = (uint8_t)(s[x] - s[x - 1]);
    }
}

/* ------------------------------------------------------------------------- */
/* Spatial Inverse Prediction (Branchless / Fast)                            */
/* ------------------------------------------------------------------------- */
static inline void nhf3_spatial_inverse_med(uint8_t *buf, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int x = 1; x < w; x++) buf[x] = (uint8_t)(buf[x] + buf[x - 1]);
    for (int y = 1; y < h; y++) {
        uint8_t *row = buf + (size_t)y * w;
        const uint8_t *prow = row - w;
        int a = (uint8_t)(row[0] + prow[0]);
        row[0] = (uint8_t)a;
        for (int x = 1; x < w; x++) {
            int b = prow[x], c = prow[x - 1];
            int mn = a < b ? a : b;
            int mx = a ^ b ^ mn;
            int g = a + b - c;
            g = g < mn ? mn : g;
            g = g > mx ? mx : g;
            a = (uint8_t)(row[x] + g);
            row[x] = (uint8_t)a;
        }
    }
}

/* Three planes in one pass: their serial dependency chains are independent, so an
 * out-of-order core overlaps them (about 2x faster than three separate passes on
 * Apple M3; little gain on in-order cores). Same output as nhf3_spatial_inverse_med. */
#define NHF3_MED_STEP(row, prow, a, x)                                          \
    do {                                                                        \
        int b_ = (prow)[x], c_ = (prow)[(x) - 1];                               \
        int mn_ = (a) < b_ ? (a) : b_;                                          \
        int mx_ = (a) ^ b_ ^ mn_;                                               \
        int g_ = (a) + b_ - c_;                                                 \
        g_ = g_ < mn_ ? mn_ : g_;                                               \
        g_ = g_ > mx_ ? mx_ : g_;                                               \
        (a) = (uint8_t)((row)[x] + g_);                                         \
        (row)[x] = (uint8_t)(a);                                                \
    } while (0)

static inline void nhf3_spatial_inverse_med3(uint8_t *p0, uint8_t *p1, uint8_t *p2, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int x = 1; x < w; x++) {
        p0[x] = (uint8_t)(p0[x] + p0[x - 1]);
        p1[x] = (uint8_t)(p1[x] + p1[x - 1]);
        p2[x] = (uint8_t)(p2[x] + p2[x - 1]);
    }
    for (int y = 1; y < h; y++) {
        uint8_t *r0 = p0 + (size_t)y * w, *r1 = p1 + (size_t)y * w, *r2 = p2 + (size_t)y * w;
        const uint8_t *q0 = r0 - w, *q1 = r1 - w, *q2 = r2 - w;
        int a0 = (uint8_t)(r0[0] + q0[0]), a1 = (uint8_t)(r1[0] + q1[0]), a2 = (uint8_t)(r2[0] + q2[0]);
        r0[0] = (uint8_t)a0; r1[0] = (uint8_t)a1; r2[0] = (uint8_t)a2;
        for (int x = 1; x < w; x++) {
            NHF3_MED_STEP(r0, q0, a0, x);
            NHF3_MED_STEP(r1, q1, a1, x);
            NHF3_MED_STEP(r2, q2, a2, x);
        }
    }
}

static inline void nhf3_spatial_inverse_left(uint8_t *buf, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int x = 1; x < w; x++) buf[x] = (uint8_t)(buf[x] + buf[x - 1]);
    for (int y = 1; y < h; y++) {
        uint8_t *row = buf + (size_t)y * w;
        row[0] = (uint8_t)(row[0] + row[-w]);
        for (int x = 1; x < w; x++) row[x] = (uint8_t)(row[x] + row[x - 1]);
    }
}

#endif /* NHF3_SIMD_H */
