/*
 * NHF3 - bit-level I/O (MSB-first).
 *
 * The reader never touches memory beyond the input buffer: reads past the end
 * return zero bits and are tracked, so decoders can reject truncated streams.
 */
#ifndef NHF3_BITIO_H
#define NHF3_BITIO_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
    #include <stdlib.h>
    #define NHF3_BSWAP32(x) _byteswap_ulong(x)
    #define NHF3_BSWAP64(x) _byteswap_uint64(x)
#elif defined(__GNUC__) || defined(__clang__)
    #define NHF3_BSWAP32(x) __builtin_bswap32(x)
    #define NHF3_BSWAP64(x) __builtin_bswap64(x)
#else
    static inline uint32_t NHF3_BSWAP32(uint32_t x) {
        return (((x) >> 24) | (((x) >> 8) & 0xff00) | (((x) << 8) & 0xff0000) | ((x) << 24));
    }
    static inline uint64_t NHF3_BSWAP64(uint64_t x) {
        return (((uint64_t)NHF3_BSWAP32((uint32_t)x) << 32) | NHF3_BSWAP32((uint32_t)(x >> 32)));
    }
#endif

/* ------------------------------------------------------------------------- */
/* Writer                                                                    */
/* ------------------------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    uint64_t acc; /* pending bits, right-aligned */
    int n;        /* number of pending bits (< 32 between calls) */
    int oom;      /* set if an allocation failed */
} BitWriter;

static inline void bw_init(BitWriter *w, size_t cap) {
    if (cap < 256) cap = 256;
    w->buf = (uint8_t *)malloc(cap);
    w->cap = w->buf ? cap : 0;
    w->pos = 0;
    w->acc = 0;
    w->n = 0;
    w->oom = (w->buf == NULL);
}

static inline int bw_reserve(BitWriter *w, size_t extra) {
    if (w->pos + extra <= w->cap) return 1;
    size_t ncap = w->cap * 2;
    if (ncap < w->pos + extra) ncap = w->pos + extra + 1024;
    uint8_t *nb = (uint8_t *)realloc(w->buf, ncap);
    if (!nb) { w->oom = 1; return 0; }
    w->buf = nb;
    w->cap = ncap;
    return 1;
}

/* Appends the low `len` bits of `code` (0 <= len <= 32). */
static inline void bw_put(BitWriter *w, uint32_t code, int len) {
    if (len <= 0) return;
    w->acc = (w->acc << len) | ((uint64_t)code & ((1ULL << len) - 1));
    w->n += len;
    if (w->n >= 32) {
        if (w->pos + 8 > w->cap && !bw_reserve(w, 8)) { w->n = 0; return; }
        uint32_t top = (uint32_t)(w->acc >> (w->n - 32));
        uint32_t be = NHF3_BSWAP32(top);
        memcpy(w->buf + w->pos, &be, 4);
        w->pos += 4;
        w->n -= 32;
    }
}

/* Pads with zero bits up to the next byte boundary and flushes. */
static inline void bw_flush(BitWriter *w) {
    if (!bw_reserve(w, 8)) return;
    while (w->n >= 8) {
        w->n -= 8;
        w->buf[w->pos++] = (uint8_t)(w->acc >> w->n);
    }
    if (w->n > 0) {
        w->buf[w->pos++] = (uint8_t)(w->acc << (8 - w->n));
        w->n = 0;
    }
    w->acc = 0;
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *p;
    size_t size;
    size_t pos;   /* real bytes consumed from p */
    size_t vpos;  /* bytes fed into acc, including zero padding past the end */
    uint64_t acc; /* left-aligned: the next bit is bit 63 */
    int n;        /* valid bits in acc */
} BitReader;

static inline void br_init(BitReader *r, const uint8_t *p, size_t size) {
    r->p = p;
    r->size = size;
    r->pos = 0;
    r->vpos = 0;
    r->acc = 0;
    r->n = 0;
}

static inline void br_fill(BitReader *r) {
    if (r->pos + 8 <= r->size) {
        while (r->n <= 32) {
            uint32_t b32;
            memcpy(&b32, r->p + r->pos, 4);
            uint64_t b = (uint64_t)NHF3_BSWAP32(b32);
            r->pos += 4;
            r->vpos += 4;
            r->acc |= b << (32 - r->n);
            r->n += 32;
        }
        while (r->n <= 56) {
            uint64_t b = r->p[r->pos++];
            r->vpos++;
            r->acc |= b << (56 - r->n);
            r->n += 8;
        }
        return;
    }
    while (r->n <= 56) {
        uint64_t b = 0;
        if (r->pos < r->size) b = r->p[r->pos++];
        r->vpos++;
        r->acc |= b << (56 - r->n);
        r->n += 8;
    }
}

/* 1 <= k <= 32. Caller must ensure r->n >= k (call br_fill first). */
static inline uint32_t br_peek(const BitReader *r, int k) {
    return (uint32_t)(r->acc >> (64 - k));
}

static inline void br_skip(BitReader *r, int k) {
    r->acc <<= k;
    r->n -= k;
}

static inline uint32_t br_get(BitReader *r, int k) {
    if (k <= 0) return 0;
    if (r->n < k) br_fill(r);
    uint32_t v = br_peek(r, k);
    br_skip(r, k);
    return v;
}

/* True if the decoder consumed more bits than the input actually had. */
static inline int br_overrun(const BitReader *r) {
    uint64_t consumed = (uint64_t)r->vpos * 8 - (uint64_t)r->n;
    return consumed > (uint64_t)r->size * 8;
}

#endif
