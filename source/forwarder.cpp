#include "forwarder.h"
#include "apk.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <switch.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

// The launcher's log, so a forwarder that fails to write says why.
void logMsg(const char* msg);

namespace {

const char* kDir      = "sdmc:/switch/Viridite Games";
const char* kTemplate = "romfs:/forwarder.nro";

// The asset blob appended after an NRO. hbmenu reads this to find the icon,
// name, author and version; offsets are relative to this header.
struct AssetHeader {
    uint32_t magic;                       // 'ASET'
    uint32_t version;                     // 0
    struct { uint64_t offset, size; } icon, nacp, romfs;
};
static_assert(sizeof(AssetHeader) == 0x38, "NRO asset header is 56 bytes");

bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    const bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

// Title-case one reverse-DNS component: "fingersoft" -> "Fingersoft".
std::string prettyComponent(const std::string& s) {
    std::string out = s;
    if (!out.empty()) out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

// Render the game's icon at 256x256 as JPEG, which is the only format the
// HOME menu and hbmenu accept for an NRO.
//
// Prefers the icon Viridite ships for the game over the one inside the APK.
// Store icons are authored at a single size with a consistent style; the ones
// packed into an APK are whatever density the phone build happened to need,
// and vary wildly in padding and shape between games.
bool buildIconJpeg(const ApkInfo& apk, std::vector<uint8_t>& out) {
    SDL_Surface* src = nullptr;

    std::string bundled = "romfs:/gameicons/" + apk.packageName + ".png";
    src = IMG_Load(bundled.c_str());
    if (!src && !apk.iconPng.empty()) {
        SDL_RWops* rw = SDL_RWFromConstMem(apk.iconPng.data(), (int)apk.iconPng.size());
        if (rw) src = IMG_Load_RW(rw, 1);
    }
    if (!src) return false;

    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, 256, 256, 32, SDL_PIXELFORMAT_RGB24);
    if (!dst) { SDL_FreeSurface(src); return false; }
    // JPEG has no alpha, so a transparent icon would otherwise come out over
    // uninitialised memory. Fill with the Viridite green the rest of the UI
    // uses, which reads as deliberate behind a rounded icon.
    SDL_FillRect(dst, nullptr, SDL_MapRGB(dst->format, 0, 200, 83));
    SDL_BlitScaled(src, nullptr, dst, nullptr);
    SDL_FreeSurface(src);

    // Into memory, not a file: the icon only exists to be concatenated.
    std::vector<uint8_t> buf(256 * 1024);
    SDL_RWops* rw = SDL_RWFromMem(buf.data(), (int)buf.size());
    if (!rw) { SDL_FreeSurface(dst); return false; }
    const int rc = IMG_SaveJPG_RW(dst, rw, 0, 90);
    const Sint64 n = SDL_RWtell(rw);
    SDL_RWclose(rw);
    SDL_FreeSurface(dst);
    if (rc != 0 || n <= 0) return false;

    out.assign(buf.begin(), buf.begin() + (size_t)n);
    return true;
}

}  // namespace

std::string forwarderPath(const std::string& pkg_name) {
    return std::string(kDir) + "/" + pkg_name + ".nro";
}

std::string forwarderAuthor(const std::string& pkg_name) {
    // Reverse-DNS packages put the vendor second: com.fingersoft.hillclimb.
    // Not universal, but it is right for every game Viridite runs, and a wrong
    // guess here shows as a slightly odd author line rather than a failure.
    std::string dev;
    size_t a = pkg_name.find('.');
    if (a != std::string::npos) {
        size_t b = pkg_name.find('.', a + 1);
        dev = prettyComponent(pkg_name.substr(a + 1, b == std::string::npos
                                                     ? std::string::npos : b - a - 1));
    }
    if (dev.empty()) return "Viridite Contributors";
    return dev + " | Viridite Contributors";
}

bool forwarderWrite(const ApkInfo& apk) {
    if (apk.packageName.empty()) return false;

    std::vector<uint8_t> stub;
    if (!readFile(kTemplate, stub) || stub.size() < 0x20 ||
        memcmp(stub.data() + 0x10, "NRO0", 4) != 0) {
        logMsg("forwarder: romfs:/forwarder.nro missing or not an NRO");
        return false;
    }

    // The stub is shipped without assets, so its own header size is the whole
    // file. Trim anything past it regardless, so re-stamping an already
    // stamped binary can never append a second asset section.
    uint32_t nro_size = 0;
    memcpy(&nro_size, stub.data() + 0x18, 4);
    if (nro_size == 0 || nro_size > stub.size()) {
        logMsg("forwarder: template NRO has a bad size field");
        return false;
    }
    stub.resize(nro_size);

    // ── NACP ──
    // Built through libnx's own struct so the field offsets are whatever the
    // running SDK says they are, not numbers copied out of a wiki page.
    NacpStruct nacp;
    memset(&nacp, 0, sizeof nacp);

    const std::string title  = apk.appName.empty() ? apk.packageName : apk.appName;
    const std::string author = forwarderAuthor(apk.packageName);
    const std::string ver    = apk.versionName.empty() ? "1.0.0" : apk.versionName;

    // Every language, so the entry reads correctly whatever the console is set
    // to rather than falling back to a blank name.
    for (int i = 0; i < 16; i++) {
        snprintf(nacp.lang[i].name,   sizeof nacp.lang[i].name,   "%s", title.c_str());
        snprintf(nacp.lang[i].author, sizeof nacp.lang[i].author, "%s", author.c_str());
    }
    snprintf(nacp.display_version, sizeof nacp.display_version, "%s", ver.c_str());

    // ── icon ──
    std::vector<uint8_t> icon;
    const bool haveIcon = buildIconJpeg(apk, icon);
    if (!haveIcon) logMsg("forwarder: no icon available — writing without one");

    // ── assemble ──
    AssetHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(&hdr.magic, "ASET", 4);
    hdr.version = 0;

    uint64_t off = sizeof hdr;
    if (haveIcon) { hdr.icon.offset = off; hdr.icon.size = icon.size(); off += icon.size(); }
    hdr.nacp.offset = off; hdr.nacp.size = sizeof nacp; off += sizeof nacp;
    hdr.romfs.offset = off; hdr.romfs.size = 0;

    mkdir("sdmc:/switch", 0777);
    mkdir(kDir, 0777);

    const std::string path = forwarderPath(apk.packageName);
    const std::string tmp  = path + ".tmp";

    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        logMsg(("forwarder: cannot write " + tmp).c_str());
        return false;
    }
    bool ok = fwrite(stub.data(), 1, stub.size(), f) == stub.size();
    ok = ok && fwrite(&hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (haveIcon) ok = ok && fwrite(icon.data(), 1, icon.size(), f) == icon.size();
    ok = ok && fwrite(&nacp, 1, sizeof nacp, f) == sizeof nacp;
    fclose(f);

    if (!ok) {
        remove(tmp.c_str());
        logMsg("forwarder: write failed — SD card full?");
        return false;
    }

    // Rename over the old one, so an interrupted write can never leave a
    // half-built NRO sitting in the menu waiting to be launched.
    remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        logMsg("forwarder: could not replace the existing forwarder");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof msg, "forwarder: wrote %s (%s, v%s)",
             path.c_str(), author.c_str(), ver.c_str());
    logMsg(msg);
    return true;
}

bool forwarderRemove(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;
    const std::string path = forwarderPath(pkg_name);
    remove(path.c_str());

    struct stat st;
    const bool gone = stat(path.c_str(), &st) != 0;
    if (gone) {
        char msg[256];
        snprintf(msg, sizeof msg, "forwarder: removed %s", path.c_str());
        logMsg(msg);
    }
    return gone;
}
