/*
 * NHF3 - N-Huffman, third generation.
 *
 * Based on the N-Huffman algorithm (Ygor D. L. Pereira, FA7, 2008): one Huffman
 * tree per context, where the context is the previous symbol (order-1 graph).
 *
 * New in NHF3:
 *   - canonical, length-limited (15 bit, JPEG Annex K.3 adjustment) codes:
 *     tables are sent as lengths only;
 *   - sparse length tables: Elias-gamma symbol gaps + 4-bit lengths;
 *   - per-context exact cost decision: own table / shared fallback table /
 *     deterministic (0 bits, as in the monograph's "immediate states");
 *   - reversible spatial prediction (MED, JPEG-LS) and channel decorrelation;
 *   - temporal (P-frame) mode for video, decoded in place;
 *   - hardened decoder: every length/size is validated.
 */
#ifndef NHF3_H
#define NHF3_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define NHF3_API __declspec(dllexport)
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define NHF3_API EMSCRIPTEN_KEEPALIVE
#else
#define NHF3_API
#endif

#define NHF3_VERSION 1
#define NHF3_HEADER_BASE 11 /* magic(4) ver(1) flags(1) w(2) h(2) ch(1) */

enum { NHF3_PRED_NONE = 0, NHF3_PRED_LEFT = 1, NHF3_PRED_MED = 2, NHF3_PRED_PIXEL = 3 };

/* Encoder effort. FAST commits to one plane variant (plain or run-length) from the
 * share of repeated symbols and encodes once: for streaming. BEST encodes both
 * variants whenever runs are plausible and keeps the smaller: for files. */
enum { NHF3_EFFORT_FAST = 0, NHF3_EFFORT_BEST = 1 };

enum {
    NHF3_OK = 0,
    NHF3_ERR_ARG = -1,
    NHF3_ERR_MEM = -2,
    NHF3_ERR_MAGIC = -3,
    NHF3_ERR_TRUNCATED = -4,
    NHF3_ERR_CORRUPT = -5,
    NHF3_ERR_SIZE = -6,
};

typedef struct {
    uint64_t header_bits; /* modes + code-length code + table lengths */
    uint64_t data_bits;   /* payload */
    uint32_t ctx_single, ctx_own, ctx_fallback, ctx_unused;
} NHF3PlaneStats;

/* ---- Plane level (one stream of bytes, order-1 context) ----------------- */
/* Encodes n symbols with NHF3_EFFORT_*. Returns a malloc'd buffer (free with nhf3_free). */
NHF3_API uint8_t *nhf3_plane_encode(const uint8_t *sym, uint32_t n, int effort, size_t *out_size, NHF3PlaneStats *stats);
/* Decodes exactly n symbols. Returns NHF3_OK or a negative error. */
NHF3_API int nhf3_plane_decode(const uint8_t *in, size_t in_size, uint8_t *out, uint32_t n);

/* ---- Image / video frame level ------------------------------------------ */
/* src:  interleaved pixels, `stride` bytes per pixel, first `ch` used (1..4).
 * prev: previous reconstructed frame (same layout) for a P-frame, or NULL.
 * pred: NHF3_PRED_NONE, NHF3_PRED_LEFT, NHF3_PRED_MED, or NHF3_PRED_PIXEL
 *       (whole-pixel palette N-Huffman mode, activated by --pixelmode).
 * effort: NHF3_EFFORT_FAST or NHF3_EFFORT_BEST.
 * Returns a malloc'd NHF3 packet. stats (optional) receives one entry per channel. */
NHF3_API uint8_t *nhf3_encode(const uint8_t *src, int w, int h, int ch, int stride,
                              const uint8_t *prev, int pred, int decorrelate, int effort,
                              size_t *out_size, NHF3PlaneStats *stats);

/* Same as nhf3_encode (always lossless). `delta` is reserved for a future
 * near-lossless mode and is currently IGNORED. If recon_out != NULL
 * (w*h*stride bytes), it receives the reconstructed frame (a copy of src). */
NHF3_API uint8_t *nhf3_encode_near(const uint8_t *src, int w, int h, int ch, int stride,
                                   const uint8_t *prev, int pred, int decorrelate, int delta,
                                   uint8_t *recon_out, size_t *out_size, NHF3PlaneStats *stats);

/* Explicit whole-pixel (palette) N-Huffman encoder (equivalent to pred = NHF3_PRED_PIXEL).
 * `delta` is reserved and currently ignored (always lossless). */
NHF3_API uint8_t *nhf3_encode_pixel(const uint8_t *src, int w, int h, int ch, int stride,
                                    const uint8_t *prev, int delta, uint8_t *recon_out,
                                    size_t *out_size, NHF3PlaneStats *stats);

/* Encodes as I-frame and (if prev != NULL) also as P-frame, returning the
 * smaller packet. Updates recon_out (if non-NULL) with the winning mode's
 * reconstruction so the caller can feed it back as `prev` on the next frame.
 * `delta` is reserved and currently ignored (always lossless). */
NHF3_API uint8_t *nhf3_encode_auto(const uint8_t *src, int w, int h, int ch, int stride,
                                   const uint8_t *prev, int pred, int decorrelate, int delta,
                                   uint8_t *recon_out, size_t *out_size, NHF3PlaneStats *stats);

/* Reads the header only. Returns NHF3_OK and fills the fields. */
NHF3_API int nhf3_probe(const uint8_t *in, size_t n, int *w, int *h, int *ch, int *is_pframe);

/* Decodes into dst (interleaved, `stride` bytes per pixel, stride >= ch).
 * For P-frames dst must contain the previous frame: it is updated IN PLACE.
 * w/h/ch must match the packet. */
NHF3_API int nhf3_decode(const uint8_t *in, size_t n, uint8_t *dst, int w, int h, int stride);

NHF3_API void nhf3_free(void *p);
NHF3_API void *nhf3_malloc(size_t n);

#endif
