/**
 * @file    gpu/d3d/hooks/device.cpp
 * @brief   Device lifecycle hooks: creation and release, the command ring,
 *          fences, command buffer recording and Swap.
 *
 * The command ring is a stub, as in reblue: m_pRing points at a scratch block
 * that KickOff simply rewinds, and none of the hooked D3D calls writes packets
 * (d3d_native.h). The renderer consumes D3D's API-level state instead (the
 * device's constant/packet shadows and the hooked state-setting and draw calls).
 *
 * The renderer runs on its own thread (render_thread.h). KickOff and Swap
 * queue the end of a batch of work and its fence, which completes once the
 * render thread has read the batch's data; D3D's fence waits wait for that
 * (BlockOnFence_hook), and Swap waits when the game gets a frame too far ahead.
 */
#include <rex/frame_latency.h>  // tw2de (2026-10-19)
#include "renut_engine/display_sync.h"
#include "renut_engine/frame_interval.h"  // tw2de (2026-10-25)
#include "renut_engine/frame_timing.h"    // tw2de (2026-11-05, tw2de_diag_vsync_timing)
#include "renut_engine/gpu/render/pipeline.h"  // tw2de (2026-10-20): SyncPipelineListLocked
#include "renut_engine/gpu/render/dynamic_resolution.h"  // tw2de (2026-11-07)
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <fmt/format.h>  // tw2de (2026-10-24): ReportFenceWaits
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include <rex/hook.h>

#include "renut_engine/gpu/tw2de_renut_tuning.h"  // tw2de: CountFenceQuery
#include "renut_engine/gpu/tw2de_renut_checks.h"  // tw2de (package 27c): QueueKickFlush
#include "renut_engine/gpu/tw2de_renut_select.h"
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ppc.h>
#include <rex/system/xvideo.h>
#include <rex/types.h>

#include "renut_engine/perf/frame_stats.h"
#include "profiler/profiler.h"
#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/memory_report.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"

#include <rex/cvar.h>
#include <rex/ppc/func.h>

// tw2de: the heartbeat's draw counters (render/draw.cpp).
extern std::atomic<uint64_t> g_tw2de_draws, g_tw2de_draws_dropped;
extern std::atomic<uint64_t> g_tw2de_hook_counts[6];  // tw2de (2026-10-26): d3d/hooks/draw.cpp
extern u32 g_tw2de_frame_resolve_dests[8];  // tw2de (2026-11-12): d3d/hooks/draw.cpp
extern u32 g_tw2de_frame_resolves;
void FnxfXiiiLrDumpThreadStacks();  // src/tw2de_stall_dump.cpp

// tw2de (2026-10-20, from sa2-nx): the SDK's Switch core layout (sdk/src/system/xthread.cpp).
extern "C" uint32_t RexSetGuestThreadCoreLayout(int main_core, uint32_t others_mask, int others_preferred);
extern "C" bool RexGuestLayoutCallerIsMain();  // tw2de (2026-10-24)

// tw2de (2026-10-20, from sa2-nx's sa2_renut_core_layout): see D3DDevice_Swap_hook.
// TW2 (2026-11-12): the cores the other guest threads may run on (tw2de_renut_core_layout): 6 = cores 1-2 (as
// before), 7 = all three (core 0 too, which the render thread no longer fills with its idle poll:
// cpu/tw2de_render_frame.cpp tw2de_render_idle_sleep_us).
// TW2 (2026-11-12, stock clocks): 7 by default (see tw2de_render_idle_sleep_us).
REXCVAR_DEFINE_INT32(tw2de_core_layout_others_mask, 7, "Game/Enhancements",
                     "With tw2de_renut_core_layout: the cores (bit mask) of the guest threads other than the one "
                     "that calls Swap (pinned to core 0); 6 = cores 1-2, 7 = all three")
    .range(1, 7);
REXCVAR_DEFINE_BOOL(tw2de_diag_frames, false, "Game/Diagnostics",
                    "A log line a Swap: time since the last, draws, clears/resolves and their destinations, front "
                    "buffer (the black frames, 2026-11-12)");
REXCVAR_DEFINE_BOOL(tw2de_renut_core_layout, true, "Game/Enhancements",
                    "Renut's renderer on the Switch: at the first Swap the guest main thread (N&B's drawing thread, the "
                    "one that calls Direct3D) is pinned to core 0 and every other guest thread gets cores 1-2 "
                    "preferring 2 (the render thread is on core 1 at a higher priority), so no game thread time-slices "
                    "with it (Horizon slices a core's threads of one priority in 10 ms). sa2-nx: render thread 12.5 -> "
                    "11.5 ms and game thread 17.6 -> 17.3 ms with its other items. Off by default from package 36q (in "
                    "Ryujinx it hung the first Swap in 4 of 12 runs: RexSetGuestThreadCoreLayout re-masking running threads "
                    "under the object table's lock); on by default again since 2026-10-25 (the console runs of 2026-10-24/25 "
                    "with it on: no hang)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// tw2de (2026-11-05): see D3DDevice_Swap_hook's present closure.
REXCVAR_DEFINE_BOOL(tw2de_diag_bookkeeping_before_present, false, "Game/Diagnostics",
                    "Diagnostic: the render thread's frame bookkeeping (texture and stream-cache frames, resolution "
                    "latch, memory report) before the frame's present, as before 2026-11-05 (off: after it, while the GPU "
                    "draws the frame)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

using namespace renut::gpu;

bool FnxfXiiiLrLoadFadeHold();  // cpu/tw2de_load_fade_fix.cpp (tw2de_fix_load_fade)

// tw2de (2026-11-05): the render thread's per-frame bookkeeping (was inline before the present). Requires
// State().mutex.
static void FrameBookkeepingLocked() {
  namespace ft = tw2de::frame_timing;
  DrsFrameStartLocked();  // tw2de (2026-11-07): the frame's dynamic resolution, before FSR 3.1's jitter (below)
  PipelineFrameReportLocked(TextureFrameLocked());  // tw2de (2026-11-05): the frame just ended ([pipes])
  RenderWorkReportLocked(TextureFrameLocked(), render_thread::BusyMsThisFrame());  // tw2de (2026-11-05): [rtwork]
  AdvanceTextureFrameLocked();  // its parts timed inside (textures.cpp)
  AdvanceStreamCacheFrameLocked();  // likewise (draw.cpp)
  ft::PartScope part(ft::kPartLatch);
  LatchResolutionScaleLocked();
}

// Kernel exports the device code uses.
REX_IMPORT(__imp__rex_GetCurrentThreadId, GuestGetCurrentThreadId, u32());
REX_IMPORT(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection, void(u32));
REX_IMPORT(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency, u32());
REX_IMPORT(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType, u32());
REX_IMPORT(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma, void(u32, u32));

namespace {

// Scratch space packets go to: the inline writers in N&B's own code (D3D's
// header helpers), which reserve against m_pRingGuarantee. Large enough for
// the biggest single reservation; the guarantee margin is what inline writers
// assume is free without calling KickOff.
constexpr u32 kRingSinkSize = 4u << 20;
constexpr u32 kRingSinkGuaranteeMargin = 256u << 10;
// Scratch handed out for inline data (RingBufferDeviceAllocate), reused in a
// circle: nothing reads it either.
constexpr u32 kRingDataSize = 1u << 20;

// CDevice-private state Direct3D_CreateDevice (0x8264DF28) sets up, as byte
// offsets into the device allocation.
// Byte whose 0x80 bit marks a device without GPU hardware (its device-type-2
// path sets it); D3D's code skips hardware work for it.
constexpr u32 kDeviceOffsetFlags = 11068;
constexpr u8 kDeviceFlagNoHardware = 0x80;
// Byte whose 0x2 bit makes CDevice::SetFence (0x82278528) write each fence's
// completion value itself instead of waiting for the GPU to. The title device
// runs without it once created: its fences complete on the render thread.
constexpr u32 kDeviceOffsetFlags2 = 11069;
constexpr u8 kDeviceFlagImmediateFences = 0x02;
// Pointer to the completed-fence word IsFencePending / BlockOnFence read.
constexpr u32 kDeviceOffsetFenceCompletion = 11024;
// Nonzero while a KickOff is in progress (BlockOnFence does not kick then).
constexpr u32 kDeviceOffsetInKickOff = 13600;  // tw2de: XDK 20871 (tools/d3d_offsets_20871.py; N&B 13232)
// Side buffer (0x12C0 bytes) every device gets.
constexpr u32 kDeviceOffsetSideBuffer = 17136;  // tw2de (N&B 16728)
constexpr u32 kSideBufferSize = 0x12C0;
// Set to -1 by the device-type-2 path.
constexpr u32 kDeviceOffsetsMinusOne[] = {21968, 21972, 15340};  // tw2de (N&B 21560, 21564, 14936)
constexpr u32 kDeviceTypeNoHardware = 2;
// Copied from the global qword at 0x82F39B38 into every device.
constexpr u32 kDeviceOffsetGlobalQword = 11008;
constexpr u32 kGlobalQwordVa = 0x83555F38;  // TW2 (patched image, the constructor's ld at +0x20); LR 0x82F4C8F0 (N&B 0x82F39B38)
// Hardware-path fields: two critical sections, QueryPerformanceFrequency and
// its reciprocal as floats, and 41 words set to -1.
constexpr u32 kDeviceOffsetCriticalSection0 = 15348;  // tw2de (N&B 14944)
constexpr u32 kDeviceOffsetCriticalSection1 = 15376;  // tw2de (N&B 14972)
constexpr u32 kDeviceOffsetPerfFrequency = 21996;  // tw2de (N&B 21588)
constexpr u32 kDeviceOffsetTitleMinusOne = 23788;  // tw2de (N&B 23380)
// Import slot holding the address of the kernel's VdGlobalDevice variable.
constexpr u32 kVdGlobalDeviceImportSlot = 0x82000700;  // TW2 (all 27 uses in the matched D3D functions); LR 0x82000A70 (N&B 0x820007FC)

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;

u32 g_ring_sink = 0;
}  // namespace
REXCVAR_DECLARE(bool, tw2de_diag_log);
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-24): sdk/src/system/xobject.cpp
REXCVAR_DEFINE_BOOL(tw2de_ring_scan, false, "Game/Renut",
                    "tw2de diagnostic: log the GPU packets the game's code writes into the ring stub itself (work the "
                    "renderer does not see: [ring] lines)");
namespace {
u32 g_ring_data = 0;
std::atomic<u32> g_ring_data_offset{0};

// Device blocks of released devices, for the next Direct3D_CreateDevice (the
// records heap never frees physical blocks). N&B creates and releases a pair
// of no-hardware devices around every section it loads.
std::mutex g_free_devices_mutex;
std::vector<u32> g_free_devices;

void WriteU32(u8 *device_bytes, u32 offset, u32 value) {
  *reinterpret_cast<be_u32 *>(device_bytes + offset) = value;
}

u32 LoadGuestU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// The newest fence queued for completion (the title device's).
std::atomic<u32> g_queued_fence{0};

// What CDevice::SetFence (0x82278528) does at a KickOff: the current fence
// covers the work issued so far, and the device moves on to the next. The
// fence completes when the render thread has read that work's data (and
// written its memexport results): until then the game must not reuse it.
// Without completing no resource tagged with a fence would stop being busy.
void KickFence(u8 *bytes) {
  const u32 fence = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  WriteU32(bytes, d3d::kDeviceOffsetFenceTag, fence + 2);
  g_queued_fence.store(fence, std::memory_order_release);
  render_thread::QueueFenceCompletion(completion, fence, LastCallback());
}

// IsFencePending's test.
bool FencePending(const u8 *bytes, u32 fence) {
  const u32 tag = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  const auto *word = GuestPtr<const be_u32>(completion);
  return word && tag - fence < tag - u32(*word);
}

// ---------------------------------------------------------------------------
// Device initialisation (N&B's D3D, natively)
// ---------------------------------------------------------------------------

#if 0  // tw2de: N&B's device initialisation, replaced by the game's own code (Direct3D_CreateDevice_hook)
// D3D::XenonGpu_Initialize (0x82663208): the register image's power-on
// values, everything dirty. (Its register writes go into the ring.)
void XenonGpuInitialize(native::Device d) {
  d.SetQ(0x2E30, 0x00000000FFFFFFFFull);
  for (u32 i = 0; i < 26; ++i)  // texture fetch constants: type 0
    d.SetW(native::off::TextureFetch + i * 24, d.W(native::off::TextureFetch + i * 24) & ~3u);
  for (u32 i = 0; i < 18; ++i) {  // vertex fetch constants: type 1
    const u32 offset = native::off::VertexFetch + i * 8;
    d.SetW(offset, (d.W(offset) & ~3u) | 1);
  }
  d.SetW(0x28BC, 0x20002000);
  d.SetW(0x29D4, 0xFF000);
  d.SetW(0x2A14, 0x10);
  d.SetW(native::off::ClipControl, d.W(native::off::ClipControl) | 0x80000);
  d.SetW(0x2984, 0xE);
  d.SetW(0x2A10, 0xE);
  d.SetW(0x296C, 8);
  d.SetW(0x29D8, 0xFF100);
  d.SetW(0x2954, 4);
  // PA_SU_SC_MODE_CNTL vtx_window_offset_enable: one-tile draws are in screen
  // space (see draw.cpp, ComputeViewport).
  d.SetW(native::off::ModeControl, d.W(native::off::ModeControl) | 0x10000);
  d.SetW(0x28CC, 0xFFFFFF);
  d.SetW(0x29C0, 4);
  d.SetW(0x2A48, 2);
  for (u32 word = 0; word < 5; ++word)
    d.SetPending(word, ~u64(0));
  d.SetW(0x2B24, 0xE);
}

// D3D::InitializeApiState (0x826597C8).
void InitializeApiState(native::Device d) {
  const u32 present_interval = d.W(0x351C);
  const u32 behavior_flags2 = d.W(0x5DAC);
  native::InitializeStateTables(d);
  d.SetW(0x2FA0, 5);
  d.SetW(0x2FA4, 1);
  native::SetScreenExtentQueryMode(d, 2);  // D3DSEQM_PRECLIP
  d.SetW(0x3090, 2);
  d.SetW(0x2FD8, 0x2A23B9);
  d.SetH(0x2FD4, 0);
  d.SetH(0x2FD6, 0);
  d.SetB(0x2FDC, 0);
  d.SetB(0x2FDD, 0);
  d.SetB(0x2FDE, 0);
  d.SetW(0x2FE4, ~0u);
  // Three zero bytes; N&B leaves the fourth from its stack.
  d.SetW(0x2FE8, 0);
  d.SetW(0x2FE0, 0x00FF0000);
  d.SetW(0x3084, 1);
  d.SetW(0x2FBC, 0);
  d.SetW(0x2FD0, 0);
  // The default vertex declaration is the device's own, at +0x2FA0.
  d.SetW(0x2E2C, d.va + 0x2FA0);
  d.Dirty(2, 0x80000);
  d.SetW(0x351C, present_interval);
  const u32 flags2 = d.W(0x5DAC);
  d.SetW(0x5DAC, (behavior_flags2 & 1) ? (flags2 | 1) : (flags2 & ~1u));
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  if (!(d.B(native::off::Flags) & native::kFlagsNoHardware))
    native::SetShaderGPRAllocation(d, 0, 0, 0);
}

// sub_8265F688: the scaler parameters (D3DPRESENT_PARAMETERS +0x60) the
// device keeps. N&B's code then derives the default scaling for the video
// mode and passes it to VdCallGraphicsNotificationRoutines: the display's.
void SetScalerParameters(native::Device d, const be_u32 *scaler) {
  u32 copy[14] = {};
  for (u32 i = 0; i < 6; ++i)
    copy[i] = scaler[i];
  copy[6] = copy[10] = scaler[6] ? 3 : 0;
  for (u32 i = 0; i < 14; ++i)
    d.SetW(0x3660 + i * 4, copy[i]);
  d.SetB(native::off::Flags2, d.B(native::off::Flags2) | 0x10);
}

void SetGammaRamp(native::Device d, const u16 *ramp);
void SetPWLGamma(native::Device d, const u16 *pwl);

// BOOL D3D::InitializePresentationParameters(CDevice*, D3DPRESENT_PARAMETERS*)
// [0x82659AA8]: the video mode, the automatic back/depth buffers and the
// presentation state. N&B's parameters (sub_823ED9E8) disable the automatic
// front buffer and depth buffer and ask for the back buffer.
bool InitializePresentationParameters(native::Device d, u32 parameters_va) {
  const auto *p = GuestPtr<const be_u32>(parameters_va);
  if (!p)
    return false;
  const u32 width = p[0];
  const u32 height = p[1];
  const u32 back_format = p[2];
  const u32 multisample = p[4];
  const u32 depth_format = p[10];
  u32 front_format = p[16];
  // The front buffer format, as the scanout reads it.
  if (front_format == 0 || front_format == 0x28280186u)
    front_format = 0x28280106u;
  else if (front_format == 0x18280186u)
    front_format = 0x18280106u;
  else if (front_format == 0x282801B6u)
    front_format = 0x28280136u;
  else if (front_format == 0x182801B6u)
    front_format = 0x18280136u;

  rex::system::X_VIDEO_MODE mode{};
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  d.SetW(0x5428, u32(mode.display_width));
  d.SetW(0x5430, u32(mode.display_width));
  d.SetW(0x5434, u32(i64(float(mode.refresh_rate) + 0.5f)));
  d.SetW(0x542C, u32(mode.display_height));

  if (!p[15])  // DisableAutoFrontBuffer
    RGPU_ERROR("InitializePresentationParameters: an automatic front buffer is not supported");
  if (!p[14]) {  // DisableAutoBackBuffer
    const u32 surface = native::CreateSurface(width, height, back_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoBackBuffer, surface);
    native::SetRenderTarget(d, 0, surface);
  }
  if (p[9]) {  // EnableAutoDepthStencil
    const u32 surface = native::CreateSurface(width, height, depth_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoDepthStencil, surface);
    native::SetDepthStencilSurface(d, surface);
  }
  for (u32 i = 0; i < 0x7C / 4; ++i)
    d.SetW(0x35A8 + i * 4, p[i]);
  if ((front_format & 0x3FFC0000u) != 0x24900000u)
    d.SetW(0x3528, (front_format & 0x3F) | 0x28280100u);
  SetScalerParameters(d, p + 0x60 / 4);
  // The default gamma: linear, as a piecewise-linear ramp for the front
  // buffer formats 7 and 0x36 (0x32 counts as 6).
  u32 gamma_format = front_format & 0x3F;
  if (gamma_format == 0x32)
    gamma_format = 6;
  else if (gamma_format == 7)
    gamma_format = 0x36;
  u16 ramp[768];
  if (gamma_format == 7 || gamma_format == 0x36) {
    for (u32 k = 0; k < 128; ++k) {
      const u16 base = u16((0xFFFFu * k) / 0x7F);
      const u16 delta = u16((0xFFFFu * (k + 1)) / 0x7F - base);
      for (u32 c = 0; c < 3; ++c) {
        ramp[c * 256 + k * 2] = base;
        ramp[c * 256 + k * 2 + 1] = delta;
      }
    }
    SetPWLGamma(d, ramp);
  } else {
    for (u32 i = 0; i < 256; ++i) {
      const u16 value = u16((((0x3FFu * i) / 0xFF) & 0x3FF) << 6);
      ramp[i] = ramp[256 + i] = ramp[512 + i] = value;
    }
    SetGammaRamp(d, ramp);
  }
  d.SetW(0x4144, ~0u);
  return true;
}

// The hardware path (D3D__InitializeHardwareDevice 0x82659F30) minus the GPU:
// the device becomes the one D3D internals find through VdGlobalDevice, and
// gets the presentation state and per-device fields they read.
bool InitializeTitleDevice(native::Device d, u32 present_parameters) {
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection0);
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection1);
  d.SetW(16712, 0);

  // D3D reads the device through the kernel's VdGlobalDevice from any thread
  // (resource locks, blockers).
  if (const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot))
    *GuestPtr<be_u32>(global) = d.va;

  const float frequency = float(KeQueryPerformanceFrequency());
  const float period = frequency > 0.0f ? 1.0f / frequency : 0.0f;
  d.SetF(kDeviceOffsetPerfFrequency, frequency);
  d.SetF(kDeviceOffsetPerfFrequency + 4, period);
  for (u32 i = 0; i < 41; ++i)
    d.SetW(kDeviceOffsetTitleMinusOne + i * 4, 0xFFFFFFFFu);

  return !present_parameters || InitializePresentationParameters(d, present_parameters);
}

// D3D's CDevice teardown (0x8265A198) for our devices: they carry the
// no-hardware flag, which skips everything but unbinding. The side buffer is
// part of our device block, and N&B never gives a device a query block.
#endif
void UninitializeDevice(native::Device d);

// HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void *pUnused,
//     DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentParameters,
//     IDirect3DDevice9 **ppReturnedDeviceInterface)   [the game 0x829CD638, XDK 2.0.9328]
// tw2de: the game's Direct3D initialises the device itself, in its "no hardware" mode (the device-type-2 path of
// the original: Flags bit 0x80), which skips every GPU access; only the hardware path's CPU-side state is added here.
// What the original does, in order (sub_82D07C58):
//   alloc 0x5F00 (sub_826CEB18) -> constructor sub_82D1D938 (ref count, global qword, thread id, title process,
//   side buffer) -> type 2: Flags |= 0x80, sub_82D14148 (ring = side buffer, Flags1 |= 0x20), sub_826D5BB8
//   (minus ones, XenonGpu_Initialize sub_82D1ECF8, InitializeApiState sub_82D1D280); otherwise
//   InitializeHardwareDevice sub_826D59A0 (critical sections, VdGlobalDevice, ring buffer, query blocks,
//   XenonGpu_Initialize, InitializePresentationParameters sub_82D1DC50, InitializeApiState, KickOff, the
//   performance frequency, D3D_ResetAllState sub_82D0BEF8).
REX_EXTERN(__imp__sub_82D1D938);  // CDevice constructor
REX_EXTERN(__imp__sub_82D14148);  // ring = side buffer
REX_EXTERN(__imp__sub_82D1ECF8);  // D3D::XenonGpu_Initialize
REX_EXTERN(__imp__sub_82D1DC50);  // D3D::InitializePresentationParameters
REX_EXTERN(__imp__sub_82D1D280);  // D3D::InitializeApiState
REX_EXTERN(__imp__sub_82D0BEF8);  // D3D::ResetAllState(CDevice*, BOOL)
REX_EXTERN(__imp__sub_82D07928);  // D3DDevice_UnsetAll
REX_EXTERN(__imp__sub_82D22C68);  // D3DDevice_SetPredication

// tw2de: the vertical blank. With no GPU there is no graphics interrupt (VdSetGraphicsInterruptCallback is a
// no-op without the SDK's graphics system), so a guest thread runs the game's own vblank handler (sub_82D16430, what
// its interrupt callback sub_826C9E08 calls for a vblank) at 60 Hz: it counts vblanks, times them and calls the
// game's vertical blank callback (D3DDevice_SetVerticalBlankCallback, device +16752). Its flip queue (+16908 /
// +16912) stays empty, because the hooked Swap presents itself, so it never touches the GPU registers.
REX_EXTERN(__imp__sub_82D16430);
REXCVAR_DEFINE_INT32(tw2de_heartbeat_seconds, 5, "tw2de",
                     "Log swaps, draws and vblanks every N seconds while the renut renderer runs (0 = off)");
REXCVAR_DEFINE_BOOL(tw2de_stall_dump, true, "tw2de",
                    "PC: when no frame is presented for two heartbeats, log every thread's stack once (needs tw2de.pdb)");
// tw2de (2026-10-29, diagnostic): the game's vblanks from a 60 Hz timer of their own, as before 2026-10-27, instead
// of the display's refresh (A/B for the uneven frames of the prerendered movies).
REXCVAR_DEFINE_BOOL(tw2de_diag_vblank_timer, false, "Game/Diagnostics",
                    "tw2de diagnostic (2026-10-29): the game's vblanks from a 60 Hz timer, as before 2026-10-27, "
                    "not from the display's refresh");
// tw2de (2026-11-05): see StartVblankPump.
REXCVAR_DEFINE_INT32(tw2de_diag_vblank_lead_ms, 10, "Game/Diagnostics",
                     "Diagnostic: how many ms before the display's refresh the game's vblank comes (0: at the refresh, as "
                     "before 2026-11-05)");
std::atomic<uint64_t> g_tw2de_swaps{0}, g_tw2de_vblanks{0};

// tw2de (2026-11-10, FF XIII-2): the GPU callbacks (D3DDevice_InsertCallback) on a guest thread of their own as the
// render thread reaches them, as the console's GPU interrupt runs them. Before, only a guest thread entering a hook ran
// them (BlockOnFence, IsFencePending, Swap, draws), which FF XIII always did. FF XIII-2's frame (sub_82910538) inserts a
// callback that sets an event (sub_82A84D88: callback 828878C8, then a fence and a kick) and then waits on that event
// itself (sub_82A84DF8, KeWaitForSingleObject, no timeout): nobody entered a hook again, and the game stopped after
// its second frame (2026-11-10, rex_stall.log). Off: as before.
REXCVAR_DEFINE_BOOL(tw2de_renut_callback_thread, true, "Game/Enhancements",
                    "Renut's renderer: the game's GPU callbacks run on a thread of their own as soon as the GPU work "
                    "before them is done (FF XIII-2 waits for one without calling Direct3D again)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
void StartCallbackThread(rex::system::KernelState *kernel) {
  if (!REXCVAR_GET(tw2de_renut_callback_thread))
    return;
  auto *thread = new rex::system::XHostThread(kernel, 64 * 1024, 0, []() {
    for (;;) {
      // Asleep until a callback is inserted; then woken by the render thread's progress (a fence completed or a
      // callback reached) until none is left to run.
      if (!WaitForCallbacks(std::chrono::milliseconds(100)))
        continue;
      if (!render_thread::WaitForFenceProgress(std::chrono::milliseconds(4)))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      RunReadyCallbacks();
    }
    return 0;
  });
  thread->set_name("tw2de GPU callbacks");
  thread->Create();
  RGPU_INFO("[callbacks] the game's GPU callbacks run on a thread of their own (tw2de_renut_callback_thread)");
}
}  // namespace

void StartVblankPump(u32 device_va) {
  static bool started = false;
  if (started)
    return;
  started = true;
  auto *kernel = rex::system::kernel_state();
  if (!kernel)
    return;
  StartCallbackThread(kernel);
  // tw2de (2026-10-27): the vblanks follow the display's refresh (its vsync event: display_sync.cpp WaitRefresh),
  // not a 60 Hz timer of our own. The game's limiter wakes its main thread at a vblank and the image shows at the next
  // refresh after it is done; on a timer, its phase to the real refresh slid as the two clocks drifted, so at 30 fps a
  // frame finished just after a refresh waited almost one more, and now and then one stayed three refreshes and the
  // next one (uneven frames). With no refresh for 1.5 periods (the display asleep, no vsync event) the timer ticks
  // instead, until a refresh comes again.
  tw2de::display_sync::Start();
  auto *thread = new rex::system::XHostThread(kernel, 64 * 1024, 0, [device_va]() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000LL / 60);
    auto next = clock::now() + period;
    uint64_t seen = tw2de::display_sync::WaitRefresh(UINT64_MAX, 0);
    bool following = false;  // refreshes came lately
    for (;;) {
      const auto now = clock::now();
      // tw2de (2026-10-29): tw2de_diag_vblank_timer: no refresh is ever seen, the timer below ticks.
      const bool timer_only = REXCVAR_GET(tw2de_diag_vblank_timer);
      if (timer_only)
        following = false;
      const uint64_t count = timer_only ? seen
                                        : tw2de::display_sync::WaitRefresh(
                                              seen, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                 (following ? now + period * 3 / 2 : std::max(next, now)) -
                                                                 now)
                                                                 .count()));
      if (count != seen) {
        seen = count;  // a refresh (two counted at once after a late wake: one vblank, as the display shows one image)
        following = true;
        next = clock::now() + period;
        // tw2de (2026-11-05): the game's vblank a little before the next refresh, not at this one. The game's limiter
        // starts a frame at a vblank and the display can show it 3 refreshes later (50 ms at 30 fps): in heavy scenes
        // its CPU frame (~44 ms) and the GPU's work left after the Swap (~11 ms) passed that by 3-8 ms, and every
        // second or third frame stayed a refresh longer (tw2de_diag_vsync_timing run 2c391440, 12:04: 39 drops in
        // 10 s, all "late: GPU" by +0.3..+8 ms). Starting the frame 10 ms before the refresh gives it 60 ms; the frames
        // still come every 2 refreshes, locked to the display (the time comes from its refresh), 10 ms more latency.
        const int lead_ms = std::clamp(REXCVAR_GET(tw2de_diag_vblank_lead_ms), 0, 15);
        if (lead_ms > 0)
          std::this_thread::sleep_until(next - std::chrono::milliseconds(lead_ms));
      } else {
        following = false;
        if (clock::now() < next)
          std::this_thread::sleep_until(next);
        next += period;
        if (clock::now() > next + period * 4)
          next = clock::now() + period;  // fell behind (debugger, suspend): no burst of vblanks
      }
      if (Video::GuestDevice() == device_va) {
        tw2de::frame_interval::NoteGameVblank();  // tw2de (2026-11-05, seen17): StartLateFrameNow's next vblank
        rex::ppc::GuestToHostFunction<void>(__imp__sub_82D16430, device_va);
        g_tw2de_vblanks.fetch_add(1, std::memory_order_relaxed);
        // tw2de (2026-11-05, tw2de_diag_vsync_timing): the device's vblank counter (+16756) after the handler.
        if (tw2de::frame_timing::On())
          tw2de::frame_timing::NoteVblank(u32(*GuestPtr<const be_u32>(device_va + 16952)));
      }
      const int beat = REXCVAR_GET(tw2de_heartbeat_seconds);
      static auto last_beat = clock::now();
      static uint64_t last_swaps = 0, last_draws = 0, last_dropped = 0, last_vblanks = 0;
      if (beat > 0 && REXCVAR_GET(log_diagnostics) && clock::now() - last_beat >= std::chrono::seconds(beat)) {
        last_beat = clock::now();
        const uint64_t swaps = g_tw2de_swaps.load(), draws = g_tw2de_draws.load(),
                       dropped = g_tw2de_draws_dropped.load(), vblanks = g_tw2de_vblanks.load();
        RGPU_INFO("[tw2de] last {} s: {} swaps, {} draws ({} dropped), {} vblanks; totals {} swaps {} draws", beat,
                  swaps - last_swaps, draws - last_draws, dropped - last_dropped, vblanks - last_vblanks, swaps,
                  draws);
        // tw2de (2026-10-26, diagnostic): the game's calls as the hooks saw them (totals).
        uint64_t h[6];
        for (int i = 0; i < 6; ++i)
          h[i] = g_tw2de_hook_counts[i].load(std::memory_order_relaxed);
        RGPU_INFO("[tw2de] hooks so far: {} draws ({} skipped, {} recorded), {} clears and resolves ({} skipped, {} "
                  "recorded); renderer ready {}", h[0], h[1], h[2], h[3], h[4], h[5], Video::HostReady());
        // No frame for two beats in a row after the game has presented: log every thread's stack, once.
        static int quiet_beats = 0;
        static bool dumped = false;
        quiet_beats = (swaps == last_swaps && swaps > 0) ? quiet_beats + 1 : 0;
        if (quiet_beats >= 2 && !dumped && REXCVAR_GET(tw2de_stall_dump)) {
          dumped = true;
          FnxfXiiiLrDumpThreadStacks();
        }
        last_swaps = swaps; last_draws = draws; last_dropped = dropped; last_vblanks = vblanks;
      }
    }
    return 0;
  });
  thread->set_name("tw2de vblank");
  thread->Create();
}

u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 device_type, u32 /*unused*/,
                               u32 behavior_flags, u32 present_parameters,
                               mapped_u32 out_device) {
  if (out_device)
    *out_device = 0u;

  if (!records::Init())
    return kE_OUTOFMEMORY;

  // The device and its fence word share one block of our own guest memory (64 KiB aligned).
  static_assert(d3d::kDeviceAllocSize % d3d::kDeviceAlignment == 0);
  constexpr u32 kFenceOffset = d3d::kDeviceAllocSize + ((kSideBufferSize + 0x7F) & ~0x7Fu);
  constexpr u32 kBlockSize = kFenceOffset + 0x80;
  u32 block_va = 0;
  {
    std::lock_guard lock(g_free_devices_mutex);
    if (!g_free_devices.empty()) {
      block_va = g_free_devices.back();
      g_free_devices.pop_back();
    }
  }
  if (!block_va)
    block_va = records::AllocPhysicalBlock(kBlockSize);
  const u32 device_va = block_va;
  const u32 fence_va = block_va ? device_va + kFenceOffset : 0;
  if (!g_ring_sink)
    g_ring_sink = records::AllocPhysicalBlock(kRingSinkSize);
  if (!g_ring_data)
    g_ring_data = records::AllocPhysicalBlock(kRingDataSize);
  if (!device_va || !g_ring_sink || !g_ring_data) {
    RGPU_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return kE_OUTOFMEMORY;
  }
  std::memset(GuestPtr<u8>(device_va), 0, kBlockSize);
  std::memset(GuestPtr<u8>(g_ring_sink), 0, kRingSinkSize);

  native::Device d = native::DeviceAt(device_va);
  auto *bytes = d.bytes;
  const bool title_device = device_type != kDeviceTypeNoHardware;

  // The original constructor: reference count 1, the global qword, the creating thread, the title-process flag and
  // the side buffer (from the game's heap).
  if (!rex::ppc::GuestToHostFunction<u32>(__imp__sub_82D1D938, device_va)) {
    RGPU_ERROR("Direct3D_CreateDevice: the device constructor failed");
    return kE_OUTOFMEMORY;
  }
  WriteU32(bytes, kDeviceOffsetFenceCompletion, fence_va);
  if (title_device) {
    if (!(behavior_flags & 0x100) && !(behavior_flags & 0x3F000000))
      behavior_flags |= 0xC000000;
    WriteU32(bytes, d3d::kDeviceOffsetBehaviorFlags, behavior_flags);
  }
  for (u32 offset : kDeviceOffsetsMinusOne)
    if (!title_device || offset != kDeviceOffsetsMinusOne[2])
      WriteU32(bytes, offset, 0xFFFFFFFFu);
  // Both kinds run without GPU hardware, and fences complete as they are written until the device is set up.
  bytes[kDeviceOffsetFlags] |= kDeviceFlagNoHardware;
  bytes[kDeviceOffsetFlags2] |= kDeviceFlagImmediateFences;
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D14148, device_va);
  native::RewindRing(d);

  if (title_device) {
    // The CPU side of InitializeHardwareDevice (sub_826D59A0).
    RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection0);
    RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection1);
    d.SetB(native::off::Flags2, d.B(native::off::Flags2) | 4);
    d.SetW(17120, 0);  // sub_826D5898
    if (const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot))
      *GuestPtr<be_u32>(global) = d.va;
  }
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D1ECF8, device_va);
  if (title_device && present_parameters &&
      !rex::ppc::GuestToHostFunction<u32>(__imp__sub_82D1DC50, device_va, present_parameters)) {
    RGPU_ERROR("Direct3D_CreateDevice: InitializePresentationParameters failed");
    return kE_OUTOFMEMORY;
  }
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D1D280, device_va);
  if (title_device) {
    const float frequency = float(KeQueryPerformanceFrequency());
    d.SetF(kDeviceOffsetPerfFrequency, frequency);
    d.SetF(kDeviceOffsetPerfFrequency + 4, frequency > 0.0f ? 1.0f / frequency : 0.0f);
    for (u32 i = 0; i < 41; ++i)
      d.SetW(kDeviceOffsetTitleMinusOne + i * 4, 0xFFFFFFFFu);
    rex::ppc::GuestToHostFunction<void>(__imp__sub_82D0BEF8, device_va, 0u);
  }
  native::RewindRing(d);
  if (title_device)
    bytes[kDeviceOffsetFlags2] &= u8(~kDeviceFlagImmediateFences);

  if (out_device)
    *out_device = device_va;
  if (title_device) {
    Video::SetGuestDevice(device_va);
    StartVblankPump(device_va);
  }
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Guest D3D device (type {}) created at {:#010x}", device_type, device_va);
  return 0;  // S_OK
}

void UninitializeDevice(native::Device d) {
  if (d.W(native::off::Ring))
    rex::ppc::GuestToHostFunction<void>(__imp__sub_82D07928, d.va);  // D3DDevice_UnsetAll
}

// ULONG D3DDevice_Release(D3DDevice*) [0x8264E080]
// As the original, but on the last release the device block goes back to our
// free list instead of N&B's heap (the original frees the side buffer and the
// raw allocation Direct3D_CreateDevice kept at device - 4).
u32 D3DDevice_Release_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  const u32 count = device->m_ReferenceCount;
  if (count != 1) {
    device->m_ReferenceCount = count - 1;
    return count - 1;
  }
  FlushDeferredVertices(device_va);
  UninitializeDevice(native::DeviceAt(device_va));
  device->m_ReferenceCount = 0u;
  if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Guest D3D device at {:#010x} released", device_va);
  // The title device stays the renderer's until another replaces it.
  if (device_va != Video::GuestDevice()) {
    std::lock_guard lock(g_free_devices_mutex);
    g_free_devices.push_back(device_va);
  }
  return 0;
}

// The GPU starts on the kicked draws: the guest buffers they read are read
// then (FlushVertexFillsLocked), before the fence tells the game it may reuse
// them. Memexport results reach guest memory later, once the GPU has finished
// the frame (detail::CompleteMemExportsLocked); draws that read them sooner
// wait for them (DrawStateLocked).
void KickDevice(d3d::Device *device, u32 device_va) {
  FlushDeferredVertices(device_va);
  // tw2de (package 27c): the texture fills too (they were read at the submit, after this fence had let the game
  // rewrite them).
  tw2de::renut_gpu::checks::QueueKickFlush();
  KickFence(reinterpret_cast<u8 *>(device));
}

// PRING CDevice::KickOff() [0x82210AB0]: hand the ring to the GPU. Nothing
// consumes it here, so rewind it and return the new write pointer. A device
// recording a command buffer (N&B records on worker threads with no-hardware
// devices) would chain its segment into the buffer; its draws are recorded on
// the host (command_buffers.h), so it only rewinds.
u32 KickOff_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  if (!IsRecordingCommandBuffer(device_va)) {
    RunReadyCallbacks();
    KickDevice(device, device_va);
  }
  native::RewindRing(native::DeviceAt(device_va));
  return device->m_pRing;
}

// void CDevice::KickOffSegment() [0x822106C8]
void KickOffSegment_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return;
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(device, device_va);
  native::RewindRing(native::DeviceAt(device_va));
}

// PRING CDevice::BeginRingBig(DWORD Count) [0x821F7C18]: room for Count
// dwords, kicking when the segment is full.
u32 BeginRingBig_hook(u32 device_va, u32 count) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  if (d.W(native::off::Ring) + count * 4 > d.W(native::off::RingLimit))
    native::RewindRing(d);
  return d.W(native::off::Ring);
}

// PRING CDevice::BeginRingManual() [0x82A11158]
u32 BeginRingManual_hook(u32 device_va) {
  const native::Device d = native::DeviceAt(device_va);
  return d ? d.W(native::off::Ring) : 0;
}

// Inline data space (vertex data of inline draws, clear rects): scratch that
// nothing reads, handed out in a circle.
u32 RingData(u32 bytes, u32 alignment) {
  if (!g_ring_data || bytes > kRingDataSize / 2)
    return 0;
  if (alignment < 4 || (alignment & (alignment - 1)))
    alignment = 4;
  u32 offset = g_ring_data_offset.fetch_add(bytes + alignment, std::memory_order_relaxed);
  offset %= kRingDataSize / 2;
  offset = (offset + alignment - 1) & ~(alignment - 1);
  return g_ring_data + offset;
}

// void *D3D::RingBufferDeviceAllocate(CDevice*, DWORD Flags, DWORD *pSize,
//     DWORD Alignment) [0x8222D3E0]
u32 RingBufferDeviceAllocate_hook(u32 /*device*/, u32 /*flags*/, mapped_u32 size, u32 alignment) {
  return size ? RingData(u32(*size), alignment) : 0;
}

// void *CDevice inline data allocation (sub_8222D240): from the ring, or from
// the allocator of the command buffer being recorded.
u32 AllocateInlineData_hook(u32 /*device*/, u32 dwords, u32 alignment) {
  return RingData(dwords * 4, alignment);
}

// HRESULT D3DDevice_SetRingBufferParameters(D3DDevice*,
//     const D3DRING_BUFFER_PARAMETERS*) [0x82657508]: would replace the ring
// (and our sink) with a freshly allocated one.
u32 D3DDevice_SetRingBufferParameters_hook(u32 /*device*/, u32 /*parameters*/) {
  return 0;  // S_OK
}

// void CDevice::BlockOnFence(DWORD Fence, CBlocker::Type, BOOL NoKick)
// [0x8228F558]: every fence wait (D3DDevice_BlockOnFence, resource locks and
// destruction, InvalidateConstants). As the original: kick the fence if no
// KickOff has covered it yet, then wait for it, here for the render thread to
// complete it (KickFence).
// The original polls through CBlocker::Check, which after 5 s without
// progress dumps the GPU's registers and traps.
// tw2de (2026-10-24): the game thread's fence waits by CBlocker::Type, which names the caller (from the generated code:
// 0 KickOff's ring wrap (sub_82D15C18), 3 sub_827F4938 / sub_82D172F8, 4 BlockUntilIdle (sub_826CB2E0, which the slow
// frames wait in), 6 sub_82D148C0, 7-15 sub_82D0D8C8's): how many waited, for how long, every 10 s in the log.
struct FenceWaitStats {
  std::atomic<uint32_t> waits{0};
  std::atomic<uint64_t> total_us{0};
  std::atomic<uint64_t> max_us{0};
};
FenceWaitStats g_fence_waits[32];

// tw2de (2026-10-24): the waits of each 10 s window that went past 20 ms, logged with that window's line (so the
// loading screens' waits do not use up the log before gameplay): how many, how long in all and by length, what the
// render thread was doing at 20 ms (the time of each kind), and the pictures of the 3 longest.
struct LongWaits {
  uint32_t count = 0;
  double total_ms = 0.0;
  uint32_t by_length[4] = {};       // 20-50, 50-100, 100-200, 200+ ms
  uint32_t by_state[3] = {};        // render thread idle, idle with fences held for a callback, busy
  double state_ms[3] = {};
  struct Top {
    double ms;
    std::string text;
  } top[3];
};
std::mutex g_long_waits_mutex;
LongWaits g_long_waits;

void ReportFenceWaits() {
  static std::atomic<int64_t> last_ms{0};
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = last_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    last_ms.compare_exchange_strong(last, now_ms);
    return;
  }
  if (now_ms - last < 10000 || !last_ms.compare_exchange_strong(last, now_ms))
    return;
  if (!REXCVAR_GET(log_diagnostics))
    return;
  std::string line;
  for (uint32_t t = 0; t < 32; ++t) {
    const uint32_t n = g_fence_waits[t].waits.exchange(0, std::memory_order_relaxed);
    const uint64_t total = g_fence_waits[t].total_us.exchange(0, std::memory_order_relaxed);
    const uint64_t worst = g_fence_waits[t].max_us.exchange(0, std::memory_order_relaxed);
    if (n)
      line += fmt::format(" | type {}: {} waits, {:.1f} ms in all, longest {:.1f} ms", t, n, double(total) / 1000.0,
                          double(worst) / 1000.0);
  }
  if (!line.empty())
    RGPU_INFO("[renut] fence waits of the game thread in {:.0f} s{}", double(now_ms - last) / 1000.0, line);
  LongWaits lw;
  {
    std::lock_guard lock(g_long_waits_mutex);
    std::swap(lw, g_long_waits);
  }
  if (lw.count) {
    RGPU_INFO("[renut] fence waits past 20 ms in those 10 s: {} ({:.1f} ms in all); 20-50 ms {}, 50-100 ms {}, "
              "100-200 ms {}, 200+ ms {}; at 20 ms the render thread was idle in {} ({:.1f} ms), idle with fence "
              "completions held for a game callback in {} ({:.1f} ms), busy in {} ({:.1f} ms)",
              lw.count, lw.total_ms, lw.by_length[0], lw.by_length[1], lw.by_length[2], lw.by_length[3],
              lw.by_state[0], lw.state_ms[0], lw.by_state[1], lw.state_ms[1], lw.by_state[2], lw.state_ms[2]);
    for (const LongWaits::Top &t : lw.top)
      if (!t.text.empty())
        RGPU_INFO("[renut]   {}", t.text);
  }
}

u32 BlockOnFence_hook(u32 device_va, u32 fence, u32 type, u32 /*no_kick*/) {
  tw2de::renut_gpu::CountFenceQuery();  // tw2de (package 27c): the stream checks' design
  auto *bytes = GuestPtr<u8>(device_va);
  // tw2de (2026-10-25, tw2de_render_720p): BlockUntilIdle (sub_826CB2E0, type 4) waits for this fence, then spins
  // until the device's word at +11012 (0x2B04) is 0. Only EndTiling's ring segment (sub_82D23148 -> sub_826D45A8 ->
  // sub_826CAB90 with r7 1) raises it, and no CPU code lowers it again but the GPU-hang recovery (sub_826D50C8): the
  // console's GPU clears it when it has run the segment. Nothing here runs ring packets (Renut draws the tiles itself),
  // so it stayed 1 and the game hung at its first BlockUntilIdle after a tiled frame: at 1280x720 with 2x MSAA the
  // scene no longer fits in EDRAM and the game tiles it (tw2de_021.log, rex_stall.log: the main thread spinning in
  // sub_826CB2E0). Once the idle wait is over, everything queued has run, as on the console: the word is cleared then.
  struct ClearSegmentsWhenIdle {
    u8 *bytes;
    bool on;
    ~ClearSegmentsWhenIdle() {
      if (!on || !bytes)
        return;
      auto *word = reinterpret_cast<be_u32 *>(bytes + 0x2B90);
      if (u32(*word) == 0)
        return;
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        RGPU_INFO("[tiling] BlockUntilIdle: the device's pending tiling segments ({}) cleared after the idle wait "
                  "(as the console's GPU does when it has run them)", u32(*word));
      *word = 0u;
    }
  } clear_segments{bytes, type == 4};
  if (!bytes || !FencePending(bytes, fence))
    return 0;
  // Devices with immediate fences (the command buffer recorders) complete
  // theirs as they write them; the original returns at once for them too.
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences)
    return 0;
  const bool covered =
      int32_t(fence - g_queued_fence.load(std::memory_order_acquire)) <= 0;
  if (!covered) {
    if (*reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetInKickOff))
      return 0;
    KickOff_hook(device_va);
  }
  // The fence may wait for callbacks, which this thread runs as the render
  // thread reaches them.
  const auto waiting = std::chrono::steady_clock::now();
  // tw2de (2026-10-24): a wait past 20 ms takes a picture of both sides (the device's fence words and the render
  // thread's state) at that point, logged when it ends.
  std::string snapshot;
  render_thread::FenceWaitState state{};
  uint32_t rounds = 0;
  for (;;) {
    RunReadyCallbacks();
    if (!FencePending(bytes, fence))
      break;
    if (snapshot.empty() && REXCVAR_GET(log_diagnostics) && std::chrono::steady_clock::now() - waiting >= std::chrono::milliseconds(20)) {
      const u32 tag = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
      const auto *word = GuestPtr<const be_u32>(*reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion));
      snapshot = fmt::format("fence {:#x}, device tag {:#x}, completed {:#x}, newest queued {:#x}; {}", fence, tag,
                             word ? u32(*word) : 0u, g_queued_fence.load(std::memory_order_relaxed),
                             render_thread::DescribeForFenceWait(&state));
    }
    ++rounds;
    if (!render_thread::WaitForFenceProgress(std::chrono::milliseconds(2)))
      break;  // no render thread: nothing would complete it
  }
  const auto waited = std::chrono::steady_clock::now() - waiting;
  if (!snapshot.empty()) {
    const double ms = std::chrono::duration<double, std::milli>(waited).count();
    const int kind = !state.idle ? 2 : state.callback_held_fences ? 1 : 0;
    std::lock_guard lock(g_long_waits_mutex);
    LongWaits &lw = g_long_waits;
    lw.count++;
    lw.total_ms += ms;
    lw.by_length[ms < 50.0 ? 0 : ms < 100.0 ? 1 : ms < 200.0 ? 2 : 3]++;
    lw.by_state[kind]++;
    lw.state_ms[kind] += ms;
    // Into the 3 longest, kept longest first.
    for (int i = 0; i < 3; ++i) {
      if (ms > lw.top[i].ms) {
        for (int j = 2; j > i; --j)
          lw.top[j] = std::move(lw.top[j - 1]);
        lw.top[i] = {ms, fmt::format("type {}, {:.1f} ms, {} rounds; at 20 ms: {}", type, ms, rounds, snapshot)};
        break;
      }
    }
  }
  renut::frame_stats::OnGameThreadWait(std::chrono::duration<double, std::milli>(waited).count());
  {
    FenceWaitStats &st = g_fence_waits[type & 31];
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count());
    st.waits.fetch_add(1, std::memory_order_relaxed);
    st.total_us.fetch_add(us, std::memory_order_relaxed);
    uint64_t prev = st.max_us.load(std::memory_order_relaxed);
    while (us > prev && !st.max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
  }
  ReportFenceWaits();
  return 0;
}

// tw2de (2026-10-22, the slow loads): a fence the game keeps asking about for a second or more is logged once
// (with what the device says: its newest tag, the completed word, the newest fence queued for completion).
void NoteLongPendingFence(const u8 *bytes, u32 fence, const char *how) {
  static std::mutex m;
  static u32 fence_seen = 0;
  static std::chrono::steady_clock::time_point since;
  static bool logged = false;
  std::lock_guard lock(m);
  const auto now = std::chrono::steady_clock::now();
  if (fence != fence_seen) {
    fence_seen = fence;
    since = now;
    logged = false;
    return;
  }
  if (!logged && now - since >= std::chrono::seconds(1)) {
    logged = true;
    const u32 tag = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
    const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
    RGPU_WARN("[fence] {}: fence {:08X} still pending after {} ms (device tag {:08X}, completed {:08X}, queued "
              "{:08X})", how, fence,
              std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count(), tag, LoadGuestU32(completion),
              g_queued_fence.load(std::memory_order_relaxed));
  }
}

// BOOL D3DDevice_IsFencePending(DWORD Fence) [0x8228F650]: on the device D3D
// finds through VdGlobalDevice; the current fence is kicked first. Games
// poll it, and the fence may be waiting for callbacks only a guest thread can
// run.
u32 IsFencePending_hook(u32 fence) {
  tw2de::renut_gpu::CountFenceQuery();  // tw2de (package 27c): the stream checks' design
  RunReadyCallbacks();
  if (!fence)
    return 0;
  const u32 device_va = native::GlobalDevice();
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return 0;
  if (fence == *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag))
    KickOff_hook(device_va);
  const bool pending = FencePending(bytes, fence);
  if (pending)
    NoteLongPendingFence(bytes, fence, "IsFencePending");
  return pending ? 1 : 0;
}

// BOOL CBlocker::Check() [0x8264DCD0]: one poll of a D3D wait for the GPU
// (ring space, fence markers, streaming). Fence waits no longer come here
// (BlockOnFence_hook); nothing on the host advances the others.
u32 CBlocker_Check_hook(u32 /*blocker*/) {
  return 0;
}

// ---------------------------------------------------------------------------
// Command buffers
// ---------------------------------------------------------------------------

// D3DCommandBuffer fields.
constexpr u32 kBufferFence = 0x08;
constexpr u32 kBufferInheritTags = 0x18;  // D3DTAGCOLLECTION
constexpr u32 kBufferSetTags = 0x40;      // its copy
constexpr u32 kBufferFlags = 0x6C;        // low nibble: state (2 recording, 1 done, 7 out of memory)
constexpr u32 kBufferFixedMemory = 0x98;  // memory the caller gave it, or 0 for its allocator
constexpr u32 kBufferAllocatorSize = 0xA8;

// D3D::ConfigureForOnePassZPass(CDevice*, BOOL, DWORD TileCount, DWORD Width)
// [0x82658818]: the EDRAM layout of a buffer recorded for a one-pass Z pass.
void ConfigureForOnePassZPass(native::Device d, bool flag, u32 tile_count, u32 width) {
  u8 flags3 = d.B(native::off::Flags3) | 0x20;
  if (flag)
    flags3 |= 0x10;
  d.SetB(native::off::Flags3, flags3);
  const u32 msaa = d.H(native::off::SurfaceInfo) & 3;
  u32 tile_height = 8, tile_width = 32, unit = 80;
  if (msaa == 0) {
    tile_height = 16;
  } else if (msaa != 1) {
    tile_width = 16;
    unit = 40;
  }
  const u32 pitch = (tile_width + width - 1) & ~(tile_width - 1);
  const u32 tiles_per_row = pitch / tile_width;
  for (u32 i = 0; i < tile_count; ++i) {
    const u32 x = d.W(native::off::TileOrigins + i * 8);
    const u32 y = d.W(native::off::TileOrigins + i * 8 + 4);
    d.SetW(0x3338 + i * 4, (y / tile_height) * tiles_per_row + x / tile_width);
  }
  d.SetW(0x343C, pitch);
  d.SetW(native::off::SurfaceInfo, native::Insert(d.W(native::off::SurfaceInfo), pitch, 18, 14));
  d.Dirty(2, 0x100);
  d.SetW(0x3378, ((unit + width - 1) / unit) * unit);
  u64 predicated = d.Q(native::off::PredicatedPending2);
  if ((d.B(native::off::Flags3) & 0x20) || (d.W(0x2E4C) & 0xE0000) != 0x40000)
    predicated |= 0x100;
  else
    predicated &= ~u64(0x100);
  d.SetQ(native::off::PredicatedPending2, predicated);
}

// HRESULT D3DDevice_BeginCommandBuffer(D3DDevice*, D3DCommandBuffer*, DWORD Flags,
//     const D3DTAGCOLLECTION *pInheritTags, ..., const D3DRECT *pTilingRects,
//     DWORD TileCount) [0x82657CB0]
// The device records into the buffer: the draws on the host (command_buffers.h),
// so the buffer's segments and the reset of their allocator are left out.
u32 D3DDevice_BeginCommandBuffer_hook(u32 device_va, u32 command_buffer, u32 flags,
                                      u32 inherit_tags, u32 /*unused*/, u32 tiling_rects,
                                      u32 tile_count) {
  // A Begin*Vertices draw recorded into the previous buffer.
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  auto *buffer = GuestPtr<be_u32>(command_buffer);
  if (!d || !buffer)
    return 0;
  auto bw = [buffer](u32 offset) -> be_u32 & { return buffer[offset / 4]; };
  bw(kBufferFlags) = native::Insert(bw(kBufferFlags), 2, 0, 4);
  if (const u32 fence = bw(kBufferFence)) {
    native::BlockOnFence(native::GlobalDevice(), fence);
    bw(kBufferFence) = 0u;
  }
  d.SetW(native::off::CommandBuffer, command_buffer);
  bw(kBufferFlags) = u32(bw(kBufferFlags)) & ~0x70u;
  for (u32 offset : {0x7Cu, 0x80u, 0x84u, 0x70u, 0x90u, 0x74u, 0x78u})
    bw(offset) = 0u;
  d.SetW(0x3A50, 8);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
  d.SetW(0x3A44, 0);
  d.SetW(0x3A40, bw(kBufferAllocatorSize));
  for (u32 offset = 0x3590; offset <= 0x34E4; offset += 4)
    d.SetW(offset, 0);
  native::RewindRing(d);

  // The tags the buffer inherits (none set unless given) and its copy.
  auto *tags = GuestPtr<const be_u64>(inherit_tags);
  auto *own = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferInheritTags);
  auto *own_copy = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferSetTags);
  for (u32 i = 0; i < 5; ++i) {
    own[i] = tags ? u64(tags[i]) : 0;
    own_copy[i] = u64(own[i]);
  }
  u32 buffer_flags = bw(kBufferFlags);
  buffer_flags &= ~0x80u;
  buffer_flags &= ~0x8000u;
  buffer_flags |= (((flags << 3) & 0x100) | ((flags >> 4) & 1)) << 7;
  if (flags & 8) {
    d.SetW(native::off::CommandBufferTagMask, ~0u);
    buffer_flags |= 0x400;
  } else {
    d.SetW(native::off::CommandBufferTagMask, 0x400000);
    buffer_flags &= ~0x400u;
  }
  bw(0x78) = 0u;
  buffer_flags &= ~0x21800u;
  bw(kBufferFlags) = buffer_flags;
  d.SetQ(native::off::PredicatedPending2, 0);
  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0x87);

  if (flags & 3) {
    if (flags & 2) {
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x800;
      d.SetB(native::off::Flags, d.B(native::off::Flags) | 8);
    } else {
      // Its ring allocation list (0x33C0) is the GPU's.
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x1000;
    }
    d.SetW(native::off::TileCount, tile_count);
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x30);
    u32 extent_x = 0, extent_y = 0;
    const auto *rects = GuestPtr<const be_u32>(tiling_rects);
    for (u32 i = 0; rects && i < tile_count; ++i) {
      const u32 x1 = rects[i * 4], y1 = rects[i * 4 + 1];
      const u32 x2 = rects[i * 4 + 2], y2 = rects[i * 4 + 3];
      if (extent_x <= x2)
        extent_x = x2;
      if (extent_y <= y2)
        extent_y = y2;
      d.SetW(native::off::TileRects + i * 16, x1);
      d.SetW(native::off::TileRects + i * 16 + 4, y1);
      d.SetW(native::off::TileRects + i * 16 + 8, x2);
      d.SetW(native::off::TileRects + i * 16 + 12, y2);
      d.SetW(native::off::TileOrigins + i * 8, x1 & ~31u);
      d.SetW(native::off::TileOrigins + i * 8 + 4, y1 & ~31u);
    }
    d.SetW(native::off::TilingExtent, extent_x);
    d.SetW(native::off::TilingExtent + 4, extent_y);
    d.SetW(0x3384, 0);
    d.SetW(0x3278, 2);
    if (flags & 0x40) {
      ConfigureForOnePassZPass(d, (flags >> 7) & 1, tile_count, extent_x);
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x20000;
    }
  }
  if (flags & 4) {
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x40);
    d.SetW(0x3268, native::Extract(d.W(native::off::DepthControl), 4, 3));
    if (d.B(native::off::Flags3) & 0x20) {
      u64 predicated = d.Q(native::off::PredicatedPending2) | (u64(1) << 37);
      if (d.W(0x3378) != (d.W(native::off::SurfaceInfo) & 0x3FFF))
        predicated |= u64(1) << 57;
      d.SetQ(native::off::PredicatedPending2, predicated);
    }
  }
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The buffer starts with no fetch constants, and dirty in everything it does
  // not inherit.
  std::memset(d.bytes + native::off::TextureFetch, 0, 0x300);
  for (u32 i = 0; i < 5; ++i)
    d.SetPending(i, ~u64(own[i]));

  BeginRecording(device_va, command_buffer);
  return 0;
}

// HRESULT D3DDevice_EndCommandBuffer(D3DDevice*) [0x826582A0]
// The state left dirty would be written into the buffer (Flags bit 5 at
// Begin); then everything is unbound, and the device is pointed at its side
// buffer until the next Begin.
u32 D3DDevice_EndCommandBuffer_hook(u32 device_va) {
  FlushDeferredVertices(device_va);
  EndRecordingNote(device_va);  // tw2de (2026-11-08, diagnostic)
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  auto *buffer = GuestPtr<be_u32>(d.W(native::off::CommandBuffer));
  if (buffer && (u32(buffer[kBufferFlags / 4]) & 0x8000)) {
    for (u32 i = 0; i < 5; ++i)
      d.SetPending(i, 0);
  }
  native::UnsetAll(d);
  // A buffer in fixed memory reports running out of it here; recorded on the
  // host, it does not.
  if (buffer) {
    if (u32(buffer[kBufferFlags / 4]) & 0x1000)
      buffer[0x78 / 4] = 0u;  // its ring allocation list: none
  }
  d.SetW(native::off::CommandBuffer, 0);
  if (buffer) {
    const u32 end = d.W(native::off::Ring) + 4;
    buffer[0x88 / 4] = end;
    buffer[0x8C / 4] = d.W(native::off::RingLimit);
    buffer[0x90 / 4] = native::GpuAddress(end) - 0x40000000u;
  }
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (buffer)
    buffer[kBufferFlags / 4] = native::Insert(buffer[kBufferFlags / 4], out_of_memory ? 7 : 1, 0, 4);
  // CDevice::MarkAsOutOfMemory: the device writes nowhere until the next Begin.
  native::RewindRing(d);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | 0x20);
  return out_of_memory ? kE_OUTOFMEMORY : 0;
}

// void D3DDevice_RunCommandBuffer(D3DDevice*, D3DCommandBuffer*,
//     DWORD PredicationSelect) [0x82222888]
// Replaces the original, which would send the buffer's packets to the GPU
// (and on a no-hardware device append to a buffer being recorded).
void D3DDevice_RunCommandBuffer_hook(u32 device_va, u32 command_buffer,
                                     u32 /*predication_select*/) {
  if (!command_buffer)
    return;
  if (IsRecordingCommandBuffer(device_va)) {
    RecordRun(device_va, command_buffer);
  } else {
    RunReadyCallbacks();
    QueueRunCommandBuffer(device_va, command_buffer);
  }
}

// void D3DDevice_Swap(D3DDevice *pDevice, D3DBaseTexture *pFrontBuffer,
//     const D3DVIDEO_SCALER_PARAMETERS *pParameters) [0x82287590]
// The frame goes to the render thread, which presents it; the game thread
// goes on with the next one, waiting only when it is more than
// render_thread::kMaxFramesAhead ahead.
// TW2 (2026-11-14): the stand-in for the pacing the GPU gave the original Swap (D3DRS_PRESENTINTERVAL, device
// +0x36B0: PaceSwap held each Swap to that many refreshes) is removed. TW2 has no frame limiter of its own
// (checked on the console: 60.0 fps in its movies and menus with a display interval of 1); the display's swap
// interval (frame_interval.cpp SwapInterval: deko3d's, 2 = 30 fps) is the only one.

// tw2de (2026-11-05, seen17, tw2de_frame_interval): a frame that missed its vblank starts the next one at once.
// The game's swap callback (sub_827B1060) sets the vblank its main thread waits for (frame +5560) to the last frame's
// start (+5564) plus its interval (+5536, GameVblanks: 2); when that vblank has passed already (the frame ran long) it
// sets the NEXT one (now + 1), and its vblank callback (sub_827B10E0, from the vblank pump) wakes the main thread there.
// So a frame of 34-40 ms of work became 50 ms (3 vblanks) and the next one started only then: the 20 fps stretches
// (runs 941c2ca4 / 029ae411: CPU frames 48.7-51 ms one after another, main 31% of those frames in that wait, the render
// thread under 1 ms and the GPU 3 ms). With the lock off (the boot menu's frame interval) the display's swap interval
// shows each image that many refreshes anyway (frame_interval.cpp SwapInterval), so the wait for a vblank the frame
// already missed only throws the time away: here the target becomes the current vblank and the game's own vblank
// callback runs at once (its spinlock, its wake of the main thread, the start it records: +5564 = now, so the frame
// after it counts its interval from here). A frame on time is left to the game as before (no queue: the game is slower
// than the display whenever this runs, so nothing waits in front of the display and the input is as fresh as before).
// Two things the wait did besides waiting are kept: the game's resource manager loads before the main thread waits,
// until a vblank has come (sub_821F1FE0, tw2de_slot_wait_diag.cpp): it may still load until the vblank the frame
// would have waited for (frame_interval LateLoadDeadlineUs), so a frame waits now only while there is loading to do;
// and the game's frame step, rounded to whole vblanks (FrameStepAfterLimiter below), carries its rounding over.
// Off with the game's own lock (frame interval 0) and while a prerendered movie plays (the game's own pacing).
// tw2de (2026-11-10): LR's frame object and its fields (FF XIII's pointer 0x82C0D328 and +5536 ... were left here:
// every swap wrote the interval into an unrelated object, and the late start never ran). LR's swap callback sub_82A853B0
// (FF XIII-2's sub_82887D98, the same instructions): the pointer at 0x8313F78C; the spinlock +6984, the vblank the main
// thread waits for +6992 (-1: none), the last frame's start +6996, its own count +7000, the interval +6968 (copied each
// frame, sub_82A85660), the "count from the swap data" flag byte +3078. Every offset is FF XIII-2's + 112.
constexpr u32 kFrameObjectPointer = 0;  // TW2 (2026-11-12): none yet (LR's 0x8313F78C is Crystal Tools'): off
constexpr u32 kFrameInterval = 6968, kFrameLock = 6984, kFrameWake = 6992, kFrameStart = 6996, kFrameCount = 7000,
              kFrameFromSwap = 3078;

void StartLateFrameNow(u8 *device) {
  if constexpr (kFrameObjectPointer == 0)
    return;
  if (tw2de::frame_interval::MoviePlaying())
    return;
  const u32 frame = *GuestPtr<const be_u32>(kFrameObjectPointer);
  const u32 vblank_callback = u32(*reinterpret_cast<const be_u32 *>(device + 16948));
  if (!frame || !vblank_callback)
    return;
  auto field = [frame](u32 offset) -> be_u32 & { return *GuestPtr<be_u32>(frame + offset); };
  const u32 now = u32(*reinterpret_cast<const be_u32 *>(device + 16952));
  // The callback's "now": the swap data's vblank (+2838 set) or the frame's own count (+5568).
  const bool from_swap = *GuestPtr<const u8>(frame + kFrameFromSwap) != 0;
  const u32 cur = from_swap ? now : u32(field(kFrameCount));
  const u32 start = u32(field(kFrameStart)), interval = u32(field(kFrameInterval));
  const u32 wake = u32(field(kFrameWake));
  // Late: the callback found its vblank (start + interval) passed and asked for the next one (cur + 1).
  if (wake == 0xFFFFFFFFu || wake != cur + 1 || cur < start + interval)
    return;
  // The target set to the current vblank under the game's own spinlock (+5552, taken as sub_827B1060 takes it).
  auto *lock = GuestPtr<std::atomic<u32>>(frame + kFrameLock);
  for (u32 expected = 0; !lock->compare_exchange_weak(expected, __builtin_bswap32(1u), std::memory_order_acquire);
       expected = 0) {
  }
  if (u32(field(kFrameWake)) == wake)
    field(kFrameWake) = now;
  lock->store(0, std::memory_order_release);
  // The game's vblank callback as the vblank handler (sub_82D16430) calls it: {vblank count, flips, 0}.
  static const u32 data_va = records::AllocPhysicalBlock(0x40);
  auto *data = data_va ? GuestPtr<be_u32>(data_va) : nullptr;
  PPCFunc *function = rex::runtime::ResolveIndirectFunction(vblank_callback);
  if (!data || !function)
    return;
  data[0] = now;
  data[1] = u32(*reinterpret_cast<const be_u32 *>(device + 16976));
  data[2] = 0u;
  // Before the wake: the resource manager's time (it loads before the main thread's wait, until a vblank is seen) and
  // the frame step's rounding (FrameStepAfterLimiter below) for the frame this starts.
  tw2de::frame_interval::NoteLateStart();
  rex::ppc::GuestToHostFunction<void>(function, data_va);
  static std::atomic<uint64_t> started{0};
  const uint64_t n = started.fetch_add(1, std::memory_order_relaxed) + 1;
  if (REXCVAR_GET(log_diagnostics) && (n <= 3 || n % 300 == 0))
    RGPU_INFO("[interval] a frame that missed its vblank: the next one started at once ({} so far)", n);
}

// tw2de (2026-11-05, seen17): the game's frame step after its limiter (sub_82AC0CD0(out r3, pacer r4)). After its
// wait it takes dt, the time between the last two wakes (the pacer's ring of four 64-bit stamps at +8, index +40, in
// its clock's units; used when 0 < dt < 1000000, else a constant), as raw = dt * 300000 / 1000000 and rounds it to whole
// vblanks: p = trunc(f32 [0x8200196C] / f32 [pacer +0]), out +0 = ((raw + p/2) / p) * p. Its frames start at vblanks,
// so that was exact; a frame StartLateFrameNow started between vblanks lasts a fraction more or less, and rounding
// each one alone would run the game slow (a 36 ms frame stepped 33.3 ms) or fast (a 42 ms frame 50 ms). For those
// frames the rounding's remainder is carried to the next ones (whole vblanks still: 2, 2, 3, 2 ...), so game time
// keeps to real time; it is reset once two frames in a row started at their vblanks. The game's own value is checked
// against the same formula first: anything else (its constant path, another layout) is left alone.
// TW2 (2026-11-12): LR's frame step (sub_82AC0CD0) is Crystal Tools code with no counterpart in TW2: not hooked.
#if 0
REX_EXTERN(__imp__sub_82AC0CD0);
extern "C" REX_FUNC(sub_82AC0CD0) {
  const u32 out = ctx.r3.u32, pacer = ctx.r4.u32;
  __imp__sub_82AC0CD0(ctx, base);
  static int64_t carry = 0;
  static bool late_before = false;
  // This frame started early, or the one before did (this one then woke at its vblank, but counted from the vblank
  // before the early start: between one and two vblanks after it).
  const bool late = tw2de::frame_interval::TakeLateStart();
  const bool carrying = late || late_before;
  late_before = late;
  if (!carrying) {
    carry = 0;
    return;
  }
  auto be32 = [](u32 va) { return u32(*GuestPtr<const be_u32>(va)); };
  auto f32 = [&](u32 va) {
    const u32 w = be32(va);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
  };
  const u32 index = be32(pacer + 40);
  auto stamp = [&](u32 k) {
    const u32 at = pacer + ((((index - k - 1u) & 3u) + 1u) << 3);
    return (uint64_t(be32(at)) << 32) | be32(at + 4);
  };
  const int32_t dt = int32_t(u32(stamp(0) - stamp(1)));
  const float fps = f32(pacer + 0);
  if (dt <= 0 || dt >= 1000000 || !(fps > 0.0f)) {
    carry = 0;
    return;
  }
  const float q = float(double(f32(0x8200196Cu)) / double(fps));
  const int32_t p = int32_t(q);
  if (p <= 1) {
    carry = 0;
    return;
  }
  const int32_t raw = int32_t(int64_t(dt) * 300000 / 1000000);
  int32_t step = ((raw + p / 2) / p) * p;
  if (u32(step) != be32(out)) {
    carry = 0;  // not the formula above: leave the game's value
    return;
  }
  carry += int64_t(raw) - step;
  if (2 * carry >= p) {
    step += p;
    carry -= p;
  } else if (2 * carry <= -p && step > p) {
    step -= p;
    carry += p;
  }
  *GuestPtr<be_u32>(out) = u32(step);
}
#endif

// tw2de: the GPU's swap interrupt (the game's handler sub_826CF2A0), which the hooked Swap no longer gets. It counts
// the swap (+16772), works out the vblank the swap shows at (+16764) and calls the title's swap callback
// (D3DDevice_SetSwapCallback, +16748) with D3DSWAPDATA. The game's frame limiter depends on it: its swap callback sets
// the vblank its vblank callback (+16752, run by StartVblankPump) waits for to wake the main thread. The flip is
// counted done at once (+16776): the hooked Swap presents itself, so there is no flip queue.
void SwapInterrupt(u8 *device, u32 front_buffer) {
  auto word = [device](u32 offset) -> be_u32 & { return *reinterpret_cast<be_u32 *>(device + offset); };
  const u32 interval = 0;  // TW2 (2026-11-14): no presentation interval (D3DRS_PRESENTINTERVAL, +0x36B0, is not used)
  word(16972) = u32(word(16972)) + 1;
  const u32 now = word(16952);
  const u32 last = word(16960);
  u32 target = last + interval;
  if (target <= now)
    target = now;
  // tw2de (2026-10-25, tw2de_frame_interval): the game's own frame limiter is not D3D's presentation interval but
  // its swap callback (sub_827B1060): it sets the vblank its main thread waits for to the last one plus the game's
  // frame interval, word +5536 of its frame object (the pointer at 0x82C0D328), which sub_82925210 copies each frame
  // from +5532, set by sub_827B0E40 (2 in play: 30 fps; 1 in some menus). Read nowhere else (the game's step does not
  // depend on it). With the lock off it is tw2de_frame_interval's (2026-10-27: was 1, with the display's swap interval
  // setting the rate, which let frames queue up: frame_interval.cpp). A movie keeps the game's own value.
  if (kFrameObjectPointer) {  // TW2: off (no frame object yet)
    if (const u32 frame = *GuestPtr<const be_u32>(kFrameObjectPointer)) {
      auto &game_interval = *GuestPtr<be_u32>(frame + kFrameInterval);
      static std::atomic<bool> logged{false};
      const u32 want = tw2de::frame_interval::GameVblanks();
      if (u32(game_interval) != want && !logged.exchange(true))
        RGPU_INFO("[interval] the game's frame limiter asked for {} vblanks a frame: {} (tw2de_frame_interval)",
                  u32(game_interval), want);
      game_interval = want;
    }
  }
  if (const u32 callback = word(16944)) {
    static const u32 data_va = records::AllocPhysicalBlock(0x80);
    if (auto *data = data_va ? GuestPtr<be_u32>(data_va) : nullptr) {
      data[0] = front_buffer != 0 ? 1u : 0u;
      data[1] = u32(word(16972));
      data[2] = now;
      data[3] = last;
      data[4] = 1u;  // percent of the frame gone since the vblank
      data[5] = target;
      if (PPCFunc *function = rex::runtime::ResolveIndirectFunction(callback)) {
        rex::ppc::GuestToHostFunction<void>(function, data_va);
        target = data[5];
      } else {
        static bool logged = false;
        if (!logged) {
          logged = true;
          RGPU_ERROR("Swap callback {:08X}: no guest function there", callback);
        }
      }
    }
  }
  word(16960) = target;
  word(16976) = u32(word(16976)) + 1;
  tw2de::frame_timing::NoteTarget(rex::latency::SwapCount(), target);  // tw2de (2026-11-05)
  StartLateFrameNow(device);  // tw2de (2026-11-05, seen17)
}

// tw2de: the scaler's source rect, the part of the front buffer the video hardware scans out (and stretches to the
// screen). The game renders below 720p and passes no parameters to Swap: the device's own (+0x3618, which
// D3DDevice_Swap compares the given ones with: D3DVIDEO_SCALER_PARAMETERS, a D3DRECT then ScaledOutputWidth / Height
// and FilterProfile) apply. Without it the whole front buffer was shown, the frame in a corner of the window.
struct ScalerSource {
  i32 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};
ScalerSource ReadScalerSource(const u8 *device, u32 parameters) {
  ScalerSource r;
  const be_u32 *p = parameters ? GuestPtr<const be_u32>(parameters)
                               : (device ? reinterpret_cast<const be_u32 *>(device + 0x36DC) : nullptr);
  if (!p)
    return r;
  r.x1 = i32(u32(p[0]));
  r.y1 = i32(u32(p[1]));
  r.x2 = i32(u32(p[2]));
  r.y2 = i32(u32(p[3]));
  return r;
}

void D3DDevice_Swap_hook(u32 device_va, u32 front_buffer, u32 parameters) {
  NoteSwapForCommandBuffers();  // tw2de (2026-11-08, diagnostic)
  // tw2de (2026-11-09, tw2de_fix_load_fade): a frame drawn from the field's unplaced camera after a loaded save's
  // chapter screen is shown black (cpu/tw2de_load_fade_fix.cpp).
  const bool black = FnxfXiiiLrLoadFadeHold();
  // tw2de (2026-10-20, from sa2-nx, tw2de_renut_core_layout, the Switch): once, the guest main thread alone on core 0,
  // the other guest threads on cores 1-2 (preferring 2: the render thread is pinned to core 1 at a higher priority).
  // Before, every guest thread could run on any core, time-sliced at their priority (0x3B).
  static bool core_layout_done = false;
  if (!core_layout_done) {
    core_layout_done = true;
#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(tw2de_renut_core_layout)) {
      const uint32_t moved = RexSetGuestThreadCoreLayout(0, uint32_t(REXCVAR_GET(tw2de_core_layout_others_mask)) & 7u, 2);
      if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("[renut] tw2de_renut_core_layout: the thread that calls Swap on core 0 (the module's main thread: {}), "
                "the other guest threads on the cores of mask {:X} ({} threads set)",
                RexGuestLayoutCallerIsMain() ? "yes" : "no", REXCVAR_GET(tw2de_core_layout_others_mask) & 7, moved);
    }
#endif
  }
  renut::frame_stats::OnGameFrame();
  tw2de::profiler::FrameBoundary();  // tw2de (2026-10-24): the CPU profiler's frame markers (slow-frame reports)
  g_tw2de_swaps.fetch_add(1, std::memory_order_relaxed);
  rex::latency::NoteGuestSwap();  // tw2de (2026-10-19, input latency): the probe
  const uint64_t swap_ordinal = rex::latency::SwapCount();
  tw2de::frame_timing::Mark(swap_ordinal, tw2de::frame_timing::kSwap);  // tw2de (2026-11-05)
  // tw2de (2026-11-12, tw2de_diag_frames): the frame's guest work, a line a Swap (the black frames).
  if (REXCVAR_GET(tw2de_diag_frames)) {
    static uint64_t prev[6] = {};
    static auto prev_time = std::chrono::steady_clock::now();
    static uint32_t lines = 0;
    uint64_t h[6];
    for (int i = 0; i < 6; ++i)
      h[i] = g_tw2de_hook_counts[i].load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - prev_time).count();
    std::string dests;
    for (u32 i = 0; i < std::min<u32>(g_tw2de_frame_resolves, 8u); ++i)
      dests += fmt::format(" {:08X}", g_tw2de_frame_resolve_dests[i]);
    if (lines++ < 30000)
      RGPU_INFO("[frames] swap {} +{:.1f} ms: draws {} (recorded {}), clears+resolves {} (recorded {}), resolves {}:{}; "
                "front buffer {:08X}",
                swap_ordinal, ms, h[0] - prev[0], h[2] - prev[2], h[3] - prev[3], h[5] - prev[5],
                g_tw2de_frame_resolves, dests, front_buffer);
    std::memcpy(prev, h, sizeof(prev));
    prev_time = now;
  }
  g_tw2de_frame_resolves = 0;
  FlushDeferredVertices(device_va);
  SnapshotDeltasNewFrame();  // tw2de (2026-10-20, from sa2-nx, tw2de_renut_snapshot_deltas)

  // What the original does after handing the frame to the video hardware:
  // count the swap, record the frame's fence tag, and end the frame with a
  // KickOff, whose fence completes once the render thread gets there.
  auto *device = GuestPtr<u8>(device_va);
  if (device) {
    auto word = [device](u32 offset) -> be_u32 & {
      return *reinterpret_cast<be_u32 *>(device + offset);
    };
    word(d3d::kDeviceOffsetSwapCount) = u32(word(d3d::kDeviceOffsetSwapCount)) + 1;
    const u32 fence_tag = word(d3d::kDeviceOffsetFenceTag);
    word(d3d::kDeviceOffsetSwapFenceTag) = fence_tag;
    word(d3d::kDeviceOffsetPresentFenceTag) = fence_tag;
    device[d3d::kDeviceOffsetSwapFlags] &= u8(~d3d::kSwapFlagPending);
    // tw2de (package 27c): the frame's last vertex and texture fills are read here, before the Swap's fence
    // completes. They were read at the present, after it: the game, told the frame was done, could rewrite a dynamic
    // buffer for the next frame first, and the frame's last draws (the composite, the HUD) took the new bytes.
    tw2de::renut_gpu::checks::QueueKickFlush();
    KickFence(device);
    SwapInterrupt(device, front_buffer);
    // tw2de (2026-10-20, tw2de_renut_game_gamma): the device's gamma ramp (StoreGamma's +0x3A9C) for the present.
    Video::NoteGameGamma(device + 0x3C30);  // tw2de (N&B 0x3A9C)
  }

  const ScalerSource scaler = ReadScalerSource(GuestPtr<u8>(device_va), parameters);
  {
    static ScalerSource last{-1, -1, -1, -1};
    if (REXCVAR_GET(tw2de_diag_log) && std::memcmp(&last, &scaler, sizeof(scaler)) != 0) {
      last = scaler;
      std::string words;  // tw2de (2026-11-10): the front buffer's header words (FF XIII-2's black screen)
      if (const auto *h = GuestPtr<const be_u32>(front_buffer))
        for (int i = 0; i < 13; ++i)
          words += fmt::format(" {:08X}", u32(h[i]));
      RGPU_INFO("[tw2de] Swap: front buffer {:08X}, scaler source ({},{})-({},{}){};{}", front_buffer, scaler.x1,
                scaler.y1, scaler.x2, scaler.y2, parameters ? " (given)" : " (the device's)", words);
    }
  }
  // tw2de (2026-11-05): the frame's bookkeeping (texture and stream-cache frames, the resolution latch, the memory
  // report) after the present: the frame goes to the GPU and the display first, the bookkeeping runs while the GPU
  // draws it (run 3 of tw2de_diag_vsync_timing: 15-37 ms spikes in it held the submit back, and the frame missed
  // its refresh). It still comes before the next frame's commands (one render thread). The diagnostic cvar puts it back
  // before the present.
  const bool bookkeeping_first = REXCVAR_GET(tw2de_diag_bookkeeping_before_present);
  render_thread::QueueFrame([front_buffer, swap_ordinal, scaler, bookkeeping_first, black] {
    if (rtw::On())
      rtw::g_frame_item_t0 = std::chrono::steady_clock::now();  // tw2de (2026-11-05, [rtwork]): this item so far
    tw2de::frame_timing::Mark(swap_ordinal, tw2de::frame_timing::kRenderBegin);  // tw2de (2026-11-05)
    if (tw2de::frame_timing::On())
      tw2de::frame_timing::NoteRenderThread(swap_ordinal, render_thread::BusyMsThisFrame(),
                                               render_thread::WaitedMsThisFrame());
    PresentImage image;
    {
      std::lock_guard lock(State().mutex);
      tw2de::frame_timing::Mark(swap_ordinal, tw2de::frame_timing::kRenderLocked);  // tw2de (2026-11-05)
      std::optional<tw2de::frame_timing::PartScope> front_part(std::in_place, tw2de::frame_timing::kPartFrontBuffer);
      image = FrontBufferImageLocked(front_buffer, scaler.x1, scaler.y1, scaler.x2, scaler.y2);
      // tw2de (diagnostic, the black frames): a front buffer the GPU did not write this far is shown from guest
      // memory (black), or not at all.
      {
        static u32 last_va = 0, logged = 0, unwritten = 0, changes = 0;
        if (!image.texture || !FrontBufferWrittenLocked(front_buffer)) {
          ++unwritten;
          if (logged++ < 16)
            RGPU_WARN("[tw2de] Swap {}: front buffer {:08X} {} ({} such swaps)", swap_ordinal, front_buffer,
                      image.texture ? "not written by the GPU" : "has no host texture", unwritten);
        }
        if (front_buffer != last_va) {
          if (REXCVAR_GET(tw2de_diag_log) && changes++ < 16)
            RGPU_INFO("[tw2de] Swap {}: front buffer {:08X} -> {:08X} ({}x{} texture, scanned {}x{})", swap_ordinal,
                      last_va, front_buffer, image.width, image.height, image.source_width, image.source_height);
          last_va = front_buffer;
        }
      }
      if (image.texture)
        State().last_front_buffer = front_buffer;
      front_part.reset();
      if (bookkeeping_first)
        FrameBookkeepingLocked();
    }
    if (bookkeeping_first) {
      tw2de::frame_timing::PartScope part(tw2de::frame_timing::kPartMemoryReport);
      MaybeReportMemory();
    }
    // The front buffer is the texture the game resolved the frame into.
    Video::NotePresentingSwap(swap_ordinal);  // tw2de (2026-10-20): the probe's "input -> GPU done"
    tw2de::frame_timing::SetPresenting(swap_ordinal);  // tw2de (2026-11-05)
    tw2de::frame_timing::Mark(swap_ordinal, tw2de::frame_timing::kRenderReady);
    rtw::Timed(rtw::kPresent, [&] { Video::Present(image.texture && !black ? &image : nullptr); });
    tw2de::frame_timing::NotePresentIndex(swap_ordinal, Video::PresentCount());
    rex::latency::NotePresented(swap_ordinal);  // tw2de (2026-10-19, input latency)
    tw2de::display_sync::NotePresented(swap_ordinal);  // tw2de (2026-10-19, latency batch 3)
    {
      std::lock_guard lock(State().mutex);
      ::renut::gpu::SyncPipelineListLocked();  // tw2de (2026-10-20): the list's new records to the card
      if (!bookkeeping_first)
        FrameBookkeepingLocked();
    }
    if (!bookkeeping_first) {
      tw2de::frame_timing::PartScope part(tw2de::frame_timing::kPartMemoryReport);
      MaybeReportMemory();
    }
    tw2de::frame_timing::TakeParts(swap_ordinal);  // tw2de (2026-11-05, tw2de_diag_vsync_timing)
  });
  RunReadyCallbacks();

  if (device) {
    native::RewindRing(native::DeviceAt(device_va));
    rex::ppc::GuestToHostFunction<void>(__imp__sub_82D0BEF8, device_va, 0u);  // D3D::ResetAllState
  }
}

// void D3DDevice_InsertCallback(D3DDevice*, D3DCALLBACKTYPE, D3DCALLBACK pCallback,
//                               DWORD Context) [0x821FB688]
// The GPU would call pCallback(Context) on reaching this point in the ring,
// and the caller may reuse whatever the preceding work reads once it runs: it
// runs once the render thread has read that work's data (callbacks.h). N&B's
// callers: EndTiling (D3D's WorkerDpc) and the XMV renderer
// (CCalVideoRenderer::Render 0x82A0D9E8, which counts frames in flight until
// IXMediaXmvPlayer_Resume and frees itself after the last).
void D3DDevice_InsertCallback_hook(u32 device, u32 /*type*/, u32 callback, u32 context) {
  if (!callback)
    return;
  FlushDeferredVertices(device);
  RunReadyCallbacks();
  InsertCallback(callback, context);
}

// ---------------------------------------------------------------------------
// Device queries, thread ownership, presentation state
// ---------------------------------------------------------------------------

constexpr u32 kDeviceOffsetReferenceCount = 0x3C;
constexpr u32 kDeviceOffsetOwnerThread = 0x2B08;
constexpr u32 kDeviceCapsVa = 0x82183B00;  // N&B's constant D3DCAPS9
constexpr u32 kDeviceCapsSize = 0x130;
constexpr u32 kFusionVa = 0x82F597B4;      // non-zero on a Fusion GPU

// ULONG D3DDevice_AddRef(D3DDevice*) [0x8264E068]
u32 D3DDevice_AddRef_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  const u32 count = d.W(kDeviceOffsetReferenceCount) + 1;
  d.SetW(kDeviceOffsetReferenceCount, count);
  return count;
}

// void D3DDevice_AcquireThreadOwnership(D3DDevice*) [0x8264E0D8]
void D3DDevice_AcquireThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, GuestGetCurrentThreadId());
}

// void D3DDevice_ReleaseThreadOwnership(D3DDevice*) [0x8264E118]
void D3DDevice_ReleaseThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  std::atomic_thread_fence(std::memory_order_seq_cst);  // eieio
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, 0);
}

// void D3DDevice_GetDeviceCaps(D3DDevice*, D3DCAPS9*) [0x82658948]
void D3DDevice_GetDeviceCaps_hook(u32 /*device*/, u32 caps_va) {
  auto *caps = GuestPtr<u8>(caps_va);
  const auto *source = GuestPtr<const u8>(kDeviceCapsVa);
  if (caps && source)
    std::memcpy(caps, source, kDeviceCapsSize);
}

// void D3DDevice_GetDisplayMode(D3DDevice*, UINT iSwapChain, D3DDISPLAYMODE*)
// [0x82658960]: width, height, refresh rate, format.
void D3DDevice_GetDisplayMode_hook(u32 device_va, u32 /*swap_chain*/, mapped_u32 mode) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d || !mode)
    return;
  mode[0] = d.W(0x367C);
  mode[1] = d.W(0x3680);
  mode[2] = d.W(0x55CC);
  mode[3] = d.W(0x35C0);
}

// void D3DDevice_BlockOnFence(DWORD Fence) [0x8228F3B0]: on the device D3D
// finds through VdGlobalDevice.
void D3DDevice_BlockOnFence_hook(u32 fence) {
  if (fence)
    native::BlockOnFence(native::GlobalDevice(), fence);
}

// void D3D::SynchronizeToPresentationInterval(CDevice*, BOOL) [0x8228A748]:
// its swap callback (SwapCallback) and the waits around it go into the ring;
// the swap is then pending.
void D3D_SynchronizeToPresentationInterval_hook(u32 device_va, u32 /*immediate*/) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return;
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D22C68, device_va, 0u);  // tw2de: D3DDevice_SetPredication
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
}

// void D3DResource_GetDevice(D3DResource*, D3DDevice**) [0x82654D50]: the
// device D3D finds through VdGlobalDevice, referenced.
void D3DResource_GetDevice_hook(u32 /*resource*/, mapped_u32 out_device) {
  const u32 device = native::GlobalDevice();
  if (device)
    D3DDevice_AddRef_hook(device);
  if (out_device)
    *out_device = device;
}

// BOOL D3D::IsFusion() [0x82B6BF60]
u32 D3D_IsFusion_hook() {
  return LoadGuestU32(kFusionVa) != 0 ? 1 : 0;
}

// D3D's gamma tables (sub_82659140, _Gamma_sRGB 0x82659090): 1024 entries
// taking a 10-bit value through the display's gamma (VdGetCurrentDisplayGamma).
// Inverse goes from the display's curve to linear. D3D caches the table per
// display gamma (unk_82FB2558); it is computed per call here.
float Fmsubs(float a, float b, float c) {
  return std::fmaf(a, b, -c);
}

float Bits(u32 bits) {
  return std::bit_cast<float>(bits);
}

float GammaSRGB(float v, bool inverse) {
  if (inverse) {
    return v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                       : v * Bits(0x3D9E8391);
  }
  return v > 0.0031308 ? Fmsubs(float(std::pow(double(v), 0.4166666567325592)), Bits(0x3F870A3D),
                                Bits(0x3D6147AE))
                       : v * Bits(0x414EB852);
}

std::array<u16, 1024> GammaTable(bool inverse) {
  // Guest words for the kernel's outputs.
  static const u32 scratch = [] {
    void *host = records::AllocRaw(8);
    return host ? REX_KERNEL_MEMORY()->HostToGuestVirtual(host) : 0u;
  }();
  u32 type = 0;
  float gamma = 0.0f;
  if (scratch) {
    VdGetCurrentDisplayGamma(scratch, scratch + 4);
    type = LoadGuestU32(scratch);
    gamma = std::bit_cast<float>(LoadGuestU32(scratch + 4));
  }
  std::array<u16, 1024> table{};
  for (u32 i = 0; i < 1024; ++i) {
    float v = float(i) * Bits(0x3A802008);  // flt_8219F708, 1 / 1023
    if (!inverse) {
      v = v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                      : v * Bits(0x3D9E8391);
    }
    float out;
    if (type == 1) {
      out = GammaSRGB(v, inverse);
    } else if (type == 3) {
      out = float(std::pow(double(v), double(inverse ? gamma : 1.0f / gamma)));
    } else if (inverse) {
      out = v < 0.0812 ? v * Bits(0x3E638E39)
                       : float(std::pow(double((v + Bits(0x3DCAC083)) * Bits(0x3F68F065)), 2.222222328186035));
    } else {
      out = v < 0.018 ? v * 4.5f
                      : Fmsubs(float(std::pow(double(v), 0.44999998807907104)), Bits(0x3F8CAC08),
                               Bits(0x3DCAC083));
    }
    if (inverse) {
      out = out > 0.0031308 ? Fmsubs(float(std::pow(double(out), 0.4166666567325592)), Bits(0x3F870A3D),
                                     Bits(0x3D6147AE))
                            : out * Bits(0x414EB852);
    }
    const i32 value = native::TruncateToI32(std::fma(double(out), 1023.0, 0.5));
    table[i] = u16(std::clamp(value, 0, 0x3FF));
  }
  return table;
}

// The device's gamma (+0x3A9C, red, green, blue: 256 words each) for the
// display to pick up; the renderer presents without it.
constexpr u32 kDeviceOffsetGammaRamp = 0x3C30;  // tw2de (N&B 0x3A9C)

void StoreGamma(native::Device d, const be_u16 (&corrected)[768]) {
  if (std::memcmp(corrected, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected)) == 0)
    return;
  if (KeGetCurrentProcessType() != 2) {
    // The display's pending copy takes the previous ramp first.
    const u32 pending = d.W(0x5F2C);
    if (pending && !(d.W(0x5F30) & 0x80000000u)) {
      if (auto *dest = GuestPtr<u8>(pending))
        std::memcpy(dest, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected));
    }
    d.SetW(0x5F30, d.W(0x5F30) | 0x80000000u);
  }
  std::memcpy(d.bytes + kDeviceOffsetGammaRamp, corrected, sizeof(corrected));
  d.SetW(0x42DC, d.W(0x42DC) | 2);
}

// D3DDevice_SetGammaRamp's work: the ramp (D3DGAMMARAMP, 3 x 256 words)
// through the gamma table (GammaRampCorrected 0x826593F8).
void SetGammaRamp(native::Device d, const u16 *ramp) {
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 i = 0; i < 768; ++i)
    corrected[i] = u16((table[ramp[i] >> 6] & 0x3FF) << 6);
  StoreGamma(d, corrected);
}

// D3DDevice_SetPWLGamma's: D3DPWLGAMMA (3 x 128 {base, delta}) through the
// table (sub_82659490), each segment's end clamped to 0x3FF.
void SetPWLGamma(native::Device d, const u16 *pwl) {
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 c = 0; c < 3; ++c) {
    for (u32 i = 0; i < 128; ++i) {
      const u32 at = c * 256 + i * 2;
      const u32 base = pwl[at] >> 6;
      const u32 end = std::min(base + (pwl[at + 1] >> 6), 0x3FFu);
      corrected[at] = u16((table[base] & 0x3FF) << 6);
      corrected[at + 1] = u16(((table[end] - table[base]) & 0x3FF) << 6);
    }
  }
  StoreGamma(d, corrected);
}

void ReadRamp(u32 ramp_va, u16 (&ramp)[768]) {
  const auto *guest = GuestPtr<const be_u16>(ramp_va);
  for (u32 i = 0; i < 768; ++i)
    ramp[i] = guest ? u16(guest[i]) : 0;
}

// void D3DDevice_SetGammaRamp(D3DDevice*, DWORD Flags, const D3DGAMMARAMP*)
// [0x8264D948]
void D3DDevice_SetGammaRamp_hook(u32 device_va, u32 /*flags*/, u32 ramp_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !ramp_va)
    return;
  u16 ramp[768];
  ReadRamp(ramp_va, ramp);
  SetGammaRamp(d, ramp);
}

// void D3DDevice_SetPWLGamma(D3DDevice*, DWORD Flags, const D3DPWLGAMMA*)
// [0x8264DA18]
void D3DDevice_SetPWLGamma_hook(u32 device_va, u32 /*flags*/, u32 pwl_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !pwl_va)
    return;
  u16 pwl[768];
  ReadRamp(pwl_va, pwl);
  SetPWLGamma(d, pwl);
}

// The D3D internals Direct3D_CreateDevice and D3DDevice_Release run, for any
// other caller.
#if 0  // tw2de: not hooked (the game's own code runs)
void D3D_XenonGpu_Initialize_hook(u32 device) {
  XenonGpuInitialize(native::DeviceAt(device));
}
void D3D_InitializeApiState_hook(u32 device) {
  InitializeApiState(native::DeviceAt(device));
}
u32 D3D_InitializePresentationParameters_hook(u32 device, u32 parameters) {
  return InitializePresentationParameters(native::DeviceAt(device), parameters) ? 1 : 0;
}
void D3D_CDevice_Uninitialize_hook(u32 device) {
  UninitializeDevice(native::DeviceAt(device));
}

#endif
} // namespace

namespace renut::gpu::native {

// tw2de (diagnostic, tw2de_ring_scan): what the game's own code wrote into the ring stub since the last rewind.
// The hooked D3D calls write nothing there; any packet is GPU work the renderer never sees (a draw, a resolve, a
// memory write, a command buffer call issued without a hooked function), which the original renderer does execute.
// Logged: every type-3 packet of a kind not seen before (with its first words), a draw's or a copy's every time
// (limited), and a summary every 10 s.
void ScanRingSink(u32 begin, u32 end) {
  if (!REXCVAR_GET(tw2de_ring_scan) || end <= begin || end - begin > kRingSinkSize)
    return;
  const auto *w = GuestPtr<const be_u32>(begin);
  if (!w)
    return;
  static uint64_t type3[128] = {}, type0_writes = 0, words_total = 0;
  static uint32_t reg_hits[0x8000 / 64] = {};
  static uint32_t logged = 0;
  static auto last = std::chrono::steady_clock::now();
  const u32 n = (end - begin) / 4;
  words_total += n;
  for (u32 i = 0; i < n;) {
    const u32 h = w[i];
    const u32 type = h >> 30;
    if (type == 0) {
      const u32 count = ((h >> 16) & 0x3FFF) + 1, reg = h & 0x7FFF;
      ++type0_writes;
      ++reg_hits[reg / 64];
      if ((reg >= 0x2318 && reg <= 0x231F) && logged++ < 200) {  // RB_COPY_*: a resolve set up by hand
        std::string words;
        for (u32 k = 0; k < count && k < 8 && i + 1 + k < n; ++k)
          words += fmt::format(" {:08X}", u32(w[i + 1 + k]));
        RGPU_INFO("[ring] type0 reg {:04X} x{}:{}", reg, count, words);
      }
      i += 1 + count;
    } else if (type == 3) {
      const u32 op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
      const bool first = type3[op]++ == 0;
      const bool interesting = op == 0x22 || op == 0x36 || op == 0x3F || op == 0x37 || op == 0x3D || op == 0x46 ||
                               op == 0x58 || op == 0x59 || op == 0x45;
      if ((first || interesting) && logged++ < 200) {
        std::string words;
        for (u32 k = 0; k < count && k < 8 && i + 1 + k < n; ++k)
          words += fmt::format(" {:08X}", u32(w[i + 1 + k]));
        RGPU_INFO("[ring] type3 op {:02X} x{}:{}", op, count, words);
      }
      i += 1 + count;
    } else if (type == 2) {
      i += 1;
    } else {
      i += 3;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(10)) {
    last = now;
    std::string ops;
    for (u32 op = 0; op < 128; ++op)
      if (type3[op])
        ops += fmt::format(" {:02X}:{}", op, type3[op]);
    std::string regs;
    for (u32 r = 0; r < 0x8000 / 64; ++r)
      if (reg_hits[r])
        regs += fmt::format(" {:04X}:{}", r * 64, reg_hits[r]);
    RGPU_INFO("[ring] 10 s: {} words; type0 writes {} (by 64-register block:{}); type3 ops:{}", words_total,
              type0_writes, regs, ops);
  }
}

void RewindRing(Device device) {
  if (!device || !g_ring_sink)
    return;
  ScanRingSink(g_ring_sink, device.W(off::Ring));  // tw2de
  device.SetW(off::Ring, g_ring_sink);
  device.SetW(off::RingLimit, g_ring_sink + kRingSinkSize);
  device.SetW(off::RingGuarantee, g_ring_sink + kRingSinkSize - kRingSinkGuaranteeMargin);
}

u32 KickOff(u32 device_va) {
  return KickOff_hook(device_va);
}

void BlockOnFence(u32 device_va, u32 fence) {
  BlockOnFence_hook(device_va, fence, 0, 0);
}

u32 GlobalDevice() {
  const u32 global = LoadGuestU32(kVdGlobalDeviceImportSlot);
  return global ? LoadGuestU32(global) : 0;
}

void Swap(u32 device_va, u32 front_buffer, u32 parameters) {
  D3DDevice_Swap_hook(device_va, front_buffer, parameters);
}

// CDevice::SetFence (0x82278528): with immediate fences the device writes the
// completion itself (unless +0x541C is set); otherwise the fence completes on
// the render thread, as a KickOff's. A device recording a command buffer
// would put it into the buffer, which runs on the host.
void SetFence(u32 device_va) {
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return;
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences) {
    Device d = DeviceAt(device_va);
    const u32 fence = d.W(off::FenceTag);
    if (!d.W(0x55B4)) {  // tw2de (N&B 0x541C)
      if (auto *completion = GuestPtr<be_u32>(d.W(off::FenceCompletion))) {
        completion[0] = fence;
        completion[1] = (d.W(0x3BDC) & 3) | d.W(off::Ring);  // tw2de (N&B 0x3A48)
      }
    }
    d.SetW(off::FenceTag, fence + 2);
    return;
  }
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(GuestPtr<d3d::Device>(device_va), device_va);
}

} // namespace renut::gpu::native

TW2DE_RENUT_HOOK(sub_82D15F48, D3DDevice_InsertCallback_hook)  /* N&B sub_821FB688 */;  // D3DDevice_InsertCallback
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_8264E068, D3DDevice_AddRef_hook);  // D3DDevice_AddRef
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_8264E0D8, D3DDevice_AcquireThreadOwnership_hook);  // D3DDevice_AcquireThreadOwnership
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_8264E118, D3DDevice_ReleaseThreadOwnership_hook);  // D3DDevice_ReleaseThreadOwnership
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_82658948, D3DDevice_GetDeviceCaps_hook);  // D3DDevice_GetDeviceCaps
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_82658960, D3DDevice_GetDisplayMode_hook);  // D3DDevice_GetDisplayMode
TW2DE_RENUT_HOOK(sub_82D148C0, D3DDevice_BlockOnFence_hook)  /* N&B sub_8228F3B0 */;  // D3DDevice_BlockOnFence
// tw2de: not hooked (only Swap and Present call it, and Swap is replaced): D3D_SynchronizeToPresentationInterval_hook
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_82654D50, D3DResource_GetDevice_hook);  // D3DResource_GetDevice
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_82B6BF60, D3D_IsFusion_hook);  // D3D_IsFusion
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_8264D948, D3DDevice_SetGammaRamp_hook);  // D3DDevice_SetGammaRamp
// tw2de: not hooked (the game's own code runs): D3DDevice_SetPWLGamma_hook
TW2DE_RENUT_HOOK(sub_82D07C58, Direct3D_CreateDevice_hook)  /* N&B sub_8264DF28 */;  // Direct3D_CreateDevice
TW2DE_RENUT_HOOK(sub_82D07698, D3DDevice_Release_hook)  /* N&B sub_8264E080 */;  // D3DDevice_Release
// tw2de: not hooked (the game's own code runs): D3D_XenonGpu_Initialize_hook
// tw2de: not hooked (the game's own code runs): D3D_InitializeApiState_hook
// tw2de: not hooked (the game's own code runs): D3D_InitializePresentationParameters_hook
// tw2de: not hooked (the game's own code runs): D3D_CDevice_Uninitialize_hook
TW2DE_RENUT_HOOK(sub_82D15C18, KickOff_hook)  /* N&B sub_82210AB0 */;  // D3D_CDevice_KickOff
TW2DE_RENUT_HOOK(sub_82D15890, KickOffSegment_hook)  /* N&B sub_822106C8 */;  // D3D_CDevice_KickOffSegment
TW2DE_RENUT_HOOK(sub_82D15E58, BeginRingBig_hook)  /* N&B sub_821F7C18 */;  // D3D_CDevice_BeginRingBig
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_82A11158, BeginRingManual_hook);  // D3D_CDevice_BeginRingManual
TW2DE_RENUT_HOOK(sub_82D145C0, RingBufferDeviceAllocate_hook)  /* N&B sub_8222D3E0 */;  // D3D_RingBufferDeviceAllocate
TW2DE_RENUT_HOOK(sub_82D14A08, AllocateInlineData_hook)  /* N&B sub_8222D240 */;  // D3D_CDevice_AllocateInlineData
TW2DE_RENUT_HOOK(sub_82D15FA0, D3DDevice_SetRingBufferParameters_hook)  /* N&B sub_82657508 */;  // D3DDevice_SetRingBufferParameters
TW2DE_RENUT_HOOK(sub_82D172F8, D3DDevice_Swap_hook)  /* N&B sub_82287590 */;  // D3DDevice_Swap
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK_SHARED(sub_82657CB0, D3DDevice_BeginCommandBuffer_hook);  // D3DDevice_BeginCommandBuffer
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_826582A0, D3DDevice_EndCommandBuffer_hook);  // D3DDevice_EndCommandBuffer
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK_SHARED(sub_82222888, D3DDevice_RunCommandBuffer_hook);  // D3DDevice_RunCommandBuffer
TW2DE_RENUT_HOOK(sub_82D147F0, BlockOnFence_hook)  /* N&B sub_8228F558 */;  // D3D_CDevice_BlockOnFence
TW2DE_RENUT_HOOK(sub_82D07318, CBlocker_Check_hook)  /* N&B sub_8264DCD0 */;  // D3D_CBlocker_Check
TW2DE_RENUT_HOOK(sub_82D15D58, IsFencePending_hook)  /* N&B sub_8228F650 */;  // D3DDevice_IsFencePending
