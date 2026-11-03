/*
 * NHF3 - canonical, length-limited Huffman codes.
 *
 * Canonical codes are fully determined by the code LENGTH of each symbol, so a
 * table can be transmitted as lengths only (no tree shape, no symbol values in
 * the leaves). This is the core of the fix for the "graph storage" problem
 * described in the 2008 monograph.
 */
#ifndef NHF3_HUFF_H
#define NHF3_HUFF_H

#include <stdint.h>
#include "bitio.h"

#define HUFF_MAXLEN 15
#define HUFF_FAST_BITS 10
#define HUFF_FAST_SIZE (1 << HUFF_FAST_BITS)

/* Computes length-limited Huffman code lengths (<= maxlen) for n <= 16384
 * symbols. Symbols with freq 0 get length 0. Requires at least 2 symbols with
 * non-zero frequency; the resulting code is always complete (Kraft sum == 1).
 * Returns the number of used symbols. */
int huff_build_lengths(const uint32_t *freq, int n, int maxlen, uint8_t *len);

/* Assigns canonical codes from lengths. */
void huff_build_codes(const uint8_t *len, int n, uint16_t *code);

/* Fast canonical decoder: O(1) 10-bit lookup table for codes <= 10 bits,
 * falling back to 16-bit left-justified limit comparison for lengths 11..15. */
typedef struct {
    uint16_t fast[HUFF_FAST_SIZE];   /* (len << 8) | sym for len <= 10, else 0 */
    uint32_t limit[HUFF_MAXLEN + 2]; /* exclusive upper bound, left-justified */
    int32_t base[HUFF_MAXLEN + 2];   /* index into sym[] minus first code */
    uint8_t sym[256];
    uint8_t minlen, maxlen;
} HuffDec;

/* Builds a decoder. Returns 0 if the lengths do not form a complete prefix
 * code (corrupt or malicious input), 1 on success. */
int huff_build_decoder(const uint8_t *len, int n, HuffDec *d);

static inline int huff_decode(BitReader *r, const HuffDec *d) {
    if (r->n < 16) br_fill(r);
    uint32_t v = br_peek(r, 16);
    uint16_t f = d->fast[v >> (16 - HUFF_FAST_BITS)];
    if (f) {
        br_skip(r, f >> 8);
        return f & 0xFF;
    }
    for (int l = HUFF_FAST_BITS + 1; l <= d->maxlen; l++) {
        if (v < d->limit[l]) {
            br_skip(r, l);
            return d->sym[d->base[l] + (int32_t)(v >> (16 - l))];
        }
    }
    return -1; /* unreachable for complete codes */
}

/* Rank-only canonical decoder for general alphabets (pixel/palette mode):
 * decodes canonical rank in [0, nsym-1], which indexes an external symbol pool. */
typedef struct {
    uint32_t fast[256]; /* (len << 24) | (sym + 1) for len <= 8, else 0 */
    uint32_t limit[HUFF_MAXLEN + 2];
    int32_t base[HUFF_MAXLEN + 2];
    uint32_t sym_off;
    uint8_t minlen, maxlen;
} HuffRankDec;

int huff_build_rank_decoder(const uint8_t *len, const uint32_t *syms, int n,
                            HuffRankDec *d, uint32_t *sym_pool, uint32_t sym_off);

static inline int huff_decode_rank(BitReader *r, const HuffRankDec *d, const uint32_t *sym_pool) {
    if (r->n < 16) br_fill(r);
    uint32_t v = br_peek(r, 16);
    uint32_t f = d->fast[v >> 8];
    if (f) {
        br_skip(r, (int)(f >> 24));
        return (int)((f & 0xFFFFFFu) - 1u);
    }
    for (int l = 9; l <= d->maxlen; l++) {
        if (v < d->limit[l]) {
            br_skip(r, l);
            return (int)sym_pool[d->sym_off + (uint32_t)(d->base[l] + (int32_t)(v >> (16 - l)))];
        }
    }
    return -1;
}

#endif
