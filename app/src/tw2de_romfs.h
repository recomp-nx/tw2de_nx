// tw2de (2026-11-06): the port's own files inside the NRO (its romfs: the repository's tex folder as romfs:/tex,
// packed by app/CMakeLists.txt). Not game data (that stays on the SD card, never in the NRO).
// tw2de (2026-11-09): read whole into RAM at the start and unmounted (Preload): libnx reads a mounted romfs from the
// NRO file as files are opened, which kept the NRO open while the game ran (a new NRO could not be copied over it).
#pragma once

#include <cstdint>
#include <vector>

namespace tw2de::romfs {

// Every file of the NRO's romfs into RAM, then the romfs unmounted (the NRO file closed). Once, at the start (the app's
// OnPreSetup); later calls do nothing.
void Preload();

// The bytes of 'relative' (e.g. "tex/controller.png"), from what Preload read; nullptr when there is no such file (an
// NRO built without a romfs, or not on the Switch).
const std::vector<uint8_t> *Find(const char *relative);

}  // namespace tw2de::romfs
