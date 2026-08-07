#pragma once
// ─── Forwarders ─────────────────────────────────────────────────────────────
//
// An installed game should exist on the console as itself, not only as a row
// inside Viridite. Installing one writes a forwarder NRO to
//
//     sdmc:/switch/Viridite Games/<package>.nro
//
// which hbmenu lists with the game's own name, icon and version; selecting it
// goes straight into the game. Uninstalling removes it again, so the folder
// always describes what is actually on the card.
//
// Every forwarder is the same stub binary from romfs:/forwarder.nro. What
// differs is the asset section appended to it — icon, name, author, version —
// and the filename, which is the package name the stub reads back from argv[0].
// So generating one is metadata work, never code generation.

#include <string>

struct ApkInfo;

// Write (or overwrite) the forwarder for a game. Returns false and logs on
// failure; a failed forwarder is never fatal to an install.
bool forwarderWrite(const ApkInfo& apk);

// Remove it. Returns true if it is gone afterwards, including when it was
// never there.
bool forwarderRemove(const std::string& pkg_name);

// Where it lives, for callers that want to check or show the path.
std::string forwarderPath(const std::string& pkg_name);

// Rebuild the forwarders an older build wrote without a picture.
//
// Encoding an icon used to kill the process — three hardware logs end
// mid-sentence inside IMG_SaveJPG_RW — so those games were given icon-less
// forwarders to keep the launcher openable. They encode locally now, but a
// forwarder is only written when one is missing, so the old ones have to go
// for the new ones to appear. Call once at startup.
void forwarderRetryBlockedIcons(void);

// "Fingersoft | Viridite Contributors" — the developer, credited first,
// followed by the people who made it run here. Exposed for the UI.
std::string forwarderAuthor(const std::string& pkg_name);
