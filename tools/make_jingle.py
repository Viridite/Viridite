#!/usr/bin/env python3
"""Write romfs/audio/viridite.mid — the boot jingle.

Viridite is a green mineral, so the jingle is a struck-crystal sound: bright
inharmonic partials with a long decay, no sustain, nothing that sounds like an
instrument being *played*. The figure is a rising D major pentatonic arpeggio
that opens onto a fifth, with a celesta echoing an octave up half a beat late
so the two drift apart into a shimmer rather than landing together.

Kept as a generator rather than a checked-in binary alone: the notes are
readable and editable here, and the .mid it emits is committed beside it so the
build never depends on Python.

    python3 tools/make_jingle.py
"""

import os
import struct

TPQ = 480          # ticks per quarter note
BPM = 96

# General MIDI programs. These are what a hardware synth would pick; the
# launcher's own renderer reads them as "how bright and how long a bell".
CRYSTAL, CELESTA, HALO_PAD, TINKLE = 98, 8, 94, 112

# D major pentatonic. C4 = 60.
D5, A5, B5, Fs6, A6, D6 = 74, 81, 83, 90, 93, 86
D3, A3, D4 = 50, 57, 62
D7, Fs7, A7 = 98, 102, 105

E = TPQ // 2       # an eighth


def var(n):
    """MIDI variable-length quantity."""
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def track(events):
    """events: (tick, status, d1, d2) — absolute ticks, sorted here."""
    events = sorted(events, key=lambda e: (e[0], e[1] & 0xF0 != 0x80))
    body, prev = bytearray(), 0
    for tick, status, d1, d2 in events:
        body += var(tick - prev) + bytes([status, d1] + ([d2] if d2 is not None else []))
        prev = tick
    body += var(0) + b"\xff\x2f\x00"                      # end of track
    return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def voice(ch, program, notes):
    """notes: (start, pitch, duration, velocity)"""
    ev = [(0, 0xC0 | ch, program, None)]
    for start, pitch, dur, vel in notes:
        ev.append((start, 0x90 | ch, pitch, vel))
        ev.append((start + dur, 0x80 | ch, pitch, 0))
    return ev


def main():
    tempo = int(60_000_000 / BPM)
    meta = [(0, 0xFF, None, None)]                        # placeholder, built by hand

    # Conductor track: tempo only.
    body = var(0) + b"\xff\x51\x03" + struct.pack(">I", tempo)[1:]
    body += var(0) + b"\xff\x2f\x00"
    conductor = b"MTrk" + struct.pack(">I", len(body)) + bytes(body)
    del meta

    # The arpeggio. Velocities rise into the F#, then the A6 is struck softest
    # of all — the peak of the phrase is the one that should ring, not shout.
    crystal = voice(0, CRYSTAL, [
        (0 * E,  D5,  3 * E, 74),
        (1 * E,  A5,  3 * E, 82),
        (2 * E,  B5,  3 * E, 88),
        (3 * E,  Fs6, 4 * E, 96),
        (4 * E,  A6,  6 * E, 78),
        # Resolution: an open fifth, left to decay under everything else.
        (5 * E,  D6,  5 * E, 88),
        (5 * E,  Fs6, 5 * E, 66),
    ])

    # Octave-up echo, half a beat behind. Quiet enough to read as reverb.
    celesta = voice(1, CELESTA, [
        (1 * E, D6,  2 * E, 46),
        (2 * E, A6,  2 * E, 50),
        (3 * E, D7,  2 * E, 44),
        (4 * E, Fs7, 3 * E, 38),
    ])

    # A bed, so the bells have something to hang in. Barely audible on purpose.
    pad = voice(2, HALO_PAD, [
        (0, D3, 10 * E, 34),
        (0, A3, 10 * E, 30),
        (0, D4, 10 * E, 26),
    ])

    # Three grains of light on the way out.
    tinkle = voice(3, TINKLE, [
        (5 * E,  D7,  3 * E, 40),
        (6 * E,  Fs7, 3 * E, 34),
        (7 * E,  A7,  3 * E, 28),
    ])

    out = os.path.join(os.path.dirname(__file__), "..", "romfs", "audio")
    os.makedirs(out, exist_ok=True)
    path = os.path.normpath(os.path.join(out, "viridite.mid"))

    tracks = [conductor, track(crystal), track(celesta), track(pad), track(tinkle)]
    header = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), TPQ)
    with open(path, "wb") as f:
        f.write(header + b"".join(tracks))

    beats = 10 * E / TPQ
    print(f"wrote {path} — {len(tracks)} tracks, {beats:.2f} beats "
          f"({beats * 60 / BPM:.2f}s at {BPM} BPM)")


if __name__ == "__main__":
    main()
