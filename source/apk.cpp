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

// Pick the best icon path from the list returned by resolveResId
static std::string bestIconPath(const std::vector<std::string>& paths) {
    static const char* densities[] = {
        "xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", nullptr
    };
    for (int d = 0; densities[d]; d++)
        for (const auto& s : paths)
            if (s.find(densities[d]) != std::string::npos) return s;
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
// ---------------------------------------------------------------------------
ApkInfo parseApk(const std::string& path) {
    ApkInfo info;
    info.path = path;

    size_t slash = path.rfind('/');
    info.filename = (slash != std::string::npos) ? path.substr(slash + 1) : path;
    // Fallback display name = filename without .apk
    info.appName = info.filename.size() > 4
        ? info.filename.substr(0, info.filename.size() - 4)
        : info.filename;

    struct stat st;
    if (stat(path.c_str(), &st) == 0) info.fileSizeBytes = (uint64_t)st.st_size;

    unzFile zf = unzOpen(path.c_str());
    if (!zf) return info;

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
                std::string iconPath = bestIconPath(paths);
                if (!iconPath.empty())
                    info.iconPng = readZipEntry(zf, iconPath.c_str());
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
        for (int i = 0; CANDIDATES[i]; i++) {
            auto icon = readZipEntry(zf, CANDIDATES[i]);
            if (!icon.empty()) { info.iconPng = std::move(icon); break; }
        }
    }

    unzClose(zf);
    return info;
}

bool apkIsInstalled(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;
    std::string marker = std::string("sdmc:/AndroidHorizonNX/games/") + pkg_name + "/.installed";
    struct stat st;
    return stat(marker.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// Per-APK settings + delete
// ---------------------------------------------------------------------------
int apkGetFpsCap(const std::string& pkg_name) {
    if (pkg_name.empty()) return 0;
    std::string path = std::string("sdmc:/AndroidHorizonNX/games/") + pkg_name + "/.fps_cap";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int fps = 0;
    if (fscanf(f, "%d", &fps) != 1) fps = 0;
    fclose(f);
    return fps;
}

bool apkSetFpsCap(const std::string& pkg_name, int fps) {
    if (pkg_name.empty()) return false;
    std::string dir  = std::string("sdmc:/AndroidHorizonNX/games/") + pkg_name;
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
    return removeRecursive(std::string("sdmc:/AndroidHorizonNX/games/") + pkg_name);
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
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".apk") == 0) {
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
