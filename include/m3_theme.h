#pragma once
// ─── Material 3 colour scheme ───────────────────────────────────────────────
// Generated, not hand-picked. Material You builds every role from tonal
// palettes derived from a seed colour, so the roles stay in correct contrast
// relationships with each other no matter which seed is chosen — which is the
// whole point of the system and the thing you lose by choosing colours by eye.
//
// Palettes are built by holding the seed's hue and chroma and sweeping L*,
// clamping chroma back into gamut where a tone cannot hold it. Material's own
// implementation does this in HCT (CAM16 hue and chroma over L*); for single
// saturated seeds the two produce visually equivalent ramps, and this avoids
// carrying a CAM16 implementation for a palette that is baked in at build time.
//
// Role names follow the M3 spec exactly so the mapping to the published
// guidance is one-to-one.
#include <SDL2/SDL.h>

struct M3Scheme {
    const char* name;
    bool        dark;
    SDL_Color   primary;
    SDL_Color   onPrimary;
    SDL_Color   primaryContainer;
    SDL_Color   onPrimaryContainer;
    SDL_Color   secondary;
    SDL_Color   onSecondary;
    SDL_Color   secondaryContainer;
    SDL_Color   onSecondaryContainer;
    SDL_Color   tertiary;
    SDL_Color   tertiaryContainer;
    SDL_Color   onTertiaryContainer;
    SDL_Color   error;
    SDL_Color   onError;
    SDL_Color   errorContainer;
    SDL_Color   onErrorContainer;
    SDL_Color   surface;
    SDL_Color   onSurface;
    SDL_Color   surfaceVariant;
    SDL_Color   onSurfaceVariant;
    SDL_Color   outline;
    SDL_Color   outlineVariant;
    SDL_Color   surfaceContainerLowest;
    SDL_Color   surfaceContainerLow;
    SDL_Color   surfaceContainer;
    SDL_Color   surfaceContainerHigh;
    SDL_Color   surfaceContainerHighest;
    SDL_Color   inverseSurface;
    SDL_Color   inverseOnSurface;
};

// Corner radius scale — M3: XS 4, S 8, M 12, L 16, XL 24 (dp).
enum M3Shape { M3_XS = 4, M3_S = 8, M3_M = 12, M3_L = 16, M3_XL = 24, M3_FULL = 999 };

static const int M3_THEME_COUNT = 6;
static const M3Scheme M3_THEMES[M3_THEME_COUNT] = {
    { "Viridite Light", false,
      {  2, 109,  49, 255},   // primary
      {254, 255, 254, 255},   // onPrimary
      {120, 253, 155, 255},   // primaryContainer
      {  0,  34,   5, 255},   // onPrimaryContainer
      { 67, 102,  74, 255},   // secondary
      {254, 255, 254, 255},   // onSecondary
      {196, 236, 202, 255},   // secondaryContainer
      {  0,  34,   6, 255},   // onSecondaryContainer
      { 49, 104,  89, 255},   // tertiary
      {180, 238, 220, 255},   // tertiaryContainer
      {  0,  33,  24, 255},   // onTertiaryContainer
      {180,  39,  31, 255},   // error
      {255, 255, 254, 255},   // onError
      {254, 219, 211, 255},   // errorContainer
      { 58,  11,   0, 255},   // onErrorContainer
      {245, 251, 246, 255},   // surface
      { 24,  29,  25, 255},   // onSurface
      {216, 230, 218, 255},   // surfaceVariant
      { 62,  74,  64, 255},   // onSurfaceVariant
      {110, 122, 111, 255},   // outline
      {188, 202, 190, 255},   // outlineVariant
      {254, 255, 254, 255},   // surfaceContainerLowest
      {239, 245, 240, 255},   // surfaceContainerLow
      {233, 239, 234, 255},   // surfaceContainer
      {228, 234, 228, 255},   // surfaceContainerHigh
      {222, 228, 223, 255},   // surfaceContainerHighest
      { 45,  49,  46, 255},   // inverseSurface
      {236, 242, 237, 255}   // inverseOnSurface
    },
    { "Viridite Dark", true,
      { 88, 223, 128, 255},   // primary
      {  0,  57,  22, 255},   // onPrimary
      {  3,  82,  36, 255},   // primaryContainer
      {120, 253, 155, 255},   // onPrimaryContainer
      {169, 208, 175, 255},   // secondary
      { 21,  55,  30, 255},   // onSecondary
      { 43,  78,  51, 255},   // secondaryContainer
      {196, 236, 202, 255},   // onSecondaryContainer
      {153, 210, 192, 255},   // tertiary
      { 23,  79,  66, 255},   // tertiaryContainer
      {180, 238, 220, 255},   // onTertiaryContainer
      {255, 180, 165, 255},   // error
      {104,   1,   0, 255},   // onError
      {146,   3,  11, 255},   // errorContainer
      {254, 219, 211, 255},   // onErrorContainer
      { 16,  21,  16, 255},   // surface
      {222, 228, 223, 255},   // onSurface
      { 62,  74,  64, 255},   // surfaceVariant
      {188, 202, 190, 255},   // onSurfaceVariant
      {135, 148, 137, 255},   // outline
      { 62,  74,  64, 255},   // outlineVariant
      {  9,  16,  10, 255},   // surfaceContainerLowest
      { 24,  29,  25, 255},   // surfaceContainerLow
      { 28,  33,  29, 255},   // surfaceContainer
      { 39,  43,  39, 255},   // surfaceContainerHigh
      { 49,  54,  50, 255},   // surfaceContainerHighest
      {222, 228, 223, 255},   // inverseSurface
      { 45,  49,  46, 255}   // inverseOnSurface
    },
    { "Mint Light", false,
      {  8, 107,  92, 255},   // primary
      {254, 255, 255, 255},   // onPrimary
      {102, 250, 221, 255},   // primaryContainer
      {  0,  33,  26, 255},   // onPrimaryContainer
      { 63, 101,  93, 255},   // secondary
      {254, 255, 255, 255},   // onSecondary
      {193, 235, 225, 255},   // secondaryContainer
      {  0,  33,  26, 255},   // onSecondaryContainer
      { 51,  99, 134, 255},   // tertiary
      {204, 229, 255, 255},   // tertiaryContainer
      {  0,  30,  49, 255},   // onTertiaryContainer
      {180,  39,  31, 255},   // error
      {255, 255, 254, 255},   // onError
      {254, 219, 211, 255},   // errorContainer
      { 58,  11,   0, 255},   // onErrorContainer
      {244, 251, 249, 255},   // surface
      { 24,  28,  27, 255},   // onSurface
      {215, 230, 226, 255},   // surfaceVariant
      { 61,  73,  70, 255},   // onSurfaceVariant
      {109, 122, 118, 255},   // outline
      {187, 202, 198, 255},   // outlineVariant
      {254, 255, 255, 255},   // surfaceContainerLowest
      {239, 245, 243, 255},   // surfaceContainerLow
      {233, 239, 237, 255},   // surfaceContainer
      {227, 233, 232, 255},   // surfaceContainerHigh
      {222, 228, 226, 255},   // surfaceContainerHighest
      { 45,  49,  48, 255},   // inverseSurface
      {236, 242, 240, 255}   // inverseOnSurface
    },
    { "Mint Dark", true,
      { 66, 221, 194, 255},   // primary
      {  0,  56,  47, 255},   // onPrimary
      {  0,  81,  69, 255},   // primaryContainer
      {102, 250, 221, 255},   // onPrimaryContainer
      {166, 207, 197, 255},   // secondary
      { 16,  54,  47, 255},   // onSecondary
      { 40,  77,  69, 255},   // secondaryContainer
      {193, 235, 225, 255},   // onSecondaryContainer
      {159, 203, 244, 255},   // tertiary
      { 19,  75, 109, 255},   // tertiaryContainer
      {204, 229, 255, 255},   // onTertiaryContainer
      {255, 180, 165, 255},   // error
      {104,   1,   0, 255},   // onError
      {146,   3,  11, 255},   // errorContainer
      {254, 219, 211, 255},   // onErrorContainer
      { 15,  20,  19, 255},   // surface
      {222, 228, 226, 255},   // onSurface
      { 61,  73,  70, 255},   // surfaceVariant
      {187, 202, 198, 255},   // onSurfaceVariant
      {134, 148, 144, 255},   // outline
      { 61,  73,  70, 255},   // outlineVariant
      {  9,  15,  14, 255},   // surfaceContainerLowest
      { 24,  28,  27, 255},   // surfaceContainerLow
      { 28,  32,  31, 255},   // surfaceContainer
      { 38,  43,  42, 255},   // surfaceContainerHigh
      { 49,  54,  52, 255},   // surfaceContainerHighest
      {222, 228, 226, 255},   // inverseSurface
      { 45,  49,  48, 255}   // inverseOnSurface
    },
    { "Violet Light", false,
      { 98,  56, 231, 255},   // primary
      {255, 255, 255, 255},   // onPrimary
      {235, 221, 254, 255},   // primaryContainer
      {  1,   0, 111, 255},   // onPrimaryContainer
      {105,  84, 140, 255},   // secondary
      {255, 255, 255, 255},   // onSecondary
      {234, 221, 254, 255},   // secondaryContainer
      { 32,  18,  66, 255},   // onSecondaryContainer
      {150,  67,  89, 255},   // tertiary
      {255, 217, 224, 255},   // tertiaryContainer
      { 65,   0,  24, 255},   // onTertiaryContainer
      {180,  39,  31, 255},   // error
      {255, 255, 254, 255},   // onError
      {254, 219, 211, 255},   // errorContainer
      { 58,  11,   0, 255},   // onErrorContainer
      {251, 248, 255, 255},   // surface
      { 29,  26,  33, 255},   // onSurface
      {232, 223, 245, 255},   // surfaceVariant
      { 75,  68,  86, 255},   // onSurfaceVariant
      {124, 116, 136, 255},   // outline
      {204, 195, 217, 255},   // outlineVariant
      {255, 255, 255, 255},   // surfaceContainerLowest
      {246, 242, 251, 255},   // surfaceContainerLow
      {240, 236, 246, 255},   // surfaceContainer
      {234, 230, 240, 255},   // surfaceContainerHigh
      {229, 225, 234, 255},   // surfaceContainerHighest
      { 50,  47,  54, 255},   // inverseSurface
      {243, 239, 248, 255}   // inverseOnSurface
    },
    { "Violet Dark", true,
      {213, 187, 253, 255},   // primary
      { 18,   0, 169, 255},   // onPrimary
      { 63,  29, 203, 255},   // primaryContainer
      {235, 221, 254, 255},   // onPrimaryContainer
      {212, 188, 250, 255},   // secondary
      { 55,  39,  89, 255},   // onSecondary
      { 80,  61, 114, 255},   // secondaryContainer
      {234, 221, 254, 255},   // onSecondaryContainer
      {255, 177, 193, 255},   // tertiary
      {123,  42,  65, 255},   // tertiaryContainer
      {255, 217, 224, 255},   // onTertiaryContainer
      {255, 180, 165, 255},   // error
      {104,   1,   0, 255},   // onError
      {146,   3,  11, 255},   // errorContainer
      {254, 219, 211, 255},   // onErrorContainer
      { 21,  18,  25, 255},   // surface
      {229, 225, 234, 255},   // onSurface
      { 75,  68,  86, 255},   // surfaceVariant
      {204, 195, 217, 255},   // onSurfaceVariant
      {150, 141, 162, 255},   // outline
      { 75,  68,  86, 255},   // outlineVariant
      { 17,  12,  21, 255},   // surfaceContainerLowest
      { 29,  26,  33, 255},   // surfaceContainerLow
      { 33,  30,  37, 255},   // surfaceContainer
      { 44,  41,  48, 255},   // surfaceContainerHigh
      { 54,  51,  59, 255},   // surfaceContainerHighest
      {229, 225, 234, 255},   // inverseSurface
      { 50,  47,  54, 255}   // inverseOnSurface
    },
};
