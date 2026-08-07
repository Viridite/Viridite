#pragma once
#include <string>

// ─── Achievement catalogue sync ─────────────────────────────────────────────
//
// Fetches the achievement list for one game and leaves it on the card for the
// Core to read while the game runs.
//
// This talks to the ExophaseScraper service — a separate project of ours, at
// https://github.com/aaronateataco/ExophaseScraper — over its documented REST
// endpoint:
//
//     GET {base}/api/v1/game/{slug}/achievements
//
// It is deliberately a *client of that service*, not a second copy of it.
// Nothing here knows what Exophase's HTML looks like, and nothing here will
// break when Exophase changes their markup — that is the service's problem to
// absorb, once, on a server that can be redeployed, rather than a problem
// baked into every Viridite build ever flashed to a card. The same service
// backs other projects, so a fix there fixes all of them at once.
//
// What Viridite depends on is only the JSON shape the service promises. If the
// service is down, the wrong version, or unreachable, the sync fails quietly
// and the game still launches — achievements are a garnish, and a launcher
// that refuses to start a game because a metadata server was busy would be
// getting its priorities wrong.
//
// The base URL is read from sdmc:/Viridite/achievements_api.txt when present,
// so a redeployed service does not need a new Viridite build to follow it.

struct SyncResult {
    bool        ok = false;
    int         count = 0;         // achievements written
    std::string slug;              // the Exophase slug that answered
    std::string error;             // human-readable, for the log and the notice
};

namespace achsync {

// The service's base URL — the file above if it exists, otherwise the built-in
// default. No trailing slash.
std::string apiBase(void);

// Guess the Exophase game slug for an Android package. Exophase slugs are the
// game's *title* plus an "-android" suffix ("hill-climb-racing-android"), not
// the package id, so this slugifies the app label we already parsed out of the
// APK. Callers may override it with a per-package pin (see slugOverride).
std::string slugFor(const std::string& appName);

// A pinned slug for a package, from sdmc:/Viridite/achievements/slugs.txt
// ("<package><TAB><slug>" per line). Returns "" when there is no pin. This is
// the escape hatch for the games whose Exophase title does not slugify to
// their store title, which is a thing that happens and is not worth guessing
// harder about.
std::string slugOverride(const std::string& pkg);

// Fetch and write sdmc:/Viridite/achievements/<pkg>.catalogue plus the icons
// beside it. Skips the network entirely if the catalogue on the card is newer
// than `maxAgeSecs`, so launching the same game repeatedly does not hammer the
// service.
SyncResult sync(const std::string& pkg, const std::string& appName,
                long maxAgeSecs = 7L * 24 * 3600);

}  // namespace achsync
