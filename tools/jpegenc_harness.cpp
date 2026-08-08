// ─── Host harness for the launcher's JPEG encoder ───────────────────────────
//
// source/jpegenc.cpp depends on nothing but the C library, which is what makes
// this possible: it compiles against the host toolchain unchanged, so the
// encoder can be checked against a real decoder on the dev box instead of only
// on a console. Reads a binary PPM, writes a JPEG, prints the byte count.
//
// Built and driven by tools/test_jpegenc.py — see there for the checks.

#include "jpegenc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// PPM headers allow comments and arbitrary whitespace between tokens.
static int readTok(FILE* f) {
    int c, v = 0;
    do {
        c = fgetc(f);
        if (c == '#') while (c != '\n' && c != EOF) c = fgetc(f);
    } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    if (c < '0' || c > '9') return -1;
    while (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); c = fgetc(f); }
    return v;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.ppm out.jpg quality [row_pad] [cap]\n", argv[0]);
        return 2;
    }
    const int  quality = atoi(argv[3]);
    const int  pad     = argc > 4 ? atoi(argv[4]) : 0;   // exercises the `stride` argument
    const long capArg  = argc > 5 ? atol(argv[5]) : 0;

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    char magic[3] = {0};
    if (fread(magic, 1, 2, f) != 2 || strcmp(magic, "P6") != 0) {
        fprintf(stderr, "not a binary PPM\n"); fclose(f); return 2;
    }
    const int w = readTok(f), h = readTok(f), maxv = readTok(f);
    if (w <= 0 || h <= 0 || maxv != 255) {
        fprintf(stderr, "bad PPM header (%d %d %d)\n", w, h, maxv); fclose(f); return 2;
    }

    // Poison the inter-row padding, so a stride bug shows up as visible damage
    // rather than as plausible-looking pixels.
    const int stride = w * 3 + pad;
    std::vector<unsigned char> img((size_t)stride * h, 0xA5);
    for (int y = 0; y < h; y++)
        if (fread(&img[(size_t)y * stride], 1, (size_t)w * 3, f) != (size_t)w * 3) {
            fprintf(stderr, "truncated PPM\n"); fclose(f); return 2;
        }
    fclose(f);

    // A guard region past `cap`: the encoder promises never to write into it.
    const size_t cap   = capArg > 0 ? (size_t)capArg : 256u * 1024u;
    const size_t guard = 4096;
    std::vector<unsigned char> out(cap + guard, 0x5A);

    const size_t n = jpegEncodeRGB(img.data(), w, h, stride, quality, out.data(), cap);

    for (size_t i = cap; i < cap + guard; i++)
        if (out[i] != 0x5A) {
            fprintf(stderr, "OVERRUN: wrote %zu bytes past cap=%zu\n", i - cap + 1, cap);
            return 3;
        }
    if (n > cap) { fprintf(stderr, "OVERRUN: returned %zu > cap %zu\n", n, cap); return 3; }
    if (n == 0) {
        fprintf(stderr, "refused (w=%d h=%d q=%d cap=%zu)\n", w, h, quality, cap);
        return 1;
    }

    FILE* g = fopen(argv[2], "wb");
    if (!g) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    fwrite(out.data(), 1, n, g);
    fclose(g);
    printf("%zu\n", n);
    return 0;
}
