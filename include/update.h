#pragma once
#include <string>
#include <functional>

// ── Self-update ──────────────────────────────────────────────────────────────
// Checks GitHub for a newer Viridite release and installs it onto the SD card.
//
// The release bundle (Viridite-sdcard.zip, built by release.yml) already has
// exactly the on-disk layout we need:
//
//   Viridite.nro
//   Viridite/Viridite-Translation-Core-x64.nro
//   Viridite/Viridite-Translation-Core-x32.nro
//
// so installing is "unpack it over sdmc:/switch/" — the launcher and both Core
// binaries move together, which matters because a launcher and a Core from
// different releases aren't a combination anyone has tested.

struct UpdateInfo {
    bool        available = false;   // a NEWER, INSTALLABLE release than us exists
    bool        checked   = false;   // the check finished (successfully or not)
    std::string tag;                 // e.g. "v0.1.19-testing-alpha"
    std::string assetUrl;            // browser_download_url of the sdcard zip
    long long   assetSize = 0;       // bytes, 0 if GitHub didn't say
    std::string error;               // non-empty if the check failed

    // Tags and releases are checked separately and the newer wins. They can
    // legitimately disagree: release.yml pushes the tag first and creates the
    // release second, so a newer tag with no build behind it is what an
    // in-flight (or failed) release run looks like. A bare tag has no
    // Viridite-sdcard.zip, so there is nothing to install from it — when that
    // happens this reports the tag rather than claiming we're up to date.
    std::string pendingTag;          // newer tag with no downloadable build yet
};

// Kicks the check off on a background thread so the app list still draws and
// responds while the network is slow or absent. Safe to call once at startup.
void updateCheckStart();

// Non-blocking poll. Returns true once the check has finished; `out` is then
// filled in. Until then it returns false and `out` is untouched.
bool updateCheckPoll(UpdateInfo* out);

// Frees the checker thread. Call before exit.
void updateCheckJoin();

// Downloads and installs `info`. Blocking — the caller is expected to be
// drawing a progress screen from `progress`, which is invoked with a short
// stage label and 0-100 (or -1 when the total size is unknown).
//
// Returns true only when every file was verified and moved into place. On
// failure nothing on the SD card has been replaced and `err` explains why.
bool updateApply(const UpdateInfo& info,
                 const std::function<void(const char* stage, int pct)>& progress,
                 std::string* err);

// Version string this binary was built as, e.g. "v0.1.19-testing-alpha", or
// "dev" for a local build. Local builds never consider themselves out of date.
const char* updateCurrentVersion();
