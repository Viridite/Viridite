#include "avatar.h"
#include <switch.h>
#include <cstdio>

// Bundled at build time as romfs:/avatar.png — no network fetch, no cache
// file, no background thread. Loaded once into memory on avatarStart() and
// handed to the About screen exactly once via avatarPollNewImage().
static std::vector<uint8_t> g_bytes;
static bool                 g_delivered = false;

void avatarStart() {
    romfsInit();  // idempotent — safe even if main.cpp's font fallback already mounted it
    FILE* f = fopen("romfs:/avatar.png", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        g_bytes.resize((size_t)sz);
        if (fread(g_bytes.data(), 1, (size_t)sz, f) != (size_t)sz) g_bytes.clear();
    }
    fclose(f);
    g_delivered = false;
}

void avatarStop() {}

bool avatarPollNewImage(std::vector<uint8_t>& out) {
    if (g_delivered || g_bytes.empty()) return false;
    out = g_bytes;
    g_delivered = true;
    return true;
}
