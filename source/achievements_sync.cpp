#include "achievements_sync.h"

#include <curl/curl.h>
#include <switch.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// The service this is a client of. Overridable on the card so a redeploy does
// not need a new build — see achievements_sync.h.
static const char* API_DEFAULT = "https://exophase-api.aaronworld.uk";
static const char* API_FILE    = "sdmc:/Viridite/achievements_api.txt";
static const char* ACH_DIR     = "sdmc:/Viridite/achievements";

namespace {

struct MemSink { std::string data; size_t cap = 4u * 1024 * 1024; };

size_t writeToMem(void* ptr, size_t sz, size_t nm, void* ud) {
    MemSink* s = (MemSink*)ud;
    const size_t n = sz * nm;
    if (s->data.size() + n > s->cap) return 0;      // refuse to grow without bound
    s->data.append((const char*)ptr, n);
    return n;
}

size_t writeToFile(void* ptr, size_t sz, size_t nm, void* ud) {
    return fwrite(ptr, sz, nm, (FILE*)ud);
}

void commonOpts(CURL* c) {
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Viridite/1.0 (+https://viridite.aaronworld.uk)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
}

bool httpGet(const std::string& url, std::string* out, std::string* err) {
    CURL* c = curl_easy_init();
    if (!c) { *err = "couldn't start curl"; return false; }
    MemSink sink;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToMem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    commonOpts(c);
    const CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) { *err = curl_easy_strerror(rc); return false; }
    if (http == 404) { *err = "no achievement list for this game"; return false; }
    if (http != 200) { *err = "HTTP " + std::to_string(http); return false; }
    *out = std::move(sink.data);
    return true;
}

bool httpGetFile(const std::string& url, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    CURL* c = curl_easy_init();
    if (!c) { fclose(f); return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    commonOpts(c);
    const CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    fclose(f);
    if (rc != CURLE_OK || http != 200) { remove(path.c_str()); return false; }
    return true;
}

// ── Just enough JSON for one known shape ───────────────────────────────────
//
// The same approach update.cpp already takes with GitHub's release payload:
// the response comes from our own service over verified TLS and its shape is
// documented, so a full parser would be a dependency bought for nothing. What
// this must do properly is unescape strings — achievement descriptions are
// free text written by game developers and do contain quotes and backslashes.

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char n = s[++i];
        switch (n) {
            case 'n': out += ' '; break;            // a toast is one line
            case 't': out += ' '; break;            // and must not carry tabs:
            case 'r': break;                        // the store file is TSV
            case 'u': {
                if (i + 4 < s.size()) {
                    const unsigned cp = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) { if (cp != '\t') out += (char)cp; }
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            default: out += n; break;               // \" \\ \/ and anything else
        }
    }
    return out;
}

// Raw (still-escaped) string value of `key` at or after `from`.
std::string jsonStr(const std::string& src, const char* key, size_t from, size_t* end = nullptr) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = src.find(pat, from);
    if (k == std::string::npos) return "";
    size_t c = src.find(':', k + pat.size());
    if (c == std::string::npos) return "";
    // null, not a string
    size_t q = src.find_first_not_of(" \t\n\r", c + 1);
    if (q == std::string::npos || src[q] != '"') { if (end) *end = c; return ""; }
    std::string out;
    for (size_t i = q + 1; i < src.size(); i++) {
        if (src[i] == '\\') { out += src[i]; if (i + 1 < src.size()) out += src[++i]; continue; }
        if (src[i] == '"') { if (end) *end = i; break; }
        out += src[i];
    }
    return out;
}

double jsonNum(const std::string& src, const char* key, size_t from) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = src.find(pat, from);
    if (k == std::string::npos) return 0.0;
    size_t c = src.find(':', k + pat.size());
    if (c == std::string::npos) return 0.0;
    return strtod(src.c_str() + c + 1, nullptr);
}

bool jsonBool(const std::string& src, const char* key, size_t from) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = src.find(pat, from);
    if (k == std::string::npos) return false;
    size_t c = src.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t v = src.find_first_not_of(" \t\n\r", c + 1);
    return v != std::string::npos && src.compare(v, 4, "true") == 0;
}

std::string readTrimmed(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return "";
    char buf[512] = {};
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return ""; }
    fclose(f);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '/'))
        s.pop_back();
    return s;
}

}  // namespace

namespace achsync {

std::string apiBase(void) {
    const std::string s = readTrimmed(API_FILE);
    return s.empty() ? std::string(API_DEFAULT) : s;
}

std::string slugFor(const std::string& appName) {
    std::string out;
    bool lastDash = true;                   // suppress a leading dash
    for (unsigned char ch : appName) {
        if (isalnum(ch)) { out += (char)tolower(ch); lastDash = false; }
        else if (!lastDash) { out += '-'; lastDash = true; }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) return "";
    return out + "-android";
}

std::string slugOverride(const std::string& pkg) {
    FILE* f = fopen("sdmc:/Viridite/achievements/slugs.txt", "r");
    if (!f) return "";
    char line[512];
    std::string found;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        std::string k = line, v = tab + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (k == pkg) { found = v; break; }
    }
    fclose(f);
    return found;
}

SyncResult sync(const std::string& pkg, const std::string& appName, long maxAgeSecs) {
    SyncResult r;
    if (pkg.empty()) { r.error = "no package name"; return r; }

    mkdir("sdmc:/Viridite", 0777);
    mkdir(ACH_DIR, 0777);

    const std::string cat = std::string(ACH_DIR) + "/" + pkg + ".catalogue";

    // Already have it, recently enough. Achievement lists change when a game
    // ships an update, which is not something worth a network round trip on
    // every single launch.
    struct stat st;
    if (maxAgeSecs > 0 && stat(cat.c_str(), &st) == 0 &&
        (long)(time(nullptr) - st.st_mtime) < maxAgeSecs) {
        r.ok = true;
        r.error = "cached";
        return r;
    }

    r.slug = slugOverride(pkg);
    if (r.slug.empty()) r.slug = slugFor(appName);
    if (r.slug.empty()) { r.error = "could not work out an Exophase slug"; return r; }

    const std::string url = apiBase() + "/api/v1/game/" + r.slug + "/achievements";

    std::string body;
    if (!httpGet(url, &body, &r.error)) return r;

    // ── Walk the achievements array ────────────────────────────────────────
    const size_t arr = body.find("\"achievements\"");
    if (arr == std::string::npos) { r.error = "unexpected response from the API"; return r; }

    const std::string tmp = cat + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) { r.error = "cannot write to the card"; return r; }
    fprintf(f, "# index\tname\tdescription\tpoints\trarity\tsecret\ttotalSteps\ticon\n");

    const std::string iconDir = std::string(ACH_DIR) + "/" + pkg;
    mkdir(iconDir.c_str(), 0777);

    size_t pos = arr;
    int    n = 0;
    while (true) {
        // Each element is delimited by its "id" field; the response lists them
        // in order, which is the order the catalogue's own index follows.
        const size_t item = body.find("\"id\"", pos);
        if (item == std::string::npos) break;

        size_t nameEnd = 0;
        const std::string name = unescape(jsonStr(body, "name", item, &nameEnd));
        if (name.empty()) { pos = item + 4; continue; }
        const std::string desc = unescape(jsonStr(body, "description", item));
        const std::string icon = jsonStr(body, "icon_url", item);
        const int   points = (int)jsonNum(body, "points", item);
        const float rarity = (float)jsonNum(body, "rarity_percent", item);
        const bool  secret = jsonBool(body, "secret", item);

        n++;
        std::string iconFile;
        if (!icon.empty()) {
            char nm[32];
            snprintf(nm, sizeof nm, "%d.png", n);
            iconFile = nm;
            const std::string dst = iconDir + "/" + iconFile;
            if (stat(dst.c_str(), &st) != 0 && !httpGetFile(icon, dst)) iconFile.clear();
        }

        // Exophase does not publish a step total, so every achievement is
        // recorded as one-shot. The Core treats totalSteps 0 as "unlocks only
        // when the game says so", which is the safe reading: a stepped
        // achievement then unlocks on the game's own final increment rather
        // than on a threshold we guessed.
        fprintf(f, "%d\t%s\t%s\t%d\t%.2f\t%d\t0\t%s\n",
                n, name.c_str(), desc.c_str(), points, rarity, secret ? 1 : 0,
                iconFile.c_str());

        pos = nameEnd ? nameEnd : item + 4;
    }
    fclose(f);

    if (n == 0) {
        remove(tmp.c_str());
        r.error = "the API returned no achievements";
        return r;
    }
    remove(cat.c_str());
    rename(tmp.c_str(), cat.c_str());

    r.ok = true;
    r.count = n;
    return r;
}

}  // namespace achsync
