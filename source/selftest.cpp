// ─── Self-test ──────────────────────────────────────────────────────────────
// Everything that is normally checked by launching a game, dumping logs to a
// PC and reading them there — done on the console, in one pass, in a few
// seconds.
//
// Each check is deliberately independent and non-destructive: it may read
// anything, but it writes only to its own scratch file and cleans up after
// itself. A self-test that could break the install it is testing would be
// worse than no self-test at all.
//
// Results also go to sdmc:/switch/Viridite/selftest.txt, so a failure can be
// read off the card afterwards rather than transcribed off the screen.

#include "selftest.h"
#include "apk.h"
#include "update.h"
#include "build_number.h"

#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace {

constexpr char kCoreX64[] = "sdmc:/switch/Viridite/Viridite-Translation-Core-x64.nro";
constexpr char kCoreX32[] = "sdmc:/switch/Viridite/Viridite-Translation-Core-x32.nro";
constexpr char kApkDir[]  = "sdmc:/Viridite/apks";
constexpr char kOutPath[] = "sdmc:/switch/Viridite/selftest.txt";

void add(std::vector<TestResult>& out, TestStatus st, const char* name,
         const char* fmt, ...) __attribute__((format(printf, 4, 5)));

void add(std::vector<TestResult>& out, TestStatus st, const char* name,
         const char* fmt, ...) {
    TestResult r;
    r.status = st;
    r.name   = name;
    char buf[240];
    va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    r.detail = buf;
    out.push_back(r);
}

bool fileExists(const char* p, size_t* size_out = nullptr) {
    struct stat st;
    if (stat(p, &st) != 0) return false;
    if (size_out) *size_out = (size_t)st.st_size;
    return S_ISREG(st.st_mode);
}

// An NRO carries "NRO0" at offset 0x10. Checking the magic separates "the file
// is there" from "the file is a build", which is the distinction that matters
// after a half-finished copy to the card.
bool isValidNro(const char* path, size_t* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[4] = {};
    bool ok = fseek(f, 0x10, SEEK_SET) == 0 && fread(magic, 1, 4, f) == 4 &&
              memcmp(magic, "NRO0", 4) == 0;
    if (ok && size_out) {
        fseek(f, 0, SEEK_END);
        *size_out = (size_t)ftell(f);
    }
    fclose(f);
    return ok;
}

}  // namespace

std::vector<TestResult> selfTestRun(const std::vector<ApkInfo>& apks,
                                    void (*progress)(const char*)) {
    std::vector<TestResult> out;

    // ── Binaries ────────────────────────────────────────────────────────────
    if (progress) progress("Checking binaries");
    {
        size_t sz = 0;
        if (!fileExists(kCoreX64))
            add(out, TestStatus::Fail, "Translation Core (x64)",
                "missing — games cannot launch without it");
        else if (!isValidNro(kCoreX64, &sz))
            add(out, TestStatus::Fail, "Translation Core (x64)",
                "present but not a valid NRO — copy it again");
        else
            add(out, TestStatus::Pass, "Translation Core (x64)",
                "valid NRO, %.1f MB", sz / 1048576.0);

        sz = 0;
        if (!fileExists(kCoreX32))
            add(out, TestStatus::Warn, "Translation Core (x32)",
                "missing — only 32-bit games are affected");
        else if (!isValidNro(kCoreX32, &sz))
            add(out, TestStatus::Warn, "Translation Core (x32)", "not a valid NRO");
        else
            add(out, TestStatus::Pass, "Translation Core (x32)",
                "valid NRO, %.1f MB", sz / 1048576.0);
    }

    // ── romfs assets ────────────────────────────────────────────────────────
    if (progress) progress("Checking bundled assets");
    {
        static const char* kAssets[] = {
            "romfs:/controllers/ctrl_pro.png",
            "romfs:/controllers/ctrl_handheld.png",
            "romfs:/controllers/ctrl_joycon_dual.png",
            "romfs:/controllers/ctrl_joycon_left.png",
            "romfs:/controllers/ctrl_joycon_right.png",
            "romfs:/controllers/ctrl_touch.png",
        };
        int missing = 0;
        std::string firstMissing;
        for (const char* a : kAssets) {
            if (!fileExists(a)) {
                missing++;
                if (firstMissing.empty()) firstMissing = a;
            }
        }
        if (missing)
            add(out, TestStatus::Fail, "Controller artwork",
                "%d of %zu missing (first: %s)", missing,
                sizeof(kAssets)/sizeof(kAssets[0]), firstMissing.c_str());
        else
            add(out, TestStatus::Pass, "Controller artwork",
                "all %zu present", sizeof(kAssets)/sizeof(kAssets[0]));
    }

    // ── Fonts ───────────────────────────────────────────────────────────────
    if (progress) progress("Checking fonts");
    {
        PlFontData fd;
        Result rc = plGetSharedFontByType(&fd, PlSharedFontType_Standard);
        if (R_FAILED(rc))
            add(out, TestStatus::Fail, "System font",
                "plGetSharedFontByType failed rc=0x%x", rc);
        else if (!fd.address || fd.size == 0)
            add(out, TestStatus::Fail, "System font", "returned an empty font");
        else
            add(out, TestStatus::Pass, "System font", "BFTTF, %u KB",
                (unsigned)(fd.size / 1024));
    }

    // ── Storage ─────────────────────────────────────────────────────────────
    if (progress) progress("Checking storage");
    {
        // Write, read back and delete. Anything less does not prove the card is
        // actually writable — a full or read-only card passes a stat().
        const char* probe = "sdmc:/switch/Viridite/.selftest_probe";
        bool wrote = false, matched = false;
        if (FILE* f = fopen(probe, "wb")) {
            wrote = fwrite("viridite", 1, 8, f) == 8;
            fclose(f);
        }
        if (wrote) {
            char rb[9] = {};
            if (FILE* f = fopen(probe, "rb")) {
                matched = fread(rb, 1, 8, f) == 8 && memcmp(rb, "viridite", 8) == 0;
                fclose(f);
            }
        }
        remove(probe);
        if (!wrote)        add(out, TestStatus::Fail, "SD card writable", "could not create a file");
        else if (!matched) add(out, TestStatus::Fail, "SD card writable", "wrote, but read back wrong");
        else               add(out, TestStatus::Pass, "SD card writable", "write/read/delete OK");

        struct statvfs vfs;
        if (statvfs("sdmc:/", &vfs) == 0) {
            double freeGb = (double)vfs.f_bavail * vfs.f_frsize / 1073741824.0;
            add(out, freeGb < 1.0 ? TestStatus::Warn : TestStatus::Pass,
                "Free space", "%.1f GB available", freeGb);
        } else {
            add(out, TestStatus::Warn, "Free space", "statvfs unavailable");
        }
    }

    // ── Games ───────────────────────────────────────────────────────────────
    if (progress) progress("Checking games");
    {
        DIR* d = opendir(kApkDir);
        if (!d) {
            add(out, TestStatus::Warn, "Games folder",
                "%s not found — nothing to launch", kApkDir);
        } else {
            closedir(d);
            if (apks.empty()) {
                add(out, TestStatus::Warn, "Games folder", "no APKs found");
            } else {
                add(out, TestStatus::Pass, "Games folder", "%zu APK(s)", apks.size());
                int noName = 0, noIcon = 0, noPkg = 0, arm32 = 0;
                for (const ApkInfo& a : apks) {
                    if (a.packageName.empty()) noPkg++;
                    if (a.appName.empty())     noName++;
                    if (a.iconPng.empty())     noIcon++;
                    if (a.arch == ApkArch::Arm32Only) arm32++;
                }
                // Each of these is a parse that silently degrades the UI rather
                // than failing outright, so they are worth surfacing.
                add(out, noPkg  ? TestStatus::Fail : TestStatus::Pass, "Manifest parsing",
                    noPkg ? "%d APK(s) have no package name" : "all packages resolved", noPkg);
                add(out, noName ? TestStatus::Warn : TestStatus::Pass, "App labels",
                    noName ? "%d without a label" : "all labelled", noName);
                add(out, noIcon ? TestStatus::Warn : TestStatus::Pass, "Icon extraction",
                    noIcon ? "%d without an icon" : "all icons extracted", noIcon);
                if (arm32)
                    add(out, TestStatus::Warn, "Architecture",
                        "%d 32-bit game(s) — experimental path", arm32);
                else
                    add(out, TestStatus::Pass, "Architecture", "all 64-bit");
            }
        }
    }

    // ── Controllers and sensors ─────────────────────────────────────────────
    if (progress) progress("Checking controllers");
    {
        int pads = 0;
        for (int i = 0; i < 8; i++)
            if (hidGetNpadStyleSet((HidNpadIdType)i) != 0) pads++;
        const bool handheld =
            (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) != 0;
        if (!pads && !handheld)
            add(out, TestStatus::Warn, "Controllers", "none detected (touch only)");
        else
            add(out, TestStatus::Pass, "Controllers", "%d pad(s)%s", pads,
                handheld ? ", Joy-Cons attached" : "");

        const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        add(out, TestStatus::Pass, "Display mode", "%s, %s",
            docked ? "docked" : "handheld", docked ? "1920x1080" : "1280x720");

        // Orientation follows this: with Joy-Cons attached the sensor speaks
        // for the console, detached it does not. Reporting which case we are in
        // makes an "auto-rotate isn't working" report answerable at a glance.
        HidSixAxisSensorHandle h[2];
        Result rc = hidGetSixAxisSensorHandles(h, 1, HidNpadIdType_Handheld,
                                               HidNpadStyleTag_NpadHandheld);
        if (R_SUCCEEDED(rc))
            add(out, TestStatus::Pass, "Motion sensor",
                handheld ? "available and speaks for the console"
                         : "available, but Joy-Cons are detached so it does not "
                           "track the console");
        else
            add(out, TestStatus::Warn, "Motion sensor",
                "unavailable rc=0x%x — orientation stays landscape", rc);
    }

    // ── Network and updates ─────────────────────────────────────────────────
    if (progress) progress("Checking network");
    {
        // nifm answers "is there a connection" without any traffic, which is
        // the right question here — a failing update check could equally be a
        // GitHub problem, and conflating the two would send someone looking in
        // the wrong place.
        NifmInternetConnectionStatus st = (NifmInternetConnectionStatus)0;
        NifmInternetConnectionType   ty = (NifmInternetConnectionType)0;
        u32 strength = 0;
        Result rc = nifmGetInternetConnectionStatus(&ty, &strength, &st);
        const bool online = R_SUCCEEDED(rc) && st == NifmInternetConnectionStatus_Connected;
        add(out, online ? TestStatus::Pass : TestStatus::Warn, "Network",
            online ? "connected (%s, strength %u)" : "not connected — updates unavailable",
            ty == NifmInternetConnectionType_WiFi ? "Wi-Fi" : "wired", strength);

        // The launcher kicks its update check off at startup, so by now it has
        // usually finished; report whatever it found rather than issuing a
        // second request.
        UpdateInfo info;
        if (!updateCheckPoll(&info)) {
            add(out, TestStatus::Warn, "Latest release", "check still running");
        } else if (!info.error.empty()) {
            add(out, TestStatus::Warn, "Latest release", "%s", info.error.c_str());
        } else if (info.available) {
            add(out, TestStatus::Warn, "Latest release",
                "%s available (running %s)", info.tag.c_str(), updateCurrentVersion());
        } else if (!info.pendingTag.empty()) {
            add(out, TestStatus::Warn, "Latest release",
                "%s tagged but has no build yet", info.pendingTag.c_str());
        } else {
            add(out, TestStatus::Pass, "Latest release", "up to date (%s)",
                updateCurrentVersion());
        }
    }

    // ── Persist ─────────────────────────────────────────────────────────────
    if (FILE* f = fopen(kOutPath, "w")) {
        fprintf(f, "Viridite self-test — build %s\n\n", updateCurrentVersion());
        int pass = 0, warn = 0, fail = 0;
        for (const TestResult& r : out) {
            const char* tag = r.status == TestStatus::Pass ? "PASS"
                            : r.status == TestStatus::Warn ? "WARN" : "FAIL";
            (r.status == TestStatus::Pass ? pass
             : r.status == TestStatus::Warn ? warn : fail)++;
            fprintf(f, "[%s] %-26s %s\n", tag, r.name.c_str(), r.detail.c_str());
        }
        fprintf(f, "\n%d passed, %d warnings, %d failed\n", pass, warn, fail);
        fclose(f);
    }

    return out;
}
