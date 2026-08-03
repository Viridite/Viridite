#include "apk.h"
#include <minizip/unzip.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------
static uint16_t r16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// String pool parser — shared by AXML and resources.arsc
// ---------------------------------------------------------------------------
static std::vector<std::string> parseStringPool(const uint8_t* chunk, size_t chunkSize) {
    std::vector<std::string> out;
    if (chunkSize < 28) return out;

    uint32_t count        = r32(chunk + 8);
    uint32_t flags        = r32(chunk + 16);
    uint32_t stringsStart = r32(chunk + 20);
    bool utf8 = (flags & 0x100) != 0;

    const uint8_t* offsets = chunk + 28;
    const uint8_t* strBase = chunk + stringsStart;

    for (uint32_t i = 0; i < count; i++) {
        if (28 + i * 4 + 4 > chunkSize) break;
        uint32_t off = r32(offsets + i * 4);
        const uint8_t* sp = strBase + off;
        if ((size_t)(sp - chunk) >= chunkSize) { out.push_back(""); continue; }

        std::string s;
        if (utf8) {
            uint32_t u = *sp++; if (u & 0x80) { u = ((u & 0x7F) << 8) | *sp++; }
            uint32_t n = *sp++; if (n & 0x80) { n = ((n & 0x7F) << 8) | *sp++; }
            s.assign((const char*)sp, n);
        } else {
            uint32_t len = r16(sp); sp += 2;
            if (len & 0x8000) { len = ((len & 0x7FFF) << 16) | r16(sp); sp += 2; }
            for (uint32_t j = 0; j < len; j++) {
                uint16_t ch = r16(sp + j * 2);
                s += (ch > 0 && ch < 128) ? (char)ch : '?';
            }
        }
        out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// AXML parser
// We only read:
//   <manifest>          → packageName, versionName
//   <application>       → android:label (ref or string), android:icon (ref)
// Depth tracking ensures we never read label from <activity> or other children.
// ---------------------------------------------------------------------------
struct AXMLResult {
    std::string packageName;
    std::string versionName;
    std::string appLabel;   // direct string if dataType==0x03
    uint32_t    labelResId = 0; // resource ref if dataType==0x01
    uint32_t    iconResId  = 0;
    int         screenOrient = -1;  // android:screenOrientation, -1 = unspecified
};

static AXMLResult parseAXML(const std::vector<uint8_t>& data) {
    AXMLResult res;
    const uint8_t* p = data.data();
    size_t sz = data.size();
    if (sz < 8) return res;

    std::vector<std::string> strs;
    int depth = 0;

    size_t pos = 8; // skip outer file chunk header
    while (pos + 8 <= sz) {
        uint16_t type  = r16(p + pos);
        uint32_t csize = r32(p + pos + 4);
        if (csize < 8 || pos + csize > sz) break;

        if (type == 0x0001) {
            strs = parseStringPool(p + pos, csize);

        } else if (type == 0x0102 && !strs.empty()) { // START_ELEMENT
            uint32_t nameIdx   = r32(p + pos + 20);
            uint16_t attrStart = r16(p + pos + 24);
            uint16_t attrSize  = r16(p + pos + 26);
            uint16_t attrCount = r16(p + pos + 28);
            std::string elem   = nameIdx < strs.size() ? strs[nameIdx] : "";

            size_t attrBase = (pos + 16) + attrStart;

            // ── <manifest> at depth 0 ──────────────────────────────────────
            if (depth == 0 && elem == "manifest") {
                for (uint16_t i = 0; i < attrCount; i++) {
                    size_t ap = attrBase + i * attrSize;
                    if (ap + 20 > sz) break;
                    uint32_t an = r32(p + ap + 4);
                    uint8_t  dt = p[ap + 15];
                    uint32_t dv = r32(p + ap + 16);
                    std::string attr = an < strs.size() ? strs[an] : "";
                    if (attr == "package" && dt == 0x03 && dv < strs.size())
                        res.packageName = strs[dv];
                    if (attr == "versionName" && dt == 0x03 && dv < strs.size())
                        res.versionName = strs[dv];
                    // versionName might also be a resource ref (dt==0x01) — ignore for now
                }

            // ── <application> at depth 1 ──────────────────────────────────
            } else if (depth == 2 && elem == "activity" && res.screenOrient < 0) {
                // First activity declared is the launcher one in practice, and
                // its orientation is what governs the game. Only the first is
                // taken so a later settings or splash activity can't override.
                for (uint16_t i = 0; i < attrCount; i++) {
                    size_t ap = attrBase + i * attrSize;
                    if (ap + 20 > sz) break;
                    uint32_t an = r32(p + ap + 4);
                    uint8_t  dt = p[ap + 15];
                    uint32_t dv = r32(p + ap + 16);
                    std::string attr = an < strs.size() ? strs[an] : "";
                    if (attr == "screenOrientation" && dt == 0x10)  // INT_DEC
                        res.screenOrient = (int)(int32_t)dv;
                }

            } else if (depth == 1 && elem == "application") {
                for (uint16_t i = 0; i < attrCount; i++) {
                    size_t ap = attrBase + i * attrSize;
                    if (ap + 20 > sz) break;
                    uint32_t an = r32(p + ap + 4);
                    uint8_t  dt = p[ap + 15];
                    uint32_t dv = r32(p + ap + 16);
                    std::string attr = an < strs.size() ? strs[an] : "";
                    if (attr == "label") {
                        if (dt == 0x03 && dv < strs.size()) res.appLabel  = strs[dv];
                        else if (dt == 0x01)                 res.labelResId = dv;
                    }
                    if (attr == "icon" && dt == 0x01)
                        res.iconResId = dv;
                }
            }
            depth++;

        } else if (type == 0x0103 && !strs.empty()) { // END_ELEMENT
            depth--;
            if (depth == 0) break; // left <manifest>
        }

        pos += csize;
    }
    return res;
}

// ---------------------------------------------------------------------------
// resources.arsc resolver
//
// Given a resource ID 0xPPTTEEEE, returns every string value found across
// all config variants (different densities / languages).
// Works for both string resources (text) and file resources (path strings).
// ---------------------------------------------------------------------------
static std::vector<std::string> resolveResId(
    const std::vector<uint8_t>& arsc, uint32_t resId)
{
    std::vector<std::string> out;
    const uint8_t* p = arsc.data();
    size_t sz = arsc.size();
    if (sz < 12 || r16(p) != 0x0002) return out; // not a resource table

    uint8_t  wantPkg   = (resId >> 24) & 0xFF;
    uint8_t  wantType  = (resId >> 16) & 0xFF;   // 1-based
    uint16_t wantEntry = (uint16_t)(resId & 0xFFFF);

    // File header size tells us where the first inner chunk starts
    size_t pos = r16(p + 2);
    if (pos + 8 > sz) return out;

    // First chunk: global string pool
    if (r16(p + pos) != 0x0001) return out;
    uint32_t gpSize = r32(p + pos + 4);
    if (pos + gpSize > sz) return out;
    auto globalStr = parseStringPool(p + pos, gpSize);
    pos += gpSize;

    // Walk package chunks (type 0x0200)
    while (pos + 8 <= sz) {
        uint16_t ct = r16(p + pos);
        uint32_t cs = r32(p + pos + 4);
        if (cs < 8 || pos + cs > sz) break;

        if (ct != 0x0200) { pos += cs; continue; }

        uint8_t pkgId = (uint8_t)r32(p + pos + 8);
        if (pkgId != wantPkg) { pos += cs; continue; }

        // Inside the package, walk type chunks
        size_t pkgEnd   = pos + cs;
        size_t inner    = pos + r16(p + pos + 2); // skip package header

        while (inner + 8 <= pkgEnd) {
            uint16_t it = r16(p + inner);
            uint32_t is = r32(p + inner + 4);
            if (is < 8 || inner + is > pkgEnd) break;

            if (it == 0x0201) { // ResTable_type
                // inner+8:  id (uint8, 1-based)
                // inner+9:  flags (uint8, 0x01 = sparse)
                // inner+12: entryCount (uint32)
                // inner+16: entriesStart (uint32)
                // inner+2:  headerSize (uint16)
                // inner+20: ResTable_config (first uint32 is its size)
                uint8_t  typeId      = p[inner + 8];
                uint8_t  typeFlags   = p[inner + 9];
                uint32_t entryCount  = r32(p + inner + 12);
                uint32_t entriesOff  = r32(p + inner + 16);
                uint16_t hdrSize     = r16(p + inner + 2);
                bool     sparse      = (typeFlags & 0x01) != 0;

                if (typeId != wantType || entryCount == 0) { inner += is; continue; }

                const uint8_t* offsetArr = p + inner + hdrSize;
                const uint8_t* entryBase = p + inner + entriesOff;

                uint32_t entryOff = 0xFFFFFFFF;
                if (sparse) {
                    // Each pair: { uint16 idx, uint16 offset/4 }
                    int lo = 0, hi = (int)entryCount - 1;
                    while (lo <= hi) {
                        int mid = (lo + hi) / 2;
                        uint16_t midIdx = r16(offsetArr + mid * 4);
                        if (midIdx == wantEntry) {
                            entryOff = (uint32_t)r16(offsetArr + mid * 4 + 2) * 4;
                            break;
                        }
                        if (midIdx < wantEntry) lo = mid + 1; else hi = mid - 1;
                    }
                } else {
                    if (wantEntry < entryCount) {
                        size_t byteOff = (size_t)wantEntry * 4;
                        if (inner + hdrSize + byteOff + 4 <= inner + is)
                            entryOff = r32(offsetArr + byteOff);
                    }
                }

                if (entryOff == 0xFFFFFFFF) { inner += is; continue; }

                const uint8_t* ep = entryBase + entryOff;
                if (ep + 8 > p + inner + is) { inner += is; continue; }

                uint16_t esize  = r16(ep);
                uint16_t eflags = r16(ep + 2);
                if (eflags & 0x0001) { inner += is; continue; } // complex entry

                const uint8_t* vp = ep + esize; // Res_value
                if (vp + 8 > p + inner + is) { inner += is; continue; }

                uint8_t  dataType = vp[3];
                uint32_t data     = r32(vp + 4);

                if (dataType == 0x03 && data < globalStr.size())
                    out.push_back(globalStr[data]);
            }

            inner += is;
        }
        pos += cs;
    }
    return out;
}

// Rank an icon path by the density bucket in its directory name. Higher is
// better; -1 means no recognisable density.
//
// Matched against the directory segment rather than by plain substring search:
// "hdpi" is a substring of "xxxhdpi", so a naive find() ranks every high-density
// icon as if it were the lowest bucket. The old code only got away with that
// because it happened to test the buckets in descending order.
static int densityRank(const std::string& path) {
    static const struct { const char* tag; int rank; } BUCKETS[] = {
        {"-xxxhdpi", 6}, {"-xxhdpi", 5}, {"-xhdpi", 4},
        {"-hdpi",    3}, {"-mdpi",   2}, {"-ldpi",  1},
        // anydpi/nodpi carry no size information of their own, and anydpi is
        // usually an adaptive-icon XML rather than a bitmap, so they rank below
        // any real density bucket but above "no idea".
        {"-anydpi",  0}, {"-nodpi",  0},
    };
    size_t slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? path : path.substr(0, slash);
    for (const auto& b : BUCKETS) {
        size_t at = dir.find(b.tag);
        if (at == std::string::npos) continue;
        // Must end the segment or be followed by a version qualifier (-v4, -v26).
        size_t after = at + strlen(b.tag);
        if (after == dir.size() || dir[after] == '-' || dir[after] == '/')
            return b.rank;
    }
    return -1;
}

// Pixel width straight out of a PNG's IHDR, or 0 if these bytes aren't a PNG.
// Used to compare icons by real resolution instead of trusting the density
// folder they were filed under.
static size_t pngWidth(const std::vector<uint8_t>& data) {
    static const uint8_t SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (data.size() < 24 || memcmp(data.data(), SIG, 8) != 0) return 0;
    if (memcmp(data.data() + 12, "IHDR", 4) != 0) return 0;
    const uint8_t* w = data.data() + 16;             // big-endian u32
    return ((size_t)w[0] << 24) | ((size_t)w[1] << 16) | ((size_t)w[2] << 8) | w[3];
}

// Adaptive-icon XML isn't an image we can decode — skip it rather than hand
// SDL_image a blob of AXML and get a blank tile.
static bool isBitmapIcon(const std::string& path) {
    auto ends = [&](const char* e) {
        size_t n = strlen(e);
        return path.size() >= n && path.compare(path.size() - n, n, e) == 0;
    };
    return ends(".png") || ends(".webp") || ends(".jpg") || ends(".jpeg");
}

// Pick the best icon path from the list returned by resolveResId.
static std::string bestIconPath(const std::vector<std::string>& paths) {
    const std::string* best = nullptr;
    int bestRank = -2;
    for (const auto& s : paths) {
        if (!isBitmapIcon(s)) continue;
        int r = densityRank(s);
        if (r > bestRank) { bestRank = r; best = &s; }
    }
    if (best) return *best;
    // Nothing recognisable — fall back to the first entry so behaviour matches
    // what this did before rather than silently giving up.
    return paths.empty() ? "" : paths[0];
}

// Pick the best label string (prefer non-empty, non-file-path)
static std::string bestLabel(const std::vector<std::string>& vals) {
    for (const auto& s : vals)
        if (!s.empty() && s.rfind("res/", 0) != 0) return s;
    return vals.empty() ? "" : vals[0];
}

// ---------------------------------------------------------------------------
// ZIP helpers
// ---------------------------------------------------------------------------
static std::vector<uint8_t> readZipEntry(unzFile zf, const char* name) {
    if (unzLocateFile(zf, name, 0) != UNZ_OK) return {};
    unz_file_info fi;
    if (unzGetCurrentFileInfo(zf, &fi, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) return {};
    std::vector<uint8_t> buf(fi.uncompressed_size);
    if (unzOpenCurrentFile(zf) != UNZ_OK) return {};
    int n = unzReadCurrentFile(zf, buf.data(), (unsigned)buf.size());
    unzCloseCurrentFile(zf);
    return n < 0 ? std::vector<uint8_t>{} : buf;
}

// ---------------------------------------------------------------------------
// Public API
// Minimal extractor for a flat JSON string field: "key":"value". Enough for a
// XAPK manifest.json (package_name/name/version_name); not a general parser.
static std::string jsonStrField(const std::string& j, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = j.find(needle);
    if (k == std::string::npos) return {};
    size_t colon = j.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    size_t q1 = j.find('"', colon);
    if (q1 == std::string::npos) return {};
    size_t q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return j.substr(q1 + 1, q2 - q1 - 1);
}

// ---------------------------------------------------------------------------
ApkInfo parseApk(const std::string& path) {
    ApkInfo info;
    info.path = path;

    size_t slash = path.rfind('/');
    info.filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;
    // Fallback display name = filename without the extension
    size_t dot = info.filename.rfind('.');
    info.appName = (dot != std::string::npos) ? info.filename.substr(0, dot) : info.filename;

    struct stat st;
    if (stat(path.c_str(), &st) == 0) info.fileSizeBytes = (uint64_t)st.st_size;

    unzFile zf = unzOpen(path.c_str());
    if (!zf) return info;

    // ── XAPK? A ZIP of APKs. Read its manifest.json + icon.png and take arch
    // from which config.<abi> split it carries (real APKs have no .apk members).
    {
        bool isXapk = false, arm64Split = false, arm32Split = false;
        if (unzGoToFirstFile(zf) == UNZ_OK) {
            char name[1024];
            do {
                if (unzGetCurrentFileInfo(zf, nullptr, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK) break;
                std::string n = name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".apk") == 0) {
                    isXapk = true;
                    if (n.find("config.arm64_v8a") != std::string::npos) arm64Split = true;
                    else if (n.find("config.armeabi") != std::string::npos || n.find("config.x86") != std::string::npos) arm32Split = true;
                }
            } while (unzGoToNextFile(zf) == UNZ_OK);
        }
        if (isXapk) {
            auto mj = readZipEntry(zf, "manifest.json");
            std::string j(mj.begin(), mj.end());
            std::string pkg = jsonStrField(j, "package_name");
            std::string nm  = jsonStrField(j, "name");
            std::string ver = jsonStrField(j, "version_name");
            if (!pkg.empty()) info.packageName = pkg;
            if (!nm.empty())  info.appName     = nm;
            if (!ver.empty()) info.versionName = ver;
            auto icon = readZipEntry(zf, "icon.png");
            if (!icon.empty()) info.iconPng = std::move(icon);
            info.arch = arm64Split ? ApkArch::Arm64
                      : arm32Split ? ApkArch::Arm32Only
                      : ApkArch::Unknown;
            unzClose(zf);
            return info;
        }
    }

    // ── Step 0: which native lib ABI(s) does this APK ship? ─────────────
    // Walk every entry once looking for lib/arm64-v8a/ vs any other lib/<abi>/
    // folder — cheap (just filenames, no decompression) and lets the picker
    // tag/gray out APKs that can't run before a full extraction is ever tried.
    {
        bool sawArm64 = false, sawOtherAbi = false;
        if (unzGoToFirstFile(zf) == UNZ_OK) {
            char name[1024];
            do {
                if (unzGetCurrentFileInfo(zf, nullptr, name, sizeof(name),
                                          nullptr, 0, nullptr, 0) != UNZ_OK) break;
                std::string n = name;
                if (n.rfind("lib/arm64-v8a/", 0) == 0) sawArm64 = true;
                else if (n.rfind("lib/", 0) == 0 && n.size() > 4) sawOtherAbi = true;
            } while (unzGoToNextFile(zf) == UNZ_OK);
        }
        info.arch = sawArm64 ? ApkArch::Arm64
                  : sawOtherAbi ? ApkArch::Arm32Only
                  : ApkArch::Unknown;
    }

    // ── Step 1: parse AndroidManifest.xml ───────────────────────────────
    auto manifest = readZipEntry(zf, "AndroidManifest.xml");
    AXMLResult ax;
    if (!manifest.empty()) ax = parseAXML(manifest);
    info.screenOrient = ax.screenOrient;

    // A bundled icon wins over the one in the APK.
    //
    // Store listings carry a clean, full-resolution icon; what ships inside the
    // APK is whatever the build packed, which for a resource-obfuscated game
    // like Hill Climb Racing is a small, arbitrarily-chosen bitmap among
    // several. Where we have the real artwork, use it — this is the same
    // artwork the boot animation uses, so the launcher and the loading screen
    // finally show the same thing.
    if (!ax.packageName.empty()) {
        std::string bundled = "romfs:/gameicons/" + ax.packageName + ".png";
        if (FILE* bf = fopen(bundled.c_str(), "rb")) {
            fseek(bf, 0, SEEK_END);
            long n = ftell(bf);
            fseek(bf, 0, SEEK_SET);
            if (n > 0) {
                std::vector<uint8_t> buf((size_t)n);
                if (fread(buf.data(), 1, (size_t)n, bf) == (size_t)n) {
                    info.iconPng = std::move(buf);
                    info.hasIcon = true;
                }
            }
            fclose(bf);
        }
    }

    if (!ax.packageName.empty()) info.packageName = ax.packageName;
    if (!ax.versionName.empty()) info.versionName = ax.versionName;
    if (!ax.appLabel.empty())    info.appName     = ax.appLabel;

    // ── Step 2: resolve resource refs via resources.arsc ────────────────
    if (ax.labelResId || ax.iconResId) {
        auto arsc = readZipEntry(zf, "resources.arsc");
        if (!arsc.empty()) {
            if (ax.labelResId && info.appName == info.filename.substr(0, info.filename.size() - 4)) {
                auto vals = resolveResId(arsc, ax.labelResId);
                std::string label = bestLabel(vals);
                if (!label.empty()) info.appName = label;
            }
            if (ax.iconResId) {
                auto paths = resolveResId(arsc, ax.iconResId);
                // Read every candidate and keep the one with the most pixels.
                //
                // Density folders are only a hint, and plenty of APKs don't
                // have them at all: Hill Climb Racing ships resource-obfuscated
                // (res/as.png, res/uS.png …), so every candidate ranked equally
                // and whichever came first won — which is how a small icon got
                // picked while a 432x432 one sat right beside it. Measuring the
                // decoded width is the only thing that actually answers "which
                // of these is the best icon".
                std::string bestPath;
                std::vector<uint8_t> bestData;
                size_t bestPx = 0;
                int    bestRank = -2;
                for (const auto& cand : paths) {
                    if (!isBitmapIcon(cand)) continue;
                    auto data = readZipEntry(zf, cand.c_str());
                    if (data.empty()) continue;
                    size_t px = pngWidth(data);         // 0 when not a PNG (WebP)
                    int    rk = densityRank(cand);
                    bool better;
                    if (px && bestPx)        better = px > bestPx;
                    else if (px && !bestPx)  better = true;      // measured beats guessed
                    else if (!px && bestPx)  better = false;
                    else                     better = rk > bestRank;
                    if (better || bestData.empty()) {
                        bestData = std::move(data);
                        bestPath = cand; bestPx = px; bestRank = rk;
                    }
                }
                if (!bestData.empty()) { info.iconPng = std::move(bestData); info.hasIcon = true; }
                else {
                    // Nothing decoded — fall back to the old path-based pick.
                    std::string iconPath = bestIconPath(paths);
                    if (!iconPath.empty())
                        info.iconPng = readZipEntry(zf, iconPath.c_str());
                        info.hasIcon = !info.iconPng.empty();
                }
            }
        }
    }

    // ── Step 3: icon fallback if resources.arsc didn't give us one ──────
    if (info.iconPng.empty()) {
        static const char* CANDIDATES[] = {
            "res/mipmap-xxxhdpi-v4/ic_launcher.png",
            "res/mipmap-xxhdpi-v4/ic_launcher.png",
            "res/mipmap-xhdpi-v4/ic_launcher.png",
            "res/mipmap-hdpi-v4/ic_launcher.png",
            "res/mipmap-xxxhdpi/ic_launcher.png",
            "res/mipmap-xxhdpi/ic_launcher.png",
            "res/mipmap-xhdpi/ic_launcher.png",
            "res/mipmap-hdpi/ic_launcher.png",
            "res/drawable-xxxhdpi/ic_launcher.png",
            "res/drawable-xxhdpi/ic_launcher.png",
            "res/drawable/ic_launcher.png",
            // WebP variants — common for modern app icons
            "res/mipmap-xxxhdpi-v4/ic_launcher.webp",
            "res/mipmap-xxhdpi-v4/ic_launcher.webp",
            "res/mipmap-xhdpi-v4/ic_launcher.webp",
            "res/mipmap-hdpi-v4/ic_launcher.webp",
            "res/mipmap-xxxhdpi/ic_launcher.webp",
            "res/mipmap-xxhdpi/ic_launcher.webp",
            nullptr
        };
        // Take the highest-resolution candidate that's actually present rather
        // than the first one that happens to exist. Density folder names are
        // only a hint — plenty of APKs ship a "xxxhdpi" icon that's smaller
        // than their "xhdpi" one — so where the bytes decode as PNG, compare
        // the real pixel width from the IHDR and keep the biggest.
        size_t bestPixels = 0;
        int    bestRank   = -2;
        for (int i = 0; CANDIDATES[i]; i++) {
            auto icon = readZipEntry(zf, CANDIDATES[i]);
            if (icon.empty()) continue;

            size_t px = pngWidth(icon);              // 0 if not a PNG (e.g. WebP)
            int    rk = densityRank(CANDIDATES[i]);

            bool better;
            if (px && bestPixels)      better = px > bestPixels;   // both measurable
            else if (px && !bestPixels) better = true;             // measurable beats guessed
            else if (!px && bestPixels) better = false;
            else                        better = rk > bestRank;    // neither: trust density

            if (better || info.iconPng.empty()) {
                info.iconPng = std::move(icon);
                bestPixels   = px;
                bestRank     = rk;
            }
        }
    }

    unzClose(zf);
    return info;
}

bool apkIsInstalled(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;
    std::string marker = std::string("sdmc:/Viridite/games/") + pkg_name + "/.installed";
    struct stat st;
    return stat(marker.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// Per-APK settings + delete
// ---------------------------------------------------------------------------
int apkGetFpsCap(const std::string& pkg_name) {
    if (pkg_name.empty()) return 0;
    std::string path = std::string("sdmc:/Viridite/games/") + pkg_name + "/.fps_cap";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int fps = 0;
    if (fscanf(f, "%d", &fps) != 1) fps = 0;
    fclose(f);
    return fps;
}

bool apkSetFpsCap(const std::string& pkg_name, int fps) {
    if (pkg_name.empty()) return false;
    std::string dir  = std::string("sdmc:/Viridite/games/") + pkg_name;
    std::string path = dir + "/.fps_cap";
    // The games/<pkg> dir may not exist yet if this APK has never been
    // launched — create it so the choice survives until first install
    // (launchApk's own mkdirp on the Core side is idempotent either way).
    mkdir(dir.c_str(), 0777);
    if (fps <= 0) { remove(path.c_str()); return true; }
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "%d", fps);
    fclose(f);
    return true;
}

// Recursively removes a file or directory. Used for wiping an extracted
// install (games/<pkg>/), which can contain a real directory tree (lib/,
// assets/, save data) rather than the single flat files the rest of this
// launcher deals with.
static bool removeRecursive(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return true; // already gone
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

bool apkDeleteInstalledData(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;
    return removeRecursive(std::string("sdmc:/Viridite/games/") + pkg_name);
}

// Android's Storage settings split, applied to how a game actually sits on the
// SD card:
//
//   sdmc:/Viridite/games/<pkg>/lib      extracted .so   — re-extractable
//   sdmc:/Viridite/games/<pkg>/assets   extracted files — re-extractable
//   sdmc:/Viridite/games/<pkg>/userdefaults.bin         — the actual save
//   sdmc:/Viridite/games/<pkg>/.installed, .fps_cap     — markers/settings
//
// Clear cache drops only what the Core can rebuild from the APK, so progress
// survives; clear storage drops the lot, which is the "start again from
// nothing" option.
bool apkClearCache(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;
    std::string base = std::string("sdmc:/Viridite/games/") + pkg_name;
    struct stat st;
    if (stat(base.c_str(), &st) != 0) return false;         // nothing installed

    bool ok = removeRecursive(base + "/lib");
    ok = removeRecursive(base + "/assets") && ok;
    // Drop the marker too, so the next launch re-extracts instead of trusting
    // a tree we just emptied.
    remove((base + "/.installed").c_str());
    return ok;
}

// Everything for this package, saves included — same as apkDeleteInstalledData
// today, but named for what it means at the call site so the two buttons don't
// read as the same operation.
bool apkClearStorage(const std::string& pkg_name) {
    return apkDeleteInstalledData(pkg_name);
}

// Bytes currently on the card for this package, split the same way, so the UI
// can show what each button would actually reclaim.
//
// ONE pass over the tree, classifying as it goes. The first version called a
// recursive size helper three times — once for lib/, once for assets/, then
// once for the whole package directory, which walked lib/ and assets/ again —
// and ran it synchronously when the Manage screen opened. Over a real
// extracted game that's thousands of files read off the SD card with nothing
// rendering, which Horizon shows as a frozen app.
static void walkUsage(const std::string& path, bool underCache,
                      uint64_t* cache, uint64_t* data) {
    DIR* d = opendir(path.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string n = ent->d_name;
        if (n == "." || n == "..") continue;
        std::string full = path + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walkUsage(full, underCache || n == "lib" || n == "assets", cache, data);
        } else {
            *(underCache ? cache : data) += (uint64_t)st.st_size;
        }
    }
    closedir(d);
}

void apkGetStorageUsage(const std::string& pkg_name, uint64_t* cacheBytes, uint64_t* dataBytes) {
    uint64_t cache = 0, data = 0;
    if (!pkg_name.empty())
        walkUsage(std::string("sdmc:/Viridite/games/") + pkg_name, false, &cache, &data);
    if (cacheBytes) *cacheBytes = cache;
    if (dataBytes)  *dataBytes  = data;
}

bool apkDeleteFile(const std::string& apk_path) {
    if (apk_path.empty()) return false;
    return remove(apk_path.c_str()) == 0;
}

std::vector<ApkInfo> scanApks(const std::string& dir) {
    std::vector<ApkInfo> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;
    struct dirent* ent;
    auto hasExt = [](const std::string& s, const char* ext) {
        size_t n = strlen(ext);
        return s.size() > n && s.compare(s.size() - n, n, ext) == 0;
    };
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (hasExt(name, ".apk") || hasExt(name, ".xapk")) {
            ApkInfo info = parseApk(dir + "/" + name);
            const std::string& pkg = info.packageName.empty() ? info.filename : info.packageName;
            info.installed = apkIsInstalled(pkg);
            result.push_back(std::move(info));
        }
    }
    closedir(d);
    std::sort(result.begin(), result.end(),
        [](const ApkInfo& a, const ApkInfo& b) { return a.appName < b.appName; });
    return result;
}
