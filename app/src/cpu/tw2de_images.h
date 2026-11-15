// tw2de (2026-10-25): a language pack's translated pictures in place of the game's textures (tw2de_images.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace tw2de {

// Once the config is loaded (FnxfXiiiLrApp::OnPreSetup): reads the pack's images/images.ini (the PNGs at their first use).
void ImagesInit();
bool ImagesActive();
// render/textures.cpp Upload, before a one-level texture's data is read: its data in guest memory, Renut's format
// (GpuTextureFormat), size, layout, and pitch in texels. A picture of the texture's size is written there (null
// returned); a larger one's blocks are returned (rows of blocks, little-endian), for the upload to use instead of the
// guest data.
const std::vector<uint8_t> *ImagesPatch(uint8_t *data, uint32_t format, uint32_t width, uint32_t height, bool tiled,
                                        uint32_t pitch_texels);

}  // namespace tw2de
