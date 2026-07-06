#pragma once
#include <cstdint>
#include <vector>

// Static, bundled GitHub avatar for the About screen (romfs:/avatar.png,
// baked into the NRO at build time). No network fetch, no cache file, no
// background thread — the picture never changes at runtime.
void avatarStart();
void avatarStop();

// Call once per frame while the About screen is visible. Returns true and
// fills `out` with the bundled PNG bytes (ready for IMG_Load_RW) exactly
// once after avatarStart(); false every call after that.
bool avatarPollNewImage(std::vector<uint8_t>& out);
