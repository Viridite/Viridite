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

// Never try to build this game's icon again.
//
// Encoding one killed the process once — a hardware log ends mid-sentence
// inside IMG_SaveJPG_RW, with neither its success nor its failure line ever
// written. A crash there happens on the "Scanning for APKs..." screen, so the
// whole-pass lock was skipping every forwarder on alternate launches forever.
// Blaming the one icon instead turns a permanent crash into a game whose
// forwarder has no picture, which is what "a forwarder is a convenience"
// should have meant all along.
void forwarderBlockIcon(const std::string& pkg_name);

// "Fingersoft | Viridite Contributors" — the developer, credited first,
// followed by the people who made it run here. Exposed for the UI.
std::string forwarderAuthor(const std::string& pkg_name);
