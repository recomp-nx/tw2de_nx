// tw2de (package 27c): QueueKickFlush. See tw2de_renut_checks.h.
#include "renut_engine/gpu/tw2de_renut_checks.h"

#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"

namespace tw2de::renut_gpu::checks {

void QueueKickFlush() {
  ::renut::gpu::render_thread::Queue([] {
    namespace rtw = ::renut::gpu::rtw;
    // tw2de (2026-11-05, seen14): the kick item's fills timed for [rtwork] (seen13: in "everything else")
    rtw::Timed(rtw::kKickVertexFills, [] { ::renut::gpu::FlushVertexFillsLocked(); });
    rtw::Timed(rtw::kKickTextureFills, [] { ::renut::gpu::FlushTextureFillsLocked(); });
    ::renut::gpu::Video::KickLocked();  // tw2de (2026-10-19): the ring cleaner and the kick submits
  });
}

}  // namespace tw2de::renut_gpu::checks
