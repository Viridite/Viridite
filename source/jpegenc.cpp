#include "jpegenc.h"

#include <cmath>
#include <cstring>

namespace {

// ── Annex K tables ──────────────────────────────────────────────────────────
// The example tables from the JPEG standard. Every decoder in the world has
// been tested against files using these.

const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63,
};

const uint16_t QUANT_LUMA[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,   12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,   14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,   24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,   72, 92, 95, 98,112,100,103, 99,
};

const uint16_t QUANT_CHROMA[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,   18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,   47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,   99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,   99, 99, 99, 99, 99, 99, 99, 99,
};

// Huffman tables, as code-length counts plus the symbols in order — the same
// shape the DHT segment carries, so they are written out verbatim.
const uint8_t DC_LUMA_BITS[17]   = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
const uint8_t DC_LUMA_VALS[12]   = {0,1,2,3,4,5,6,7,8,9,10,11};
const uint8_t DC_CHROMA_BITS[17] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
const uint8_t DC_CHROMA_VALS[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

const uint8_t AC_LUMA_BITS[17] = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
const uint8_t AC_LUMA_VALS[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa,
};

const uint8_t AC_CHROMA_BITS[17] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
const uint8_t AC_CHROMA_VALS[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa,
};

// A canonical Huffman table expanded into (code, length) per symbol.
struct Huff { uint16_t code[256]; uint8_t size[256]; };

void buildHuff(const uint8_t* bits, const uint8_t* vals, int nvals, Huff& h) {
    memset(&h, 0, sizeof h);
    uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len]; i++) {
            if (k >= nvals) break;
            h.code[vals[k]] = code;
            h.size[vals[k]] = (uint8_t)len;
            code++; k++;
        }
        code <<= 1;
    }
}

// ── Output, with the buffer limit respected everywhere ──────────────────────
struct Sink {
    uint8_t* p;
    size_t   cap, n = 0;
    bool     over = false;

    void byte(uint8_t b) {
        if (n >= cap) { over = true; return; }
        p[n++] = b;
    }
    void word(uint16_t v) { byte((uint8_t)(v >> 8)); byte((uint8_t)v); }
    void raw(const uint8_t* d, size_t len) { for (size_t i = 0; i < len; i++) byte(d[i]); }

    // Entropy-coded bits. A 0xFF byte in the stream must be followed by 0x00,
    // or a decoder reads it as the start of a marker.
    uint32_t acc = 0;
    int      accBits = 0;
    void bits(uint16_t code, int len) {
        if (len <= 0) return;
        acc = (acc << len) | (code & ((1u << len) - 1));
        accBits += len;
        while (accBits >= 8) {
            accBits -= 8;
            const uint8_t b = (uint8_t)(acc >> accBits);
            byte(b);
            if (b == 0xFF) byte(0x00);
        }
    }
    void flushBits() {
        while (accBits > 0) {
            const int take = accBits >= 8 ? 8 : accBits;
            const uint8_t b = (uint8_t)((acc << (8 - accBits)) | ((1u << (8 - accBits)) - 1));
            (void)take;
            byte(b);
            if (b == 0xFF) byte(0x00);
            accBits -= (accBits >= 8 ? 8 : accBits);
        }
        accBits = 0;
    }
};

// ── Forward DCT, the separable float one ────────────────────────────────────
// Accuracy is not the constraint here — an icon at quality 90 has margin to
// spare — and the plain separable form is the version that is obviously right
// when read, which matters more than the cycles a fast integer DCT would save
// on a 256x256 image encoded once per forwarder.
void fdct8x8(const float* in, float* out) {
    static float cosTab[8][8];
    static bool  ready = false;
    if (!ready) {
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                cosTab[x][u] = cosf((2.0f * x + 1.0f) * u * 3.14159265358979f / 16.0f);
        ready = true;
    }
    float tmp[64];
    for (int y = 0; y < 8; y++)                    // rows
        for (int u = 0; u < 8; u++) {
            float s = 0.0f;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * cosTab[x][u];
            tmp[y * 8 + u] = s * (u == 0 ? 0.353553391f : 0.5f);
        }
    for (int u = 0; u < 8; u++)                    // columns
        for (int v = 0; v < 8; v++) {
            float s = 0.0f;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + u] * cosTab[y][v];
            out[v * 8 + u] = s * (v == 0 ? 0.353553391f : 0.5f);
        }
}

int magnitude(int v) {
    int n = 0;
    int a = v < 0 ? -v : v;
    while (a) { n++; a >>= 1; }
    return n;
}

void encodeBlock(Sink& s, const float* pixels, const uint16_t* quant,
                 const Huff& dc, const Huff& ac, int& prevDC) {
    float coef[64];
    fdct8x8(pixels, coef);

    int q[64];
    for (int i = 0; i < 64; i++) {
        const float v = coef[i] / quant[i];
        q[i] = (int)lrintf(v);
    }

    // DC: coded as a difference from the previous block of the same component.
    const int diff = q[0] - prevDC;
    prevDC = q[0];
    const int dcMag = magnitude(diff);
    s.bits(dc.code[dcMag], dc.size[dcMag]);
    if (dcMag) s.bits((uint16_t)(diff < 0 ? diff - 1 : diff), dcMag);

    // AC: run-length of zeros in zigzag order, then the value.
    int run = 0;
    for (int k = 1; k < 64; k++) {
        const int v = q[ZIGZAG[k]];
        if (v == 0) { run++; continue; }
        while (run > 15) { s.bits(ac.code[0xF0], ac.size[0xF0]); run -= 16; }  // ZRL
        const int mag = magnitude(v);
        const int sym = (run << 4) | mag;
        s.bits(ac.code[sym], ac.size[sym]);
        s.bits((uint16_t)(v < 0 ? v - 1 : v), mag);
        run = 0;
    }
    if (run > 0) s.bits(ac.code[0x00], ac.size[0x00]);   // EOB
}

void writeDQT(Sink& s, const uint16_t* tbl, int id) {
    s.word(0xFFDB);
    s.word(2 + 1 + 64);
    s.byte((uint8_t)id);                                  // 8-bit precision
    for (int i = 0; i < 64; i++) s.byte((uint8_t)tbl[ZIGZAG[i]]);
}

void writeDHT(Sink& s, int cls, int id, const uint8_t* bits, const uint8_t* vals, int nvals) {
    s.word(0xFFC4);
    s.word((uint16_t)(2 + 1 + 16 + nvals));
    s.byte((uint8_t)((cls << 4) | id));
    for (int i = 1; i <= 16; i++) s.byte(bits[i]);
    s.raw(vals, (size_t)nvals);
}

}  // namespace

size_t jpegEncodeRGB(const uint8_t* rgb, int w, int h, int stride,
                     int quality, uint8_t* out, size_t cap) {
    if (!rgb || !out || w <= 0 || h <= 0 || cap < 1024) return 0;

    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

    uint16_t qL[64], qC[64];
    for (int i = 0; i < 64; i++) {
        int a = (QUANT_LUMA[i]   * scale + 50) / 100;
        int b = (QUANT_CHROMA[i] * scale + 50) / 100;
        qL[i] = (uint16_t)(a < 1 ? 1 : (a > 255 ? 255 : a));
        qC[i] = (uint16_t)(b < 1 ? 1 : (b > 255 ? 255 : b));
    }

    Huff dcL, acL, dcC, acC;
    buildHuff(DC_LUMA_BITS,   DC_LUMA_VALS,   12,  dcL);
    buildHuff(AC_LUMA_BITS,   AC_LUMA_VALS,   162, acL);
    buildHuff(DC_CHROMA_BITS, DC_CHROMA_VALS, 12,  dcC);
    buildHuff(AC_CHROMA_BITS, AC_CHROMA_VALS, 162, acC);

    Sink s{out, cap};

    s.word(0xFFD8);                                       // SOI

    s.word(0xFFE0); s.word(16);                           // APP0 / JFIF
    s.raw((const uint8_t*)"JFIF\0", 5);
    s.byte(1); s.byte(1);                                 // version 1.1
    s.byte(0);                                            // no density units
    s.word(1); s.word(1);                                 // aspect 1:1
    s.byte(0); s.byte(0);                                 // no thumbnail

    writeDQT(s, qL, 0);
    writeDQT(s, qC, 1);

    s.word(0xFFC0); s.word(8 + 3 * 3);                    // SOF0, 3 components
    s.byte(8);
    s.word((uint16_t)h); s.word((uint16_t)w);
    s.byte(3);
    for (int c = 0; c < 3; c++) {
        s.byte((uint8_t)(c + 1));
        s.byte(0x11);                                     // 4:4:4, no subsampling
        s.byte((uint8_t)(c == 0 ? 0 : 1));                // quant table
    }

    writeDHT(s, 0, 0, DC_LUMA_BITS,   DC_LUMA_VALS,   12);
    writeDHT(s, 1, 0, AC_LUMA_BITS,   AC_LUMA_VALS,   162);
    writeDHT(s, 0, 1, DC_CHROMA_BITS, DC_CHROMA_VALS, 12);
    writeDHT(s, 1, 1, AC_CHROMA_BITS, AC_CHROMA_VALS, 162);

    s.word(0xFFDA); s.word(6 + 2 * 3);                    // SOS
    s.byte(3);
    for (int c = 0; c < 3; c++) {
        s.byte((uint8_t)(c + 1));
        s.byte((uint8_t)(c == 0 ? 0x00 : 0x11));          // DC/AC table ids
    }
    s.byte(0); s.byte(63); s.byte(0);                     // baseline spectral range

    int dcY = 0, dcCb = 0, dcCr = 0;
    const int mcuX = (w + 7) / 8, mcuY = (h + 7) / 8;

    for (int my = 0; my < mcuY && !s.over; my++) {
        for (int mx = 0; mx < mcuX && !s.over; mx++) {
            float Y[64], Cb[64], Cr[64];
            for (int y = 0; y < 8; y++) {
                // Edge blocks repeat the last row/column rather than reading
                // past the image, which is what keeps a non-multiple-of-8 size
                // from smearing garbage into the bottom and right edges.
                const int sy = my * 8 + y < h ? my * 8 + y : h - 1;
                for (int x = 0; x < 8; x++) {
                    const int sx = mx * 8 + x < w ? mx * 8 + x : w - 1;
                    const uint8_t* px = rgb + (size_t)sy * stride + (size_t)sx * 3;
                    const float r = px[0], g = px[1], b = px[2];
                    Y [y * 8 + x] =  0.299000f * r + 0.587000f * g + 0.114000f * b - 128.0f;
                    Cb[y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.500000f * b;
                    Cr[y * 8 + x] =  0.500000f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            encodeBlock(s, Y,  qL, dcL, acL, dcY);
            encodeBlock(s, Cb, qC, dcC, acC, dcCb);
            encodeBlock(s, Cr, qC, dcC, acC, dcCr);
        }
    }

    s.flushBits();
    s.word(0xFFD9);                                       // EOI

    return s.over ? 0 : s.n;
}
