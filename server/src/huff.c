#include "huff.h"
#include <stdlib.h>
#include <string.h>

/* ---- Unbounded Huffman lengths via a simple binary heap ----------------- */
typedef struct { uint32_t w; int32_t node; } HItem;

static void hpush(HItem *h, int *sz, HItem it) {
    int i = (*sz)++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        /* tie-break on node index for deterministic output */
        if (h[p].w < it.w || (h[p].w == it.w && h[p].node < it.node)) break;
        h[i] = h[p];
        i = p;
    }
    h[i] = it;
}

static HItem hpop(HItem *h, int *sz) {
    HItem top = h[0], last = h[--(*sz)];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= *sz) break;
        if (c + 1 < *sz && (h[c + 1].w < h[c].w || (h[c + 1].w == h[c].w && h[c + 1].node < h[c].node))) c++;
        if (last.w < h[c].w || (last.w == h[c].w && last.node < h[c].node)) break;
        h[i] = h[c];
        i = c;
    }
    h[i] = last;
    return top;
}

int huff_build_lengths(const uint32_t *freq, int n, int maxlen, uint8_t *len) {
    HItem s_heap[256];
    int32_t s_parent[512];
    uint16_t s_depth[512];
    int s_order[256];

    HItem *heap = s_heap;
    int32_t *parent = s_parent;
    uint16_t *depth_internal = s_depth;
    int *order = s_order;
    void *dyn = NULL;

    if (n > 256) {
        size_t bytes = (size_t)n * sizeof(HItem) + 2 * (size_t)n * sizeof(int32_t)
                     + 2 * (size_t)n * sizeof(uint16_t) + (size_t)n * sizeof(int);
        dyn = malloc(bytes);
        if (!dyn) return 0;
        heap = (HItem *)dyn;
        parent = (int32_t *)(heap + n);
        depth_internal = (uint16_t *)(parent + 2 * n);
        order = (int *)(depth_internal + 2 * n);
    }

    int sz = 0, nodes = 0, used = 0;
    memset(len, 0, (size_t)n);
    for (int i = 0; i < n; i++) {
        if (freq[i]) {
            HItem it = { freq[i], (int32_t)i };
            hpush(heap, &sz, it);
            used++;
        }
    }
    if (used < 2) { free(dyn); return used; }
    if (used == 2) {
        len[heap[0].node] = 1;
        len[heap[1].node] = 1;
        free(dyn);
        return 2;
    }

    /* Internal nodes are numbered n.. */
    nodes = n;
    while (sz > 1) {
        HItem a = hpop(heap, &sz), b = hpop(heap, &sz);
        parent[a.node] = (int32_t)nodes;
        parent[b.node] = (int32_t)nodes;
        HItem it = { a.w + b.w, (int32_t)nodes };
        nodes++;
        hpush(heap, &sz, it);
    }
    int root = heap[0].node;

    /* Depth of every leaf; also histogram of depths (can exceed maxlen). */
    int bits[512] = { 0 };
    depth_internal[root] = 0;
    for (int v = root - 1; v >= n; v--) depth_internal[v] = (uint16_t)(depth_internal[parent[v]] + 1);
    int maxd = 0;
    for (int i = 0; i < n; i++) {
        if (freq[i]) {
            int d = depth_internal[parent[i]] + 1;
            if (d > 511) d = 511;
            bits[d]++;
            if (d > maxd) maxd = d;
        }
    }

    /* Length limiting (JPEG Annex K.3): keeps the code complete. */
    for (int i = maxd; i > maxlen; i--) {
        while (bits[i] > 0) {
            int j = i - 2;
            while (bits[j] == 0) j--;
            bits[i] -= 2;
            bits[i - 1] += 1;
            bits[j + 1] += 2;
            bits[j] -= 1;
        }
    }

    /* Hand out the lengths: most frequent symbols get the shortest codes. */
    int k = 0;
    for (int i = 0; i < n; i++) if (freq[i]) order[k++] = i;
    for (int gap = k / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < k; i++) {
            int temp = order[i];
            int j = i;
            while (j >= gap && (freq[order[j - gap]] < freq[temp] || (freq[order[j - gap]] == freq[temp] && order[j - gap] > temp))) {
                order[j] = order[j - gap];
                j -= gap;
            }
            order[j] = temp;
        }
    }
    int idx = 0;
    for (int l = 1; l <= maxlen; l++)
        for (int c = 0; c < bits[l]; c++) len[order[idx++]] = (uint8_t)l;
    free(dyn);
    return used;
}

void huff_build_codes(const uint8_t *len, int n, uint16_t *code) {
    int count[HUFF_MAXLEN + 1] = { 0 };
    uint16_t next[HUFF_MAXLEN + 2];
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint16_t c = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) {
        c = (uint16_t)((c + count[l - 1]) << 1);
        next[l] = c;
    }
    for (int i = 0; i < n; i++) code[i] = len[i] ? next[len[i]]++ : 0;
}

int huff_build_decoder(const uint8_t *len, int n, HuffDec *d) {
    int count[HUFF_MAXLEN + 2] = { 0 };
    for (int i = 0; i < n; i++) {
        if (len[i] > HUFF_MAXLEN) return 0;
        count[len[i]]++;
    }
    count[0] = 0;

    /* Kraft check: must be exactly a complete code with >= 2 symbols. */
    uint32_t kraft = 0, nsym = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) {
        kraft += (uint32_t)count[l] << (HUFF_MAXLEN - l);
        nsym += (uint32_t)count[l];
    }
    if (nsym < 2 || kraft != (1u << HUFF_MAXLEN)) return 0;

    int offs[HUFF_MAXLEN + 2];
    offs[1] = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) offs[l + 1] = offs[l] + count[l];
    {
        int fill[HUFF_MAXLEN + 2];
        memcpy(fill, offs, sizeof(fill));
        for (int i = 0; i < n; i++) if (len[i]) d->sym[fill[len[i]]++] = (uint8_t)i;
    }

    memset(d->fast, 0, sizeof(d->fast));
    d->minlen = 0;
    d->maxlen = 0;
    uint32_t code = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) {
        code <<= 1;                         /* first canonical code of length l */
        uint32_t first_c = code;
        d->base[l] = offs[l] - (int32_t)code;
        code += (uint32_t)count[l];
        d->limit[l] = code << (16 - l);     /* exclusive, left-justified */
        if (count[l]) {
            if (!d->minlen) d->minlen = (uint8_t)l;
            d->maxlen = (uint8_t)l;
            if (l <= HUFF_FAST_BITS) {
                int shift = HUFF_FAST_BITS - l;
                int span = 1 << shift;
                for (int k = 0; k < count[l]; k++) {
                    uint16_t entry = (uint16_t)((l << 8) | d->sym[offs[l] + k]);
                    uint32_t idx = (first_c + (uint32_t)k) << shift;
                    for (int m = 0; m < span; m++) d->fast[idx + m] = entry;
                }
            }
        }
    }
    return 1;
}

int huff_build_rank_decoder(const uint8_t *len, const uint32_t *syms, int n,
                            HuffRankDec *d, uint32_t *sym_pool, uint32_t sym_off) {
    int count[HUFF_MAXLEN + 2] = { 0 };
    for (int i = 0; i < n; i++) {
        if (len[i] > HUFF_MAXLEN) return 0;
        count[len[i]]++;
    }
    count[0] = 0;

    uint32_t kraft = 0, nsym = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) {
        kraft += (uint32_t)count[l] << (HUFF_MAXLEN - l);
        nsym += (uint32_t)count[l];
    }
    if (nsym < 2 || kraft != (1u << HUFF_MAXLEN)) return 0;

    int offs[HUFF_MAXLEN + 2];
    offs[1] = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) offs[l + 1] = offs[l] + count[l];
    {
        int fill[HUFF_MAXLEN + 2];
        memcpy(fill, offs, sizeof(fill));
        for (int i = 0; i < n; i++) {
            if (len[i]) sym_pool[sym_off + (uint32_t)(fill[len[i]]++)] = syms ? syms[i] : (uint32_t)i;
        }
    }

    memset(d->fast, 0, sizeof(d->fast));
    d->sym_off = sym_off;
    d->minlen = 0;
    d->maxlen = 0;
    uint32_t code = 0;
    for (int l = 1; l <= HUFF_MAXLEN; l++) {
        code <<= 1;
        uint32_t first_c = code;
        d->base[l] = offs[l] - (int32_t)code;
        code += (uint32_t)count[l];
        d->limit[l] = code << (16 - l);
        if (count[l]) {
            if (!d->minlen) d->minlen = (uint8_t)l;
            d->maxlen = (uint8_t)l;
            if (l <= 8) {
                int shift = 8 - l;
                int span = 1 << shift;
                for (int k = 0; k < count[l]; k++) {
                    uint32_t sv = sym_pool[sym_off + (uint32_t)(offs[l] + k)];
                    uint32_t entry = ((uint32_t)l << 24) | ((sv + 1u) & 0xFFFFFFu);
                    uint32_t idx = (first_c + (uint32_t)k) << shift;
                    for (int m = 0; m < span; m++) d->fast[idx + m] = entry;
                }
            }
        }
    }
    return 1;
}
