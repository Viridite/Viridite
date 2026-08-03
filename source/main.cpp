#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

#include <curl/curl.h>

#include "m3_theme.h"
#include "apk.h"
#include "avatar.h"
#include "build_number.h"
#include "update.h"
#include "selftest.h"

static const char* APK_DIR = "sdmc:/Viridite/apks";

// Translation Core NROs live in a subfolder next to this launcher — same
// convention as any other homebrew "app + resources" layout.
static const char* CORE_X64_PATH = "sdmc:/switch/Viridite/Viridite-Translation-Core-x64.nro";
// Nothing chain-loads this any more: allowlisted 32-bit titles go through the
// x64 Core's ARM32 layer (see launchGame), so every launch targets x64. The
// x32 NRO is still built and shipped to complete the on-disk layout, so the
// path stays here as the record of where it lands — marked maybe_unused
// rather than deleted, since dropping it would lose that and leave the
// shipped file undocumented in the launcher that defines the layout.
[[maybe_unused]] static const char* CORE_X32_PATH = "sdmc:/switch/Viridite/Viridite-Translation-Core-x32.nro";

// Compatibility allowlist. The translation layer is validated end-to-end for
// exactly these titles right now; every other APK boots far enough to look like
// it might work and then fails in game-specific ways, so we mark them
// incompatible up front rather than let someone chase a crash we already know
// about. Keyed by package id.
// Games whose own code has a controller path we drive (see the controller
// natives the Core resolves). Only these get asked how they should be
// launched — for anything else the question has one answer, so asking would
// just be a step in the way.
// What the console actually has attached right now. The picker offers these
// rather than a fixed list, so nobody is asked to choose a Pro Controller
// they haven't got — and the guide the game shows can match the thing in
// their hands instead of a generic pad.
enum class PadKind { None, Pro, Handheld, JoyDual, JoyLeft, JoyRight };

static const char* padKindId(PadKind k) {
    switch (k) {
        case PadKind::Pro:      return "pro";
        case PadKind::Handheld: return "handheld";
        case PadKind::JoyDual:  return "joycon_dual";
        case PadKind::JoyLeft:  return "joycon_left";
        case PadKind::JoyRight: return "joycon_right";
        default:                return "touch";
    }
}
static const char* padKindIcon(PadKind k) {
    switch (k) {
        case PadKind::Pro:      return "romfs:/controllers/ctrl_pro.png";
        case PadKind::Handheld: return "romfs:/controllers/ctrl_handheld.png";
        case PadKind::JoyDual:  return "romfs:/controllers/ctrl_joycon_dual.png";
        case PadKind::JoyLeft:  return "romfs:/controllers/ctrl_joycon_left.png";
        case PadKind::JoyRight: return "romfs:/controllers/ctrl_joycon_right.png";
        default:                return "romfs:/controllers/ctrl_touch.png";
    }
}
static const char* padKindName(PadKind k) {
    switch (k) {
        case PadKind::Pro:      return "Pro Controller";
        case PadKind::Handheld: return "Handheld (Joy-Cons attached)";
        case PadKind::JoyDual:  return "Joy-Cons (detached pair)";
        case PadKind::JoyLeft:  return "Left Joy-Con (sideways)";
        case PadKind::JoyRight: return "Right Joy-Con (sideways)";
        default:                return "Touch screen";
    }
}

// Distinct controllers currently connected, best-fit first. Handheld and a
// paired set report separately, so both can be offered when both exist.
static std::vector<PadKind> detectPads() {
    std::vector<PadKind> out;
    auto add = [&](PadKind k) {
        if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    };
    hidInitializeNpad();
    u32 hh = hidGetNpadStyleSet(HidNpadIdType_Handheld);
    if (hh & HidNpadStyleTag_NpadHandheld) add(PadKind::Handheld);
    // Players 1-8; in practice only the first couple matter, but a pad paired
    // to a later slot is still a pad someone can play with.
    for (int i = 0; i < 8; i++) {
        u32 st = hidGetNpadStyleSet((HidNpadIdType)(HidNpadIdType_No1 + i));
        if (st & HidNpadStyleTag_NpadFullKey)  add(PadKind::Pro);
        if (st & HidNpadStyleTag_NpadHandheld) add(PadKind::Handheld);
        if (st & HidNpadStyleTag_NpadJoyDual)  add(PadKind::JoyDual);
        if (st & HidNpadStyleTag_NpadJoyLeft)  add(PadKind::JoyLeft);
        if (st & HidNpadStyleTag_NpadJoyRight) add(PadKind::JoyRight);
    }
    return out;
}

static bool hasControllerSupport(const std::string& pkg) {
    return pkg == "com.fingersoft.hillclimb";
}

static bool isCompatibleGame(const std::string& pkg) {
    return pkg == "com.fingersoft.hillclimb"   // Hill Climb Racing (cocos2d-x)
        || pkg == "com.fingersoft.hcr2"        // Hill Climb Racing 2 (cocos2d-x, arm32)
        || pkg == "com.orbital.brainiton";     // Brain It On! (Unity IL2CPP)
}

// ---------------------------------------------------------------------------
// Layout (1280×720) — matches the Translation Core's own UI exactly so the
// chain-load between the two feels like a single continuous app.
// ---------------------------------------------------------------------------
static const int SW       = 1280;
static const int SH       = 720;
static const int HEADER_H = 72;
static const int FOOTER_H = 48;
static const int LIST_Y   = HEADER_H;
static const int LIST_H   = SH - HEADER_H - FOOTER_H;
static const int ITEM_H   = 108;
static const int ICON_SZ  = 84;
static const int VISIBLE  = LIST_H / ITEM_H;

// ─── Theme ──────────────────────────────────────────────────────────────────
// These are Material 3 colour roles under their old names, not fixed colours.
// Keeping the names means every existing call site keeps reading the way it
// did — C_WHITE for headings, C_GRAY for sub-text — while what they resolve to
// now comes from the active scheme, so switching theme re-colours the whole UI
// without touching a single drawing call.
//
// The mapping follows the spec rather than taste: list selection is
// secondaryContainer because that is what M3 uses for a selected list item,
// dividers are outlineVariant, the app bar is surfaceContainerLow. Getting
// these right is what makes it read as Android rather than as green paint on
// the old design.
static SDL_Color C_BG, C_HEADER, C_FOOTER, C_SEL, C_DIV, C_WHITE, C_GRAY,
                 C_DIM, C_OK, C_ERR, C_WARN, C_INST, C_RIM,
                 C_ON_PRIMARY, C_PRIMARY_CONTAINER, C_ON_PRIMARY_CONTAINER,
                 C_SURF_HIGH, C_SURF_HIGHEST, C_ERR_CONTAINER, C_ON_ERR_CONTAINER;

static int      g_themeIndex = 0;
static const M3Scheme* g_theme = &M3_THEMES[0];

static void themeApply(int idx) {
    if (idx < 0 || idx >= M3_THEME_COUNT) idx = 0;
    g_themeIndex = idx;
    const M3Scheme& m = M3_THEMES[idx];
    g_theme  = &m;
    C_BG     = m.surface;
    C_HEADER = m.surfaceContainerLow;
    C_FOOTER = m.surfaceContainer;
    C_SEL    = m.secondaryContainer;      // M3 selected list item
    C_DIV    = m.outlineVariant;
    C_WHITE  = m.onSurface;               // primary text
    C_GRAY   = m.onSurfaceVariant;        // secondary text
    C_DIM    = m.outline;                 // tertiary text
    C_OK     = m.primary;
    C_ERR    = m.error;
    C_WARN   = m.tertiary;
    C_INST   = m.primary;
    C_RIM    = m.primary;
    C_ON_PRIMARY           = m.onPrimary;
    C_PRIMARY_CONTAINER    = m.primaryContainer;
    C_ON_PRIMARY_CONTAINER = m.onPrimaryContainer;
    C_SURF_HIGH            = m.surfaceContainerHigh;
    C_SURF_HIGHEST         = m.surfaceContainerHighest;
    C_ERR_CONTAINER        = m.errorContainer;
    C_ON_ERR_CONTAINER     = m.onErrorContainer;
}

// Remembered between runs, next to the other settings on the card.
static const char* THEME_PATH = "sdmc:/Viridite/theme.txt";
static void themeLoad() {
    int idx = 0;
    if (FILE* f = fopen(THEME_PATH, "r")) { if (fscanf(f, "%d", &idx) != 1) idx = 0; fclose(f); }
    themeApply(idx);
}
static void themeSave() {
    if (FILE* f = fopen(THEME_PATH, "w")) { fprintf(f, "%d\n", g_themeIndex); fclose(f); }
}

// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void logOpen()  { g_log = fopen("sdmc:/Viridite/launcher_log.txt", "w"); }
static void logClose() { if (g_log) { fclose(g_log); g_log = nullptr; } }
static void logMsg(const char* msg) {
    if (g_log) { fputs(msg, g_log); fputc('\n', g_log); fflush(g_log); }
}
static void logSDL(const char* prefix) {
    if (!g_log) return;
    fputs(prefix, g_log); fputs(": ", g_log);
    fputs(SDL_GetError(), g_log); fputc('\n', g_log); fflush(g_log);
}

// SDL exposes the Switch pad in libnx's HidNpadButton order.
static const int BTN_A      = 0;
static const int BTN_B      = 1;
static const int BTN_X      = 2;
static const int BTN_Y      = 3;
static const int BTN_LSTICK = 4;
static const int BTN_RSTICK = 5;
static const int BTN_L      = 6;
static const int BTN_R      = 7;
static const int BTN_ZL     = 8;
static const int BTN_ZR     = 9;
static const int BTN_PLUS   = 10;
static const int BTN_MINUS  = 11;
// Depending on SDL build the D-pad arrives as these buttons, as a hat, or both,
// so everything below accepts all three rather than assuming one.
static const int BTN_DLEFT  = 12;
static const int BTN_DUP    = 13;
static const int BTN_DRIGHT = 14;
static const int BTN_DDOWN  = 15;

// Stick axes: 0/1 left, 2/3 right. Only axis 1 used to be read, so the right
// stick and all horizontal movement did nothing at all.
static const int AXIS_LX = 0, AXIS_LY = 1, AXIS_RX = 2, AXIS_RY = 3;
static const int AXIS_DEADZONE = 16384;

// One logical action per input, so a handset button, the D-pad, either stick,
// a shoulder button and a keyboard key can all mean the same thing without
// every screen re-deriving that mapping.
enum class Act {
    None, Up, Down, PageUp, PageDown, Home, End,
    Confirm, Back, Manage, Reinstall, Rescan, About, SelfTest, Theme, Quit
};

// Buttons and keys. Stick/hat motion is continuous and rate-limited, so it's
// handled separately by the caller rather than folded in here.
static Act actionFor(const SDL_Event& ev) {
    if (ev.type == SDL_JOYBUTTONDOWN) {
        switch (ev.jbutton.button) {
            case BTN_A:      return Act::Confirm;
            case BTN_B:      return Act::Manage;
            case BTN_X:      return Act::Reinstall;
            // BTN_Y is handled by the main loop: tap rescans, hold opens
            // the self-test, and only a release can tell them apart.
            case BTN_MINUS:  return Act::About;
            case BTN_PLUS:   return Act::Quit;
            case BTN_DUP:    return Act::Up;
            case BTN_DDOWN:  return Act::Down;
            case BTN_ZL:     return Act::Theme;
            case BTN_L:
            case BTN_DLEFT:  return Act::PageUp;
            case BTN_R:
            case BTN_ZR:
            case BTN_DRIGHT: return Act::PageDown;
            case BTN_LSTICK:
            case BTN_RSTICK: return Act::Home;
            default:         return Act::None;
        }
    }
    if (ev.type == SDL_KEYDOWN) {
        switch (ev.key.keysym.sym) {
            case SDLK_UP:                       return Act::Up;
            case SDLK_DOWN:                     return Act::Down;
            case SDLK_PAGEUP:   case SDLK_LEFT: return Act::PageUp;
            case SDLK_PAGEDOWN: case SDLK_RIGHT:return Act::PageDown;
            case SDLK_HOME:                     return Act::Home;
            case SDLK_END:                      return Act::End;
            case SDLK_RETURN:   case SDLK_SPACE:return Act::Confirm;
            case SDLK_BACKSPACE:                return Act::Manage;
            case SDLK_r:                        return Act::Rescan;
            case SDLK_t:                        return Act::SelfTest;
            case SDLK_p:                        return Act::Theme;
            case SDLK_i:                        return Act::About;
            case SDLK_ESCAPE:   case SDLK_q:    return Act::Quit;
            default:                            return Act::None;
        }
    }
    return Act::None;
}

// romfs:/contributors.txt is (re)generated by the release workflow from the
// live contributor list of every repo in the org, so it's baked fresh into
// each release build rather than hand-maintained here. Format is plain text
// so this doesn't need to pull in a JSON dependency:
//   #Category title
//   username:contributions
//   username:contributions
//   #Next category title
//   ...
// One entry per HUMAN, not per category. contributors.txt is grouped by area
// (Launcher / Translation Core / Website / Testers), so someone who worked
// across several of them used to be listed once per group — with a single
// active contributor that made the whole credits reel the same name repeated.
// Roles are collected onto the person instead.
struct Person {
    std::string name;
    int contributions = 0;
    std::vector<std::string> roles;
    SDL_Texture* avatar = nullptr;
};

// ---------------------------------------------------------------------------
struct App {
    SDL_Window*    win  = nullptr;
    SDL_Renderer*  rdr  = nullptr;
    TTF_Font*      fLg  = nullptr;
    TTF_Font*      fMd  = nullptr;
    TTF_Font*      fSm  = nullptr;
    SDL_Joystick*  joy  = nullptr;

    std::vector<ApkInfo>      apks;
    std::vector<SDL_Texture*> icons;
    int selected = 0;
    int scroll   = 0;

    SDL_Texture* avatarTex = nullptr;

    SDL_Texture* bgTex = nullptr;
    TTF_Font*    fBtn  = nullptr;
    struct Star { float x, y; int sz; float phase, speed; };
    std::vector<Star> stars;
    float selAnimY = -1.0f;

    Uint32 noticeUntil = 0;
    std::string noticeText;

    // ── Self-update ───────────────────────────────────────────────────
    bool       netReady    = false;   // socket + curl came up
    bool       updateSeen  = false;   // the check's result has been consumed
    UpdateInfo update;                // valid once updateSeen

    // ── Touch navigation ──────────────────────────────────────────────
    // Footer button hitboxes, refreshed every drawFooterBar() call so a
    // tap can be matched back to whichever hint (by index, same order the
    // caller passed in) was drawn at that screen position — layout is
    // computed dynamically (text-width dependent), so this is the only
    // reliable way to hit-test it rather than duplicating that math here.
    std::vector<SDL_Rect> footerHitboxes;
    // Drag state for swipe-to-scroll: only start scrolling once the finger
    // has moved past a small threshold, so a tap-to-launch/select doesn't
    // register as an accidental scroll from natural finger jitter.
    bool  touchDragging   = false;
    float touchStartX     = 0.0f;
    float touchStartY     = 0.0f;
    float touchScrollAccY = 0.0f;

    // Credits (About screen), loaded once from romfs at startup.
    std::vector<Person> people;
    std::string curTitle;                 // category being parsed
    std::vector<SDL_Texture*> contributorAvatarTextures; // owns every unique avatar texture, for cleanup()
    float  creditsScroll     = 0.0f;
    Uint32 lastCreditsStick  = 0;
    Uint32 lastCreditsInput  = 0; // last manual scroll input; auto-scroll only kicks in once idle

    // ------------------------------------------------------------------
    // plGetSharedFontByType hands back a ready-to-use TTF — the pl service has
    // already decoded it. Pass address/size straight through, exactly as
    // devkitPro's own shared_font example feeds FT_New_Memory_Face.
    //
    // This was wrong twice. It originally skipped 8 bytes (fd.address + 8,
    // fd.size - 8), which corrupted the font header, so TTF_OpenFontRW failed
    // on every launch and the UI silently ran on the romfs DejaVuSans fallback
    // — which has no console button glyphs, hence "NintendoExt font
    // unavailable — text hints" and A/B rendering as plain letters. It was then
    // "fixed" by XOR-decoding it as BFTTF, which is not what this API returns
    // and failed identically on hardware.
    TTF_Font* openSharedFont(PlSharedFontType type, int ptsize) {
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, type) != 0) return nullptr;
        if (!fd.address || fd.size == 0) return nullptr;
        SDL_RWops* rw = SDL_RWFromConstMem(fd.address, (int)fd.size);
        if (!rw) return nullptr;
        return TTF_OpenFontRW(rw, 1, ptsize);
    }

    TTF_Font* openFont(int ptsize) {
        // Roboto first. The Switch's own font is a fine UI face but it is
        // unmistakably Nintendo's, and this is supposed to read as Android —
        // typography does more of that work than colour does. Roboto Flex is
        // Google's, shipped under the OFL, and bundled rather than fetched.
        romfsInit();
        TTF_Font* f = TTF_OpenFont("romfs:/fonts/RobotoFlex.ttf", ptsize);
        if (f) { logMsg("  font: Roboto Flex (Google Fonts)"); return f; }
        logSDL("  Roboto open failed");

        // Then the system font, which at least has the full CJK coverage that
        // a bundled Latin face does not.
        plInitialize(PlServiceType_User);
        f = openSharedFont(PlSharedFontType_Standard, ptsize);
        if (f) { logMsg("  font: system BFTTF"); return f; }
        logSDL("  BFTTF open failed");

        f = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", ptsize);
        if (f) { logMsg("  font: romfs DejaVuSans"); return f; }
        logSDL("  romfs font open failed");
        return nullptr;
    }

    TTF_Font* openExtFont(int ptsize) {
        TTF_Font* f = openSharedFont(PlSharedFontType_NintendoExt, ptsize);
        if (f) { logMsg("  font: NintendoExt glyphs"); return f; }
        logMsg("  NintendoExt font unavailable — text hints");
        return nullptr;
    }

    // ------------------------------------------------------------------
    bool init() {
        // One-time migration for installs from before the Viridite rename: the
        // data folder (apks, extracted games, saves, logs) used to live at
        // sdmc:/AndroidHorizonNX. If that exists and the new path doesn't yet,
        // move it wholesale so nobody loses their installed games on upgrade.
        {
            struct stat st;
            if (stat("sdmc:/Viridite", &st) != 0 && stat("sdmc:/AndroidHorizonNX", &st) == 0)
                rename("sdmc:/AndroidHorizonNX", "sdmc:/Viridite");
        }
        mkdir("sdmc:/Viridite", 0777);
        logOpen();
        logMsg("Viridite launcher starting");

        // Networking, for the self-update check. curl on this toolchain runs
        // TLS through the Switch's own ssl sysmodule, so it needs the socket
        // driver up but no CA bundle of our own. A console with no network
        // just makes the check fail quietly — it never blocks the launcher.
        socketInitializeDefault();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        netReady = true;
        updateCheckStart();
        // Logged on the way IN, not just on the way out. The check waits up to
        // 10s for the console to associate with a network before it can say
        // anything, and a log dumped inside that window used to end at "init
        // complete" — indistinguishable from the launcher having hung.
        logMsg("update check: started (waiting for network, up to 10s)");

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            logSDL("SDL_Init failed"); logClose(); return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP) == 0)
            logSDL("IMG_Init warning");
        if (TTF_Init() != 0) {
            logSDL("TTF_Init failed"); logClose(); return false;
        }

        win = SDL_CreateWindow("Viridite",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            SW, SH, SDL_WINDOW_SHOWN);
        if (!win) { logSDL("CreateWindow failed"); logClose(); return false; }

        rdr = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!rdr) {
            logSDL("Accelerated renderer failed, trying software");
            rdr = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!rdr) { logSDL("CreateRenderer failed"); logClose(); return false; }

        SDL_SetRenderDrawBlendMode(rdr, SDL_BLENDMODE_BLEND);

        fLg = openFont(28);
        fMd = openFont(20);
        fSm = openFont(17);
        if (!fLg || !fSm) { logMsg("Font load failed"); logClose(); return false; }
        if (!fMd) fMd = fSm;
        fBtn = openExtFont(22);

        buildBackground();

        if (SDL_NumJoysticks() > 0) {
            joy = SDL_JoystickOpen(0);
            if (!joy) logSDL("JoystickOpen warning");
        }
        avatarStart();
        loadContributors();
        logMsg("init complete");
        return true;
    }

    // Parses romfs:/contributors.txt (see the format comment near the
    // Contributor struct above). Missing file just means an empty credits
    // list — e.g. a local dev build made without running the CI step that
    // generates it — not a fatal error.
    void loadContributors() {
        people.clear();
        curTitle.clear();
        FILE* f = fopen("romfs:/contributors.txt", "r");
        if (!f) { logMsg("contributors.txt not present — About screen credits will be empty"); return; }
        auto addPerson = [&](const std::string& n, const std::string& role, int contrib) {
            std::string key = n; std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            for (auto& pr : people) {
                std::string k2 = pr.name; std::transform(k2.begin(), k2.end(), k2.begin(), ::tolower);
                if (k2 == key) {
                    pr.contributions += contrib;
                    if (std::find(pr.roles.begin(), pr.roles.end(), role) == pr.roles.end())
                        pr.roles.push_back(role);
                    return;
                }
            }
            Person np; np.name = n; np.contributions = contrib; np.roles.push_back(role);
            people.push_back(np);
        };
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            if (s[0] == '#') {
                curTitle = s.substr(1);
            } else if (!curTitle.empty()) {
                size_t p = s.find(':');
                std::string name = (p == std::string::npos) ? s : s.substr(0, p);
                int  n = (p == std::string::npos) ? 0 : atoi(s.substr(p + 1).c_str());
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
                while (!name.empty() && (name.back()  == ' ' || name.back()  == '\t')) name.pop_back();
                if (name.empty()) continue;
                addPerson(name, curTitle, n);
            }
        }
        fclose(f);

        // Most commits first; people credited only for testing (no commits)
        // sort after, alphabetically.
        std::sort(people.begin(), people.end(), [](const Person& a, const Person& b) {
            if (a.contributions != b.contributions) return a.contributions > b.contributions;
            std::string la = a.name, lb = b.name;
            std::transform(la.begin(), la.end(), la.begin(), ::tolower);
            std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
            return la < lb;
        });

        logMsg(("contributors.txt loaded: " + std::to_string(people.size()) +
                " people").c_str());
        loadContributorAvatars();
    }

    // CI bakes one romfs:/avatars/<name>.png per unique contributor/tester
    // into the NRO alongside contributors.txt (see release.yml) — no network
    // fetch at runtime, same "bundled, not fetched" philosophy as the single
    // About-screen avatar already used. A name can appear in more than one
    // category (e.g. a coder who also tested a game), so textures are loaded
    // once per unique name and shared by reference across rows.
    void loadContributorAvatars() {
        for (auto* t : contributorAvatarTextures) if (t) SDL_DestroyTexture(t);
        contributorAvatarTextures.clear();
        std::map<std::string, SDL_Texture*> byName;
        for (auto& p : people) {
            auto it = byName.find(p.name);
            if (it != byName.end()) { p.avatar = it->second; continue; }
            std::string path = "romfs:/avatars/" + p.name + ".png";
            SDL_Surface* surf = IMG_Load(path.c_str());
            SDL_Texture* tex = surf ? SDL_CreateTextureFromSurface(rdr, surf) : nullptr;
            if (surf) SDL_FreeSurface(surf);
            byName[p.name] = tex;
            if (tex) contributorAvatarTextures.push_back(tex);
            p.avatar = tex;
        }
    }

    void cleanup() {
        avatarStop();
        if (avatarTex) SDL_DestroyTexture(avatarTex);
        if (bgTex) SDL_DestroyTexture(bgTex);
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        for (auto* t : contributorAvatarTextures) if (t) SDL_DestroyTexture(t);
        if (fBtn) TTF_CloseFont(fBtn);
        if (fLg)  TTF_CloseFont(fLg);
        if (fMd && fMd != fSm) TTF_CloseFont(fMd);
        if (fSm)  TTF_CloseFont(fSm);
        if (joy)  SDL_JoystickClose(joy);
        if (rdr)  SDL_DestroyRenderer(rdr);
        if (win)  SDL_DestroyWindow(win);
        romfsExit(); plExit();
        TTF_Quit(); IMG_Quit(); SDL_Quit();
        if (netReady) {
            updateCheckJoin();          // must outlive curl_global_cleanup()
            curl_global_cleanup();
            socketExit();
        }
        logMsg("cleanup done");
        logClose();
    }

    // ------------------------------------------------------------------
    void fill(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(rdr, &r);
    }

    // ── Material 3 shape ────────────────────────────────────────────────────
    // Corner radius is the single most recognisable thing about the Material
    // You look — the spec's scale is XS 4, S 8, M 12, L 16, XL 24, and M3_FULL
    // for the pill shapes buttons and chips use. Everything the UI draws goes
    // through here rather than SDL_RenderFillRect so nothing is left square by
    // accident.
    void fillRounded(int x, int y, int w, int h, int rad, SDL_Color c) {
        if (w <= 0 || h <= 0) return;
        if (rad == M3_FULL) rad = (w < h ? w : h) / 2;
        if (rad * 2 > w) rad = w / 2;
        if (rad * 2 > h) rad = h / 2;
        if (rad <= 0) { fill(x, y, w, h, c); return; }
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        fill(x + rad, y, w - 2 * rad, h, c);
        fill(x, y + rad, rad, h - 2 * rad, c);
        fill(x + w - rad, y + rad, rad, h - 2 * rad, c);
        for (int dy = 0; dy < rad; dy++) {
            float fy = rad - dy - 0.5f;
            int   dx = (int)(rad - sqrtf((float)rad * rad - fy * fy) + 0.5f);
            int   run = w - dx * 2;
            if (run <= 0) continue;
            fill(x + dx, y + dy,           run, 1, c);
            fill(x + dx, y + h - 1 - dy,   run, 1, c);
        }
    }

    // Paint the surface colour back over an icon's corners. Clipping to a
    // rounded shape would need a render target per icon per frame; painting
    // four corners costs nothing and looks the same over a flat background,
    // which is what a list row is.
    void roundIconCorners(int x, int y, int sz, int rad) {
        SDL_Color bg = C_BG;
        for (int dy = 0; dy < rad; dy++) {
            float fy = rad - dy - 0.5f;
            int   dx = (int)(rad - sqrtf((float)rad * rad - fy * fy) + 0.5f);
            if (dx <= 0) continue;
            fill(x,            y + dy,          dx, 1, bg);
            fill(x + sz - dx,  y + dy,          dx, 1, bg);
            fill(x,            y + sz - 1 - dy, dx, 1, bg);
            fill(x + sz - dx,  y + sz - 1 - dy, dx, 1, bg);
        }
    }

    // 1dp outline in the same shape — M3 outlined cards and buttons.
    void strokeRounded(int x, int y, int w, int h, int rad, SDL_Color c) {
        fillRounded(x, y, w, h, rad, c);
        SDL_Color inner = C_BG;
        fillRounded(x + 1, y + 1, w - 2, h - 2, rad > 0 ? rad - 1 : 0, inner);
    }

    // ── Material Symbols ────────────────────────────────────────────────────
    // Rendered from Google's icon font at build time and tinted here, which is
    // how one white glyph serves an enabled row, a disabled one and an error
    // state without three copies of it.
    std::map<std::string, SDL_Texture*> iconCache;

    SDL_Texture* icon(const char* name) {
        auto it = iconCache.find(name);
        if (it != iconCache.end()) return it->second;
        std::string path = std::string("romfs:/icons/") + name + ".png";
        SDL_Texture* t = IMG_LoadTexture(rdr, path.c_str());
        if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        iconCache.emplace(name, t);
        return t;
    }

    void drawIcon(const char* name, int x, int y, int size, SDL_Color tint) {
        SDL_Texture* t = icon(name);
        if (!t) return;
        SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(t, tint.a);
        SDL_Rect d = {x, y, size, size};
        SDL_RenderCopy(rdr, t, nullptr, &d);
    }

    int drawText(TTF_Font* f, const std::string& s, SDL_Color col, int x, int y) {
        if (s.empty() || !f) return 0;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
        if (!surf) return 0;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(rdr, surf);
        int w = surf->w;
        SDL_FreeSurface(surf);
        if (!tex) return 0;
        int tw, th;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(rdr, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        return w;
    }

    static SDL_Color lerpCol(SDL_Color a, SDL_Color b, float t) {
        return { (Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t),
                 (Uint8)(a.b + (b.b - a.b) * t), 255 };
    }

    void fillCircle(int cx, int cy, int r, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        for (int dy = -r; dy <= r; dy++) {
            int hw = (int)sqrtf((float)(r * r - dy * dy));
            SDL_Rect row = {cx - hw, cy + dy, hw * 2, 1};
            SDL_RenderFillRect(rdr, &row);
        }
    }

    static constexpr float PLANET_R    = 2200.0f;
    static constexpr int   PLANET_BUMP = 130;

    void buildBackground() {
        // Light theme: no dark space scene. A scatter of faint green motes
        // drifting over the near-white background gives a little life without
        // the old navy sky. bgTex stays null so drawBackground fills C_BG.
        stars.clear();
        uint32_t rng = 0x5EED5EED;
        auto rnd = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int i = 0; i < 60; i++) {
            Star s;
            s.x     = (float)(rnd() % SW);
            s.y     = (float)(rnd() % SH);
            s.sz    = (rnd() % 100 < 22) ? 3 : 2;
            s.phase = (rnd() % 628) / 100.0f;
            s.speed = 0.25f + (rnd() % 100) / 120.0f;
            stars.push_back(s);
        }
        // No background.svg on the light theme — a flat near-white ground reads
        // cleaner and lets the vivid-green accents do the work.
    }

    void drawBackground() {
        Uint32 now = SDL_GetTicks();
        fill(0, 0, SW, SH, C_BG);
        for (auto& s : stars) {
            s.x -= 0.02f * s.speed;
            if (s.x < 0) s.x += SW;
            float tw = 0.5f + 0.5f * sinf(now / 1000.0f * s.speed * 6.2832f + s.phase);
            Uint8 a  = (Uint8)(12 + 24 * tw);   // very faint green motes on white
            fill((int)s.x, (int)s.y, s.sz, s.sz, {0, 190, 110, a});
        }
    }

    void drawHeaderBar(const std::string& rightText = "") {
        fill(0, 0, SW, HEADER_H, {255, 255, 255, 235});
        // M3 top app bars sit on a container tone and separate by colour, not
        // by a rule underneath.
        int w = drawText(fLg, "Virid", C_WHITE, 30, (HEADER_H - 28) / 2);
        w += drawText(fLg, "ite", C_OK, 30 + w, (HEADER_H - 28) / 2);
        drawText(fSm, BUILD_VERSION, C_DIM, 30 + w + 14, (HEADER_H + 4) / 2);
        if (!rightText.empty()) {
            int tw = 0, th = 0;
            TTF_SizeUTF8(fSm, rightText.c_str(), &tw, &th);
            drawText(fSm, rightText, C_DIM, SW - tw - 30, (HEADER_H - 18) / 2);
        }
    }

    void drawFooterBar(const std::vector<std::pair<std::string, std::string>>& hints,
                       const std::string& leftText = "") {
        fill(0, SH - FOOTER_H, SW, FOOTER_H, {242, 248, 245, 235});
        fill(0, SH - FOOTER_H, SW, 2, C_RIM);
        int cy = SH - FOOTER_H / 2;
        if (!leftText.empty())
            drawText(fSm, leftText, C_WARN, 30, cy - 9);
        footerHitboxes.assign(hints.size(), SDL_Rect{0, 0, 0, 0});
        int x = SW - 30;
        size_t idx = hints.size();
        for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
            --idx;
            int right = x;
            int lw = 0, lh = 0;
            TTF_SizeUTF8(fSm, it->second.c_str(), &lw, &lh);
            x -= lw;
            drawText(fSm, it->second, C_GRAY, x, cy - lh / 2);
            x -= 8;
            if (fBtn && it->first.size() > 1) {
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fBtn, it->first.c_str(), &gw, &gh);
                x -= gw;
                drawText(fBtn, it->first, C_WHITE, x, cy - gh / 2);
            } else {
                x -= 26;
                fillCircle(x + 13, cy, 13, {205, 244, 224, 255});
                std::string letter = it->first.size() > 1 ? "?" : it->first;
                int gw = 0, gh = 0;
                TTF_SizeUTF8(fSm, letter.c_str(), &gw, &gh);
                drawText(fSm, letter, C_WHITE, x + 13 - gw / 2, cy - gh / 2);
            }
            // Hitbox covers the whole hint (glyph+label), padded a bit
            // vertically/horizontally so small touch inaccuracy still hits it.
            footerHitboxes[idx] = SDL_Rect{x - 10, SH - FOOTER_H, right - x + 20, FOOTER_H};
            x -= 34;
        }
    }

    // Returns the index into the last drawFooterBar() hints vector that
    // contains (px,py), or -1 if none. Lets touch taps trigger the same
    // action as the corresponding button glyph.
    int hitTestFooter(int px, int py) const {
        for (size_t i = 0; i < footerHitboxes.size(); i++) {
            const SDL_Rect& r = footerHitboxes[i];
            if (r.w <= 0) continue;
            if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) return (int)i;
        }
        return -1;
    }

    // Renders the credits list (contributors, grouped by category/repo)
    // starting at screen-y `top`, clipped to the region above the footer and
    // scrolled by creditsScroll — clipping (rather than skipping off-screen
    // rows) keeps this simple since category headers and person rows have
    // different heights.
    void drawContributors(int top) {
        if (people.empty()) return;
        const int PERSON_H = 46;
        const int GAP_H    = 16;
        const int AV_SZ    = 28;

        int total = 0;
        total = (int)people.size() * PERSON_H + GAP_H;

        int viewH = (SH - FOOTER_H - 10) - top;
        int maxScroll = std::max(0, total - viewH);
        // Wrap rather than clamp — lets the auto-scroll (and a manual scroll
        // run off either end) loop back around like a real credits reel
        // instead of getting stuck at the top/bottom.
        if (maxScroll <= 0)            creditsScroll = 0.0f;
        else if (creditsScroll > (float)maxScroll) creditsScroll = 0.0f;
        else if (creditsScroll < 0.0f)             creditsScroll = (float)maxScroll;

        SDL_Rect clip = {0, top, SW, viewH};
        SDL_RenderSetClipRect(rdr, &clip);

        int y = top - (int)creditsScroll;
        int cx = SW / 2 - 220;
        for (auto& p : people) {
            if (y + PERSON_H >= top && y < top + viewH) {
                int avY = y + (PERSON_H - AV_SZ) / 2;
                if (p.avatar) {
                    SDL_Rect dst = {cx, avY, AV_SZ, AV_SZ};
                    SDL_RenderCopy(rdr, p.avatar, nullptr, &dst);
                } else {
                    drawMonogram(p.name, cx, avY, AV_SZ);
                }
                drawText(fSm, p.name, C_WHITE, cx + AV_SZ + 12, y + 3);

                // Which areas they worked on, in place of the old per-category
                // headings — the same person no longer needs a row each.
                std::string roles;
                for (size_t i = 0; i < p.roles.size(); i++)
                    roles += (i ? " · " : "") + p.roles[i];
                if (!roles.empty())
                    drawText(fSm, roles, C_DIM, cx + AV_SZ + 12, y + 3 + 18);

                if (p.contributions > 0) {
                    std::string cnt = std::to_string(p.contributions) +
                                      (p.contributions == 1 ? " commit" : " commits");
                    int cw = 0, ch = 0;
                    TTF_SizeUTF8(fSm, cnt.c_str(), &cw, &ch);
                    drawText(fSm, cnt, C_DIM, cx + 440 - cw, y + 3);
                }
            }
            y += PERSON_H;
        }

        SDL_RenderSetClipRect(rdr, nullptr);

        if (maxScroll > 0) {
            int barH = viewH * viewH / total;
            int barY = top + viewH * (int)creditsScroll / total;
            fill(SW - 6, barY, 6, barH, {150, 195, 172, 200});
        }
    }

    static constexpr const char* GLYPH_A     = "\xEE\x83\xA0";
    static constexpr const char* GLYPH_B     = "\xEE\x83\xA1";
    static constexpr const char* GLYPH_X     = "\xEE\x83\xA2";
    static constexpr const char* GLYPH_Y     = "\xEE\x83\xA3";
    static constexpr const char* GLYPH_PLUS  = "\xEE\x83\xAF";
    static constexpr const char* GLYPH_MINUS = "\xEE\x83\xB0";

    std::string BG(const char* glyph, const char* letter) const {
        return fBtn ? glyph : letter;
    }

    static std::string formatSize(uint64_t bytes) {
        char buf[32];
        if (bytes >= 1024ull * 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024ull * 1024)
            snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
        else if (bytes >= 1024ull)
            snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
        return buf;
    }

    void drawMonogram(const std::string& name, int x, int y, int sz) {
        static const SDL_Color PALETTE[] = {
            {239, 83,  80,  255}, {171, 71,  188, 255}, {66,  165, 245, 255},
            {38,  166, 154, 255}, {255, 167, 38,  255}, {126, 87,  194, 255},
            {92,  107, 192, 255}, {255, 112, 67,  255},
        };
        uint32_t h = 2166136261u;
        for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
        SDL_Color bg = PALETTE[h % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
        fill(x, y, sz, sz, bg);
        char letter = name.empty() ? '?' : (char)toupper((unsigned char)name[0]);
        std::string s(1, letter);
        int w = 0, h2 = 0;
        TTF_SizeUTF8(fLg, s.c_str(), &w, &h2);
        drawText(fLg, s, C_WHITE, x + (sz - w) / 2, y + (sz - h2) / 2);
    }

    std::string clamp(TTF_Font* f, const std::string& s, int maxW) {
        int w = 0, h = 0;
        TTF_SizeUTF8(f, s.c_str(), &w, &h);
        if (w <= maxW) return s;
        std::string t = s;
        while (!t.empty()) {
            t.pop_back();
            std::string try_ = t + "...";
            TTF_SizeUTF8(f, try_.c_str(), &w, &h);
            if (w <= maxW) return try_;
        }
        return "...";
    }

    // ------------------------------------------------------------------
    void loadIcons() {
        icons.assign(apks.size(), nullptr);
        for (size_t i = 0; i < apks.size(); i++) {
            if (apks[i].iconPng.empty()) continue;
            SDL_RWops* rw = SDL_RWFromConstMem(
                apks[i].iconPng.data(), (int)apks[i].iconPng.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) continue;
            icons[i] = SDL_CreateTextureFromSurface(rdr, surf);
            SDL_FreeSurface(surf);
            apks[i].iconPng.clear();
        }
    }

    void rescan() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        icons.clear();
        apks = ::scanApks(APK_DIR);
        loadIcons();
        selected = 0; scroll = 0;
    }

    // ------------------------------------------------------------------
    void render() {
        Uint32 now = SDL_GetTicks();
        drawBackground();

        if (apks.empty()) {
            drawText(fSm,
                "No APKs found — place .apk files in sdmc:/Viridite/apks/",
                C_GRAY, 30, LIST_Y + 30);
        } else {
            int targetY = LIST_Y + (selected - scroll) * ITEM_H;
            if (selAnimY < 0) selAnimY = (float)targetY;
            selAnimY += (targetY - selAnimY) * 0.35f;
            if (fabsf(selAnimY - targetY) < 0.5f) selAnimY = (float)targetY;
            {
                int cy2 = (int)selAnimY;
                // M3 selected list item: a filled secondaryContainer shape with
                // a large corner radius. The old treatment was a hard-edged box
                // with an animated green glow around it, which is a games-console
                // idiom — Android marks selection by filling the row, not by
                // outlining and pulsing it.
                fillRounded(12, cy2 + 4, SW - 24, ITEM_H - 8, M3_L, C_SEL);
            }

            int end = std::min((int)apks.size(), scroll + VISIBLE);
            for (int i = scroll; i < end; i++) {
                int iy = LIST_Y + (i - scroll) * ITEM_H;
                // A selected row is already a filled shape, so a divider under
                // it would cut through the fill.
                if (i != selected)
                    fill(28, iy + ITEM_H - 1, SW - 56, 1, C_DIV);

                int iconY = iy + (ITEM_H - ICON_SZ) / 2;
                if (i < (int)icons.size() && icons[i]) {
                    // Android clips launcher icons to a rounded shape rather
                    // than showing the raw square the APK ships.
                    SDL_Rect dst = {28, iconY, ICON_SZ, ICON_SZ};
                    SDL_RenderCopy(rdr, icons[i], nullptr, &dst);
                    roundIconCorners(28, iconY, ICON_SZ, M3_L);
                } else {
                    drawMonogram(apks[i].appName, 28, iconY, ICON_SZ);
                }

                const std::string& rowPkg =
                    apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName;
                bool rowCompatible = isCompatibleGame(rowPkg);

                int tx   = 28 + ICON_SZ + 16;
                int maxW = SW - tx - 40;
                SDL_Color nameCol = (apks[i].arch == ApkArch::Arm32Only || !rowCompatible)
                                    ? C_DIM : C_WHITE;
                drawText(fLg, clamp(fLg, apks[i].appName, maxW), nameCol, tx, iy + 14);

                if (apks[i].arch == ApkArch::Arm32Only) {
                    static const std::string TAG = "32-BIT — UNSUPPORTED";
                    int bw = 0, bh = 0;
                    TTF_SizeUTF8(fSm, TAG.c_str(), &bw, &bh);
                    int bx = SW - bw - 40;
                    fill(bx - 6, iy + 14, bw + 12, bh, {255, 238, 210, 220});
                    drawText(fSm, TAG, C_WARN, bx, iy + 14);
                } else if (!rowCompatible) {
                    static const std::string TAG = "INCOMPATIBLE";
                    int bw = 0, bh = 0;
                    TTF_SizeUTF8(fSm, TAG.c_str(), &bw, &bh);
                    int bx = SW - bw - 40;
                    fill(bx - 6, iy + 14, bw + 12, bh, {253, 226, 226, 220});
                    drawText(fSm, TAG, C_ERR, bx, iy + 14);
                } else if (apks[i].installed) {
                    static const std::string INST = "INSTALLED";
                    int bw = 0, bh = 0;
                    TTF_SizeUTF8(fSm, INST.c_str(), &bw, &bh);
                    int bx = SW - bw - 40;
                    fill(bx - 6, iy + 14, bw + 12, bh, {205, 244, 224, 220});
                    drawText(fSm, INST, C_INST, bx, iy + 14);
                }

                std::string pkgLine =
                    (apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName);
                if (!apks[i].versionName.empty())
                    pkgLine += "  v" + apks[i].versionName;
                if (apks[i].fileSizeBytes > 0)
                    pkgLine += "  ·  " + formatSize(apks[i].fileSizeBytes);
                drawText(fSm, clamp(fSm, pkgLine, maxW), C_GRAY, tx, iy + 58);
            }
            if ((int)apks.size() > VISIBLE) {
                int barH = LIST_H * VISIBLE / (int)apks.size();
                int barY = LIST_Y + LIST_H * scroll / (int)apks.size();
                fill(SW - 6, barY, 6, barH, {150, 195, 172, 200});
            }
        }

        std::string cnt;
        if (!apks.empty())
            cnt = std::to_string(apks.size()) + (apks.size() == 1 ? " APK" : " APKs");
        drawHeaderBar(cnt);

        if (noticeUntil && now < noticeUntil) {
            const char* msg = noticeText.c_str();
            int w = 0, h = 0;
            TTF_SizeUTF8(fSm, msg, &w, &h);
            fill((SW - w) / 2 - 16, SH - FOOTER_H - 44, w + 32, 34, {253, 235, 238, 240});
            drawText(fSm, msg, C_WARN, (SW - w) / 2, SH - FOOTER_H - 36);
        }

        bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        // L/R page through the list; only worth the footer space once there's
        // more than one screenful to page through.
        std::vector<std::pair<std::string, std::string>> hints = {
            {BG(GLYPH_A, "A"), "Launch"}, {BG(GLYPH_B, "B"), "Manage"},
            {BG(GLYPH_X, "X"), "Reinstall"}, {BG(GLYPH_Y, "Y"), "Rescan / hold: test"},
            {BG(GLYPH_MINUS, "-"), "About"}, {BG(GLYPH_PLUS, "+"), "Quit"}};
        if ((int)apks.size() > VISIBLE)
            hints.insert(hints.begin() + 1, {"L/R", "Page"});
        drawFooterBar(hints,
                      docked ? "Docked — games need handheld (touch screen)" : "");

        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Update prompt. Deliberately a confirm rather than a silent auto-apply:
    // this replaces the NRO the console is about to run, and doing that to
    // someone mid-session without asking is how a good session turns into a
    // non-booting SD card. The CHECK is automatic; applying is one button.
    void showUpdatePrompt() {
        if (!update.available) return;
        bool done = false, install = false;

        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN) {
                    if (ev.jbutton.button == BTN_A) { install = true; done = true; }
                    if (ev.jbutton.button == BTN_B) { done = true; }
                }
            }

            drawBackground();
            drawHeaderBar();
            int y = LIST_Y + 30;
            drawText(fLg, "Update available", C_WHITE, 30, y);              y += 46;
            drawText(fMd, std::string("New:     ") + update.tag, C_WHITE, 30, y);   y += 30;
            drawText(fMd, std::string("Current: ") + updateCurrentVersion(), C_DIM, 30, y); y += 40;
            if (update.assetSize > 0)
                drawText(fSm, "Download: " + formatSize((uint64_t)update.assetSize), C_DIM, 30, y);
            y += 34;
            drawText(fSm, "Replaces the launcher and both Translation Core binaries", C_DIM, 30, y); y += 22;
            drawText(fSm, "on your SD card. Your APKs, saves and settings are untouched.", C_DIM, 30, y);

            drawFooterBar({{BG(GLYPH_A, "A"), "Install"}, {BG(GLYPH_B, "B"), "Not now"}}, "");
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }

        if (!install) return;
        runUpdateInstall();
    }

    void runUpdateInstall() {
        // updateApply() blocks, so the progress callback is what keeps the
        // screen alive — it draws and presents a frame each time it's called.
        auto draw = [&](const char* stage, int pct) {
            drawBackground();
            drawHeaderBar();
            int y = LIST_Y + 40;
            drawText(fLg, stage, C_WHITE, 30, y); y += 50;

            int barW = SW - 120, barH = 18, barX = 30;
            fill(barX, y, barW, barH, {0, 0, 0, 40});
            if (pct >= 0) {
                fill(barX, y, (barW * std::clamp(pct, 0, 100)) / 100, barH, C_OK);
                char buf[16]; snprintf(buf, sizeof buf, "%d%%", std::clamp(pct, 0, 100));
                drawText(fSm, buf, C_DIM, barX, y + barH + 10);
            } else {
                // No Content-Length — show motion rather than a fake percentage.
                fill(barX, y, barW, barH, {0, 0, 0, 25});
                drawText(fSm, "working...", C_DIM, barX, y + barH + 10);
            }
            drawText(fSm, "Don't power off or eject the SD card.", C_WARN, 30, y + barH + 42);
            SDL_RenderPresent(rdr);
            // Keep the applet responsive so Horizon doesn't consider us hung.
            SDL_Event e; while (SDL_PollEvent(&e)) {}
        };

        std::string err;
        bool ok = updateApply(update, draw, &err);

        logMsg(ok ? "update: installed OK" : ("update: FAILED — " + err).c_str());

        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) done = true;
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    (ev.jbutton.button == BTN_A || ev.jbutton.button == BTN_B)) done = true;
            }
            drawBackground();
            drawHeaderBar();
            int y = LIST_Y + 40;
            if (ok) {
                drawText(fLg, "Update installed", C_WHITE, 30, y); y += 46;
                drawText(fMd, "Now running " + update.tag + " after a restart.", C_DIM, 30, y); y += 34;
                drawText(fSm, "Press A to close Viridite, then launch it again", C_DIM, 30, y); y += 22;
                drawText(fSm, "from hbmenu to start the new version.", C_DIM, 30, y);
            } else {
                drawText(fLg, "Update failed", C_WARN, 30, y); y += 46;
                drawText(fSm, err, C_DIM, 30, y); y += 30;
                drawText(fSm, "Nothing was replaced — your install is unchanged.", C_DIM, 30, y); y += 22;
                drawText(fSm, "Details are in sdmc:/Viridite/launcher_log.txt", C_DIM, 30, y);
            }
            drawFooterBar({{BG(GLYPH_A, "A"), ok ? "Quit" : "Back"}}, "");
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
        updateQuitRequested = ok;
    }

    bool updateQuitRequested = false;

    // ------------------------------------------------------------------
    // Self-test results. Same action layer as everywhere else, so every input
    // works here too rather than only the one it was written against.
    bool deepTestHandoff = false;   // set when X hands over to the Core

    void showSelfTest() {
        noticeText.clear();

        // Run it with the progress screen live: a few of the checks touch the
        // SD card or the network, and a frozen screen for two seconds reads as
        // a crash.
        static App* s_self = nullptr; s_self = this;
        auto onStage = [](const char* stage) {
            if (!s_self) return;
            s_self->noticeText  = std::string("Self-test: ") + stage + "...";
            s_self->noticeUntil = SDL_GetTicks() + 4000;
            s_self->render();
        };
        logMsg("self-test: starting");
        std::vector<TestResult> res = selfTestRun(apks, onStage);
        s_self = nullptr;
        noticeText.clear();

        int pass = 0, warn = 0, fail = 0;
        for (const TestResult& r : res)
            (r.status == TestStatus::Pass ? pass
             : r.status == TestStatus::Warn ? warn : fail)++;
        // Every line goes to launcher_log.txt as well as selftest.txt. The
        // logs that actually get sent over are the three standard ones, so a
        // result that only exists in a fourth file is a result nobody reads.
        for (const TestResult& r : res) {
            const char* tag = r.status == TestStatus::Pass ? "PASS"
                            : r.status == TestStatus::Warn ? "WARN" : "FAIL";
            logMsg(("self-test [" + std::string(tag) + "] " + r.name +
                    ": " + r.detail).c_str());
        }
        logMsg(("self-test: " + std::to_string(pass) + " passed, " +
                std::to_string(warn) + " warnings, " + std::to_string(fail) +
                " failed").c_str());

        int scroll = 0;
        const int VIS = 9;   // rows on screen; the list scrolls past this
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; break; }
                switch (actionFor(ev)) {
                    case Act::Up:       scroll = std::max(0, scroll - 1); break;
                    case Act::Down:     scroll = std::min(std::max(0, (int)res.size() - VIS), scroll + 1); break;
                    case Act::PageUp:
                    case Act::Home:     scroll = 0; break;
                    case Act::PageDown:
                    case Act::End:      scroll = std::max(0, (int)res.size() - VIS); break;
                    case Act::Reinstall: {
                        // X starts the deep test. It has to run in the Core —
                        // that is where the loader lives, and testing a copy of
                        // it in the launcher would be testing the wrong code.
                        // So hand over, let it dry-load everything, and it
                        // hands straight back to us with results on disk.
                        struct stat cst;
                        if (stat(CORE_X64_PATH, &cst) != 0) {
                            noticeText  = "Translation Core not found — cannot deep test.";
                            noticeUntil = SDL_GetTicks() + 5000;
                            break;
                        }
                        // Name the selected game. The Core tests one per
                        // invocation, which is both what you want when you are
                        // checking a specific title and the only thing that
                        // works — a second load in the same process inherits
                        // the first game's mappings.
                        if (apks.empty()) {
                            noticeText  = "No games to deep test.";
                            noticeUntil = SDL_GetTicks() + 5000;
                            break;
                        }
                        const ApkInfo& sel = apks[std::clamp(selected, 0, (int)apks.size() - 1)];
                        std::string selPkg = sel.packageName.empty() ? sel.filename : sel.packageName;
                        std::string arg = std::string(CORE_X64_PATH) + " --selftest " + selPkg;
                        logMsg(("self-test: deep-testing " + selPkg + " in the Core").c_str());
                        if (R_SUCCEEDED(envSetNextLoad(CORE_X64_PATH, arg.c_str()))) {
                            deepTestHandoff = true;
                            done = true;
                        } else {
                            noticeText  = "Could not hand off to the Core.";
                            noticeUntil = SDL_GetTicks() + 5000;
                        }
                        break;
                    }
                    case Act::Back:
                    case Act::Manage:
                    case Act::Quit:
                    case Act::Confirm:  done = true; break;
                    default: break;
                }
                if (ev.type == SDL_JOYAXISMOTION) {
                    int a = ev.jaxis.axis, v = ev.jaxis.value;
                    static Uint32 last = 0; Uint32 now = SDL_GetTicks();
                    if (now - last > 180) {
                        if ((a == AXIS_LY || a == AXIS_RY) && v >  AXIS_DEADZONE) { scroll = std::min(std::max(0,(int)res.size()-VIS), scroll+1); last = now; }
                        if ((a == AXIS_LY || a == AXIS_RY) && v < -AXIS_DEADZONE) { scroll = std::max(0, scroll-1); last = now; }
                    }
                }
            }

            drawBackground();
            drawHeaderBar();
            fill(40, LIST_Y + 6, SW - 80, SH - LIST_Y - FOOTER_H - 12, {247, 251, 249, 235});

            char hdr[96];
            snprintf(hdr, sizeof(hdr), "SELF-TEST  —  %d passed, %d warnings, %d failed",
                     pass, warn, fail);
            drawText(fMd, hdr, fail ? C_ERR : warn ? C_WARN : C_OK, 64, LIST_Y + 22);

            int y = LIST_Y + 58;
            for (int i = scroll; i < (int)res.size() && i < scroll + VIS; i++) {
                const TestResult& r = res[i];
                    SDL_Color c = r.status == TestStatus::Pass ? C_OK
                            : r.status == TestStatus::Warn ? C_WARN : C_ERR;
                const char* tag = r.status == TestStatus::Pass ? "PASS"
                                : r.status == TestStatus::Warn ? "WARN" : "FAIL";
                drawText(fSm, tag,   c,                 64,  y);
                drawText(fSm, r.name, C_WHITE,           128, y);
                drawText(fSm, clamp(fSm, r.detail, SW - 220), C_GRAY, 128, y + 20);
                y += 46;
            }

            if ((int)res.size() > VIS) {
                char sc[48];
                snprintf(sc, sizeof(sc), "%d-%d of %zu", scroll + 1,
                         std::min((int)res.size(), scroll + VIS), res.size());
                drawText(fSm, sc, C_DIM, SW - 200, SH - FOOTER_H - 34);
            }

            drawFooterBar({{"B", "Back"}, {"X", "Deep test"}, {"\u2191\u2193", "Scroll"}},
                          "Saved to selftest.txt");
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }

    // Theme picker. Every scheme is a full Material You palette generated from
    // its own seed, so each row previews itself with its own colours rather
    // than describing them.
    void showThemes() {
        int sel  = g_themeIndex;
        int start = g_themeIndex;
        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; break; }
                switch (actionFor(ev)) {
                    case Act::Up:   sel = (sel - 1 + M3_THEME_COUNT) % M3_THEME_COUNT; themeApply(sel); break;
                    case Act::Down: sel = (sel + 1) % M3_THEME_COUNT;                  themeApply(sel); break;
                    case Act::Confirm: themeSave(); done = true; break;
                    case Act::Back:
                    case Act::Manage:
                    case Act::Quit: themeApply(start); done = true; break;   // revert
                    default: break;
                }
            }

            drawBackground();
            drawHeaderBar();
            int y = LIST_Y + 26;
            drawIcon("palette", 30, y - 2, 30, C_OK);
            drawText(fLg, "Theme", C_WHITE, 74, y);
            y += 52;

            const int ROW_H = 76;
            for (int i = 0; i < M3_THEME_COUNT; i++) {
                const M3Scheme& m = M3_THEMES[i];
                bool on = (i == sel);
                SDL_Rect card = {24, y, SW - 48, ROW_H - 10};
                fillRounded(card.x, card.y, card.w, card.h, M3_L,
                            on ? m.secondaryContainer : m.surfaceContainerHigh);

                // Three swatches: the roles a scheme is actually judged by.
                int sx = card.x + 18, sy = card.y + (card.h - 30) / 2;
                fillRounded(sx,      sy, 30, 30, M3_FULL, m.primary);
                fillRounded(sx + 38, sy, 30, 30, M3_FULL, m.tertiary);
                fillRounded(sx + 76, sy, 30, 30, M3_FULL, m.surfaceContainerHighest);

                drawText(fMd, m.name, on ? m.onSecondaryContainer : m.onSurface,
                         sx + 126, card.y + (card.h - 22) / 2);
                if (on) drawIcon("check_circle", card.x + card.w - 46,
                                 card.y + (card.h - 26) / 2, 26, m.primary);
                y += ROW_H;
            }

            drawFooterBar({{BG(GLYPH_B, "B"), "Cancel"}, {BG(GLYPH_A, "A"), "Use this theme"}});
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }

    void showAbout() {
        bool done = false;
        creditsScroll    = 0.0f;
        lastCreditsInput = SDL_GetTicks();
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                // Any "do something" input closes this — the credits have no
                // actions of their own beyond scrolling, so nobody should get
                // stuck here for pressing the wrong button. Routed through the
                // shared action layer so the shoulders and a keyboard work too.
                {
                    Act a = actionFor(ev);
                    if (a == Act::Confirm || a == Act::Back || a == Act::Manage ||
                        a == Act::Quit    || a == Act::About)
                        done = true;
                }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                    { done = true; }

                if (ev.type == SDL_JOYHATMOTION) {
                    if (ev.jhat.value & SDL_HAT_DOWN) { creditsScroll += 40.0f; lastCreditsInput = SDL_GetTicks(); }
                    if (ev.jhat.value & SDL_HAT_UP)   { creditsScroll -= 40.0f; lastCreditsInput = SDL_GetTicks(); }
                }
                if (ev.type == SDL_JOYAXISMOTION && ev.jaxis.axis == 1) {
                    Uint32 now = SDL_GetTicks();
                    if (now - lastCreditsStick > 90) {
                        if (ev.jaxis.value > 16384)       { creditsScroll += 24.0f; lastCreditsStick = now; lastCreditsInput = now; }
                        else if (ev.jaxis.value < -16384) { creditsScroll -= 24.0f; lastCreditsStick = now; lastCreditsInput = now; }
                    }
                }

                // Drag to scroll the credits list; a release without any
                // drag is treated as a tap-to-dismiss, since nothing else on
                // this screen is individually tappable.
                if (ev.type == SDL_FINGERDOWN) {
                    touchDragging = false;
                }
                if (ev.type == SDL_FINGERMOTION) {
                    creditsScroll -= ev.tfinger.dy * SH;
                    touchDragging = true;
                    lastCreditsInput = SDL_GetTicks();
                }
                if (ev.type == SDL_FINGERUP && !touchDragging) {
                    done = true;
                }
            }

            // Rolls on its own like end credits once the visitor's left it
            // alone for a bit — any manual scroll above resets the idle
            // timer, so it never fights input while someone's actually
            // reading a specific section.
            if (!people.empty() && SDL_GetTicks() - lastCreditsInput > 2500)
                creditsScroll += 0.6f;

            std::vector<uint8_t> img;
            if (avatarPollNewImage(img)) {
                SDL_RWops* rw = SDL_RWFromConstMem(img.data(), (int)img.size());
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) {
                    if (avatarTex) SDL_DestroyTexture(avatarTex);
                    avatarTex = SDL_CreateTextureFromSurface(rdr, surf);
                    SDL_FreeSurface(surf);
                }
            }

            drawBackground();
            drawHeaderBar();

            int avSz = 160;
            int avX  = (SW - avSz) / 2;
            int avY  = LIST_Y + 30;
            if (avatarTex) {
                SDL_Rect dst = {avX, avY, avSz, avSz};
                SDL_RenderCopy(rdr, avatarTex, nullptr, &dst);
            } else {
                drawMonogram("Viridite", avX, avY, avSz);
                static const std::string FETCH = "Loading avatar...";
                int fw = 0, fh = 0;
                TTF_SizeUTF8(fSm, FETCH.c_str(), &fw, &fh);
                drawText(fSm, FETCH, C_DIM, (SW - fw) / 2, avY + avSz + 8);
            }

            int y = avY + avSz + 40;
            auto center = [&](TTF_Font* f, const std::string& s, SDL_Color col) {
                int w = 0, h = 0;
                TTF_SizeUTF8(f, s.c_str(), &w, &h);
                drawText(f, s, col, (SW - w) / 2, y);
                y += h + 10;
            };
            center(fLg, "Viridite", C_WHITE);
            center(fSm, BUILD_VERSION, C_DIM);
            center(fSm, "by aaronworld.uk", C_GRAY);
            y += 10;
            center(fSm, "Android NDK compatibility layer for Nintendo Switch (HorizonOS)", C_GRAY);

            drawContributors(y + 20);

            drawFooterBar({{BG(GLYPH_B, "B"), "Back to menu"}},
                          people.empty() ? "" : "Scroll credits: D-Pad / stick / touch drag");

            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }

    // ------------------------------------------------------------------
    // Manage overlay for the currently-selected APK: framerate cap and
    // delete. Bound to B on the main list, which was otherwise unused there
    // (B only means "back" on the sub-screens this launcher already has).
    // The actual cap is read and applied by the Viridite Translation Core at launch
    // (see readFpsCap in loader.cpp) — this screen just writes the
    // .fps_cap marker file the same way apkIsInstalled reads .installed.
    void showManage() {
        if (apks.empty()) return;
        int idx = selected;
        if (idx < 0 || idx >= (int)apks.size()) return;
        ApkInfo apk = apks[idx]; // copy — index may shift under us after a delete+rescan
        std::string pkg = apk.packageName.empty() ? apk.filename : apk.packageName;

        // Mirrors Android's app-info screen: cache and storage are separate
        // actions, because "make it re-extract" and "throw my save away" are
        // very different intentions that a single Delete button conflates.
        static const int ROW_FPS = 0, ROW_CACHE = 1, ROW_STORAGE = 2,
                         ROW_DELETE = 3, ROW_COUNT = 4;
        int  row           = 0;
        Uint32 lastManageStick = 0;
        int  fpsCap        = apkGetFpsCap(pkg); // 0 = default/uncapped
        bool confirmDelete = false;
        bool confirmCache  = false;
        bool confirmData   = false;
        bool deleted       = false;
        bool done          = false;
        // Measured on a worker thread: over a real extracted game this reads
        // thousands of files off the SD card, and doing it inline froze the
        // screen until it finished.
        std::atomic<uint64_t> cacheBytes{0}, dataBytes{0};
        std::atomic<bool>     sizesReady{false};
        std::thread sizer;
        auto startSizing = [&]() {
            if (sizer.joinable()) sizer.join();
            sizesReady = false;
            sizer = std::thread([&, pkg]() {
                uint64_t c = 0, d = 0;
                apkGetStorageUsage(pkg, &c, &d);
                cacheBytes = c; dataBytes = d; sizesReady = true;
            });
        };
        startSizing();

        std::vector<SDL_Rect> rowRects(ROW_COUNT);

        auto toggleFps = [&]() {
            fpsCap = (fpsCap == 30) ? 0 : 30;
            apkSetFpsCap(pkg, fpsCap);
        };
        auto activateDelete = [&]() {
            if (!confirmDelete) { confirmDelete = true; return; }
            apkDeleteInstalledData(pkg);
            apkDeleteFile(apk.path);
            deleted = true;
            done    = true;
        };
        auto activateCache = [&]() {
            if (!confirmCache) { confirmCache = true; return; }
            bool ok = apkClearCache(pkg);
            startSizing();
            confirmCache = false;
            noticeText  = ok ? "Cache cleared — the game will re-extract next launch."
                             : "Nothing cached for this game.";
            noticeUntil = SDL_GetTicks() + 4000;
            logMsg(("manage: clear cache " + pkg + (ok ? " OK" : " (nothing to clear)")).c_str());
        };
        auto activateStorage = [&]() {
            if (!confirmData) { confirmData = true; return; }
            bool ok = apkClearStorage(pkg);
            startSizing();
            confirmData = false;
            noticeText  = ok ? "Storage cleared — saves and settings for this game are gone."
                             : "No stored data for this game.";
            noticeUntil = SDL_GetTicks() + 4000;
            logMsg(("manage: clear storage " + pkg + (ok ? " OK" : " (nothing to clear)")).c_str());
        };
        auto activate = [&](int r) {
            if (r == ROW_FPS)          toggleFps();
            else if (r == ROW_CACHE)   activateCache();
            else if (r == ROW_STORAGE) activateStorage();
            else if (r == ROW_DELETE)  activateDelete();
        };
        auto clearConfirms = [&]() { confirmDelete = confirmCache = confirmData = false; };

        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; break; }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) done = true;

                // Same action layer the app list uses, so this screen answers
                // to the stick, the shoulders and a keyboard rather than only
                // A/B and the D-pad — it was the one screen that didn't.
                Act a = actionFor(ev);
                if (ev.type == SDL_JOYHATMOTION) {
                    if (ev.jhat.value & SDL_HAT_DOWN) a = Act::Down;
                    if (ev.jhat.value & SDL_HAT_UP)   a = Act::Up;
                }
                if (ev.type == SDL_JOYAXISMOTION) {
                    Uint32 now = SDL_GetTicks();
                    int ax = ev.jaxis.axis, v = ev.jaxis.value;
                    if ((ax == AXIS_LY || ax == AXIS_RY) && now - lastManageStick > 180) {
                        if      (v >  AXIS_DEADZONE) { a = Act::Down; lastManageStick = now; }
                        else if (v < -AXIS_DEADZONE) { a = Act::Up;   lastManageStick = now; }
                    }
                }

                switch (a) {
                    case Act::Up:       row = std::max(row - 1, 0);              clearConfirms(); break;
                    case Act::Down:     row = std::min(row + 1, ROW_COUNT - 1);  clearConfirms(); break;
                    case Act::Home:
                    case Act::PageUp:   row = 0;                                 clearConfirms(); break;
                    case Act::End:
                    case Act::PageDown: row = ROW_COUNT - 1;                     clearConfirms(); break;
                    case Act::Confirm:  activate(row); break;
                    // B / Escape backs out — or cancels a pending confirmation
                    // first, so it never destroys anything on the way out.
                    case Act::Manage:
                    case Act::Back:
                    case Act::Quit:
                        if (confirmDelete || confirmCache || confirmData) clearConfirms();
                        else done = true;
                        break;
                    default: break;
                }

                if (ev.type == SDL_FINGERDOWN) touchDragging = false;
                if (ev.type == SDL_FINGERUP && !touchDragging) {
                    int px = (int)(ev.tfinger.x * SW);
                    int py = (int)(ev.tfinger.y * SH);
                    int hit = hitTestFooter(px, py);
                    if (hit == 0) {
                        if (confirmDelete || confirmCache || confirmData) clearConfirms();
                        else done = true;
                    } else if (hit == 1) {
                        activate(row);
                    } else {
                        for (int r = 0; r < ROW_COUNT; r++) {
                            const SDL_Rect& rr = rowRects[r];
                            if (rr.w > 0 && px >= rr.x && px < rr.x + rr.w &&
                                py >= rr.y && py < rr.y + rr.h) {
                                if (r == row) activate(r);
                                else { row = r; clearConfirms(); }
                                break;
                            }
                        }
                    }
                }
            }

            drawBackground();
            drawHeaderBar();

            int y = LIST_Y + 30;
            drawText(fLg, clamp(fLg, "Manage: " + apk.appName, SW - 60), C_WHITE, 30, y);
            y += 50;

            const int ROW_H = 74;
            const char* fpsLabel = (fpsCap == 30) ? "Framerate cap: 30fps (battery-friendly)"
                                                    : "Framerate cap: Default (uncapped)";
            const char* deleteLabel = confirmDelete
                ? "Delete this game — press A again to confirm, cannot be undone"
                : "Delete this game (removes the APK and any installed data)";
            std::string cacheLabel = confirmCache
                ? "Clear cache — press A again to confirm (saves are kept)"
                : "Clear cache (" + (sizesReady ? formatSize(cacheBytes) : std::string("measuring…")) +
                  ") — re-extracts next launch, saves kept";
            std::string dataLabel = confirmData
                ? "Clear storage — press A again to confirm, SAVES WILL BE LOST"
                : "Clear storage (" + (sizesReady ? formatSize(dataBytes) : std::string("measuring…")) +
                  ") — deletes saves and settings too";
            const char* labels[ROW_COUNT] = { fpsLabel, cacheLabel.c_str(),
                                              dataLabel.c_str(), deleteLabel };

            // Android labels these actions with icons, and they are the same
            // icons Settings uses for the same jobs — a broom for cache, a bin
            // for storage. Borrowing the vocabulary is most of what makes a
            // screen read as Android rather than as a list of sentences.
            static const char* rowIcons[ROW_COUNT] = {
                "tune",                // framerate cap
                "cleaning_services",   // clear cache
                "storage",             // clear storage
                "delete_forever",      // delete game
            };

            for (int r = 0; r < ROW_COUNT; r++) {
                SDL_Rect card = {24, y, SW - 48, ROW_H - 10};
                rowRects[r] = card;
                bool sel  = (r == row);
                bool dang = (r == ROW_DELETE && confirmDelete);
                SDL_Color bg = dang ? C_ERR_CONTAINER
                             : sel  ? C_SEL
                                    : C_SURF_HIGH;
                fillRounded(card.x, card.y, card.w, card.h, M3_L, bg);
                SDL_Color textCol = dang ? C_ON_ERR_CONTAINER
                                  : (r == ROW_DELETE) ? C_ERR
                                  : sel ? C_ON_PRIMARY_CONTAINER : C_WHITE;
                const int ICO = 26;
                drawIcon(rowIcons[r], card.x + 20, card.y + (card.h - ICO) / 2, ICO, textCol);
                drawText(fMd, clamp(fMd, labels[r], card.w - 84),
                         textCol, card.x + 20 + ICO + 16, card.y + (card.h - 22) / 2);
                y += ROW_H;
            }

            drawFooterBar({{BG(GLYPH_B, "B"), confirmDelete ? "Cancel" : "Back"},
                           {BG(GLYPH_A, "A"), row == ROW_DELETE ? (confirmDelete ? "Confirm delete" : "Delete")
                                                                 : "Toggle"}});

            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }

        if (deleted) rescan();
    }

    // ------------------------------------------------------------------
    // Chain-load into the right Translation Core for this game's architecture,
    // passing the package name as argv[0] (libnx's argv parser has no
    // synthetic program-name slot — the first word IS argv[0]) —
    // envSetNextLoad(path, argv) is the
    // same mechanism external forwarders (Sphaira etc.) already use to jump
    // straight into a game; this launcher just uses it internally now too.
    // Returning from main() after this call lets hbloader perform the switch.
    // Docked there's no touch screen, so a controller is the only way to play
    // and there's nothing to choose. Handheld genuinely has both, and which one
    // someone wants isn't guessable — HCR plays quite differently on a pad than
    // with touch steering. Returns false if the launch was cancelled.
    // Offers whatever is actually plugged in, plus touch when the console has a
    // screen to touch. Docked with one pad attached there's nothing to choose,
    // so this isn't called at all — see launchGame.
    // Returns false if the launch was cancelled.
    // Picks how to play by SHOWING the options — the actual controller you're
    // holding, and a touch screen — rather than listing them as text. The
    // artwork is the same set the in-game guide gets patched with, so what you
    // choose here is what you see in the game's own help screen.
    // Returns false if the launch was cancelled.
    bool askLaunchMode(const std::string& appName,
                       const std::vector<PadKind>& pads, bool offerTouch,
                       PadKind* chosen) {
        std::vector<PadKind> opts = pads;
        if (offerTouch) opts.push_back(PadKind::None);
        if (opts.empty()) { *chosen = PadKind::None; return true; }

        // Load each icon once up front; a missing one just falls back to the
        // name, so the picker still works if artwork is ever absent.
        std::vector<SDL_Texture*> tex(opts.size(), nullptr);
        std::vector<int> tw(opts.size(), 0), th(opts.size(), 0);
        for (size_t i = 0; i < opts.size(); i++) {
            SDL_Surface* sf = IMG_Load(padKindIcon(opts[i]));
            if (!sf) { logMsg((std::string("picker: missing icon ") + padKindIcon(opts[i])).c_str()); continue; }
            tex[i] = SDL_CreateTextureFromSurface(rdr, sf);
            tw[i] = sf->w; th[i] = sf->h;
            SDL_FreeSurface(sf);
        }

        int  sel = 0;
        bool done = false, cancelled = false;

        // Tiles sized to fit however many options there are, so two options get
        // large icons and five still fit across the screen.
        const int n       = (int)opts.size();
        const int gap     = 24;
        const int availW  = SW - 80;
        const int tileW   = std::min(340, (availW - gap * (n - 1)) / std::max(1, n));
        const int tileH   = 260;
        const int totalW  = tileW * n + gap * (n - 1);
        const int startX  = (SW - totalW) / 2;
        const int tileY   = LIST_Y + 96;

        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { cancelled = true; done = true; break; }
                Act a = actionFor(ev);
                if (ev.type == SDL_JOYHATMOTION) {
                    if (ev.jhat.value & SDL_HAT_LEFT)  a = Act::PageUp;
                    if (ev.jhat.value & SDL_HAT_RIGHT) a = Act::PageDown;
                }
                // Horizontal row, so left/right move between tiles; up/down are
                // accepted too rather than doing nothing.
                if      (a == Act::PageUp   || a == Act::Up)   sel = (sel - 1 + n) % n;
                else if (a == Act::PageDown || a == Act::Down) sel = (sel + 1) % n;
                else if (a == Act::Confirm) done = true;
                else if (a == Act::Manage || a == Act::Quit) { cancelled = true; done = true; }

                if (ev.type == SDL_FINGERUP) {
                    int px = (int)(ev.tfinger.x * SW), py = (int)(ev.tfinger.y * SH);
                    for (int i = 0; i < n; i++) {
                        int tx = startX + i * (tileW + gap);
                        if (px >= tx && px < tx + tileW && py >= tileY && py < tileY + tileH) {
                            if (i == sel) done = true; else sel = i;
                            break;
                        }
                    }
                }
            }

            drawBackground();
            drawHeaderBar();
            int y = LIST_Y + 26;
            drawText(fLg, clamp(fLg, appName, SW - 60), C_WHITE, 30, y); y += 40;
            drawText(fSm, "How do you want to play?", C_GRAY, 30, y);

            for (int i = 0; i < n; i++) {
                int tx = startX + i * (tileW + gap);
                bool on = (i == sel);

                fill(tx, tileY, tileW, tileH, on ? C_SEL : C_HEADER);
                // Accent frame on the selection so it reads at a glance.
                if (on) {
                    fill(tx, tileY, tileW, 4, C_OK);
                    fill(tx, tileY + tileH - 4, tileW, 4, C_OK);
                    fill(tx, tileY, 4, tileH, C_OK);
                    fill(tx + tileW - 4, tileY, 4, tileH, C_OK);
                } else {
                    fill(tx, tileY, tileW, 1, C_DIV);
                    fill(tx, tileY + tileH - 1, tileW, 1, C_DIV);
                }

                // Icon, fitted inside the tile with its aspect ratio kept.
                if (tex[i] && tw[i] > 0 && th[i] > 0) {
                    int boxW = tileW - 40, boxH = tileH - 78;
                    float sc = std::min((float)boxW / tw[i], (float)boxH / th[i]);
                    int dw = (int)(tw[i] * sc), dh = (int)(th[i] * sc);
                    SDL_Rect dst{tx + (tileW - dw) / 2, tileY + 20 + (boxH - dh) / 2, dw, dh};
                    SDL_RenderCopy(rdr, tex[i], nullptr, &dst);
                }

                std::string nm = clamp(fSm, padKindName(opts[i]), tileW - 20);
                int nw = 0, nh = 0;
                TTF_SizeUTF8(fSm, nm.c_str(), &nw, &nh);
                drawText(fSm, nm, on ? C_WHITE : C_GRAY,
                         tx + (tileW - nw) / 2, tileY + tileH - 34);
            }

            drawFooterBar({{BG(GLYPH_A, "A"), "Play"}, {BG(GLYPH_B, "B"), "Back"}}, "");
            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }

        for (auto* t : tex) if (t) SDL_DestroyTexture(t);
        *chosen = opts[sel];
        return !cancelled;
    }

    bool launchGame(const ApkInfo& apk, bool* outHandled, bool skipInputPrompt = false) {
        *outHandled = false;
        const std::string& pkg =
            apk.packageName.empty() ? apk.filename : apk.packageName;
        if (!isCompatibleGame(pkg)) {
            noticeText  = "This game isn't compatible yet — only Hill Climb Racing "
                          "and Brain It On! are supported in this build.";
            noticeUntil = SDL_GetTicks() + 7000;
            logMsg(("launch blocked (incompatible): " + pkg).c_str());
            return false;
        }
        // 32-bit (armeabi-v7a) games now chain-load the x64 Core, which runs
        // them under the ARM32 emulation layer. This is experimental and slow —
        // only allowlisted titles reach here. (Kept behind the allowlist so a
        // random arm32 APK doesn't drop into a half-built emulator.)
        if (apk.arch == ApkArch::Arm32Only) {
            noticeText  = "32-bit game — running under the experimental ARM32 layer. "
                          "Expect it to be slow/incomplete.";
            noticeUntil = SDL_GetTicks() + 5000;
            logMsg(("launch (32-bit via ARM32 layer): " + apk.packageName).c_str());
            // fall through — chain-load the x64 Core below
        }
        const char* corePath = CORE_X64_PATH;
        struct stat st;
        if (stat(corePath, &st) != 0) {
            noticeText  = "Viridite-Translation-Core-x64.nro not found next to the launcher.";
            noticeUntil = SDL_GetTicks() + 7000;
            logMsg(("core NRO missing: " + std::string(corePath)).c_str());
            return false;
        }
        // Chain-loading is an hbloader feature — if this process wasn't
        // started under hbloader (e.g. a forwarder that launches it some
        // other way), envSetNextLoad may silently have nowhere to hand off
        // to. Log this explicitly so a failed launch attempt is diagnosable
        // from launcher_log.txt instead of a guess.
        bool hasNext = envHasNextLoad();
        logMsg(hasNext ? "envHasNextLoad: true" : "envHasNextLoad: FALSE — chain-load unsupported in this launch context");
        // argv[0] MUST be the Core's own real path, not our package name —
        // libnx's romfsInit() falls back to argv[0] to find and open its own
        // .nro file on the SD card and read its embedded RomFS section from
        // it (confirmed against libnx's actual romfs-mounting behavior).
        // Overwriting argv[0] with the package name broke RomFS entirely —
        // every font load failed immediately on the Core side, confirmed via
        // its own log.txt. The real argument goes at argv[1] instead, same
        // convention any normal argv[0]-is-the-program-path command line
        // would use.
        // Two builds of one game can share a package id (e.g. an arm64 and an
        // arm32 HCR). argv only carries the package, so record the exact file
        // the user picked in a marker the Core reads to disambiguate.
        {
            FILE* lm = fopen("sdmc:/Viridite/.launch_apk", "w");
            if (lm) { fputs(apk.path.c_str(), lm); fclose(lm); }
        }

        // Input mode, passed to the Core the same way as the FPS cap: a marker
        // file it reads once at launch. The Core uses it both to decide whether
        // to register a controller at all and to pick which guide image to
        // patch into the game, so the diagram matches what's in your hands.
        {
            PadKind chosen = PadKind::None;
            bool dockedNow = appletGetOperationMode() == AppletOperationMode_Console;
            std::vector<PadKind> pads = detectPads();

            if (skipInputPrompt) {
                // Reinstall path: keep whatever was chosen last time rather
                // than asking again for an action that isn't "start a game".
                chosen = pads.empty() ? PadKind::None : pads[0];
            } else if (!hasControllerSupport(pkg)) {
                // No controller path in this game — touch is the only answer,
                // and asking would be a step in the way.
                chosen = PadKind::None;
            } else if (pads.empty()) {
                chosen = PadKind::None;               // nothing attached
            } else if (dockedNow && pads.size() == 1) {
                chosen = pads[0];                     // no touch screen, one pad
            } else {
                // Docked offers pads only (no screen to touch); handheld offers
                // touch as well.
                if (!askLaunchMode(apk.appName, pads, !dockedNow, &chosen)) {
                    if (outHandled) *outHandled = false;
                    return false;                     // backed out of the picker
                }
            }
            FILE* im = fopen("sdmc:/Viridite/.launch_input", "w");
            if (im) { fputs(padKindId(chosen), im); fclose(im); }
            logMsg((std::string("launch input mode: ") + padKindId(chosen) +
                    (dockedNow ? " (docked)" : " (handheld)") +
                    " — detected " + std::to_string(pads.size()) + " pad(s)").c_str());
        }

        std::string argvStr = std::string(corePath) + " " + pkg;
        logMsg(("launchGame: chain-loading to " + std::string(corePath) +
                " argv=\"" + argvStr + "\"").c_str());
        Result rc = envSetNextLoad(corePath, argvStr.c_str());
        if (R_FAILED(rc)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "envSetNextLoad failed: 0x%08x", rc);
            noticeText  = buf;
            noticeUntil = SDL_GetTicks() + 7000;
            logMsg(buf);
            return false;
        }
        logMsg("envSetNextLoad OK — returning from main() to hand off");
        *outHandled = true;
        return true;
    }
};

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    themeLoad();   // before anything draws

    App app;

    if (!app.init()) return 1;

    mkdir(APK_DIR, 0777);

    app.drawBackground();
    app.drawHeaderBar();
    app.drawText(app.fSm, "Scanning for APKs...", C_GRAY, 30, LIST_Y + 30);
    SDL_RenderPresent(app.rdr);

    app.apks = scanApks(APK_DIR);
    app.loadIcons();
    app.render();

    bool   quit      = false;
    bool   handoff   = false;
    Uint32 lastStick = 0;

    // The Core hands back with this after a deep test. Going straight to the
    // results is the whole point — otherwise the run finishes and drops you on
    // the app list with no sign anything happened.
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--selftest-results") == 0) {
            logMsg("returned from the Core deep test — showing results");
            app.showSelfTest();
            app.render();
            break;
        }
    }

    // Hold Y to open the self-test. A tap is still Rescan, so the two share a
    // button without either getting in the way: the hold only fires once the
    // threshold passes, and the tap only fires on release if it never did.
    constexpr Uint32 SELFTEST_HOLD_MS = 5000;
    Uint32 yHeldSince = 0;      // 0 = not held
    bool   yConsumed  = false;  // the hold already fired; ignore the release

    while (!quit && !handoff) {
        SDL_Event ev;

        // Checked outside the event loop: SDL sends no events while a button is
        // simply held, so waiting for one would mean the timer never elapses.
        if (yHeldSince && !yConsumed) {
            Uint32 held = SDL_GetTicks() - yHeldSince;
            if (held >= SELFTEST_HOLD_MS) {
                yConsumed = true;
                app.noticeText.clear();
                app.showSelfTest();
                if (app.deepTestHandoff) { handoff = true; break; }
                app.render();
                continue;
            }
            // Count down on screen from the first second, so the hold is
            // discoverable and obviously deliberate rather than looking like a
            // button that stopped working.
            if (held > 700) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Keep holding Y for the self-test... %u",
                         (unsigned)((SELFTEST_HOLD_MS - held) / 1000) + 1);
                app.noticeText  = msg;
                app.noticeUntil = SDL_GetTicks() + 1000;
                app.render();
            }
        }

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { quit = true; break; }

            if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == BTN_Y) {
                yHeldSince = SDL_GetTicks();
                yConsumed  = false;
                continue;              // decide on release, or on the timer
            }
            if (ev.type == SDL_JOYBUTTONUP && ev.jbutton.button == BTN_Y) {
                const bool wasHeld = yConsumed;
                yHeldSince = 0; yConsumed = false;
                if (wasHeld) continue; // the hold already did something
                app.rescan();          // short press keeps its old meaning
                continue;
            }

            Act act = actionFor(ev);

            // Hat and both sticks feed the same movement actions. The stick is
            // rate-limited so holding it steps at a readable pace instead of
            // flying to the end of the list.
            if (ev.type == SDL_JOYHATMOTION) {
                if (ev.jhat.value & SDL_HAT_DOWN)  act = Act::Down;
                if (ev.jhat.value & SDL_HAT_UP)    act = Act::Up;
                if (ev.jhat.value & SDL_HAT_LEFT)  act = Act::PageUp;
                if (ev.jhat.value & SDL_HAT_RIGHT) act = Act::PageDown;
            }
            if (ev.type == SDL_JOYAXISMOTION) {
                Uint32 now = SDL_GetTicks();
                int a = ev.jaxis.axis, v = ev.jaxis.value;
                if (now - lastStick > 180) {
                    if ((a == AXIS_LY || a == AXIS_RY) && v >  AXIS_DEADZONE) { act = Act::Down;     lastStick = now; }
                    else if ((a == AXIS_LY || a == AXIS_RY) && v < -AXIS_DEADZONE) { act = Act::Up;  lastStick = now; }
                    else if ((a == AXIS_LX || a == AXIS_RX) && v >  AXIS_DEADZONE) { act = Act::PageDown; lastStick = now; }
                    else if ((a == AXIS_LX || a == AXIS_RX) && v < -AXIS_DEADZONE) { act = Act::PageUp;   lastStick = now; }
                }
            }

            const int  count   = (int)app.apks.size();
            const bool haveApks = count > 0;
            auto clampView = [&]() {
                app.selected = std::clamp(app.selected, 0, std::max(0, count - 1));
                if (app.selected <  app.scroll)           app.scroll = app.selected;
                if (app.selected >= app.scroll + VISIBLE) app.scroll = app.selected - VISIBLE + 1;
                app.scroll = std::clamp(app.scroll, 0, std::max(0, count - VISIBLE));
            };

            switch (act) {
                case Act::Up:        if (haveApks) { app.selected--;           clampView(); } break;
                case Act::Down:      if (haveApks) { app.selected++;           clampView(); } break;
                case Act::PageUp:    if (haveApks) { app.selected -= VISIBLE;  clampView(); } break;
                case Act::PageDown:  if (haveApks) { app.selected += VISIBLE;  clampView(); } break;
                case Act::Home:      if (haveApks) { app.selected = 0;         clampView(); } break;
                case Act::End:       if (haveApks) { app.selected = count - 1; clampView(); } break;
                case Act::Confirm:
                    if (haveApks) app.launchGame(app.apks[app.selected], &handoff);
                    break;
                case Act::Reinstall:
                    // X means "reinstall", so actually make it reinstall: the
                    // Core only re-extracts when the .installed marker is
                    // absent, and this used to be the same call as A — which
                    // launched the cached install unchanged. Dropping the
                    // marker is what makes the next launch re-extract.
                    //
                    // It also skips the how-do-you-want-to-play prompt. That
                    // question belongs to starting a game, not to reinstalling
                    // one, and being asked it after pressing X reads like the
                    // wrong button did something.
                    if (haveApks) {
                        const ApkInfo& a = app.apks[app.selected];
                        std::string pk = a.packageName.empty() ? a.filename : a.packageName;
                        apkClearCache(pk);
                        logMsg(("reinstall: cleared cached install for " + pk).c_str());
                        app.launchGame(a, &handoff, /*skipInputPrompt=*/true);
                    }
                    break;
                case Act::Manage:    if (haveApks) app.showManage(); break;
                case Act::Rescan:    app.rescan(); break;
                case Act::Theme:     app.showThemes(); break;
                case Act::SelfTest:  app.showSelfTest();
                                     if (app.deepTestHandoff) handoff = true;
                                     break;
                case Act::About:     app.showAbout(); break;
                case Act::Quit:      quit = true; break;
                // The app list is the root screen — there's nothing to go back
                // to. Back is for the modal screens that sit on top of it.
                case Act::Back:
                case Act::None:      break;
            }

            // Touch coords from SDL are normalized [0,1] against the touch
            // device (which covers the whole screen 1:1 with SW/SH here) —
            // convert to pixels the same way the rest of the UI is laid out.
            if (ev.type == SDL_FINGERDOWN) {
                app.touchStartX     = ev.tfinger.x * SW;
                app.touchStartY     = ev.tfinger.y * SH;
                app.touchDragging   = false;
                app.touchScrollAccY = 0.0f;
            }

            if (ev.type == SDL_FINGERMOTION) {
                float dyPix = ev.tfinger.dy * SH;
                app.touchScrollAccY += dyPix;
                // Small threshold so a tap's natural finger jitter doesn't
                // register as a drag and steal the tap-to-select/launch below.
                if (!app.touchDragging && fabsf(app.touchScrollAccY) > 12.0f)
                    app.touchDragging = true;
                if (app.touchDragging && !app.apks.empty()) {
                    while (app.touchScrollAccY >= ITEM_H) {
                        if (app.scroll > 0) app.scroll--;
                        app.touchScrollAccY -= ITEM_H;
                    }
                    while (app.touchScrollAccY <= -ITEM_H) {
                        if (app.scroll < (int)app.apks.size() - VISIBLE) app.scroll++;
                        app.touchScrollAccY += ITEM_H;
                    }
                    int maxScroll = std::max(0, (int)app.apks.size() - VISIBLE);
                    app.scroll = std::clamp(app.scroll, 0, maxScroll);
                }
            }

            if (ev.type == SDL_FINGERUP && !app.touchDragging) {
                int px = (int)(ev.tfinger.x * SW);
                int py = (int)(ev.tfinger.y * SH);
                int hit = app.hitTestFooter(px, py);
                // Hitbox index matches the order hints were passed to
                // drawFooterBar() in render(): A/Launch, B/Manage,
                // X/Reinstall, Y/Rescan, -/About, +/Quit.
                if (hit == 0) {
                    if (!app.apks.empty()) app.launchGame(app.apks[app.selected], &handoff);
                } else if (hit == 1) {
                    if (!app.apks.empty()) app.showManage();
                } else if (hit == 2) {
                    if (!app.apks.empty()) app.launchGame(app.apks[app.selected], &handoff);
                } else if (hit == 3) {
                    app.rescan();
                } else if (hit == 4) {
                    app.showAbout();
                } else if (hit == 5) {
                    quit = true;
                } else if (!app.apks.empty() && py >= LIST_Y && py < LIST_Y + LIST_H) {
                    int row = app.scroll + (py - LIST_Y) / ITEM_H;
                    if (row >= 0 && row < (int)app.apks.size()) {
                        // First tap selects (so users can see what they're
                        // about to launch); tapping the already-selected row
                        // again launches it — mirrors physically pressing A
                        // after moving the cursor there with the D-pad.
                        if (row == app.selected)
                            app.launchGame(app.apks[app.selected], &handoff);
                        else
                            app.selected = row;
                    }
                }
            }
        }

        // The update check runs on its own thread; pick the answer up whenever
        // it lands rather than blocking startup on the network. Only a genuine
        // newer release interrupts — a failed check just goes in the log, since
        // "couldn't reach GitHub" is not something to stop someone's session
        // for, and plenty of Switches run permanently offline.
        if (!quit && !handoff && app.netReady && !app.updateSeen) {
            UpdateInfo info;
            if (updateCheckPoll(&info)) {
                app.updateSeen = true;
                app.update     = info;
                if (!info.pendingTag.empty()) {
                    logMsg(("update check: " + info.pendingTag +
                            " is tagged but has no published build yet").c_str());
                }
                if (!info.error.empty()) {
                    logMsg(("update check: " + info.error).c_str());
                } else if (info.available) {
                    logMsg(("update check: " + info.tag + " available (running " +
                                updateCurrentVersion() + ")").c_str());
                    app.showUpdatePrompt();
                    if (app.updateQuitRequested) quit = true;
                } else {
                    logMsg(("update check: up to date (" + info.tag + ")").c_str());
                }
            }
        }

        if (!quit && !handoff) app.render();
        SDL_Delay(16);
    }

    app.cleanup();
    return 0;
}
