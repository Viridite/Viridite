// Viridite forwarder stub.
//
// One of these is written to the SD card for each installed game, so a game
// appears in hbmenu as itself — its own name, its own icon, its own version —
// rather than only existing inside Viridite's list. Selecting it goes straight
// into the game.
//
// The stub is the same binary every time. What differs per game is the asset
// section appended to it (icon, name, author, version) and its filename, which
// is the Android package name. Deriving the package from argv[0] rather than
// embedding it in the code is what lets one prebuilt stub serve every game:
// the launcher only has to rewrite metadata it can already read from the APK,
// never machine code.

#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define CORE_PATH "sdmc:/switch/Viridite/Viridite-Translation-Core-x64.nro"

static void basenameNoExt(const char* path, char* out, size_t cap) {
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;

    snprintf(out, cap, "%s", base);
    char* dot = strrchr(out, '.');
    if (dot && strcasecmp(dot, ".nro") == 0) *dot = '\0';
}

int main(int argc, char** argv) {
    char pkg[128] = {0};
    if (argc > 0 && argv[0]) basenameNoExt(argv[0], pkg, sizeof pkg);

    // No package means the file was renamed to something without one. Falling
    // through to the launcher is better than failing: the player still reaches
    // their games, just via the list.
    char args[512];
    if (pkg[0]) snprintf(args, sizeof args, "\"%s\" %s", CORE_PATH, pkg);
    else        snprintf(args, sizeof args, "\"%s\"", CORE_PATH);

    // Exactly what the launcher does when it hands off. argv[0] must be the
    // Core's real path — libnx's romfsInit() reopens it to find the Core's own
    // embedded romfs, so a package name there would leave the Core without its
    // assets.
    if (R_FAILED(envSetNextLoad(CORE_PATH, args))) {
        // Nothing to hand off to: hbloader was not the thing that started us,
        // or the Core is not installed. Say so rather than closing silently on
        // a black screen.
        consoleInit(NULL);
        printf("\n  Viridite forwarder\n\n");
        printf("  Could not hand off to the Translation Core.\n\n");
        printf("  Expected it at:\n    %s\n\n", CORE_PATH);
        printf("  Reinstall Viridite, or launch this from hbmenu.\n\n");
        printf("  Press + to exit.\n");

        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
    }
    return 0;
}
