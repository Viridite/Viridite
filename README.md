<div align="center">

<img src="AndroidHorizonNX.jpg" width="128" height="128" style="border-radius:16px" alt="Android Horizon logo" />

# Android Horizon

**Run Android games natively on Nintendo Switch Horizon OS — without Android**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![GitHub Stars](https://img.shields.io/github/stars/FascinatingPistachio/AndroidHorizonNX?style=social)](https://github.com/FascinatingPistachio/AndroidHorizonNX/stargazers)
[![Status](https://img.shields.io/badge/status-pre--alpha-red.svg)](#status)
[![Built with Claude AI](https://img.shields.io/badge/built%20with-Claude%20AI-orange.svg)](https://anthropic.com)
[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red.svg)](#)

*Made by [aaronworld.uk](https://aaronworld.uk) · [Give it a star ⭐](https://github.com/FascinatingPistachio/AndroidHorizonNX/stargazers) if you find it interesting!*

</div>

---

> **Disclaimer — Please Read**
>
> Approximately **99.8% or more of the code in this project was written by Anthropic's Claude AI** (claude-sonnet-4-6). I (the author) am not a developer and am not capable of writing this myself. Claude and I are working on this together as an experiment. I handle testing on real hardware, describe problems, and decide direction; Claude writes and debugs the code.
>
> This project is in extremely early stages. It may not run anything at all reliably yet. Use entirely at your own risk.

---

## What Is This?

Android Horizon is a **compatibility / translation layer** that lets Android native (NDK) games run directly on the Nintendo Switch's **Horizon OS** — the Switch's real operating system.

**This is NOT Android running on the Switch.** There is no Android OS, no emulator, no virtual machine. The game's ARM64 machine code runs directly on the Switch's Tegra X1 processor, the same chip Android phones use. A thin shim layer fakes just enough of the Android runtime (libc, OpenGL ES, JNI, asset management) that the game's native `.so` library doesn't notice it isn't on Android.

Think of it like Wine on Linux — not emulation, but translation.

### Current Focus

We are currently only attempting **simple, old, 2D Android games** — specifically games that:

- Ship an `arm64-v8a` native library (`.so` file)
- Use OpenGL ES 2.0 or 3.0 for rendering
- Have **no** Google Play Services dependency
- Save locally (no cloud saves required)
- **Do not require network/online connectivity**

Our test target is **Hill Climb Racing 1.x** by Fingersoft — a simple 2D physics game with no online requirement, widely used in compatibility testing.

### What We've Achieved So Far

- The NRO launches from hbmenu and shows a working APK browser UI
- The Switch correctly **extracts the APK** — unpacking the game's native libraries and assets from the `.apk` file onto the SD card. This works reliably.
- The Switch loads and parses all three ELF binaries (`libapplovin-native-crash-reporter.so`, `libquack.so`, `libgame.so`) and links them against our shim table
- **All 403 JMPREL entries resolve successfully** — confirmed in the latest test log
- The on-screen progress display shows a **live feed of `compat_log.txt`** in real time, with an animated scan bar so the screen never looks frozen even during long silent phases (e.g. running 417 native constructors)
- **Docked mode is detected** — the footer warns you when launched in docked mode, since games require touch screen input (handheld only)
- Full diagnostic output is written to `sdmc:/AndroidHorizonNX/compat_log.txt`

The current frontier is **native constructors**: after JMPREL completes, 417 C++ constructors in `libgame.so` need to run before the game can start. We are working to determine if they succeed or where they stall.

---

## Controls

> **Handheld mode only.** Android games use touch screen input. The Switch touchscreen only works in handheld mode — in docked mode there is no touch input, so games will be uncontrollable. Android Horizon detects docked mode and shows a warning in the footer.

**Android Horizon launcher controls (in the APK browser):**

| Button | Action |
|--------|--------|
| D-pad / Left stick | Navigate APK list |
| **A** | Launch selected APK |
| **X** | Reinstall + Launch (re-extracts APK) |
| **Y** | Rescan APK folder |
| **−** | About screen |
| **+** | Quit |
| **B** | Back (on result screen) |

---

## Setup

1. Copy `AndroidHorizonNX.nro` to `sdmc:/switch/`
2. Place `.apk` files in `sdmc:/AndroidHorizonNX/apks/`
3. Launch from hbmenu (Atmosphere CFW required)
4. Navigate with D-pad or left stick, press **A** to launch
5. If a launch fails, check `sdmc:/AndroidHorizonNX/compat_log.txt` for the full error log

---

## Building

Requires [devkitPro](https://devkitpro.org/) with `devkitA64` and `libnx` installed.

```sh
export DEVKITPRO=/opt/devkitpro
make
```

Output: `AndroidHorizonNX.nro`

### Dependencies (via pacman/devkitPro)

```
switch-sdl2 switch-sdl2_image switch-sdl2_ttf
switch-libpng switch-libjpeg-turbo switch-minizip
switch-mesa switch-glad switch-curl switch-mbedtls
```

---

## Current Blockers

These are the known issues preventing Hill Climb Racing from running:

### 1. ~~Code pages not executable~~ — **RESOLVED**

The ELF loader now uses the SplitMap technique: two `svcCreateCodeMemory` handles placed at adjacent virtual addresses. The code segment is mapped `MapSlave` (Rx) and the data segment `MapOwner` (Rw). ARM64 ADRP instructions can address ±4GB, so adjacent placement lets executed code reach GOT/data without any permission transitions. The old `0xD801` error no longer occurs.

### 2. ~~JMPREL unresolved~~ — **RESOLVED (all 403 entries)**

All 403 JMPREL entries in the crash reporter library resolve successfully. The test log confirms `JMPREL: all 403 entries processed` and `ELF: jmprel done`.

### 3. ~~SplitMap memcpy crash~~ — **RESOLVED**

After JMPREL, the ELF loader was crashing on `memcpy(code_heap_buf, ...)` because `svcCreateCodeMemory` was called too early, making the backing buffer inaccessible before we wrote to it. Fixed: `svcCreateCodeMemory` + `svcControlCodeMemory` are now deferred until after the memcpy.

### 4. Native constructors — **ACTIVE INVESTIGATION**

After ELF loading, 417 C++ constructors in `libgame.so` need to run. The launcher logs each constructor address immediately before calling it and force-flushes to disk — look for the last `ELF: ctor[k/417] @0x...` line in `compat_log.txt` with no `OK` or `FAULT` after it to find which one stalls. We need a clean run showing `ELF: ctors done` to know where we stand.

### 5. Background threads not supported

`pthread_create` is stubbed to return a fake handle and never actually spawn a thread. Games that rely on a separate render or physics thread will appear frozen or crash.

### 6. Game may crash at runtime

Even with all symbols resolved and code executable, the game could crash during initialisation (NULL deref, bad GLES call, unimplemented JNI method, etc.). The next step is to get a crash address from the compat log and identify which code path is failing.

---

## Performance Expectations (Hill Climb Racing 1.67.0)

We're testing the `.apk` release of **Hill Climb Racing 1.67.0** specifically — the current Play Store release ships as a `.xapk`. Android Horizon's APK parser only understands plain `.apk` files right now, so `.xapk` support is out of scope until a later phase.

There's no measured frame rate yet — the game hasn't booted far enough to render a single frame. Here's the theoretical ceiling based on the hardware alone:

- Hill Climb Racing is a simple 2D physics game, originally tuned for 60 FPS on 2012-era phones with GPUs far weaker than the Switch's Tegra X1.
- **Theoretical ceiling: a locked 60 FPS** — assuming it boots and renders at all.
- The real risk to frame rate is the compat layer: `pthread_create` is stubbed (no real background thread), so any physics/render thread split will serialize onto one thread; GLES calls are translated through switch-mesa rather than a native Android GPU driver.

This section gets replaced with real measured numbers once the game boots far enough to render a frame.

---

## TODO / Roadmap

> Items are roughly ordered by priority. "Phase 0" is the current work.

### Phase 0 — Make any game do *something* (in progress)

- [x] APK browser UI with icon extraction
- [x] APK extraction (libs + assets) onto SD card
- [x] Custom ARM64 ELF loader with RELA relocation
- [x] JNI environment shim (fake JavaVM / JNIEnv)
- [x] EGL setup (GLES 2 + 3 shim table passthrough)
- [x] On-screen progress display during launch stages
- [x] Full diagnostic result screen with error details
- [x] Docked mode detection — footer warns when not in handheld mode
- [x] Early abort when code pages are not executable — shows diagnostic screen instead of crashing Switch
- [x] **SplitMap JIT** — adjacent RW+Rx dual-mapping via two `svcCreateCodeMemory` handles; 0xD801 blocker resolved
- [x] All 120+ unresolved symbols shimmed
- [x] **Per-constructor logging** — crash site shows in `compat_log.txt`
- [x] **Load all .so files** — all three libs loaded smallest-first; cross-library symbols available before constructors run
- [x] **40+ additional shims** — signal handling, thread naming, memory, barriers, etc.
- [x] **Live progress screen** — loader runs on a background thread; main thread renders at ~60fps with animated scan bar and live `compat_log.txt` tail (13 lines, colour-coded)
- [x] **Elapsed time per stage** — stage label shows "(Xs)" so you can tell how long each step is taking
- [x] **All 403 JMPREL entries resolved** — confirmed in hardware test
- [ ] Identify whether constructors complete or stall — need a clean run showing `ELF: ctors done`
- [ ] Real touch input delivery via `AInputQueue` / `ALooper`

### Phase 1 — Touch input

- [ ] Map Switch touchscreen events to `AInputQueue` touch events delivered to the game
- [x] Docked-mode detection — footer shows warning when not in handheld mode

### Phase 2 — Stability

- [ ] Real `pthread` support using libnx `Thread` (for games with background render/physics threads)
- [x] Load all `.so` files in dependency order (smallest-first)
- [ ] Implement `dl_iterate_phdr` so stack unwinders work
- [ ] Save/load state via proper `internalDataPath` on the SD card

### Phase 3 — Polish

- [x] NRO icon (Android Horizon themed — green planet with curved text)
- [x] Dynamic GitHub avatar on About screen
- [x] About screen (press **−**)
- [x] Reinstall button (**X** in APK list)
- [ ] Version bump system — each build increments `APP_VERSION`
- [ ] Per-APK settings overlay (resolution, framerate cap)
- [ ] APK delete / manage from the UI

### Not Planned (for now)

- Online / multiplayer games (Roblox, Fortnite, etc.)
- Google Play Services (GMS) stub
- ARM32 (`armeabi-v7a`) game support — Switch is 64-bit only

---

## Changelog

> Most recent first.

### [Current Build]

- [x] **SplitMap crash fixed** — root cause: `svcCreateCodeMemory` was called before `memcpy`, making the backing buffer inaccessible at userspace. Fixed by deferring `svcCreateCodeMemory` + `svcControlCodeMemory` to after the memcpy. All three `.so` files should now load past the ELF copy stage.
- [x] **RELA/JMPREL logging dramatically reduced** — removed per-entry log lines (one `fflush` per entry on FAT32 was taking ~38 seconds for libapplovin alone). Now only unresolved symbols, WARN lines, and the end-of-table summary are logged — `applyRela` goes from ~8000 lines to ~30 per library.
- [x] **+ button exits progress screen** — press **+** at any time during ELF loading to stop waiting and return to the APK list
- [x] **Renamed to Android Horizon / AndroidHorizonNX** — reflects the project's purpose (Android on HorizonOS) more clearly
- [x] **Live animated progress screen** — loader now runs on a background libnx thread; main thread renders at ~60fps with: animated scan bar (always moving, independent of load progress), live tail of `compat_log.txt` (13 lines, colour-coded for errors/warnings), elapsed time display per stage, "still working" notice after 30s
- [x] **Log timestamps** — every `compat_log.txt` entry prefixed with `[Xs]` seconds-since-launch
- [x] **Immediate constructor logging** — each `ELF: ctor[k/417] @ptr` is force-flushed to disk and the live display the instant before the constructor runs, so a hanging constructor is immediately identifiable
- [x] **"Please wait" patience notice** — log and screen show total constructor count and estimated time before the phase begins
- [x] **Avatar fix** — `socketInitializeDefault()` added so curl/BSD sockets work in homebrew; GitHub URL corrected to FascinatingPistachio
- [x] **Expanded log ring buffer** (5×92 → 20×128 bytes) for richer in-memory log feed
- [x] **All 403 JMPREL entries resolve** — hardware-confirmed; constructors are now the active frontier
- [x] **Android Horizon icon** — green planet with "ANDROID HORIZON" curved above the horizon, space background with stars
- [x] **About screen** (press **−**) with live GitHub avatar + project info
- [x] **Reinstall button** (**X**) — re-extracts APK without needing to delete the game folder

### [Previous builds]

- [x] **Performance Expectations section** added to README
- [x] **APK chooser QoL**: WebP icon decoding, linear icon scaling, colored monogram placeholders, larger icons, file size shown per APK
- [x] **Heap staging buffer for ELF loading** — fixed hard crash on first write to JIT-writable memory
- [x] **Per-relocation-entry logging + bounds checks** in `applyRela()` — bounds-checks `sym.st_name` against `DT_STRSZ`, caps `R_AARCH64_COPY` size at 64KB
- [x] **Per-constructor logging** — `elfRunCtors()` logs each constructor address + index and flushes before calling it
- [x] **Load all .so files** — replaced `findMainSo` with `findAllSos`; all three libs are loaded smallest-first
- [x] **40+ new shims** — `sigaction`, signal sets, `prctl`, `gettid`, `getpid`, `getuid`, `getgid`, `getauxval`, `mprotect`, `mmap`, `munmap`, `pipe`, `dup`, `dup2`, `ioctl`, `access`, `chmod`, `fchmod`, `lstat`, `pthread_setname_np`, `pthread_getname_np`, `pthread_attr_setstack`, `pthread_barrier_*`, `kill`, `raise`, `pthread_kill`, `sleep`, `usleep`, `clock_nanosleep`, `strtod_l`, `strtof_l`, `__register_atfork`
- [x] **ELF loader: SplitMap JIT** — `MapSlave` (Rx) + `MapOwner` (Rw) at adjacent VAs; 0xD801 blocker resolved
- [x] **120+ shims**: `setjmp`/`longjmp`, `sem_*`, all time functions, all wide-char and locale functions, all Bionic fortified string wrappers, networking stubs, `android_set_abort_message`, `sincosf`, `stpcpy`, `vasprintf`, `vsscanf`, `strerror`, `strtold`, `puts`/`putchar`, `rename`/`remove`, `__sF`, `__stack_chk_guard`, `pthread_mutexattr_*`, `__cxa_finalize`
- [x] Capture svc result codes from ELF loader and surface on diagnostic screen
- [x] Abort launch early when code pages not executable — diagnostic screen instead of Switch crash
- [x] Add `LaunchResult` struct — structured error info instead of bare bool
- [x] Add `ProgressCb` callback — UI shows each launch stage on-screen in real time
- [x] Docked mode detection — footer turns amber and warns

### 0.1.0 — Initial release

- [x] APK browser UI (SDL2, 1280×720)
- [x] APK icon + metadata extraction (AndroidManifest.xml + resources.arsc parser)
- [x] Full APK extraction to SD card (libs + assets)
- [x] Custom ARM64 ELF loader with RELA relocation processing
- [x] JNI / JavaVM fake environment
- [x] GLES 2 + GLES 3 shim table (400+ functions)
- [x] EGL setup via switch-mesa
- [x] SD card logging (`log.txt` + `compat_log.txt`)

---

## License

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for the full text.

**In plain English:** You can use, copy, modify, share, and even sell this code freely, as long as you keep the copyright notice. There is no warranty — if it breaks your Switch, that's on you (please use CFW responsibly).

The MIT license does **not** cover the Android games themselves — those belong to their respective developers. Android Horizon only provides the compatibility layer.

---

## About

Made by [Aaron](https://aaronworld.uk) — a non-developer who wanted to see Android games on their modded Switch, and is figuring it out one step at a time with the help of Claude AI.

> **~99.8% of all code in this repo was written by [Claude](https://anthropic.com) (claude-sonnet-4-6 model).** I describe the problem, test on real hardware, and point out what's broken. Claude writes the fixes. This is an honest experiment in AI-assisted hardware hacking.

If this interests you, star the repo and check back. Progress will be slow but real.
