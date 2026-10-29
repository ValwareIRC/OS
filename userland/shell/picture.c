#define _GNU_SOURCE
#include "picture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Baseline JPEG (ITU T.81) with the standard Annex K tables: what every
 * decoder reads, small enough to send a screenshot to a model in a few
 * tens of kilobytes. Straightforward rather than fast: the DCT is the
 * textbook separable one, and a half-size screenshot takes a few
 * milliseconds anyway. */

static const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                   12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                   35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                   58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const uint8_t q_luma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                   14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                   18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                   49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

static const uint8_t q_chroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                     24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

/* Huffman tables: how many codes of each length 1..16, then the symbols. */
static const uint8_t dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    uint16_t code[256];
    uint8_t size[256];
} huff;

/* Canonical codes from the counts per length (T.81 Annex C). */
static void huff_build(huff *h, const uint8_t bits[16], const uint8_t *vals) {
    memset(h, 0, sizeof *h);
    unsigned code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++, k++) {
            h->code[vals[k]] = (uint16_t)code++;
            h->size[vals[k]] = (uint8_t)len;
        }
        code <<= 1;
    }
}

typedef struct {
    unsigned char *p;
    size_t len, cap;
    uint32_t acc; /* bits waiting to be written, high bits first */
    int nacc;
} out;

static void put(out *o, unsigned char b) {
    if (o->len == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 65536;
        o->p = realloc(o->p, o->cap);
    }
    o->p[o->len++] = b;
}

static void put16(out *o, unsigned v) {
    put(o, (unsigned char)(v >> 8));
    put(o, (unsigned char)v);
}

static void bits(out *o, unsigned code, int n) {
    o->acc = (o->acc << n) | (code & ((1u << n) - 1));
    o->nacc += n;
    while (o->nacc >= 8) {
        unsigned char b = (unsigned char)(o->acc >> (o->nacc - 8));
        put(o, b);
        if (b == 0xff) /* a marker byte inside data is stuffed with a zero */
            put(o, 0);
        o->nacc -= 8;
    }
}

/* Category (bit length) of a coefficient and the bits that encode it. */
static int category(int v, unsigned *enc) {
    int a = v < 0 ? -v : v, n = 0;
    while (a) {
        n++;
        a >>= 1;
    }
    *enc = v < 0 ? (unsigned)(v + (1 << n) - 1) : (unsigned)v;
    return n;
}

static float cosines[8][8]; /* cos((2x+1) u pi / 16) */

/* One 8x8 block: DCT, quantise, entropy-code. Returns the new DC value. */
static int block(out *o, const float in[64], const float qdiv[64], int prev_dc, const huff *dc, const huff *ac) {
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++)
                s += in[y * 8 + x] * cosines[x][u];
            tmp[y * 8 + u] = s;
        }
    int q[64];
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int y = 0; y < 8; y++)
                s += tmp[y * 8 + u] * cosines[y][v];
            float cu = u ? 1 : (float)M_SQRT1_2, cv = v ? 1 : (float)M_SQRT1_2;
            float f = 0.25f * cu * cv * s;
            q[v * 8 + u] = (int)lroundf(f / qdiv[v * 8 + u]);
        }
    unsigned enc;
    int diff = q[0] - prev_dc, n = category(diff, &enc);
    bits(o, dc->code[n], dc->size[n]);
    if (n)
        bits(o, enc, n);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = q[zigzag[k]];
        if (!v) {
            run++;
            continue;
        }
        while (run >= 16) {
            bits(o, ac->code[0xf0], ac->size[0xf0]);
            run -= 16;
        }
        n = category(v, &enc);
        int sym = run << 4 | n;
        bits(o, ac->code[sym], ac->size[sym]);
        bits(o, enc, n);
        run = 0;
    }
    if (run)
        bits(o, ac->code[0], ac->size[0]); /* end of block */
    return q[0];
}

static void dht(out *o, int id, const uint8_t b[16], const uint8_t *vals) {
    int n = 0;
    put(o, (unsigned char)id);
    for (int i = 0; i < 16; i++) {
        put(o, b[i]);
        n += b[i];
    }
    for (int i = 0; i < n; i++)
        put(o, vals[i]);
}

unsigned char *jpeg_encode(const uint32_t *px, int w, int h, int stride, int quality, size_t *len) {
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535)
        return NULL;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t ql[64], qc[64];
    float dl[64], dc[64];
    for (int i = 0; i < 64; i++) {
        int a = (q_luma[i] * scale + 50) / 100, b = (q_chroma[i] * scale + 50) / 100;
        ql[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
        qc[i] = (uint8_t)(b < 1 ? 1 : b > 255 ? 255 : b);
        dl[i] = ql[i];
        dc[i] = qc[i];
    }
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            cosines[x][u] = cosf((2 * x + 1) * u * (float)M_PI / 16);
    huff hdl, hal, hdc, hac;
    huff_build(&hdl, dc_luma_bits, dc_vals);
    huff_build(&hal, ac_luma_bits, ac_luma_vals);
    huff_build(&hdc, dc_chroma_bits, dc_vals);
    huff_build(&hac, ac_chroma_bits, ac_chroma_vals);

    out o = {0};
    static const unsigned char jfif[] = {0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    for (size_t i = 0; i < sizeof jfif; i++)
        put(&o, jfif[i]);
    put16(&o, 0xffdb);
    put16(&o, 2 + 2 * 65);
    put(&o, 0);
    for (int k = 0; k < 64; k++)
        put(&o, ql[zigzag[k]]);
    put(&o, 1);
    for (int k = 0; k < 64; k++)
        put(&o, qc[zigzag[k]]);
    put16(&o, 0xffc0); /* baseline frame: Y at 2x2, Cb and Cr at 1x1 */
    put16(&o, 17);
    put(&o, 8);
    put16(&o, (unsigned)h);
    put16(&o, (unsigned)w);
    put(&o, 3);
    put(&o, 1), put(&o, 0x22), put(&o, 0);
    put(&o, 2), put(&o, 0x11), put(&o, 1);
    put(&o, 3), put(&o, 0x11), put(&o, 1);
    put16(&o, 0xffc4);
    put16(&o, 2 + (1 + 16 + 12) * 2 + (1 + 16 + 162) * 2);
    dht(&o, 0x00, dc_luma_bits, dc_vals);
    dht(&o, 0x10, ac_luma_bits, ac_luma_vals);
    dht(&o, 0x01, dc_chroma_bits, dc_vals);
    dht(&o, 0x11, ac_chroma_bits, ac_chroma_vals);
    static const unsigned char sos[] = {0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    for (size_t i = 0; i < sizeof sos; i++)
        put(&o, sos[i]);

    int pdy = 0, pdb = 0, pdr = 0;
    for (int my = 0; my < h; my += 16)
        for (int mx = 0; mx < w; mx += 16) {
            float Y[4][64], Cb[64] = {0}, Cr[64] = {0};
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    int sx = mx + x < w ? mx + x : w - 1, sy = my + y < h ? my + y : h - 1;
                    uint32_t p = px[(size_t)sy * (size_t)stride + (size_t)sx];
                    float r = (float)C_R(p), g = (float)C_G(p), b = (float)C_B(p);
                    int blk = (y >> 3) * 2 + (x >> 3);
                    Y[blk][(y & 7) * 8 + (x & 7)] = 0.299f * r + 0.587f * g + 0.114f * b - 128;
                    int ci = (y >> 1) * 8 + (x >> 1);
                    Cb[ci] += 0.25f * (-0.168736f * r - 0.331264f * g + 0.5f * b);
                    Cr[ci] += 0.25f * (0.5f * r - 0.418688f * g - 0.081312f * b);
                }
            for (int k = 0; k < 4; k++)
                pdy = block(&o, Y[k], dl, pdy, &hdl, &hal);
            pdb = block(&o, Cb, dc, pdb, &hdc, &hac);
            pdr = block(&o, Cr, dc, pdr, &hdc, &hac);
        }
    if (o.nacc > 0) /* pad the last byte with ones */
        bits(&o, 0x7f, 8 - o.nacc);
    put16(&o, 0xffd9);
    *len = o.len;
    return o.p;
}

canvas *canvas_shrink(const canvas *src, int f) {
    if (f < 1)
        f = 1;
    int w = src->w / f, h = src->h / f;
    canvas *c = canvas_new(w, h);
    if (!c)
        return NULL;
    int n = f * f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < f; j++)
                for (int i = 0; i < f; i++) {
                    uint32_t p = src->px[(size_t)(y * f + j) * (size_t)src->w + (size_t)(x * f + i)];
                    r += C_R(p), g += C_G(p), b += C_B(p);
                }
            c->px[(size_t)y * (size_t)w + (size_t)x] = RGBA(r / n, g / n, b / n, 255);
        }
    return c;
}

char *base64_encode(const unsigned char *d, size_t len) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *s = malloc((len + 2) / 3 * 4 + 1), *o = s;
    if (!s)
        return NULL;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        *o++ = tab[v >> 18], *o++ = tab[(v >> 12) & 63], *o++ = tab[(v >> 6) & 63], *o++ = tab[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < len ? (uint32_t)d[i + 1] << 8 : 0);
        *o++ = tab[v >> 18], *o++ = tab[(v >> 12) & 63];
        *o++ = i + 1 < len ? tab[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
    return s;
}
