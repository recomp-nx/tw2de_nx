/**
 * @file    gpu/backend/video.h
 * @brief   The native renderer's device, frame ring and shared GPU state.
 *
 * Threading: the render thread (render_thread.h) records draws and presents
 * the work the game's threads queue.
 * Every mutation of VideoState happens under VideoState::mutex.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "renut_engine/gpu/render/upload_ring.h"

namespace renut::gpu {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kTextureDescriptorCount = 8192;
constexpr uint32_t kSamplerDescriptorCount = 256;
constexpr uint32_t kInvalidDescriptor = UINT32_MAX;

// Fixed bindless slots. The table is shared by the 2D, 3D and cube arrays the
// recompiled shaders declare, so each dimension has its own null texture.
constexpr uint32_t kNullTextureSlot = 0;        // 2D, transparent black; retired slots point here
constexpr uint32_t kNullTexture3DSlot = 1;
constexpr uint32_t kNullTextureCubeSlot = 2;
constexpr uint32_t kFirstDynamicTextureSlot = 3;
constexpr uint32_t kLinearClampSamplerSlot = 0;
constexpr uint32_t kPointClampSamplerSlot = 1;
constexpr uint32_t kFirstDynamicSamplerSlot = 2;

// Depth resolve outputs (shaders/host/depth_resolve.hlsli): the destination
// texture, and the guest words for 8_8_8_8 textures over its memory.
constexpr plume::RenderFormat kDepthResolveFormat = plume::RenderFormat::R32_FLOAT;
constexpr plume::RenderFormat kDepthResolveWordsFormat = plume::RenderFormat::R8G8B8A8_UNORM;

// Matches UtilityPush in shaders/host/utility.hlsli.
struct UtilityPush {
  float scale[2];
  float translate[2];
  uint32_t texture_index;
  uint32_t sampler_index;
  float param0;
  float param1;
};
static_assert(sizeof(UtilityPush) == 32);

// Matches StreamConvertPush in shaders/host/stream_convert_cs.hlsl.
constexpr uint32_t kStreamConvertMaxElements = 8;
struct StreamConvertPush {
  uint32_t source;
  uint32_t source_offset;
  uint32_t dest_offset;
  uint32_t vertices;
  uint32_t guest_stride_words;
  uint32_t host_stride_words;
  uint32_t element_count;
  uint32_t pad;
  uint32_t elements[2 * kStreamConvertMaxElements];
};
static_assert(sizeof(StreamConvertPush) == 96);
// stream_convert_cs: the export buffers of the slots come first in its set.
constexpr uint32_t kStreamConvertBuffers = kFramesInFlight + 1;
constexpr uint32_t kStreamConvertMaxStrideWords = 255;

// Matches TargetReadbackPush in shaders/host/target_readback_cs.hlsl.
struct TargetReadbackPush {
  uint32_t texture_index;
  uint32_t width;  // guest pixels
  uint32_t height;
  uint32_t scale_numerator;
  uint32_t scale_denominator;
  uint32_t pad[3];
};
static_assert(sizeof(TargetReadbackPush) == 32);

// What Present scans out: a sampleable host texture and its tracked layout.
struct PresentImage {
  plume::RenderTexture *texture = nullptr;
  plume::RenderTextureLayout *layout = nullptr;  // updated by Present's barriers
  uint32_t descriptor = kInvalidDescriptor;
  uint32_t width = 0;   // full texture size
  uint32_t height = 0;
  uint32_t source_width = 0;   // region to scan out (0 = whole texture)
  uint32_t source_height = 0;
  float gamma = 0.0f;  // tw2de (tw2de_renut_diag_show_texture): the scan-out's exponent, 0 = the output gamma
};


struct VideoState {
  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::string backend_description;

  // Frame ring. command_list aliases command_lists[frame] while one is open.
  std::unique_ptr<plume::RenderCommandList> command_lists[kFramesInFlight];
  std::unique_ptr<plume::RenderCommandFence> fences[kFramesInFlight];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores[kFramesInFlight];
  bool submitted[kFramesInFlight] = {};
  UploadRing uploads[kFramesInFlight];
  uint32_t frame = 0;
  plume::RenderCommandList *command_list = nullptr;
  bool list_open = false;
  // tw2de (2026-10-19, tw2de_deko3d_kick_submit): a slot's frame may record into more lists, submitted at the
  // game's kicks: the list the slot records into now (command_lists[slot] or one of kick_lists[slot]), the kick lists
  // taken in the slot's frame, the list ended at the last kick and not yet submitted (with the cleaner's ticket for
  // its memory), and the draws recorded into the open list.
  static constexpr uint32_t kKickLists = 6;  // tw2de (2026-11-05): pieces of a frame (progressive submit)
  std::unique_ptr<plume::RenderCommandList> kick_lists[kFramesInFlight][kKickLists];
  plume::RenderCommandList *slot_list[kFramesInFlight] = {};
  uint32_t kick_lists_used[kFramesInFlight] = {};
  plume::RenderCommandList *pending_list = nullptr;
  uint64_t pending_ticket = 0;
  uint32_t list_draws = 0;
  uint32_t pending_draws = 0;  // tw2de (2026-11-05, progressive submit): the draws in pending_list
  // tw2de (2026-11-05, progressive submit): the frame being recorded: its draws, the draws already sent to the GPU
  // in pieces, its kicks, its pieces sent, the render thread's time submitting; and the previous frame's draws (the
  // piece size).
  uint32_t frame_draws = 0, frame_draws_sent = 0, frame_kicks = 0, frame_pieces = 0, last_frame_draws = 0;
  uint64_t frame_submit_ns = 0;
  // Bumped each time a command list begins; bound state cached against an
  // older value has to be set again.
  uint64_t list_generation = 0;

  // GPU time for the frame statistics: a timestamp pair around each command list a
  // slot submits in its frame (memexport flushes submit mid-frame), summed
  // once the slot's fence has been waited. Null without query pool support.
  static constexpr uint32_t kTimedLists = 32;
  std::unique_ptr<plume::RenderQueryPool> timestamps[kFramesInFlight];
  uint32_t timed_lists[kFramesInFlight] = {};  // pairs begun in the slot's frame
  bool timing_open = false;                    // the open list wrote its first timestamp
  // Swapchain; present semaphores are per swapchain image (a binary semaphore
  // can only be re-signalled once its image has been re-acquired).
  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> swap_framebuffers;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> present_semaphores;
  std::atomic<bool> resize_requested{false};

  // Bindless tables, shared by every pipeline layout.
  std::unique_ptr<plume::RenderDescriptorSet> texture_set;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_set;
  std::vector<bool> texture_slot_used;
  uint32_t texture_slots_in_use = 0;  // dynamic slots taken (TextureSlotsInUseLocked)
  std::vector<bool> sampler_slot_used;
  std::unique_ptr<plume::RenderTexture> null_texture;
  std::unique_ptr<plume::RenderTextureView> null_texture_view;
  std::unique_ptr<plume::RenderTexture> null_texture_3d;
  std::unique_ptr<plume::RenderTextureView> null_texture_3d_view;
  std::unique_ptr<plume::RenderTexture> null_texture_cube;
  std::unique_ptr<plume::RenderTextureView> null_texture_cube_view;
  // tw2de: tw2de_renut_diag_poison_slots' magenta texture, which slots given back point at (null without the key).
  std::unique_ptr<plume::RenderTexture> poison_texture;
  std::unique_ptr<plume::RenderTextureView> poison_texture_view;
  bool null_texture_initialized = false;
  std::unique_ptr<plume::RenderSampler> linear_clamp_sampler;
  std::unique_ptr<plume::RenderSampler> point_clamp_sampler;

  // Renderer-owned pipelines (present, compile notice).
  std::unique_ptr<plume::RenderPipelineLayout> utility_layout;
  std::unique_ptr<plume::RenderShader> present_vs;
  std::unique_ptr<plume::RenderShader> present_ps;
  std::unique_ptr<plume::RenderPipeline> present_pipeline;
  // tw2de (2026-11-07): the scan-out with Lanczos-2 or FSR 1's EASU (the boot menu's Upscaler), when it upscales
  // (a 1280x720 scene to a 1920x1080 output); optional.
  std::unique_ptr<plume::RenderShader> present_lanczos_ps, present_easu_ps;
  std::unique_ptr<plume::RenderPipeline> present_lanczos_pipeline, present_easu_pipeline;
  float output_gamma = 1.0f;
  // The guest texture the last Swap scanned out; stall presents (the guest not
  // presenting for a while) show it again rather than a black frame.
  uint32_t last_front_buffer = 0;
  // Depth resolves (draw.cpp): [0] single-sampled source, [1] multisampled.
  std::unique_ptr<plume::RenderShader> depth_resolve_ps[2];
  std::unique_ptr<plume::RenderShader> resolve_copy_ps;  // tw2de (2026-10-18): DrawResolveCopy
  std::unique_ptr<plume::RenderShader> resolve_copy_ms_ps;  // tw2de: DrawResolveCopy of one sample
  // tw2de (2026-10-25, tw2de_renut_upscale_filter): Renut's upscalers, the pixel shader of the game's upscale draw
  // instead of the game's (draw.cpp): Lanczos-2 (shaders/deko3d/lanczos2_ps.glsl) and the fit of the game's filter
  // (upscale_l15_ps.glsl). deko3d only; nullptr elsewhere.
  std::unique_ptr<plume::RenderShader> lanczos2_ps;
  // TW2 (2026-11-12): a depth target filled from another's depth plane (render/draw.cpp HalfDepthFillLocked).
  std::unique_ptr<plume::RenderShader> depth_downsample_ps;
  std::unique_ptr<plume::RenderShader> upscale_l15_ps;
  std::unique_ptr<plume::RenderShader> fsr1_easu_ps;
  std::unique_ptr<plume::RenderShader> upscale_bilinear_ps;  // tw2de (2026-10-25): plain bilinear (filter 4)  // tw2de (2026-10-25): FSR 1's EASU (filter 3)
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> resolve_copy_pipelines;  // by format
  std::unique_ptr<plume::RenderPipeline> depth_resolve_pipeline[2];
  // tw2de (2026-10-19, tw2de_renut_depth_words_lazy): the same with the words target's writes masked off.
  std::unique_ptr<plume::RenderPipeline> depth_resolve_float_pipeline[2];
  // Host mip levels of scaled resolve destinations (textures.cpp); null when
  // it failed to build, which leaves them single-level.
  std::unique_ptr<plume::RenderShader> texture_mip_ps;
  // Expands rectangle-list draws (pipeline.cpp); null without geometry shader
  // support, which leaves them drawn as triangle lists.
  std::unique_ptr<plume::RenderShader> rectangle_list_gs;
  // Expands point-list draws into point sprites (pipeline.cpp); null like
  // rectangle_list_gs, which leaves them single-pixel points.
  std::unique_ptr<plume::RenderShader> point_sprite_gs;

  // Layout of the recompiled guest shaders (see pipeline.h).
  std::unique_ptr<plume::RenderPipelineLayout> draw_layout;
  // Memexport (see draw.cpp), per slot: the buffer shader exports store to
  // (root UAV / device address), and the readback buffer their results are
  // copied into. A slot's results reach guest memory once its fence is waited.
  std::unique_ptr<plume::RenderBuffer> memexport_buffer[kFramesInFlight];
  uint64_t memexport_capacity[kFramesInFlight] = {};
  std::unique_ptr<plume::RenderBuffer> memexport_readback[kFramesInFlight];
  uint64_t memexport_readback_capacity[kFramesInFlight] = {};
  // Seeds from the slot's own earlier windows pass through here (a buffer
  // cannot be copied into itself); allocated linearly through the slot's frame.
  std::unique_ptr<plume::RenderBuffer> memexport_scratch[kFramesInFlight];
  uint64_t memexport_scratch_capacity[kFramesInFlight] = {};
  uint64_t memexport_scratch_used[kFramesInFlight] = {};
  // Vertex streams built on the GPU from export windows (stream_convert_cs;
  // draw.cpp): null when the pipeline could not be made, and draws wait for
  // the exports instead. The set is replaced, never rewritten, when the
  // buffers change (earlier dispatches of the slot still use it).
  std::unique_ptr<plume::RenderPipelineLayout> stream_convert_layout;
  std::unique_ptr<plume::RenderShader> stream_convert_cs;
  std::unique_ptr<plume::RenderPipeline> stream_convert_pipeline;
  std::unique_ptr<plume::RenderDescriptorSet> stream_convert_set;
  const plume::RenderBuffer *stream_convert_set_buffers[kStreamConvertBuffers] = {};
  std::unique_ptr<plume::RenderBuffer> exported_streams[kFramesInFlight];
  uint64_t exported_streams_capacity[kFramesInFlight] = {};
  uint64_t exported_streams_used[kFramesInFlight] = {};
  // Vertex fetch data (vfetchMemory / vfetchBound) some of whose buffers are
  // copied from export windows on the GPU (UploadVertexFetchData).
  std::unique_ptr<plume::RenderBuffer> gpu_fetch_data[kFramesInFlight];
  uint64_t gpu_fetch_data_capacity[kFramesInFlight] = {};
  uint64_t gpu_fetch_data_used[kFramesInFlight] = {};
  // Render target readback for the CPU (target_readback_cs; draw.cpp
  // ReadRenderTargetLocked): null when it could not be made.
  std::unique_ptr<plume::RenderPipelineLayout> target_readback_layout;
  std::unique_ptr<plume::RenderShader> target_readback_cs;
  std::unique_ptr<plume::RenderPipeline> target_readback_pipeline;

  // Destruction deferred until the slot's fence has been waited.
  std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures[kFramesInFlight];
  std::vector<std::unique_ptr<plume::RenderTextureView>> retired_views[kFramesInFlight];
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired_buffers[kFramesInFlight];
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_framebuffers[kFramesInFlight];
  std::vector<std::unique_ptr<plume::RenderDescriptorSet>> retired_sets[kFramesInFlight];
  std::vector<uint32_t> retired_texture_slots[kFramesInFlight];


  std::mutex mutex;
  bool host_ready = false;   // device + swapchain + utility pipelines
  bool guest_ready = false;  // guest D3D device created on top of it
  std::atomic<uint32_t> guest_device{0};
  // steady_clock milliseconds of the last guest Swap presentation.
  std::atomic<int64_t> last_guest_present_ms{0};
  std::atomic<bool> shutting_down{false};
};

// The renderer's state (constructed on first use). tw2de (2026-10-19): inline, a pointer load after the first call.
extern VideoState *g_state;
VideoState &StateSlow();
inline VideoState &State() {
  VideoState *s = g_state;
  return s ? *s : StateSlow();
}

class Video {
public:
  // UI thread. Creates the plume device, swapchain and renderer pipelines.
  // Idempotent; safe before the guest runtime exists.
  static bool CreateHostDevice();
  static bool HostReady();

  // Stops recording/presenting. Resources are intentionally leaked at exit.
  static void BeginShutdown();

  static plume::RenderDevice *Device();

  // Called from the UI thread on window resize; applied at the next present.
  static void RequestResize();


  // Guest D3DDevice_Swap: scan out 'image' (nullptr = no guest image this
  // frame), submit and present.
  static void Present(const PresentImage *image);
  // tw2de (2026-10-20, tw2de_renut_game_gamma): the device's corrected gamma ramp at a Swap (768 big-endian words,
  // red, green, blue: the 10-bit entry << 6, d3d/hooks/device.cpp StoreGamma); the present applies it when the key is
  // on. Any thread.
  static void NoteGameGamma(const uint8_t *ramp);
  // tw2de (2026-10-20, input latency): the guest Swap the next present ends (the render thread, right before it),
  // and tw2de_deko3d_latency_trim's 10 s report line (empty when off).
  static void NotePresentingSwap(uint64_t swap);
  static std::string LatencyTrimReport();
  // tw2de (2026-10-19, latency batch 3, renut_engine/display_sync.cpp): the guest presents so far, and whether the
  // GPU has finished the frame of the 'index'-th (1-based): 1 yes, 0 not yet, -1 unknown (no such present, or a back
  // end without fence copies). Any thread; PresentDone never waits.
  static uint64_t PresentCount();
  static int PresentDone(uint64_t index);
  // tw2de (2026-11-05, tw2de_diag_vsync_timing): waits for that frame's GPU end; false when it is not known.
  static bool WaitPresentDone(uint64_t index);

  // tw2de (2026-10-22): the swapchain's size at the last present (0 x 0 before one), any thread.
  static bool LastOutputSize(uint32_t &width, uint32_t &height);

  // The guest D3DDevice (guest VA) created by the Direct3D_CreateDevice hook.
  static void SetGuestDevice(uint32_t device_va);
  static uint32_t GuestDevice();

  // --- Callers must hold State().mutex for everything below. ---

  // Opens the current frame's command list if it is not open yet.
  static bool EnsureCommandListLocked();
  // tw2de (2026-10-19, tw2de_deko3d_kick_submit / tw2de_deko3d_ring_cleaner): the game kicked its ring and its
  // fills are done (QueueKickFlush): the upload ring written so far goes to the cleaner, the list ended at the last
  // kick to the GPU, and the open list ends here when it holds enough draws. And a draw recorded into the open list.
  static void KickLocked();
  static void NoteDrawLocked();
  static plume::RenderCommandList *CommandListLocked();
  static UploadRing &UploadsLocked();

  static uint32_t AllocateTextureSlotLocked();
  static void SetTextureSlotLocked(uint32_t slot, plume::RenderTexture *texture,
                                   plume::RenderTextureView *view);
  // Rewritten to the null texture once the in-flight frames are done with it.
  static void FreeTextureSlotLocked(uint32_t slot);
  // The bindless texture slots taken (freed ones count until the in-flight
  // frames are done with them).
  static uint32_t TextureSlotsInUseLocked();

  static void RetireLocked(std::unique_ptr<plume::RenderTexture> texture);
  static void RetireLocked(std::unique_ptr<plume::RenderTextureView> view);
  static void RetireLocked(std::unique_ptr<plume::RenderBuffer> buffer);
  static void RetireLocked(std::unique_ptr<plume::RenderFramebuffer> framebuffer);
  static void RetireLocked(std::unique_ptr<plume::RenderDescriptorSet> set);

  // Records an RGBA8 upload into 'texture' (mip 0) through the frame's upload ring.
  static bool UploadRGBA8Locked(plume::RenderTexture *texture, uint32_t width,
                                uint32_t height, const uint8_t *rgba);
};

} // namespace renut::gpu
