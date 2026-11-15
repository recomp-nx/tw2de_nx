// tw2de (2026-10-25): a language pack's own characters drawn by the game's fonts (tw2de_glyphs.cpp).
#pragma once

#include <cstdint>

namespace tw2de {

// Once the config is loaded (FnxfXiiiLrApp::OnPreSetup): reads the pack's glyphs/glyphs.ini and its PNGs.
void GlyphsInit();
bool GlyphsActive();
// render/textures.cpp Upload, before a DXT1 texture's base level is read: its tiled data in guest memory.
void GlyphsPatchAtlas(uint8_t *data, uint32_t width, uint32_t height);
// The game's code (0x21-0xFF) a character of the pack is written with: a replaced character's, or -1 (not replaced).
int GlyphCode(char32_t c);

}  // namespace tw2de
