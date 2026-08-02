#include "update.h"
#include "build_number.h"

#include <switch.h>
#include <curl/curl.h>
#include <minizip/unzip.h>

#include <dirent.h>
#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// Releases live on the launcher repo; the bundle asset is built by release.yml.
static const char* RELEASES_API =
    "https://api.github.com/repos/Viridite/Viridite/releases?per_page=10";
// Tags are checked too: the release workflow pushes the tag before creating the
// release, so a tag can be ahead of anything downloadable. Knowing that is the
// difference between "you're up to date" and "the build isn't published yet".
static const char* TAGS_API =
    "https://api.github.com/repos/Viridite/Viridite/tags?per_page=10";
static const char* ASSET_NAME = "Viridite-sdcard.zip";

// GitHub rejects requests without a User-Agent.
static const char* UA = "Viridite-Updater";

// Everything lands under here first and is only moved into sdmc:/switch/ once
// all of it has been unpacked and sanity-checked.
static const char* WORK_DIR    = "sdmc:/Viridite/update";
static const char* STAGING_DIR = "sdmc:/Viridite/update/staging";
static const char* ZIP_PATH    = "sdmc:/Viridite/update/Viridite-sdcard.zip";
static const char* INSTALL_DIR = "sdmc:/switch";

// ─────────────────────────────────────────────────────────────── small utils ─

static bool removeRecursive(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return true;      // already gone
    if (!S_ISDIR(st.st_mode)) return remove(path.c_str()) == 0;

    DIR* d = opendir(path.c_str());
    if (!d) return false;
    bool ok = true;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!removeRecursive(path + "/" + name)) ok = false;
    }
    closedir(d);
    return (remove(path.c_str()) == 0) && ok;
}

// Pull "key": "value" out of a JSON blob without dragging in a parser. Only
// used against GitHub's own release payload, whose shape is fixed and which we
// reach over a verified TLS connection.
static std::string jsonStr(const std::string& src, const std::string& key, size_t from = 0) {
    std::string pat = "\"" + key + "\"";
    size_t k = src.find(pat, from);
    if (k == std::string::npos) return "";
    size_t c = src.find(':', k + pat.size());
    if (c == std::string::npos) return "";
    size_t q = src.find('"', c);
    if (q == std::string::npos) return "";
    std::string out;
    for (size_t i = q + 1; i < src.size(); i++) {
        char ch = src[i];
        if (ch == '\\' && i + 1 < src.size()) { out += src[++i]; continue; }
        if (ch == '"') break;
        out += ch;
    }
    return out;
}

static long long jsonNum(const std::string& src, const std::string& key, size_t from = 0) {
    std::string pat = "\"" + key + "\"";
    size_t k = src.find(pat, from);
    if (k == std::string::npos) return 0;
    size_t c = src.find(':', k + pat.size());
    if (c == std::string::npos) return 0;
    return strtoll(src.c_str() + c + 1, nullptr, 10);
}

// "v0.1.19-testing-alpha" -> 0,1,19. Anything unparseable stays 0 so it sorts
// below a real release rather than above one.
static void parseVersion(const std::string& v, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    const char* p = v.c_str();
    while (*p && (*p < '0' || *p > '9')) p++;       // skip a leading 'v'
    for (int i = 0; i < 3 && *p; i++) {
        out[i] = (int)strtol(p, (char**)&p, 10);
        if (*p == '.') p++; else break;
    }
}

static bool isNewer(const std::string& remote, const std::string& local) {
    int r[3], l[3];
    parseVersion(remote, r);
    parseVersion(local, l);
    for (int i = 0; i < 3; i++) {
        if (r[i] != l[i]) return r[i] > l[i];
    }
    return false;
}

const char* updateCurrentVersion() {
#ifdef VIRIDITE_VERSION
    return VIRIDITE_VERSION;
#else
    return "dev";
#endif
}

// ────────────────────────────────────────────────────────────────────── curl ─

struct MemSink { std::string data; size_t cap; };

static size_t writeToMem(void* ptr, size_t sz, size_t nm, void* ud) {
    MemSink* s = (MemSink*)ud;
    size_t n = sz * nm;
    if (s->data.size() + n > s->cap) return 0;   // refuse to grow without bound
    s->data.append((const char*)ptr, n);
    return n;
}

struct FileSink {
    FILE* fp;
    long long got;
    long long total;
    const std::function<void(const char*, int)>* cb;
};

static size_t writeToFile(void* ptr, size_t sz, size_t nm, void* ud) {
    FileSink* s = (FileSink*)ud;
    size_t n = fwrite(ptr, sz, nm, s->fp) * sz;
    s->got += (long long)(sz * nm);
    if (s->cb) {
        int pct = s->total > 0 ? (int)((s->got * 100) / s->total) : -1;
        (*s->cb)("Downloading update", pct);
    }
    return n / (sz ? sz : 1);
}

static void applyCommonOpts(CURL* c) {
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // release assets redirect
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    // Certificate verification stays ON. devkitPro's curl runs TLS through the
    // Switch's own ssl sysmodule and its CA store, so this needs no bundled
    // cacert.pem — and it must never be turned off here: this code downloads
    // something the console then executes, so a MITM would be arbitrary code
    // execution, not just a corrupted file.
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
}

// ───────────────────────────────────────────────────────────────── the check ─

static std::atomic<bool> g_checkDone{false};
static UpdateInfo        g_result;
static std::thread       g_thread;
static std::atomic<bool> g_running{false};

// Waits (up to timeoutMs) for the console to report an actual internet
// connection, via the same nifm check the Core already uses.
//
// socketInitializeDefault() returning cleanly only means the socket driver is
// up — it says nothing about whether the console has associated with a network,
// and DNS fails with a bare "Couldn't resolve host name" until it has. Hardware
// logs showed exactly that: the check fired the instant the launcher started
// and lost the race. Since this all runs on the background thread, waiting here
// costs the UI nothing.
static bool waitForInternet(int timeoutMs) {
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return false;
    bool ok = false;
    for (int waited = 0; ; waited += 500) {
        NifmInternetConnectionType ct;
        u32 strength = 0;
        NifmInternetConnectionStatus st;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&ct, &strength, &st)) &&
            st == NifmInternetConnectionStatus_Connected) { ok = true; break; }
        if (waited >= timeoutMs) break;
        svcSleepThread(500ULL * 1000000ULL);   // 500ms
    }
    nifmExit();
    return ok;
}

// GET into memory. Returns false and fills `err` on transport or HTTP failure.
static bool httpGet(const char* url, std::string* out, std::string* err) {
    CURL* c = curl_easy_init();
    if (!c) { *err = "couldn't start curl"; return false; }

    MemSink sink; sink.cap = 1024 * 1024;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToMem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    applyCommonOpts(c);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        // Name resolution is the failure people actually hit, and curl's own
        // wording for it doesn't tell a Switch owner anything actionable.
        if (rc == CURLE_COULDNT_RESOLVE_HOST)
            *err = "couldn't reach GitHub (DNS failed — is the console online?)";
        else
            *err = std::string("network: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http != 200) {
        char buf[96];
        // 403 here is almost always the anonymous rate limit (60/hour per IP).
        if (http == 403) snprintf(buf, sizeof buf, "GitHub rate-limited us (HTTP 403) — try later");
        else             snprintf(buf, sizeof buf, "GitHub returned HTTP %ld", http);
        *err = buf;
        return false;
    }
    *out = sink.data;
    return true;
}

// Highest version among a /tags payload. Empty if none parse.
static std::string highestTag(const std::string& body) {
    std::string best;
    for (size_t p = body.find("\"name\""); p != std::string::npos; p = body.find("\"name\"", p + 1)) {
        std::string span = body.substr(p, 128);
        std::string name = jsonStr(span, "name");
        if (name.empty()) continue;
        // Tags carry version-shaped names; anything else (a branch-ish tag)
        // parses as 0.0.0 and loses to a real version.
        if (best.empty() || isNewer(name, best)) best = name;
    }
    return best;
}

static void doCheck() {
    UpdateInfo info;
    info.checked = true;

    // Give the console a moment to actually get online before asking DNS
    // anything. An offline Switch is a completely normal way to run this, so
    // that isn't an error worth alarming anyone about — just say so and stop.
    if (!waitForInternet(10000)) {
        info.error = "no internet connection — skipped the update check";
        g_result = info; g_checkDone = true; return;
    }

    std::string body, err;
    if (!httpGet(RELEASES_API, &body, &err)) {
        info.error = err; g_result = info; g_checkDone = true; return;
    }

    // Deliberately NOT /releases/latest: every Viridite release is published as
    // a prerelease, and that endpoint ignores prereleases — it 404s.
    //
    // The list endpoint does come back newest-first, but rather than trust
    // position we pick the highest version among the entries we got. That
    // costs nothing and stays correct if a release is ever published
    // out-of-order or backdated, where "newest by date" and "highest version"
    // stop being the same release.

    // Split the array into per-release spans on "tag_name" boundaries so an
    // asset URL can never be attributed to a different release than its tag.
    std::vector<size_t> starts;
    for (size_t p = body.find("\"tag_name\""); p != std::string::npos;
         p = body.find("\"tag_name\"", p + 1)) {
        starts.push_back(p);
    }
    if (starts.empty()) {
        info.error = "no releases published yet";
        g_result = info; g_checkDone = true; return;
    }

    std::string bestTag, bestUrl;
    long long   bestSize = 0;
    for (size_t i = 0; i < starts.size(); i++) {
        size_t from = starts[i];
        size_t to   = (i + 1 < starts.size()) ? starts[i + 1] : body.size();
        std::string span = body.substr(from, to - from);

        std::string tag = jsonStr(span, "tag_name");
        if (tag.empty()) continue;
        if (!bestTag.empty() && !isNewer(tag, bestTag)) continue;

        size_t nameAt = span.find(std::string("\"") + ASSET_NAME + "\"");
        if (nameAt == std::string::npos) continue;    // no bundle: not installable

        bestTag  = tag;
        bestUrl  = jsonStr(span, "browser_download_url", nameAt);
        bestSize = jsonNum(span, "size", nameAt);
    }

    if (bestTag.empty()) {
        info.error = "no release with a " + std::string(ASSET_NAME) + " asset";
        g_result = info; g_checkDone = true; return;
    }
    info.tag       = bestTag;
    info.assetUrl  = bestUrl;
    info.assetSize = bestSize;

    // Now the tags, so "newest tag or newest release, whichever is ahead" is
    // what actually gets reported. A failure here is not fatal — the release
    // answer alone is still useful, so this only ever adds information.
    std::string tagsBody, tagsErr;
    std::string newestTag;
    if (httpGet(TAGS_API, &tagsBody, &tagsErr)) {
        newestTag = highestTag(tagsBody);
    }

    std::string cur = updateCurrentVersion();
    if (cur == "dev") {
        // A locally built binary has no meaningful place in the release
        // ordering; offering to "update" it would just overwrite your own build.
        info.available = false;
    } else if (info.assetUrl.empty()) {
        info.error = "release has no " + std::string(ASSET_NAME) + " asset";
    } else {
        info.available = isNewer(info.tag, cur);
    }

    // A tag ahead of the newest downloadable release means a release run is
    // mid-flight or failed. Say so instead of reporting "up to date", but don't
    // offer it as an update — there's no build behind it to install.
    if (!newestTag.empty() && isNewer(newestTag, info.tag) && cur != "dev" &&
        isNewer(newestTag, cur)) {
        info.pendingTag = newestTag;
    }

    g_result = info;
    g_checkDone = true;
}

void updateCheckStart() {
    if (g_running) return;
    g_running   = true;
    g_checkDone = false;
    g_thread = std::thread(doCheck);
}

bool updateCheckPoll(UpdateInfo* out) {
    if (!g_checkDone) return false;
    if (out) *out = g_result;
    return true;
}

void updateCheckJoin() {
    if (g_thread.joinable()) g_thread.join();
    g_running = false;
}

// ───────────────────────────────────────────────────────────────── the apply ─

// A Switch executable starts with "NRO0" at offset 0x10. Checking it catches a
// truncated or HTML-error-page download before we put it where hbmenu will
// try to run it.
static bool looksLikeNro(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {0};
    bool ok = fseek(f, 0x10, SEEK_SET) == 0 && fread(magic, 1, 4, f) == 4;
    fclose(f);
    return ok && memcmp(magic, "NRO0", 4) == 0;
}

static bool ensureDir(const std::string& p) {
    mkdir(p.c_str(), 0777);
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Unpacks every entry into destRoot, keeping the archive's own directory
// layout. Rejects paths that try to escape it.
static bool unzipAll(const std::string& zipPath, const std::string& destRoot,
                     const std::function<void(const char*, int)>& progress,
                     std::string* err) {
    unzFile z = unzOpen(zipPath.c_str());
    if (!z) { *err = "downloaded file isn't a readable zip"; return false; }

    unz_global_info gi;
    if (unzGetGlobalInfo(z, &gi) != UNZ_OK) { unzClose(z); *err = "corrupt zip"; return false; }

    bool ok = true;
    for (uLong i = 0; i < gi.number_entry && ok; i++) {
        char name[512];
        unz_file_info fi;
        if (unzGetCurrentFileInfo(z, &fi, name, sizeof name, nullptr, 0, nullptr, 0) != UNZ_OK) {
            *err = "corrupt zip entry"; ok = false; break;
        }
        std::string rel = name;

        // Zip-slip guard: an archive entry must not climb out of the staging
        // directory. Ours never would, but this code writes into sdmc:/switch.
        if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
            *err = "zip contains an unsafe path: " + rel; ok = false; break;
        }

        progress("Unpacking update", (int)((i * 100) / (gi.number_entry ? gi.number_entry : 1)));

        std::string out = destRoot + "/" + rel;
        if (!rel.empty() && rel.back() == '/') {          // directory entry
            ensureDir(out);
        } else {
            size_t slash = out.find_last_of('/');
            if (slash != std::string::npos) ensureDir(out.substr(0, slash));

            if (unzOpenCurrentFile(z) != UNZ_OK) { *err = "can't read " + rel; ok = false; break; }
            FILE* f = fopen(out.c_str(), "wb");
            if (!f) { unzCloseCurrentFile(z); *err = "can't write " + out; ok = false; break; }

            std::vector<char> buf(64 * 1024);
            int n;
            while ((n = unzReadCurrentFile(z, buf.data(), (unsigned)buf.size())) > 0) {
                if (fwrite(buf.data(), 1, (size_t)n, f) != (size_t)n) {
                    *err = "SD card write failed (full?)"; ok = false; break;
                }
            }
            if (n < 0) { *err = "decompression failed for " + rel; ok = false; }
            fclose(f);
            unzCloseCurrentFile(z);
        }

        if (i + 1 < gi.number_entry && unzGoToNextFile(z) != UNZ_OK) break;
    }

    unzClose(z);
    return ok;
}

// Collects every regular file under root, as paths relative to it.
static void listFiles(const std::string& root, const std::string& prefix,
                      std::vector<std::string>& out) {
    DIR* d = opendir((root + (prefix.empty() ? "" : "/" + prefix)).c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string rel  = prefix.empty() ? name : prefix + "/" + name;
        std::string full = root + "/" + rel;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) listFiles(root, rel, out);
        else                     out.push_back(rel);
    }
    closedir(d);
}

bool updateApply(const UpdateInfo& info,
                 const std::function<void(const char*, int)>& progress,
                 std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;

    if (info.assetUrl.empty()) { *err = "no download URL for this release"; return false; }

    ensureDir("sdmc:/Viridite");
    removeRecursive(WORK_DIR);                 // never reuse a half-finished attempt
    if (!ensureDir(WORK_DIR) || !ensureDir(STAGING_DIR)) {
        *err = "couldn't create " + std::string(WORK_DIR); return false;
    }

    // ── download ──
    progress("Downloading update", 0);
    FILE* fp = fopen(ZIP_PATH, "wb");
    if (!fp) { *err = "couldn't open " + std::string(ZIP_PATH); return false; }

    CURL* c = curl_easy_init();
    if (!c) { fclose(fp); *err = "couldn't start curl"; return false; }

    FileSink sink{fp, 0, info.assetSize, &progress};
    curl_easy_setopt(c, CURLOPT_URL, info.assetUrl.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    applyCommonOpts(c);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    fclose(fp);

    if (rc != CURLE_OK) { *err = std::string("download failed: ") + curl_easy_strerror(rc); removeRecursive(WORK_DIR); return false; }
    if (http != 200)    { char b[64]; snprintf(b, sizeof b, "download failed (HTTP %ld)", http); *err = b; removeRecursive(WORK_DIR); return false; }

    // ── unpack into staging ──
    if (!unzipAll(ZIP_PATH, STAGING_DIR, progress, err)) { removeRecursive(WORK_DIR); return false; }
    remove(ZIP_PATH);                                   // reclaim the space early

    // ── verify before anything is replaced ──
    progress("Verifying", 100);
    std::vector<std::string> files;
    listFiles(STAGING_DIR, "", files);
    if (files.empty()) { *err = "update bundle was empty"; removeRecursive(WORK_DIR); return false; }

    bool sawLauncher = false;
    for (const auto& rel : files) {
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".nro") == 0) {
            if (!looksLikeNro(STAGING_DIR + std::string("/") + rel)) {
                *err = "downloaded " + rel + " isn't a valid NRO — aborting";
                removeRecursive(WORK_DIR);
                return false;
            }
            if (rel == "Viridite.nro") sawLauncher = true;
        }
    }
    if (!sawLauncher) {
        *err = "update bundle has no Viridite.nro — aborting";
        removeRecursive(WORK_DIR);
        return false;
    }

    // ── install ──
    // Only now does anything outside the work directory change. Each file is
    // replaced by renaming the verified copy over it, keeping one .bak behind
    // so a failure part-way still leaves a runnable launcher on the card.
    progress("Installing", 0);
    int done = 0;
    for (const auto& rel : files) {
        std::string from = STAGING_DIR + std::string("/") + rel;
        std::string to   = INSTALL_DIR + std::string("/") + rel;

        size_t slash = to.find_last_of('/');
        if (slash != std::string::npos) ensureDir(to.substr(0, slash));

        std::string bak = to + ".bak";
        remove(bak.c_str());
        rename(to.c_str(), bak.c_str());                // no-op if `to` is absent

        if (rename(from.c_str(), to.c_str()) != 0) {
            // Put the old one back rather than leaving a hole where an NRO was.
            rename(bak.c_str(), to.c_str());
            *err = "couldn't write " + to;
            removeRecursive(WORK_DIR);
            return false;
        }
        remove(bak.c_str());
        progress("Installing", (int)((++done * 100) / files.size()));
    }

    removeRecursive(WORK_DIR);
    return true;
}
