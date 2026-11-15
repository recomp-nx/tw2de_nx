// tw2de (2026-10-25): changes to entries of the game's WPD files (.xgr, ...) as their data arrives in guest
// memory: a language pack's font tables, UI layouts, texture headers (tw2de_wpd_patch.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace tw2de {

// Called with an entry's data once all of it has arrived (each time its file is loaded).
using WpdPatchFn = std::function<void(uint8_t *data, uint32_t size)>;

// An entry, by its name and type ("wfnt16t", "wfl"). Before the game's files are read (OnPreSetup).
void WpdPatchRegister(const std::string &name, const std::string &ext, WpdPatchFn fn);
bool WpdPatchActive();
// Guest memory just written from a file (a read, tw2de_texture_sources.cpp) or by an inflate (tw2de_native_zlib.cpp).
void WpdPatchNoteWrite(uint32_t guest_va, uint32_t bytes);

}  // namespace tw2de
