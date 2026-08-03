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
// Alongside launcher_log.txt and compat_log.txt, not next to the NROs. That is
// where every other log lives and where anyone would look for this one.
constexpr char kOutPath[]  = "sdmc:/Viridite/selftest.txt";
// Written by the Core's dry run and read back here after it hands control
// back to us.
constexpr char kCorePath[] = "sdmc:/Viridite/selftest_core.txt";

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

// Reads back the previous launch of this package from the Core's log. The
// information is already being written; the only thing missing was anywhere on
// the console to see it.
static void selfTestLastRun(const std::string& pkg, const std::string& label,
                            std::vector<TestResult>& out) {
    // Try both plausible locations and say which was tried. "No log yet" and
    // "the log is somewhere else" are different problems with different fixes,
    // and a bare shrug does not distinguish them — the Core writes to the first
    // of these, but a log that has been moved off the card looks identical to
    // one that was never written.
    static const char* kLogPaths[] = {
        "sdmc:/Viridite/compat_log.txt",
        "sdmc:/switch/Viridite/compat_log.txt",
    };
    FILE* f = nullptr;
    const char* found = nullptr;
    for (const char* lp : kLogPaths)
        if ((f = fopen(lp, "r")) != nullptr) { found = lp; break; }
    if (!f) {
        add(out, TestStatus::Warn, (label + " — last run").c_str(),
            "no log at %s (has it been moved off the card?)", kLogPaths[0]);
        return;
    }
    (void)found;
    char line[512];
    bool   thisGame = false, loaded = false, handedOff = false;
    int    faults = 0, wedged = 0;
    char   version[64] = "?";
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "launchApk:") && strstr(line, pkg.c_str())) thisGame = true;
        if (strstr(line, "env: Viridite")) {
            const char* v = strstr(line, "v0.");
            if (v) { size_t n = 0; while (n < sizeof(version) - 1 && v[n] > ' ') { version[n] = v[n]; n++; } version[n] = 0; }
        }
        if (strstr(line, "FAULT"))                       faults++;
        if (strstr(line, "loading complete"))            loaded = true;
        if (strstr(line, "handing off to the game"))     handedOff = true;
        if (strstr(line, "WATCHDOG: main thread"))       wedged++;
    }
    fclose(f);

    if (!thisGame) {
        add(out, TestStatus::Warn, (label + " — last run").c_str(),
            "not the game in the current log");
        return;
    }
    if (wedged)
        add(out, TestStatus::Fail, (label + " — last run").c_str(),
            "%s: loaded=%s, %d ctor fault(s), main thread wedged", version,
            loaded ? "yes" : "no", faults);
    else if (handedOff)
        add(out, TestStatus::Pass, (label + " — last run").c_str(),
            "%s: reached the game, %d ctor fault(s)", version, faults);
    else if (loaded)
        add(out, TestStatus::Warn, (label + " — last run").c_str(),
            "%s: loaded but never handed off, %d ctor fault(s)", version, faults);
    else
        add(out, TestStatus::Warn, (label + " — last run").c_str(),
            "%s: did not finish loading, %d ctor fault(s)", version, faults);
}

// Results of the last Core dry run, if one has happened. This is the part that
// actually exercises the loader — extract, map, relocate, resolve every
// imported symbol — rather than reporting on a previous launch.
static void selfTestCoreResults(std::vector<TestResult>& out) {
    FILE* f = fopen(kCorePath, "r");
    if (!f) {
        add(out, TestStatus::Warn, "Deep test",
            "not run yet — select a game, then press X here to load it "
            "without starting it");
        return;
    }
    char line[512];
    bool ok = fgets(line, sizeof(line), f) && strncmp(line, "v1", 2) == 0;
    if (!ok) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        char* p1 = strchr(line, '|');  if (!p1) continue;
        char* p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        *p1 = 0; *p2 = 0;
        const char* verdict = line, *pkg = p1 + 1, *detail = p2 + 1;
        TestStatus st = !strcmp(verdict, "PASS") ? TestStatus::Pass
                      : !strcmp(verdict, "FAIL") ? TestStatus::Fail : TestStatus::Warn;
        add(out, st, (std::string("Deep test — ") + pkg).c_str(), "%s", detail);
    }
    fclose(f);
}

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
        const char* probe = "sdmc:/Viridite/.selftest_probe";
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
    // Per game, not aggregate counts. The point of this mode is to answer "did
    // my change break Brain It On" without launching it and reading logs on a
    // PC, so each game gets checked through the whole chain the loader will
    // walk: the APK parses, the install exists, the native libraries are
    // actually there and are arm64, and what happened the last time it ran.
    if (progress) progress("Checking games");
    {
        DIR* d = opendir(kApkDir);
        if (!d) {
            add(out, TestStatus::Warn, "Games folder",
                "%s not found — nothing to launch", kApkDir);
        } else {
            closedir(d);
            if (apks.empty())
                add(out, TestStatus::Warn, "Games folder", "no APKs found");
            else
                add(out, TestStatus::Pass, "Games folder", "%zu APK(s)", apks.size());
        }

        for (const ApkInfo& a : apks) {
            const std::string pkg = a.packageName.empty() ? a.filename : a.packageName;
            const std::string label = a.appName.empty() ? pkg : a.appName;

            // Manifest — a package name is the one field nothing downstream can
            // work without, so its absence is a failure rather than a warning.
            if (a.packageName.empty())
                add(out, TestStatus::Fail, label.c_str(), "manifest gave no package name");
            else
                add(out, TestStatus::Pass, label.c_str(), "%s%s%s, %s%s",
                    a.packageName.c_str(),
                    a.versionName.empty() ? "" : " v",
                    a.versionName.c_str(),
                    a.arch == ApkArch::Arm64 ? "arm64"
                      : a.arch == ApkArch::Arm32Only ? "32-bit only" : "unknown ABI",
                    a.hasIcon ? "" : ", no icon");

            // Install — the marker alone is not proof; the loader needs the
            // extracted libraries, and a marker left behind by a failed extract
            // is exactly the state that looks fine and then does not launch.
            // Both library directories. A 32-bit-only game has an empty lib/
            // and everything in lib32/, so checking lib/ alone reported Hill
            // Climb Racing 2 as a broken install and told you to reinstall a
            // game that had extracted perfectly.
            std::string dir  = std::string("sdmc:/Viridite/games/") + pkg;
            std::string lib  = dir + "/lib";
            std::string lib32 = dir + "/lib32";
            if (!apkIsInstalled(pkg)) {
                add(out, TestStatus::Warn, (label + " — install").c_str(),
                    "not installed yet (will extract on first launch)");
            } else {
                int    nlibs = 0;
                size_t total = 0, smallest = (size_t)-1;
                std::string smallestName;
                int n64 = 0, n32 = 0;
                for (const std::string* d : {&lib, &lib32}) {
                    if (DIR* ld = opendir(d->c_str())) {
                        while (dirent* e = readdir(ld)) {
                            std::string n = e->d_name;
                            if (n.size() < 4 || n.substr(n.size() - 3) != ".so") continue;
                            struct stat st;
                            if (stat((*d + "/" + n).c_str(), &st) != 0) continue;
                            nlibs++; total += (size_t)st.st_size;
                            (d == &lib ? n64 : n32)++;
                            if ((size_t)st.st_size < smallest) { smallest = st.st_size; smallestName = n; }
                        }
                        closedir(ld);
                    }
                }
                // Two versions of one game share a package name and therefore
                // one install directory, so whichever was launched last owns
                // it. Without saying so, Hill Climb Racing 1.67 reports three
                // 32-bit libraries and looks broken, when what it is actually
                // showing is 1.70's install. The Core re-extracts on launch
                // when the marker does not match, so this is not a fault —
                // but it is confusing to read without the explanation.
                std::string from;
                if (FILE* mf = fopen((dir + "/.installed").c_str(), "r")) {
                    char b[512] = {0};
                    if (fgets(b, sizeof(b), mf)) from = b;
                    fclose(mf);
                    while (!from.empty() && (from.back() == '\n' || from.back() == '\r'))
                        from.pop_back();
                }
                if (!from.empty() && from != a.path) {
                    add(out, TestStatus::Warn, (label + " — install").c_str(),
                        "belongs to another build of this package — launching will "
                        "re-extract (%d arm64, %d arm32 currently)", n64, n32);
                } else if (nlibs == 0)
                    add(out, TestStatus::Fail, (label + " — install").c_str(),
                        "marked installed but neither lib/ nor lib32/ has any .so — "
                        "reinstall with X");
                else if (smallest == 0)
                    add(out, TestStatus::Fail, (label + " — install").c_str(),
                        "%s is 0 bytes — the extract was interrupted", smallestName.c_str());
                else
                    add(out, TestStatus::Pass, (label + " — install").c_str(),
                        "%d lib(s) extracted, %.1f MB (%d arm64, %d arm32)",
                        nlibs, total / 1048576.0, n64, n32);
            }

            // Last run — the log already records what happened; reading it back
            // turns "does this still work" into something answerable on the
            // console instead of a launch and a file transfer.
            selfTestLastRun(pkg, label, out);
        }
    }

    selfTestCoreResults(out);

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
