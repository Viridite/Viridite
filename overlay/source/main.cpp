// ─── Viridite install overlay ───────────────────────────────────────────────
// A Tesla overlay (nx-ovlloader) that draws Viridite's install progress on top
// of whatever is on screen — including the HOME menu, which is the point: a
// game being set up by Viridite should tell you so from the same place a real
// one would, instead of only inside Viridite where you can't see it.
//
// It knows nothing about the loader. The Core rewrites
//   sdmc:/switch/Viridite/install_status.txt
// as it works, and this reads it. Two processes, one small file, no IPC.
//
// On why the card is placed by hand rather than asked for:
// there is no way to ask the HOME menu where a tile is. qlaunch exposes no
// layout information, homebrew can't read its framebuffer, and tile order is
// user-rearrangeable. What is knowable is that a just-installed title sorts to
// the front of the row, so its tile is at a fixed rect — which is what the
// default anchor below is. It's a layout constant, so it lives in a config
// file the user can nudge rather than being baked in.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char kStatusPath[] = "sdmc:/switch/Viridite/install_status.txt";
constexpr char kConfigPath[] = "sdmc:/switch/Viridite/overlay.cfg";

// ── Viridite palette ────────────────────────────────────────────────────────
// Tesla's framebuffer is RGBA4444, so each channel is one nibble. These are the
// launcher's colours reduced to 4-bit: green #34E686, deep ink #0A1F14.
constexpr tsl::Color kGreen   = tsl::Color(0x3, 0xE, 0x8, 0xF);
constexpr tsl::Color kGreenDim= tsl::Color(0x1, 0x6, 0x3, 0xF);
constexpr tsl::Color kInk     = tsl::Color(0x0, 0x1, 0x1, 0xE);
constexpr tsl::Color kInkSolid= tsl::Color(0x0, 0x1, 0x1, 0xF);
constexpr tsl::Color kText    = tsl::Color(0xF, 0xF, 0xF, 0xF);
constexpr tsl::Color kMuted   = tsl::Color(0x9, 0xB, 0xA, 0xF);
constexpr tsl::Color kRed     = tsl::Color(0xE, 0x5, 0x5, 0xF);

struct Status {
    std::string state = "idle";
    std::string pkg, name, stage;
    int         pct = 0;

    bool active() const { return state == "installing"; }
    bool ended()  const { return state == "done" || state == "error"; }
};

// Anchor rect, in 1920x1080 screen coordinates. The default sits over the
// first tile of the HOME menu row — where a freshly installed game lands.
struct Anchor { int x = 300, y = 300, w = 460, h = 190; };

Status readStatus() {
    Status s;
    FILE* f = fopen(kStatusPath, "r");
    if (!f) return s;

    char line[256];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "v1", 2) != 0) {
        // Unknown version: say nothing rather than guess at the fields. A
        // future Core can bump this and older overlays will simply stay quiet.
        fclose(f);
        return s;
    }
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if      (!strcmp(key, "state")) s.state = val;
        else if (!strcmp(key, "pkg"))   s.pkg   = val;
        else if (!strcmp(key, "name"))  s.name  = val;
        else if (!strcmp(key, "stage")) s.stage = val;
        else if (!strcmp(key, "pct"))   s.pct   = atoi(val);
    }
    fclose(f);
    return s;
}

Anchor readAnchor() {
    Anchor a;
    FILE* f = fopen(kConfigPath, "r");
    if (!f) return a;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if      (sscanf(line, "anchor_x=%d", &v) == 1) a.x = v;
        else if (sscanf(line, "anchor_y=%d", &v) == 1) a.y = v;
        else if (sscanf(line, "anchor_w=%d", &v) == 1) a.w = v;
        else if (sscanf(line, "anchor_h=%d", &v) == 1) a.h = v;
    }
    fclose(f);
    return a;
}

// Rounded-rect fill. Tesla's renderer only gives us axis-aligned rects, so this
// is three rects plus four quarter-circles — cheap at this size, and square
// corners on a card that sits over the HOME menu look conspicuously wrong.
void fillRounded(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h, s32 rad,
                 tsl::Color c) {
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    r->drawRect(x + rad, y,           w - rad * 2, h,           c);
    r->drawRect(x,       y + rad,     rad,         h - rad * 2, c);
    r->drawRect(x + w - rad, y + rad, rad,         h - rad * 2, c);
    for (s32 dy = 0; dy < rad; dy++) {
        // Half-pixel centres keep the arc from flattening on the top row.
        float fy  = rad - dy - 0.5f;
        s32   dx  = (s32)(rad - sqrtf((float)rad * rad - fy * fy) + 0.5f);
        s32   run = w - dx * 2;
        if (run <= 0) continue;
        r->drawRect(x + dx, y + dy,             run, 1, c);
        r->drawRect(x + dx, y + h - 1 - dy,     run, 1, c);
    }
}

}  // namespace

// ─── The card ───────────────────────────────────────────────────────────────
class InstallGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        return new tsl::elm::CustomDrawer(
            [this](tsl::gfx::Renderer* r, s32, s32, s32 w, s32 h) { draw(r, w, h); });
    }

    void update() override {
        // Cheap enough at 4Hz; the file is a few hundred bytes and the Core
        // only rewrites it when something actually changed.
        if (++m_tick % 15 == 0) m_status = readStatus();
        m_frame++;
    }

private:
    void draw(tsl::gfx::Renderer* r, s32 w, s32 h) {
        const Status& s = m_status;

        // Card body — inset a little so the shadow ring has somewhere to go.
        const s32 pad = 6;
        const s32 cx = pad, cy = pad, cw = w - pad * 2, ch = h - pad * 2;
        fillRounded(r, cx, cy, cw, ch, 14, kInkSolid);

        // Viridite green rule down the left edge: the one bit of the card that
        // reads as "this isn't the system" at a glance.
        r->drawRect(cx, cy + 14, 4, ch - 28, kGreen);

        if (!s.active() && !s.ended()) {
            r->drawString("Viridite", false, cx + 18, cy + 34, 22, kMuted);
            r->drawString("nothing installing", false, cx + 18, cy + 58, 15, kMuted);
            return;
        }

        const bool failed = (s.state == "error");
        const bool done   = (s.state == "done");

        // Title: the game, not the tool. The word Viridite goes underneath in
        // small caps — this card is about what's happening to your game.
        std::string title = s.name.empty() ? s.pkg : s.name;
        if (title.empty()) title = "Game";
        r->drawString(title.c_str(), false, cx + 18, cy + 36, 23, kText,
                      cw - 36 /*clip, long names are common*/);

        const char* verb = failed ? "Install failed"
                         : done   ? "Ready to play"
                                  : "Installing with Viridite";
        r->drawString(verb, false, cx + 18, cy + 60, 15,
                      failed ? kRed : kGreen);

        // Progress bar
        const s32 bx = cx + 18, bw = cw - 36, by = cy + ch - 46, bh = 8;
        fillRounded(r, bx, by, bw, bh, bh / 2, kGreenDim);
        if (!failed) {
            s32 fw = (s32)(bw * (s.pct / 100.0f));
            if (done) fw = bw;
            if (fw > 0) fillRounded(r, bx, by, fw, bh, bh / 2, kGreen);

            // A sheen sweeping the filled portion, so a long stage that sits on
            // one percentage still looks alive rather than hung.
            if (!done && fw > 12) {
                s32 sweep = (m_frame * 4) % (fw + 60) - 30;
                for (s32 i = 0; i < 30; i++) {
                    s32 px = bx + sweep + i;
                    if (px < bx || px >= bx + fw) continue;
                    u8 alpha = (u8)(0xF * (1.0f - fabsf(i - 15) / 15.0f));
                    r->drawRect(px, by, 1, bh, tsl::Color(0xF, 0xF, 0xF, alpha / 3));
                }
            }
        }

        // Stage caption + percentage on one line under the bar
        char pctbuf[8];
        snprintf(pctbuf, sizeof(pctbuf), "%d%%", s.pct);
        std::string stage = s.stage.empty() ? "Working" : s.stage;
        r->drawString(stage.c_str(), false, bx, by - 8, 14, kMuted, bw - 46);
        if (!failed)
            r->drawString(pctbuf, false, bx + bw - 40, by - 8, 14, kGreen);
    }

    Status m_status;
    u32    m_tick  = 0;
    u32    m_frame = 0;
};

// ─── Overlay ────────────────────────────────────────────────────────────────
class ViriditeOverlay : public tsl::Overlay {
public:
    void initServices() override {
        // Place and size the layer over the anchor rect instead of taking
        // Tesla's default right-hand strip. This is what lets the card sit on
        // the tile rather than in a sidebar; it has to happen before the
        // framebuffer is created, which is why it's here and not in the Gui.
        Anchor a = readAnchor();
        tsl::cfg::LayerPosX = (u16)a.x;
        tsl::cfg::LayerPosY = (u16)a.y;
        tsl::cfg::LayerWidth  = (u16)a.w;
        tsl::cfg::LayerHeight = (u16)a.h;
        // Framebuffer width must be a multiple of 32 for the swizzled layout
        // the renderer writes (see tesla.hpp's tmpPos maths) — round up, and
        // let the layer scale it back to the requested width.
        tsl::cfg::FramebufferWidth  = (u16)((a.w + 31) / 32 * 32);
        tsl::cfg::FramebufferHeight = (u16)a.h;
    }

    void exitServices() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<InstallGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<ViriditeOverlay>(argc, argv);
}
