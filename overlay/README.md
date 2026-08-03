# Viridite install overlay

A Tesla overlay that draws Viridite's install progress on top of whatever is on
screen — including the HOME menu. The point is that a game Viridite is setting
up should say so from the same place a real one would, rather than only inside
Viridite where you can't see it.

## What it needs

[nx-ovlloader](https://github.com/WerWolv/nx-ovlloader) installed. Without it,
`Viridite.ovl` just sits on the card doing nothing — it isn't loaded by
hbmenu and can't be launched on its own, so shipping it costs nothing for
people who don't use overlays.

The release zip puts it at `/switch/.overlays/Viridite.ovl`, which is where
nx-ovlloader looks. Summon it with the usual Tesla combo (default
`L + D-Pad Down + Right Stick`).

## How it gets its data

It has no connection to the loader at all. The Translation Core rewrites

```
sdmc:/switch/Viridite/install_status.txt
```

as it works, and the overlay reads it:

```
v1
state=installing        # installing | done | error | idle
pct=42
pkg=com.fingersoft.hillclimb
name=Hill Climb Racing
stage=Extracting assets
```

Two processes, one small file, no IPC. The Core writes to a temp path and
renames over the real one, so a read either gets the previous record or the
new one, never half of either. It also skips writes when nothing changed and
rate-limits the rest to 4/s — install progress comes from a loop already
competing with the SD card for the extraction itself, and this must not become
a second source of stalls.

The version line is checked. A future Core can bump it and older overlays will
stay quiet instead of misreading fields.

## Where the card sits

There is no way to ask the HOME menu where a tile is. qlaunch exposes no layout
information, homebrew can't read its framebuffer, and the row is
user-rearrangeable — so the position is placed by hand, not tracked.

What *is* knowable: a just-installed title sorts to the front of the row, so
its tile lands at a predictable rect. That rect is the default anchor. Because
it's a layout constant rather than something we can derive, it lives in a
config file you can nudge instead of being baked into the binary:

```
sdmc:/switch/Viridite/overlay.cfg

anchor_x=300
anchor_y=300
anchor_w=460
anchor_h=190
```

Coordinates are in 1920×1080 screen space, top-left origin. The file is
optional; the defaults above apply when it's absent. It is deliberately not
shipped in the release zip so that updating Viridite never overwrites a
position you've tuned.

Note that the overlay repositions its *layer*, not just its contents
(`viSetLayerPosition`/`viSetLayerSize`, set from `initServices()` before the
framebuffer exists) — which is what lets it sit on the tile rather than in
Tesla's default right-hand strip.

## Building

```sh
export DEVKITPRO=/opt/devkitpro
make
```

Produces `Viridite.ovl`. An `.ovl` is an ordinary NRO; it's only the directory
it sits in and the loader that picks it up that differ. RTTI is left on because
libtesla uses `dynamic_cast` internally.

`include/tesla.hpp` and `include/stb_truetype.h` are vendored from
[libtesla](https://github.com/WerWolv/libtesla) and
[stb](https://github.com/nothings/stb).
