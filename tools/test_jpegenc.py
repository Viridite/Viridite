#!/usr/bin/env python3
"""Check the launcher's JPEG encoder against a real decoder, on the dev box.

source/jpegenc.cpp is a hand-written codec that replaced IMG_SaveJPG_RW, and its
own header states the requirement it has to meet: "a file a normal decoder
accepts, not merely one that round-trips through ours." That is not something
reading the code can settle, and it is not something the console can settle
either — the console has no reference decoder to disagree with.

So: encode here, decode with libjpeg through Pillow, and compare. Also proves
the two promises the caller in source/forwarder.cpp leans on — that the encoder
never writes past the buffer it was given, and that it refuses rather than
truncates when the image will not fit.

    python3 tools/test_jpegenc.py

Needs g++, Pillow, and nothing else. No devkitPro, no hardware.
"""

import math
import os
import subprocess
import sys
import warnings

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs Pillow:  pip install --user Pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
WORK = os.path.join(TOOLS, ".jpegenc_test")
HARNESS = os.path.join(WORK, "harness")

# Quality genuinely costs fidelity, so the floor has to follow it. These are
# well below what the encoder actually achieves — they catch a broken encoder,
# not a slightly-worse-tuned one.
PSNR_FLOOR = {1: 20.0, 10: 24.0, 25: 27.0, 50: 30.0, 75: 32.0,
              90: 34.0, 95: 36.0, 100: 38.0}

failures = []


def build():
    os.makedirs(WORK, exist_ok=True)
    cmd = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra",
           "-I" + os.path.join(ROOT, "include"),
           os.path.join(TOOLS, "jpegenc_harness.cpp"),
           os.path.join(ROOT, "source", "jpegenc.cpp"),
           "-o", HARNESS]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness build failed:\n" + r.stderr)
    if r.stderr.strip():
        print("warnings building the encoder:\n" + r.stderr.strip())


def encode(img, name, q, pad=0, cap=0):
    """Returns (jpeg_path, size) or (None, reason)."""
    ppm = os.path.join(WORK, name + ".ppm")
    jpg = os.path.join(WORK, name + ".jpg")
    img.save(ppm)
    r = subprocess.run([HARNESS, ppm, jpg, str(q), str(pad), str(cap)],
                       capture_output=True, text=True)
    if r.returncode == 3:          # the harness detected an overrun itself
        return None, "OVERRUN: " + r.stderr.strip()
    if r.returncode != 0:
        return None, r.stderr.strip() or ("rc=%d" % r.returncode)
    return jpg, int(r.stdout.strip())


def psnr(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    mse = sum((x - y) ** 2 for x, y in zip(pa, pb)) / len(pa)
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def check(label, img, q, pad=0, cap=0, must_refuse=False, floor=None):
    name = "".join(c if c.isalnum() else "_" for c in label)
    jpg, info = encode(img, name, q, pad, cap)

    if jpg is None:
        if must_refuse and not info.startswith("OVERRUN"):
            print("  ok      %-38s refused, as it should" % label)
            return
        failures.append(label)
        print("  FAIL    %-38s %s" % (label, info))
        return

    if must_refuse:
        failures.append(label)
        print("  FAIL    %-38s produced %s bytes instead of refusing" % (label, info))
        return

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        try:
            dec = Image.open(jpg)
            dec.load()
        except Exception as exc:
            failures.append(label)
            print("  FAIL    %-38s libjpeg rejected it: %s" % (label, exc))
            return
        warned = [str(w.message) for w in caught]

    if dec.size != img.size:
        failures.append(label)
        print("  FAIL    %-38s decoded %s, expected %s" % (label, dec.size, img.size))
        return
    if warned:
        failures.append(label)
        print("  FAIL    %-38s decoder warned: %s" % (label, "; ".join(warned)))
        return

    p = psnr(img.convert("RGB"), dec.convert("RGB"))
    limit = floor if floor is not None else PSNR_FLOOR.get(q, 30.0)
    if p < limit:
        failures.append(label)
        print("  FAIL    %-38s psnr %.1f dB, below %.1f" % (label, p, limit))
        return
    print("  ok      %-38s %7s B   psnr %.1f dB" % (label, info, p))


# ── test images ──────────────────────────────────────────────────────────────

def icon_like(size=256):
    """The shape of the real workload: art over Viridite green, hard edges."""
    im = Image.new("RGB", (size, size), (0, 200, 83))
    d = ImageDraw.Draw(im)
    d.ellipse([size * .1, size * .1, size * .9, size * .9], fill=(20, 20, 28))
    d.rectangle([size * .3, size * .3, size * .7, size * .45], fill=(255, 255, 255))
    d.rectangle([size * .3, size * .55, size * .7, size * .7], fill=(255, 40, 40))
    return im


def gradient(w, h):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), 128)
    return im


def noise(w, h, seed=1):
    import random
    random.seed(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (random.randrange(256), random.randrange(256), random.randrange(256))
    return im


def dct_basis(u, v, size=32):
    """Every block driven to one DCT basis function at full 0/255 swing.

    This is what pushes coefficients to their largest possible magnitude, which
    is where an encoder runs out of Huffman symbols — the Annex K tables only
    define magnitudes 1..10 for AC and 0..11 for DC. If a real image could ever
    need more, the encoder would emit a zero-length code and silently corrupt
    the stream, so every basis function gets tried.
    """
    im = Image.new("RGB", (size, size))
    px = im.load()
    for y in range(size):
        for x in range(size):
            c = (math.cos((2 * (x % 8) + 1) * u * math.pi / 16) *
                 math.cos((2 * (y % 8) + 1) * v * math.pi / 16))
            val = 255 if c >= 0 else 0
            px[x, y] = (val, val, val)
    return im


def dc_swing(size=64):
    """Adjacent blocks slam black to white: the largest possible DC difference."""
    im = Image.new("RGB", (size, size))
    px = im.load()
    for by in range(0, size, 8):
        for bx in range(0, size, 8):
            val = 255 if ((bx // 8) + (by // 8)) % 2 else 0
            for y in range(8):
                for x in range(8):
                    px[bx + x, by + y] = (val, val, val)
    return im


def main():
    build()

    print("the real path — 256x256 at quality 90, what forwarder.cpp encodes")
    check("game icon", icon_like(), 90)
    shipped = os.path.join(ROOT, "Viridite.jpg")
    if os.path.exists(shipped):
        art = Image.open(shipped).convert("RGB").resize((256, 256), Image.LANCZOS)
        check("shipped Viridite.jpg, rescaled", art, 90)

    print("\nquality sweep")
    for q in (1, 10, 25, 50, 75, 90, 95, 100):
        check("icon at q%d" % q, icon_like(), q)

    print("\nsizes, including non-multiples of 8")
    for w, h in [(1, 1), (7, 3), (8, 8), (9, 9), (16, 15), (250, 137), (255, 255), (257, 3)]:
        check("gradient %dx%d" % (w, h), gradient(w, h), 90)

    print("\nstride — an SDL surface's pitch is padded, and is passed in raw")
    for pad in (1, 3, 13):
        check("gradient 250x137, %d-byte pad" % pad, gradient(250, 137), 90, pad=pad)

    print("\ncoefficient extremes — all 64 DCT basis functions at full swing")
    worst, worst_at, done = float("inf"), "none", 0
    for u in range(8):
        for v in range(8):
            img = dct_basis(u, v)
            jpg, info = encode(img, "basis_%d%d" % (u, v), 100, cap=1024 * 1024)
            if jpg is None:
                failures.append("basis(%d,%d)" % (u, v))
                print("  FAIL    basis(%d,%d): %s" % (u, v, info))
                continue
            dec = Image.open(jpg)
            dec.load()
            p = psnr(img, dec.convert("RGB"))
            done += 1
            if p < worst:
                worst, worst_at = p, "basis(%d,%d)" % (u, v)
    if done != 64:
        failures.append("dct basis sweep")
        print("  FAIL    only %d of 64 basis functions encoded" % done)
    elif worst < 30.0:
        failures.append("dct basis sweep")
        print("  FAIL    worst basis %s at %.1f dB — stream is being mis-decoded" % (worst_at, worst))
    else:
        print("  ok      %-38s worst %.1f dB (%s)" % ("all 64 basis functions", worst, worst_at))
    check("maximum DC swing", dc_swing(), 100)

    print("\nbuffer limits — must refuse, never overrun")
    # Random noise is the least compressible thing a 256x256 icon could ever be,
    # so this is the case that decides whether forwarder.cpp's fixed 256 KB is
    # actually enough. Its own fidelity is beside the point and cannot be high —
    # JPEG does not represent noise well at any quality — hence the loose floor.
    check("noise 256x256 into 256 KB at q90", noise(256, 256), 90,
          cap=256 * 1024, floor=25.0)
    check("noise 256x256 into 4 KB", noise(256, 256), 100, cap=4096, must_refuse=True)
    check("icon into 1023 bytes", icon_like(), 90, cap=1023, must_refuse=True)

    print()
    if failures:
        print("%d FAILED: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
