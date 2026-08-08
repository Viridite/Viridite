#include "forwarder.h"
#include "jpegenc.h"
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

// A buffer that owns malloc'd memory and reports failure instead of dying.
//
// The launcher is built with -fno-exceptions, so a std::vector that cannot get
// its memory does not throw something catchable — it calls std::terminate, and
// the console shows "The software has closed because an error occurred" with
// nothing written to the log. This runs at startup, before the menu exists, on
// buffers of a couple of hundred kilobytes. It has to be allowed to fail.
struct Buf {
    uint8_t* p = nullptr;
    size_t   n = 0;
    ~Buf() { free(p); }
    bool alloc(size_t bytes) {
        free(p);
        p = (uint8_t*)malloc(bytes);
        n = p ? bytes : 0;
        return p != nullptr;
    }
    Buf() = default;
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
};

bool readFile(const char* path, Buf& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (8 << 20) || !out.alloc((size_t)n)) { fclose(f); return false; }
    const bool ok = fread(out.p, 1, (size_t)n, f) == (size_t)n;
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
// Packages whose icon encode has already taken the process down once.
const char* kNoIcon = "sdmc:/Viridite/.forwarder_noicon";

bool buildIconJpeg(const ApkInfo& apk, Buf& out) {
    SDL_Surface* src = nullptr;

    std::string bundled = "romfs:/gameicons/" + apk.packageName + ".png";
    logMsg(("forwarder:   icon: loading " + bundled).c_str());
    src = IMG_Load(bundled.c_str());
    if (!src && !apk.iconPng.empty()) {
        logMsg("forwarder:   icon: no bundled icon, using the APK's");
        SDL_RWops* rw = SDL_RWFromConstMem(apk.iconPng.data(), (int)apk.iconPng.size());
        if (rw) src = IMG_Load_RW(rw, 1);
    }
    if (!src) { logMsg("forwarder:   icon: nothing loadable"); return false; }

    logMsg("forwarder:   icon: scaling to 256x256");
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, 256, 256, 24, SDL_PIXELFORMAT_RGB24);
    if (!dst) { SDL_FreeSurface(src); return false; }
    // JPEG has no alpha, so a transparent icon would otherwise come out over
    // uninitialised memory. Fill with the Viridite green the rest of the UI
    // uses, which reads as deliberate behind a rounded icon.
    SDL_FillRect(dst, nullptr, SDL_MapRGB(dst->format, 0, 200, 83));
    SDL_BlitScaled(src, nullptr, dst, nullptr);
    SDL_FreeSurface(src);

    // Into memory, not a file: the icon only exists to be concatenated. A fixed
    // 256 KB clears the worst case with room to spare — 256x256 at quality 90
    // measures ~150 KB even for incompressible colour noise, and a real icon is
    // a tenth of that — and the encoder stops at the limit instead of running
    // past it, so an image that somehow needed more fails the save rather than
    // walking off the end of the buffer.
    if (!out.alloc(256 * 1024)) { SDL_FreeSurface(dst); return false; }
    logMsg("forwarder:   icon: encoding JPEG");
    // Our own encoder, not IMG_SaveJPG_RW. Three hardware logs ended inside
    // that call, on three unrelated games, and the run that finally reported
    // IMG_Init said jpg=1 — so JPEG was up and the fault is inside libjpeg,
    // not in initialisation and not in the images. There is nothing to
    // configure our way out of, so the dependency is gone instead.
    const size_t n = jpegEncodeRGB((const uint8_t*)dst->pixels, dst->w, dst->h,
                                   dst->pitch, 90, out.p, out.n);
    const int iw = dst->w, ih = dst->h;
    SDL_FreeSurface(dst);
    if (n == 0) {
        // No SDL call is involved any more, so SDL_GetError() here would print
        // whatever unrelated thing last failed — worse than saying nothing.
        // The encoder returns 0 for exactly two reasons, and both are sizes.
        char m[160];
        snprintf(m, sizeof m,
                 "forwarder:   icon: JPEG encode failed (%dx%d would not fit %zu KB)",
                 iw, ih, out.n / 1024);
        logMsg(m);
        return false;
    }
    logMsg("forwarder:   icon: encoded");

    out.n = (size_t)n;                    // keep the allocation, shrink the length
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
    logMsg(("forwarder: building for " + apk.packageName).c_str());

    Buf stub;
    if (!readFile(kTemplate, stub) || stub.n < 0x20 ||
        memcmp(stub.p + 0x10, "NRO0", 4) != 0) {
        logMsg("forwarder: romfs:/forwarder.nro missing or not an NRO");
        return false;
    }

    // The stub is shipped without assets, so its own header size is the whole
    // file. Trim anything past it regardless, so re-stamping an already
    // stamped binary can never append a second asset section.
    uint32_t nro_size = 0;
    memcpy(&nro_size, stub.p + 0x18, 4);
    if (nro_size == 0 || nro_size > stub.n) {
        logMsg("forwarder: template NRO has a bad size field");
        return false;
    }
    stub.n = nro_size;
    logMsg("forwarder:   stub read");

    // ── NACP ──
    // Built through libnx's own struct so the field offsets are whatever the
    // running SDK says they are, not numbers copied out of a wiki page.
    static NacpStruct nacp;
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

    logMsg("forwarder:   nacp built");

    // ── icon ──
    Buf icon;
    const bool haveIcon = buildIconJpeg(apk, icon);
    if (!haveIcon) logMsg("forwarder: no icon available — writing without one");

    // ── assemble ──
    AssetHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(&hdr.magic, "ASET", 4);
    hdr.version = 0;

    uint64_t off = sizeof hdr;
    if (haveIcon) { hdr.icon.offset = off; hdr.icon.size = icon.n; off += icon.n; }
    hdr.nacp.offset = off; hdr.nacp.size = sizeof nacp; off += sizeof nacp;
    hdr.romfs.offset = off; hdr.romfs.size = 0;

    mkdir("sdmc:/switch", 0777);
    mkdir(kDir, 0777);

    const std::string path = forwarderPath(apk.packageName);
    const std::string tmp  = path + ".tmp";

    logMsg(("forwarder:   writing " + tmp).c_str());
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        logMsg(("forwarder: cannot write " + tmp).c_str());
        return false;
    }
    bool ok = fwrite(stub.p, 1, stub.n, f) == stub.n;
    ok = ok && fwrite(&hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (haveIcon) ok = ok && fwrite(icon.p, 1, icon.n, f) == icon.n;
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

void forwarderRetryBlockedIcons(void) {
    // Everything the old blocklist named was written without a picture,
    // because encoding one used to take the process down. It cannot any more,
    // so delete those forwarders and let the next pass build them properly —
    // otherwise they keep their missing icons forever, since a forwarder is
    // only written when one is absent.
    FILE* f = fopen(kNoIcon, "r");
    if (!f) return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
        if (!line[0]) continue;
        if (remove(forwarderPath(line).c_str()) == 0) n++;
    }
    fclose(f);
    remove(kNoIcon);
    if (n > 0) {
        char m[128];
        snprintf(m, sizeof m, "forwarders: %d written without an icon by an older "
                              "build — rebuilding them", n);
        logMsg(m);
    }
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
