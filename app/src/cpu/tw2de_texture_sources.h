// tw2de (2026-10-25, tw2de_diag_texture_sources): where each texture Renut uploads came from in the game's files
// (tw2de_texture_sources.cpp).
#pragma once

#include <cstdint>

namespace tw2de {

bool TextureSourcesOn();
// Once the config is loaded (FnxfXiiiLrApp::OnPreSetup): starts recording the game's file reads.
void TextureSourcesInit();
// tw2de_native_zlib.cpp: after each inflate call ('total_in' and 'total_out' the stream's counts before it).
void TextureSourcesNoteInflate(uint32_t strm, uint32_t next_in, uint32_t used_in, uint32_t total_in, uint32_t next_out,
                               uint32_t made_out, uint32_t total_out);
// tw2de_native_zlib.cpp: a stream initialised or ended.
void TextureSourcesForgetStream(uint32_t strm);
// render/textures.cpp Upload: logs the texture's source, once per texture.
void TextureSourcesNoteUpload(uint32_t texture_va, uint32_t base, uint32_t bytes, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t format, uint32_t mip_levels, bool tiled, uint64_t frame);

}  // namespace tw2de
