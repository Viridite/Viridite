<div align="center">

<img src="AndroidHorizonNX.jpg" width="128" height="128" style="border-radius:16px" alt="Android Horizon logo" />

# Android Horizon

**Run Android games natively on Nintendo Switch Horizon OS — without Android**

[![License: Source Available](https://img.shields.io/badge/License-Source__Available-orange.svg)](LICENSE)
[![GitHub Stars](https://img.shields.io/github/stars/Aaronateataco/AndroidHorizonNX?style=social)](https://github.com/Aaronateataco/AndroidHorizonNX/stargazers)
[![Status](https://img.shields.io/badge/status-pre--alpha-red.svg)](#status)
[![Built with Claude AI](https://img.shields.io/badge/built%20with-Claude%20AI-orange.svg)](https://anthropic.com)
[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red.svg)](#)

*Made by [aaronworld.uk](https://aaronworld.uk) · [Give it a star ⭐](https://github.com/Aaronateataco/AndroidHorizonNX/stargazers) if you find it interesting!*

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

**Hill Climb Racing boots and renders on real hardware.** The Fingersoft logo animates, the loading screen runs to completion, the game enters its main menu logic and starts preloading menu music and sound effects. That's real Android NDK game code rendering through switch-mesa on Horizon OS.

The full pipeline that works today:

- The NRO launches from hbmenu (or an application-mode forwarder) with a themed APK browser UI
- APK extraction — native libraries and assets unpacked from the `.apk` onto the SD card
- All three ELF binaries (`libapplovin-native-crash-reporter.so`, `libquack.so`, `libgame.so`) load, relocate, and link against the shim table with **zero unresolved symbols**
- All **417 native C++ constructors** run clean (the fix: JIT data pages needed to be flipped back to RW after the executable transition)
- `JNI_OnLoad` runs, `Java_` entry points are discovered, `nativeInit` completes, and the render loop drives `nativeRender` at roughly **18 fps** in the menu
- The fake JNI layer answers hundreds of thousands of calls (UserDefaults, market queries, AES encrypt/decrypt passthrough, audio engine hooks)
- Live on-screen log feed during loading, full diagnostics in `sdmc:/AndroidHorizonNX/compat_log.txt`, and **automatic screenshots** of key moments saved to `sdmc:/AndroidHorizonNX/screenshots/`
- Crash forensics: symbolized abort/exit backtraces, unrecovered-fault PC capture, and the game's own debug output routed into the log

The journey so far, each root-caused on real hardware: JIT data pages needed RW after the executable transition (417 ctor faults) → `std::random_device` aborted because `/dev/urandom` doesn't exist (now served by the Switch CSRNG) → the asset-loader thread froze the game because `pthread_create` ran it inline (threads are real libnx threads now). As of 0.1.59 the game reaches its **Terms-of-Service screen fully loaded and rendering**, and the build adds touch input and SDL2_mixer audio — the first potentially *playable* build.

---

## Screenshots

Android Horizon captures these **automatically on real hardware** — the launcher saves PNGs of each screen to `sdmc:/AndroidHorizonNX/screenshots/`, and the game loop snapshots the GL framebuffer at milestone frames (30 / 300 / 900) to prove what actually rendered. Copy them from the SD card into `docs/screenshots/` to update this section.

| | |
|---|---|
| ![APK browser](docs/screenshots/ui_menu.png) | ![Loading screen](docs/screenshots/ui_loading.png) |
| *APK browser — starfield, planet horizon, HOS button glyphs* | *Live loading screen with real-time compat log* |
| ![Fingersoft splash](docs/screenshots/game_frame30.png) | ![Game loading screen](docs/screenshots/game_frame300.png) |
| *Fingersoft splash animating (game frame 30)* | *Hill Climb Racing loading screen rendering on Horizon OS (frame 300)* |
| ![Terms of Service screen](docs/screenshots/game_frame900.png) | |
| *Fully loaded and interactive — the game's Terms of Service screen (frame 900)* | |

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

### 4. ~~Native constructors~~ — **RESOLVED (all 417 run clean)**

Root cause: `jitTransitionToExecutable` made the entire JIT region RX, including the data segment, so the very first constructor writing a C++ global hit a permission fault. Fix: data-segment pages are flipped back to RW after the executable transition, matching the Android linker's layout. `ctors done ok=417 failed=0`.

### 5. Game calls `abort()` after ~165s — **ACTIVE INVESTIGATION**

The game boots, renders its splash + loading screen, enters menu logic — then deliberately aborts right after its daily-missions bookkeeping. The symbolized caller sits in libc++'s `system_error` machinery, i.e. a `-fno-exceptions` build turning `__throw_system_error` into `abort()`. The current build logs a full symbolized backtrace at abort, captures the game's stderr, and implements `getrandom` (a prime suspect — `std::random_device` has no entropy source on Switch by default).

### 6. No audio yet

Cocos2d-x SimpleAudioEngine routes all sound through JNI to Java-side `Cocos2dxSound`/`Cocos2dxMusic`, which are no-op stubs. The calls are logged (you can see every `preloadEffect`/`playBackgroundMusic` in the compat log) — actually playing the assets via SDL/audren is a planned phase.

### 7. Background threads run synchronously

`pthread_create` executes the thread function inline on the calling thread. This unblocks init-wait patterns, but long-running worker loops would stall the game. Real threads via libnx are a planned phase.

### 8. Touch input not delivered

The game renders but can't be played yet — touchscreen events aren't delivered through `AInputQueue` yet. Next major feature after the abort is fixed.

---

## Performance Expectations (Hill Climb Racing 1.67.0)

We're testing the `.apk` release of **Hill Climb Racing 1.67.0** specifically — the current Play Store release ships as a `.xapk`. Android Horizon's APK parser only understands plain `.apk` files right now, so `.xapk` support is out of scope until a later phase.

First real measured numbers from hardware:

- **~18 fps** in the menu/loading phase (300 frames in ~17 s), unoptimized — everything runs on one thread and GLES goes through switch-mesa rather than a native Android GPU driver
- **~143 s** for the game's `nativeInit` (its loading screen) — dominated by first-time asset loading and tens of thousands of logged JNI UserDefault calls; log throttling and asset caching should cut this substantially
- Theoretical ceiling remains a locked 60 FPS — Hill Climb Racing was tuned for 2012-era phones far weaker than the Tegra X1. Performance work starts after the game is stable and playable.

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
- [x] **All 417 constructors run clean** — JIT data pages flipped back to RW after executable transition
- [x] **Game boots and renders** — splash animation, full loading screen, menu logic, ~18 fps
- [x] **Crash forensics** — symbolized abort/exit backtraces, unrecovered-fault PC capture, game stderr + debug strings routed to compat log
- [x] **Automatic screenshots** — launcher screens + GL framebuffer at milestone frames saved to `sdmc:/AndroidHorizonNX/screenshots/`
- [ ] Fix the ~165s `abort()` (libc++ `__throw_system_error` path — backtrace instrumentation in place)
- [x] **Touch input delivered** — SDL finger events → Cocos2dxRenderer touch natives (1:1 coords); B button → Android BACK key
- [x] **Audio playback** — SDL2_mixer backend for SimpleAudioEngine (music + effects, OGG/MP3/Opus/FLAC)

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
- [x] **Icon-themed UI** — animated starfield, planet-horizon scenery, borealis-style glowing focus card, HOS button glyphs from the system font
- [x] Dynamic GitHub avatar on About screen
- [x] About screen (press **−**)
- [x] Reinstall button (**X** in APK list)
- [x] Version bump system — NACP + in-app version both track the build number (`0.1.<build>`)
- [ ] Per-APK settings overlay (resolution, framerate cap)
- [ ] APK delete / manage from the UI

### Not Planned (for now)

- Online / multiplayer games (Roblox, Fortnite, etc.)
- Google Play Services (GMS) stub
- ARM32 (`armeabi-v7a`) game support — Switch is 64-bit only

---

## Changelog

> Most recent first.

### 0.1.68 — IT'S PLAYABLE + persistent saves

- [x] **HILL CLIMB RACING IS PLAYABLE ON SWITCH** — driving with touch steering, engine sounds, music, 60 fps. Confirmed on hardware. Known issues from the playtest below.
- [x] **Saves now persist** — the UserDefault store was RAM-only, so every launch looked like a first run (ToS screen again, progress lost). It now loads from `<game>/userdefaults.bin` before the game starts and saves on every `UserDefault.flush` and at exit. Ints, floats, bools, and strings all covered (floats/bools weren't even stored before).
- [x] **Real connectivity reported** — `isNetworkAvailable` now asks the Switch (nifm) instead of always claiming "online". An offline Switch gets Android's normal offline code paths — the game handles that natively, and it likely defuses the shop's network/IAP flows (the playtest's only crash).
- [x] **Effect volume fixed** — `playEffect`'s gain parameter was discarded, so looping effects (the engine!) played at 100%. Gain is now applied per channel. (Pitch modulation isn't supported by SDL_mixer, so the engine is volume-correct but monotone for now.)
- [x] **Relaunch guard** — launching a second game session in one app run crashes the loader (leftover JIT regions/threads from the first session; "not an ARM binary"). Until real unloading exists, a second launch shows a "restart Android Horizon to play again" notice instead of crashing.
- [x] Async Java callback pattern established (`compatFindGameSym`) — used for `returnCountryCode`, ready for `setServerTime`/`returnMissionJson`/`returnFileDownloadResult` if the game stalls on them.

### 0.1.66 — Touch confirmed + post-EULA spinner fixed

- [x] **TOUCH CONFIRMED WORKING** — the Terms-of-Service screen was accepted by tapping on hardware. Also: the logging diet pushed the game loop to a solid **60 fps** (frame counter: 300 frames per 5 s) — the SD-card fsyncs were throttling rendering too.
- [x] **Post-EULA spinner root-caused** — after accepting, the game calls `fetchCountryCode()`; on Android that's an async Java web request answered via the native callback `returnCountryCode(jstring)`. No reply = infinite spinner. The JNI layer now invokes the game's registered callback immediately with `"US"` (which also keeps the game on the simpler non-GDPR consent path).
- [x] **Music no longer keeps playing after quitting the game** — the mixer is silenced when the game loop exits back to the APK browser.

### 0.1.65 — Audio works! + loading-screen speedup

- [x] **AUDIO CONFIRMED WORKING ON HARDWARE** — build 64's Tremor fix holds: menu music (`bgmusic00.ogg`) loops from the Switch speakers while the game renders. First sound ever from an Android game on Horizon OS via this layer.
- [x] **"Stuck" loading screen root-caused: it wasn't stuck** — the game reads thousands of per-vehicle/per-stage save keys, and every read produced 3 compat-log lines each fsync'd to the SD card (~90 keys/second measured). The run was minutes from finishing when it looked frozen.
- [x] **JNI logging diet** — class/method/field lookups and repeated calls log once per unique message; save-key reads are silent with a periodic progress counter; writes log once per key. Loading should now be dramatically faster.

### 0.1.64 — The real frame-2 killer: vorbis ABI mismatch

- [x] **Frame-2 crash fully root-caused** — build 63 still crashed, and the forensics narrowed it to `_free_r` freeing SDL_mixer's own `sdl_seek_func` (an OGG callback *function*). Disassembling portlibs' `libSDL2_mixer.a` showed its OGG decoder calls `ov_read` with 4 arguments — the **Tremor** (`libvorbisidec`) ABI — while we linked regular `libvorbisfile`, whose `ov_read` takes 7 arguments and whose `OggVorbis_File` struct is a different size. `ov_open_callbacks` scribbled past the mixer's smaller struct on the first `preloadEffect(*.ogg)`, corrupting the heap.
- [x] **Fix:** link `-lvorbisidec` (Tremor) instead of `-lvorbisfile -lvorbis`, matching what SDL2_mixer was compiled against.

### 0.1.63 — Allocator crash root-caused: JNI string constants

- [x] **Build 59/60 frame-2 crash root-caused via the new forensics** — `svcQueryMemory` + host-symbol anchoring resolved the fault to newlib's `_free_r` writing a free-list link into our NRO's read-only segment: the game `free()`d a JNI string we handed it. On real Android, `GetStringUTFChars` returns a malloc'd copy (so game code that `free()`s it instead of calling `ReleaseStringUTFChars` gets away with it); ours returned the string constant itself.
- [x] **Fix 1:** `GetStringUTFChars` now returns a real heap copy (ART-compatible) and `ReleaseStringUTFChars` frees it.
- [x] **Fix 2:** guarded allocator — `free`/`realloc` verify the pointer is actually heap (`svcQueryMemory`) before touching it, and log the symbolized caller of any non-heap free instead of corrupting the allocator.
- [x] Mixer init now happens before the game loop starts (was lazily mid-frame).
- [x] New crash forensics stay in: fault pc/far region type+permissions, host addresses anchored to a known symbol for offline resolution against the build's `.elf`.

### 0.1.59 — Touch + audio

- [x] **Real threads worked** — build 56 reached the game's Terms-of-Service / privacy screen: fully loaded, rendering, interactive (2,280 frames over 339 s in the test run, clean exit via +). The remaining gaps were input and sound.
- [x] **Touch input** — SDL finger events are forwarded to the game's registered `Cocos2dxRenderer` natives: `nativeTouchesBegin`/`End` per finger, `nativeTouchesMove` with real JNI arrays (the fake JNI array layer now implements `GetArrayLength` and all typed `Get/SetArrayRegion` calls instead of returning zeros). Touchscreen coordinates map 1:1 to the game's 1280×720 surface.
- [x] **B button → Android BACK key** via `nativeKeyDown(AKEYCODE_BACK)` — backs out of game menus like on a phone.
- [x] **Audio** — new SDL2_mixer backend (`source/compat/audio.cpp`) behind all SimpleAudioEngine JNI calls: background music (play/stop/pause/resume/rewind/volume) and sound effects (preload/play/stop/volume, cached chunks), OGG/MP3/Opus/FLAC decoding, reading straight from the extracted APK assets.

### 0.1.56 — Real threads

- [x] **The urandom fix worked** — build 55 sailed past the old ~170s abort: `std::random_device` now reads the Switch CSRNG, and the game went on to initialize vehicles, achievements, and event assets. New blocker: it spawned a persistent asset-loader thread, and the old "run thread functions synchronously" shim froze the game on the HCR loading screen.
- [x] **`pthread_create` now creates real threads** (libnx `threadCreate`, 1 MB stack, pinned to cores 1/2 so workers can't starve the render loop on core 0), each with its own fake Bionic TLS block. `pthread_self`/`pthread_equal`/`pthread_join` are consistent between creator and thread.
- [x] **Real synchronization** — mutexes/condvars/rwlocks/semaphores are now backed by newlib/libnx primitives embedded inside the game's larger Bionic structs (recursive semantics; Bionic's static recursive-initializer bit pattern sanitized). `pthread_cond_timedwait` converts Bionic absolute timeouts and returns Bionic's `ETIMEDOUT`.
- [x] **Thread-safety hardening** — the compat logger and the JNI UserDefault store are mutex-guarded; crash recovery only `longjmp`s on the thread that armed it (worker faults get the symbolized unrecovered-fault log instead).

### 0.1.55 — Abort root-caused: `/dev/urandom`

- [x] **The ~170s abort is fully root-caused** — build 54's symbolized backtrace shows `std::random_device` ctor → `__throw_system_error` → `abort()`. The ctor opens `/dev/urandom` through bionic's *fortified* `__open_2` (which our logging never saw), the path doesn't exist on Horizon OS, and the `-fno-exceptions` build turns the throw into an abort.
- [x] **Fix: `/dev/urandom` virtual fd** — `open`/`__open_2` on `/dev/urandom` (or `/dev/random`) now return a magic fd backed by the Switch's hardware CSRNG; `read`/`__read_chk`/`close` handle it transparently.
- [x] Screenshots confirmed working on hardware — launcher UI, live loading screen, Fingersoft splash (frame 30), and the HCR loading bar (frame 300) all captured automatically.

### 0.1.53+ — The game renders!

- [x] **Hill Climb Racing boots on hardware** — Fingersoft splash animates, loading screen completes, menu logic runs at ~18 fps. Root cause of the constructor crashes: the JIT transition made data pages RX; they're now flipped back to RW, and all 417 ctors pass.
- [x] **Crash identified & instrumented** — the game calls `abort()` at ~165s from libc++'s `__throw_system_error` path (confirmed by symbolized caller: `libgame.so` `error_category::equivalent+0xec`). Added: frame-pointer backtrace with per-frame symbolization on abort/exit, unrecovered-fault PC/LR capture in the exception handler, recovery window widened over the whole game loop (incl. `eglSwapBuffers`).
- [x] **Game output captured** — `debugStringOnAndroid` JNI payloads, writes to the game's stdout/stderr (including through the fake Bionic `__sF`), and `android_set_abort_message` all land in `compat_log.txt` now.
- [x] **`getrandom` implemented** — `syscall(__NR_getrandom)`, `getentropy`, and `getrandom` are backed by the Switch CSRNG (`std::random_device` suspect for the abort).
- [x] **Automatic screenshots** — launcher saves `ui_menu/ui_loading/ui_result/ui_about.png`; the game loop saves the GL framebuffer at frames 30/300/900. All in `sdmc:/AndroidHorizonNX/screenshots/`.
- [x] **Epic UI overhaul** — icon-matched theme: gradient space sky, 110 twinkling/drifting stars, green planet horizon with glowing rim (cached to a texture), borealis-style eased focus card with pulsing glow, real HOS button glyphs (, , …) from the NintendoExt system font with chip fallback, translucent header/footer, 60 fps menu.
- [x] **Version system** — NACP version (shown in hbmenu) and the in-app version both derive from the auto-incrementing build number; the stale `.nacp` is regenerated every build.

### Loader bring-up

- [x] **SplitMap crash fixed** — root cause: `svcCreateCodeMemory` was called before `memcpy`, making the backing buffer inaccessible at userspace. Fixed by deferring `svcCreateCodeMemory` + `svcControlCodeMemory` to after the memcpy. All three `.so` files should now load past the ELF copy stage.
- [x] **RELA/JMPREL logging dramatically reduced** — removed per-entry log lines (one `fflush` per entry on FAT32 was taking ~38 seconds for libapplovin alone). Now only unresolved symbols, WARN lines, and the end-of-table summary are logged — `applyRela` goes from ~8000 lines to ~30 per library.
- [x] **+ button exits progress screen** — press **+** at any time during ELF loading to stop waiting and return to the APK list
- [x] **Renamed to Android Horizon / AndroidHorizonNX** — reflects the project's purpose (Android on HorizonOS) more clearly
- [x] **Live animated progress screen** — loader now runs on a background libnx thread; main thread renders at ~60fps with: animated scan bar (always moving, independent of load progress), live tail of `compat_log.txt` (13 lines, colour-coded for errors/warnings), elapsed time display per stage, "still working" notice after 30s
- [x] **Log timestamps** — every `compat_log.txt` entry prefixed with `[Xs]` seconds-since-launch
- [x] **Immediate constructor logging** — each `ELF: ctor[k/417] @ptr` is force-flushed to disk and the live display the instant before the constructor runs, so a hanging constructor is immediately identifiable
- [x] **"Please wait" patience notice** — log and screen show total constructor count and estimated time before the phase begins
- [x] **Avatar fix** — `socketInitializeDefault()` added so curl/BSD sockets work in homebrew; GitHub URL corrected to Aaronateataco
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

This project is licensed under the Android Horizon NX Free & Source-Available License v1.0 - see [LICENSE](LICENSE) for the full text.

In plain English: This project is 100% free for the community to use, copy, modify, and share. However, you are strictly forbidden from selling this code, its ports, or any derivative works. If you fork or modify this project, you must prominently credit Aaronateataco, keep your source code publicly available, and distribute your version under this exact same license.

Voluntary donations/tips are perfectly fine, but the software itself must always remain free.

There is no warranty — if it breaks your Switch, that's on you (please use CFW responsibly).

The license does not cover the Android games themselves — those belong to their respective developers. Android Horizon only provides the compatibility layer.

---

## About

Made by [Aaron](https://aaronworld.uk) — a non-developer who wanted to see Android games on their modded Switch, and is figuring it out one step at a time with the help of Claude AI.

> **~99.8% of all code in this repo was written by [Claude](https://anthropic.com) (claude-sonnet-4-6 model).** I describe the problem, test on real hardware, and point out what's broken. Claude writes the fixes. This is an honest experiment in AI-assisted hardware hacking.

If this interests you, star the repo and check back. Progress will be slow but real.
