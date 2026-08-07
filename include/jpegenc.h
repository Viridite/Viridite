#pragma once
// ─── A baseline JPEG encoder ────────────────────────────────────────────────
//
// Because SDL_image's cannot be relied on here. Three hardware logs end
// mid-sentence inside IMG_SaveJPG_RW, on three unrelated games, one of them
// encoding an icon we ship in romfs ourselves — so it is not the image data,
// and a launcher that dies on its scanning screen is not something to leave
// resting on a library call that has never once come back.
//
// This is the whole format we need and nothing else: baseline sequential,
// 4:4:4, the standard Annex K quantisation and Huffman tables. No subsampling,
// no progressive, no restart markers, no EXIF. A 256x256 icon encodes in a few
// milliseconds and the code has no dependency beyond the C library, which is
// the entire point — there is nothing left in it that can fail to initialise.
//
// The output is what the HOME menu and hbmenu read out of an NRO's asset
// section, so it has to be a file a normal decoder accepts, not merely one
// that round-trips through ours.

#include <stddef.h>
#include <stdint.h>

// Encode 8-bit RGB into `out`, writing at most `cap` bytes. `stride` is the
// byte offset between rows, so an SDL surface's pitch can be passed directly.
// Returns the number of bytes written, or 0 if the buffer was too small or the
// inputs were unusable — never writes past `cap`.
size_t jpegEncodeRGB(const uint8_t* rgb, int w, int h, int stride,
                     int quality, uint8_t* out, size_t cap);
