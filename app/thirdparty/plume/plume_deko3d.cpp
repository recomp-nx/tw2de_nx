//
// plume deko3d back end (tw2de, 2026-10-17): device, memory, resources, shaders and pipelines. The command lists,
// the queue and the swapchain are in plume_deko3d_commands.cpp. docs/deko3d-renderer.md is the design; the facts about
// deko3d 0.5.0 used here (and its bugs) are listed there with where they were found.
//
#include "plume_deko3d.h"
#include "plume_deko3d_spec_defaults.h"

#include <switch.h>
#include <pthread.h>

#include <algorithm>
#include <unordered_map>
#include <map>
#include <chrono>
#include <mutex>
#include <string>
#include <cassert>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include <uam_api.h>

extern "C" {
extern volatile uint64_t g_rex_uam_compile_start, g_rex_uam_compile_hash, g_rex_uam_compiles, g_rex_uam_compile_end;
extern volatile uint32_t g_rex_uam_compile_stage, g_rex_uam_compile_size;
}

namespace plume {
    void NameSlowBlock(DkMemBlock block, const std::string &name);  // tw2de (2026-10-28)
    // Image heap: 64 MB chunks (bigger for a bigger image), first fit (Deko3DDevice::imageAlloc).
    static constexpr uint32_t ImageChunkSize = 64u << 20;
    // ---- helpers

    static void (*g_logCallback)(const char *message) = nullptr;
    void SetDeko3DLogCallback(void (*callback)(const char *message)) { g_logCallback = callback; }

    void deko3dLog(const char *fmt, ...) {
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        if (g_logCallback)
            g_logCallback(line);
        else
            fprintf(stderr, "[deko3d] %s\n", line);
    }
#define logf deko3dLog

    // Free memory, for the log lines that may be about running out of it: the heap's free bytes (newlib) and the
    // process's unused memory (the system's figures).
    extern "C" char *fake_heap_end; // libnx: the heap's end (sbrk grows towards it)
    // tw2de (2026-11-05): newlib's malloc bins (devkitA64 libc.a mallocr.c; the third word is the top chunk) and lock.
    extern "C" void *__malloc_av_[];
    extern "C" void __malloc_lock(struct _reent *);
    extern "C" void __malloc_unlock(struct _reent *);
    // tw2de (2026-11-05): the heap's room without mallinfo, for the logs made while the game runs. mallinfo takes the
    // malloc lock and walks every free chunk (newlib's __malloc_update_mallinfo; 13-20 ms with the game's heap): the
    // "memory block" line of the first 24 blocks of 8 MB or more (the upload ring's chunks, the stream buffers) cost the
    // render thread about 20 ms each in the area entry bursts (tw2de_diag_vsync_timing run 51b6ca3e: single texture
    // levels of 2 MB "copied" in 20 ms, the block itself 2.6 ms), and every thread's malloc waited meanwhile. The room:
    // newlib's top chunk (__malloc_av_[2], its size word masked, what mallinfo calls keepcost) and what sbrk can still
    // give; free chunks inside the heap are not counted.
    static void memoryFiguresQuick(uint64_t &heapRoom, uint64_t &processFree) {
        __malloc_lock(_REENT);
        const size_t *top = static_cast<const size_t *>(__malloc_av_[2]);
        const size_t keepcost = top ? top[1] & ~size_t(3) : 0;
        __malloc_unlock(_REENT);
        char *brk = static_cast<char *>(sbrk(0));
        heapRoom = uint64_t(keepcost) + (fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0);
        uint64_t total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        processFree = total > used ? total - used : 0;
    }

    static void memoryFigures(uint64_t &heapFree, uint64_t &processFree) {
        // Free chunks inside what malloc has taken, and what sbrk can still give.
        struct mallinfo mi = mallinfo();
        char *brk = static_cast<char *>(sbrk(0));
        heapFree = uint64_t(mi.fordblks) + (fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0);
        uint64_t total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        processFree = total > used ? total - used : 0;
    }

    // tw2de (2026-10-24): the buffer arena (Deko3DOptions::bufferArenaMB). Blocks of 1 MB << order; free blocks per
    // order; a freed block merges with its buddy while that one is free too.
    namespace {
    struct BufferArena {
        static constexpr uint64_t Unit = 1ull << 20;
        std::mutex mutex;
        uint8_t *base = nullptr;
        uint64_t units = 0;
        std::vector<std::vector<uint64_t>> freeList;  // per order: offsets in units
        std::unordered_map<uint64_t, uint32_t> taken;  // offset -> order
        uint64_t usedUnits = 0, peakUnits = 0;
        uint32_t fallbacks = 0;

        static uint32_t orderOf(uint64_t units) {
            uint32_t o = 0;
            while ((1ull << o) < units)
                o++;
            return o;
        }
        bool init(uint64_t megabytes) {
            if (!megabytes)
                return false;
            base = static_cast<uint8_t *>(memalign(Unit, megabytes * Unit));
            if (!base)
                return false;
            units = megabytes;
            freeList.assign(orderOf(units) + 1, {});
            // [0, units) as aligned power-of-two blocks, the largest first.
            uint64_t at = 0;
            while (at < units) {
                uint32_t o = orderOf(units - at);
                while ((1ull << o) > units - at || (at & ((1ull << o) - 1)))
                    o--;
                freeList[o].push_back(at);
                at += 1ull << o;
            }
            return true;
        }
        uint8_t *alloc(uint64_t bytes) {
            if (!base)
                return nullptr;
            const uint32_t want = orderOf((bytes + Unit - 1) / Unit);
            std::lock_guard<std::mutex> lock(mutex);
            uint32_t o = want;
            while (o < freeList.size() && freeList[o].empty())
                o++;
            if (o >= freeList.size()) {
                fallbacks++;
                return nullptr;
            }
            uint64_t at = freeList[o].back();
            freeList[o].pop_back();
            while (o > want) {  // split: the upper half stays free
                o--;
                freeList[o].push_back(at + (1ull << o));
            }
            taken[at] = want;
            usedUnits += 1ull << want;
            peakUnits = std::max(peakUnits, usedUnits);
            return base + at * Unit;
        }
        bool owns(const uint8_t *p) const { return base && p >= base && p < base + units * Unit; }
        void release(const uint8_t *p) {
            std::lock_guard<std::mutex> lock(mutex);
            uint64_t at = uint64_t(p - base) / Unit;
            auto it = taken.find(at);
            if (it == taken.end())
                return;
            uint32_t o = it->second;
            taken.erase(it);
            usedUnits -= 1ull << o;
            while (o + 1 < freeList.size()) {
                const uint64_t buddy = at ^ (1ull << o);
                if (buddy + (1ull << o) > units)
                    break;
                auto &list = freeList[o];
                auto b = std::find(list.begin(), list.end(), buddy);
                if (b == list.end())
                    break;
                list.erase(b);
                at = std::min(at, buddy);
                o++;
            }
            freeList[o].push_back(at);
        }
    };
    BufferArena g_bufferArena;

    void freeStorage(uint8_t *storage) {
        if (g_bufferArena.owns(storage))
            g_bufferArena.release(storage);
        else
            free(storage);
    }
    }  // namespace

    // tw2de (2026-10-28): the buffer arena's use (MB), for the renderer's choice to keep buffers between bursts.
    void Deko3DBufferArenaUse(uint64_t &usedMB, uint64_t &totalMB, uint32_t &fallbacks) {
        std::lock_guard<std::mutex> lock(g_bufferArena.mutex);
        usedMB = g_bufferArena.usedUnits;
        totalMB = g_bufferArena.units;
        fallbacks = g_bufferArena.fallbacks;
    }

    static constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
    static constexpr uint64_t alignUp64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

    // FNV-1a 64: the variant cache key (tools/deko3d_shaders.py computes the same over the same text).
    static uint64_t fnv1a64(const void *data, size_t size) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; i++) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    static uint64_t mixHash(uint64_t h, const void *data, size_t size) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    Deko3DStats g_deko3dStats;
    // PLUME_DEKO3D_GUARDS: bytes after each block's storage, and their value.
    static constexpr uint32_t GuardBytes = 64u << 10;
    static constexpr uint8_t GuardByte = 0xA5;


    Deko3DOptions g_deko3dOptions;
    void Deko3DSetOptions(const Deko3DOptions &options) { g_deko3dOptions = options; }

    static Deko3DShaderCacheLookup g_cacheLookup;
    static Deko3DShaderCacheStore g_cacheStore;
    static Deko3DVariantRecorder g_variantRecorder;
    void SetDeko3DVariantRecorder(Deko3DVariantRecorder recorder) { g_variantRecorder = std::move(recorder); }
    static Deko3DCompileNotice g_compileNotice;
    void SetDeko3DCompileNotice(Deko3DCompileNotice notice) { g_compileNotice = std::move(notice); }
    static Deko3DLateCompile g_lateCompile;  // tw2de (2026-10-23)
    void SetDeko3DLateCompile(Deko3DLateCompile compile) { g_lateCompile = std::move(compile); }

    void SetDeko3DShaderCache(Deko3DShaderCacheLookup lookup, Deko3DShaderCacheStore store) {
        g_cacheLookup = std::move(lookup);
        g_cacheStore = std::move(store);
    }

    static Deko3DBuiltinDkshLookup g_builtinDksh;  // tw2de (2026-11-06)
    void SetDeko3DBuiltinDkshLookup(Deko3DBuiltinDkshLookup lookup) {
        g_builtinDksh = std::move(lookup);
    }

    // ---- formats

    DkImageFormat toDkFormat(RenderFormat format) {
        switch (format) {
        case RenderFormat::R32G32B32A32_TYPELESS:
        case RenderFormat::R32G32B32A32_FLOAT: return DkImageFormat_RGBA32_Float;
        case RenderFormat::R32G32B32A32_UINT: return DkImageFormat_RGBA32_Uint;
        case RenderFormat::R32G32B32A32_SINT: return DkImageFormat_RGBA32_Sint;
        case RenderFormat::R32G32B32_TYPELESS:
        case RenderFormat::R32G32B32_FLOAT: return DkImageFormat_RGB32_Float;
        case RenderFormat::R32G32B32_UINT: return DkImageFormat_RGB32_Uint;
        case RenderFormat::R32G32B32_SINT: return DkImageFormat_RGB32_Sint;
        case RenderFormat::R16G16B16A16_TYPELESS:
        case RenderFormat::R16G16B16A16_FLOAT: return DkImageFormat_RGBA16_Float;
        case RenderFormat::R16G16B16A16_UNORM: return DkImageFormat_RGBA16_Unorm;
        case RenderFormat::R16G16B16A16_UINT: return DkImageFormat_RGBA16_Uint;
        case RenderFormat::R16G16B16A16_SNORM: return DkImageFormat_RGBA16_Snorm;
        case RenderFormat::R16G16B16A16_SINT: return DkImageFormat_RGBA16_Sint;
        case RenderFormat::R32G32_TYPELESS:
        case RenderFormat::R32G32_FLOAT: return DkImageFormat_RG32_Float;
        case RenderFormat::R32G32_UINT: return DkImageFormat_RG32_Uint;
        case RenderFormat::R32G32_SINT: return DkImageFormat_RG32_Sint;
        case RenderFormat::R8G8B8A8_TYPELESS:
        case RenderFormat::R8G8B8A8_UNORM: return DkImageFormat_RGBA8_Unorm;
        case RenderFormat::R8G8B8A8_UINT: return DkImageFormat_RGBA8_Uint;
        case RenderFormat::R8G8B8A8_SNORM: return DkImageFormat_RGBA8_Snorm;
        case RenderFormat::R8G8B8A8_SINT: return DkImageFormat_RGBA8_Sint;
        case RenderFormat::B8G8R8A8_UNORM: return DkImageFormat_BGRA8_Unorm;
        case RenderFormat::R16G16_TYPELESS:
        case RenderFormat::R16G16_FLOAT: return DkImageFormat_RG16_Float;
        case RenderFormat::R16G16_UNORM: return DkImageFormat_RG16_Unorm;
        case RenderFormat::R16G16_UINT: return DkImageFormat_RG16_Uint;
        case RenderFormat::R16G16_SNORM: return DkImageFormat_RG16_Snorm;
        case RenderFormat::R16G16_SINT: return DkImageFormat_RG16_Sint;
        case RenderFormat::R32_TYPELESS:
        case RenderFormat::R32_FLOAT: return DkImageFormat_R32_Float;
        case RenderFormat::D32_FLOAT: return DkImageFormat_ZF32;
        case RenderFormat::D32_FLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
        case RenderFormat::D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;  // tw2de (2026-10-19)
        // tw2de (2026-10-25): the packed formats (plume_render_interface_types.h)
        case RenderFormat::R10G10B10A2_UNORM: return DkImageFormat_RGB10A2_Unorm;
        case RenderFormat::R5G6B5_UNORM: return DkImageFormat_RGB565_Unorm;
        case RenderFormat::R5G5B5A1_UNORM: return DkImageFormat_RGB5A1_Unorm;
        case RenderFormat::R4G4B4A4_UNORM: return DkImageFormat_RGBA4_Unorm;
        case RenderFormat::R32_UINT: return DkImageFormat_R32_Uint;
        case RenderFormat::R32_SINT: return DkImageFormat_R32_Sint;
        case RenderFormat::R8G8_TYPELESS:
        case RenderFormat::R8G8_UNORM: return DkImageFormat_RG8_Unorm;
        case RenderFormat::R8G8_UINT: return DkImageFormat_RG8_Uint;
        case RenderFormat::R8G8_SNORM: return DkImageFormat_RG8_Snorm;
        case RenderFormat::R8G8_SINT: return DkImageFormat_RG8_Sint;
        case RenderFormat::R16_TYPELESS:
        case RenderFormat::R16_FLOAT: return DkImageFormat_R16_Float;
        case RenderFormat::D16_UNORM: return DkImageFormat_Z16;
        case RenderFormat::R16_UNORM: return DkImageFormat_R16_Unorm;
        case RenderFormat::R16_UINT: return DkImageFormat_R16_Uint;
        case RenderFormat::R16_SNORM: return DkImageFormat_R16_Snorm;
        case RenderFormat::R16_SINT: return DkImageFormat_R16_Sint;
        case RenderFormat::R8_TYPELESS:
        case RenderFormat::R8_UNORM: return DkImageFormat_R8_Unorm;
        case RenderFormat::R8_UINT: return DkImageFormat_R8_Uint;
        case RenderFormat::R8_SNORM: return DkImageFormat_R8_Snorm;
        case RenderFormat::R8_SINT: return DkImageFormat_R8_Sint;
        case RenderFormat::BC1_TYPELESS:
        case RenderFormat::BC1_UNORM: return DkImageFormat_RGBA_BC1;
        case RenderFormat::BC1_UNORM_SRGB: return DkImageFormat_RGBA_BC1_sRGB;
        case RenderFormat::BC2_TYPELESS:
        case RenderFormat::BC2_UNORM: return DkImageFormat_RGBA_BC2;
        case RenderFormat::BC2_UNORM_SRGB: return DkImageFormat_RGBA_BC2_sRGB;
        case RenderFormat::BC3_TYPELESS:
        case RenderFormat::BC3_UNORM: return DkImageFormat_RGBA_BC3;
        case RenderFormat::BC3_UNORM_SRGB: return DkImageFormat_RGBA_BC3_sRGB;
        case RenderFormat::BC4_TYPELESS:
        case RenderFormat::BC4_UNORM: return DkImageFormat_R_BC4_Unorm;
        case RenderFormat::BC4_SNORM: return DkImageFormat_R_BC4_Snorm;
        case RenderFormat::BC5_TYPELESS:
        case RenderFormat::BC5_UNORM: return DkImageFormat_RG_BC5_Unorm;
        case RenderFormat::BC5_SNORM: return DkImageFormat_RG_BC5_Snorm;
        case RenderFormat::BC6H_TYPELESS:
        case RenderFormat::BC6H_UF16: return DkImageFormat_RGBA_BC6H_UF16_Float;
        case RenderFormat::BC6H_SF16: return DkImageFormat_RGBA_BC6H_SF16_Float;
        case RenderFormat::BC7_TYPELESS:
        case RenderFormat::BC7_UNORM: return DkImageFormat_RGBA_BC7_Unorm;
        case RenderFormat::BC7_UNORM_SRGB: return DkImageFormat_RGBA_BC7_Unorm_sRGB;
        case RenderFormat::S8_UINT: return DkImageFormat_S8;
        default: return DkImageFormat_None;
        }
    }

    static bool isDepthFormat(DkImageFormat f) {
        return f == DkImageFormat_Z16 || f == DkImageFormat_Z24X8 || f == DkImageFormat_ZF32 ||
               f == DkImageFormat_Z24S8 || f == DkImageFormat_ZF32_X24S8 || f == DkImageFormat_S8;
    }

    static bool isCompressedFormat(DkImageFormat f) {
        return f >= DkImageFormat_RGB_BC1 && f <= DkImageFormat_RGBA_ASTC_12x12_sRGB;
    }

    static bool isSrgbFormat(DkImageFormat f) {
        return f == DkImageFormat_RGBA8_Unorm_sRGB || f == DkImageFormat_RGBX8_Unorm_sRGB ||
               f == DkImageFormat_BGRA8_Unorm_sRGB || f == DkImageFormat_BGRX8_Unorm_sRGB;
    }

    // The 2D engine (blits, MSAA resolves) takes uncompressed colour formats.
    static bool canUse2DEngine(DkImageFormat f) {
        return !isDepthFormat(f) && !isCompressedFormat(f) && f != DkImageFormat_RGB32_Float &&
               f != DkImageFormat_RGB32_Uint && f != DkImageFormat_RGB32_Sint;
    }

    // What a texture of this plume format is made of when it is a depth target (R32_TYPELESS and friends).
    static DkImageFormat depthTargetFormat(RenderFormat format) {
        switch (format) {
        case RenderFormat::R32_TYPELESS:
        case RenderFormat::R32_FLOAT:
        case RenderFormat::D32_FLOAT: return DkImageFormat_ZF32;
        case RenderFormat::R16_TYPELESS:
        case RenderFormat::R16_UNORM:
        case RenderFormat::D16_UNORM: return DkImageFormat_Z16;
        case RenderFormat::D32_FLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
        case RenderFormat::D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;
        default: return toDkFormat(format);
        }
    }

    static bool vertexFormat(RenderFormat format, DkVtxAttribSize &size, DkVtxAttribType &type, bool &bgra) {
        bgra = false;
        switch (format) {
        case RenderFormat::R32G32B32A32_FLOAT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32G32B32A32_UINT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32G32B32A32_SINT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R32G32B32_FLOAT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32G32B32_UINT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32G32B32_SINT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R32G32_FLOAT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32G32_UINT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32G32_SINT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R32_FLOAT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32_UINT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32_SINT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R16G16B16A16_FLOAT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R16G16B16A16_UNORM: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R16G16B16A16_SNORM: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R16G16B16A16_UINT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R16G16B16A16_SINT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R16G16_FLOAT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R16G16_UNORM: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R16G16_SNORM: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R16G16_UINT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R16G16_SINT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R16_FLOAT: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R16_UNORM: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R16_SNORM: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R16_UINT: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R16_SINT: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R8G8B8A8_UNORM: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R8G8B8A8_SNORM: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R8G8B8A8_UINT: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R8G8B8A8_SINT: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::B8G8R8A8_UNORM: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Unorm; bgra = true; return true;
        case RenderFormat::R8G8_UNORM: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R8G8_SNORM: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R8G8_UINT: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R8G8_SINT: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R8_UNORM: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R8_SNORM: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R8_UINT: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R8_SINT: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Sint; return true;
        default: return false;
        }
    }

    static DkCompareOp toDk(RenderComparisonFunction f) {
        switch (f) {
        case RenderComparisonFunction::NEVER: return DkCompareOp_Never;
        case RenderComparisonFunction::LESS: return DkCompareOp_Less;
        case RenderComparisonFunction::EQUAL: return DkCompareOp_Equal;
        case RenderComparisonFunction::LESS_EQUAL: return DkCompareOp_Lequal;
        case RenderComparisonFunction::GREATER: return DkCompareOp_Greater;
        case RenderComparisonFunction::NOT_EQUAL: return DkCompareOp_NotEqual;
        case RenderComparisonFunction::GREATER_EQUAL: return DkCompareOp_Gequal;
        case RenderComparisonFunction::ALWAYS:
        default: return DkCompareOp_Always;
        }
    }

    static DkStencilOp toDk(RenderStencilOp op) {
        switch (op) {
        case RenderStencilOp::ZERO: return DkStencilOp_Zero;
        case RenderStencilOp::REPLACE: return DkStencilOp_Replace;
        case RenderStencilOp::INCREMENT_AND_CLAMP: return DkStencilOp_Incr;
        case RenderStencilOp::DECREMENT_AND_CLAMP: return DkStencilOp_Decr;
        case RenderStencilOp::INVERT: return DkStencilOp_Invert;
        case RenderStencilOp::INCREMENT_AND_WRAP: return DkStencilOp_IncrWrap;
        case RenderStencilOp::DECREMENT_AND_WRAP: return DkStencilOp_DecrWrap;
        case RenderStencilOp::KEEP:
        default: return DkStencilOp_Keep;
        }
    }

    static DkBlendFactor toDk(RenderBlend b) {
        switch (b) {
        case RenderBlend::ZERO: return DkBlendFactor_Zero;
        case RenderBlend::ONE: return DkBlendFactor_One;
        case RenderBlend::SRC_COLOR: return DkBlendFactor_SrcColor;
        case RenderBlend::INV_SRC_COLOR: return DkBlendFactor_InvSrcColor;
        case RenderBlend::SRC_ALPHA: return DkBlendFactor_SrcAlpha;
        case RenderBlend::INV_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
        case RenderBlend::DEST_ALPHA: return DkBlendFactor_DstAlpha;
        case RenderBlend::INV_DEST_ALPHA: return DkBlendFactor_InvDstAlpha;
        case RenderBlend::DEST_COLOR: return DkBlendFactor_DstColor;
        case RenderBlend::INV_DEST_COLOR: return DkBlendFactor_InvDstColor;
        case RenderBlend::SRC_ALPHA_SAT: return DkBlendFactor_SrcAlphaSaturate;
        case RenderBlend::BLEND_FACTOR: return DkBlendFactor_ConstColor;
        case RenderBlend::INV_BLEND_FACTOR: return DkBlendFactor_InvConstColor;
        case RenderBlend::SRC1_COLOR: return DkBlendFactor_Src1Color;
        case RenderBlend::INV_SRC1_COLOR: return DkBlendFactor_InvSrc1Color;
        case RenderBlend::SRC1_ALPHA: return DkBlendFactor_Src1Alpha;
        case RenderBlend::INV_SRC1_ALPHA: return DkBlendFactor_InvSrc1Alpha;
        default: return DkBlendFactor_One;
        }
    }

    // In the alpha factors, the constant's colour factors mean its alpha (D3D's BLEND_FACTOR on alpha).
    static DkBlendFactor toDkAlpha(RenderBlend b) {
        switch (b) {
        case RenderBlend::BLEND_FACTOR: return DkBlendFactor_ConstAlpha;
        case RenderBlend::INV_BLEND_FACTOR: return DkBlendFactor_InvConstAlpha;
        case RenderBlend::SRC_COLOR: return DkBlendFactor_SrcAlpha;
        case RenderBlend::INV_SRC_COLOR: return DkBlendFactor_InvSrcAlpha;
        case RenderBlend::DEST_COLOR: return DkBlendFactor_DstAlpha;
        case RenderBlend::INV_DEST_COLOR: return DkBlendFactor_InvDstAlpha;
        case RenderBlend::SRC1_COLOR: return DkBlendFactor_Src1Alpha;
        case RenderBlend::INV_SRC1_COLOR: return DkBlendFactor_InvSrc1Alpha;
        default: return toDk(b);
        }
    }

    static DkBlendOp toDk(RenderBlendOperation op) {
        switch (op) {
        case RenderBlendOperation::SUBTRACT: return DkBlendOp_Sub;
        case RenderBlendOperation::REV_SUBTRACT: return DkBlendOp_RevSub;
        case RenderBlendOperation::MIN: return DkBlendOp_Min;
        case RenderBlendOperation::MAX: return DkBlendOp_Max;
        case RenderBlendOperation::ADD:
        default: return DkBlendOp_Add;
        }
    }

    static DkLogicOp toDk(RenderLogicOperation op) {
        switch (op) {
        case RenderLogicOperation::CLEAR: return DkLogicOp_Clear;
        case RenderLogicOperation::SET: return DkLogicOp_Set;
        case RenderLogicOperation::COPY: return DkLogicOp_Copy;
        case RenderLogicOperation::COPY_INVERTED: return DkLogicOp_CopyInverted;
        case RenderLogicOperation::NOOP: return DkLogicOp_NoOp;
        case RenderLogicOperation::INVERT: return DkLogicOp_Invert;
        case RenderLogicOperation::AND: return DkLogicOp_And;
        case RenderLogicOperation::NAND: return DkLogicOp_Nand;
        case RenderLogicOperation::OR: return DkLogicOp_Or;
        case RenderLogicOperation::NOR: return DkLogicOp_Nor;
        case RenderLogicOperation::XOR: return DkLogicOp_Xor;
        case RenderLogicOperation::EQUIV: return DkLogicOp_Equivalent;
        case RenderLogicOperation::AND_REVERSE: return DkLogicOp_AndReverse;
        case RenderLogicOperation::AND_INVERTED: return DkLogicOp_AndInverted;
        case RenderLogicOperation::OR_REVERSE: return DkLogicOp_OrReverse;
        case RenderLogicOperation::OR_INVERTED: return DkLogicOp_OrInverted;
        default: return DkLogicOp_Copy;
        }
    }

    static DkWrapMode toDk(RenderTextureAddressMode m) {
        switch (m) {
        case RenderTextureAddressMode::MIRROR: return DkWrapMode_MirroredRepeat;
        case RenderTextureAddressMode::CLAMP: return DkWrapMode_ClampToEdge;
        case RenderTextureAddressMode::BORDER: return DkWrapMode_ClampToBorder;
        case RenderTextureAddressMode::MIRROR_ONCE: return DkWrapMode_MirrorClampToEdge;
        case RenderTextureAddressMode::WRAP:
        default: return DkWrapMode_Repeat;
        }
    }

    static DkImageSwizzle toDk(RenderSwizzle s, DkImageSwizzle identity) {
        switch (s) {
        case RenderSwizzle::ZERO: return DkImageSwizzle_Zero;
        case RenderSwizzle::ONE: return DkImageSwizzle_One;
        case RenderSwizzle::R: return DkImageSwizzle_Red;
        case RenderSwizzle::G: return DkImageSwizzle_Green;
        case RenderSwizzle::B: return DkImageSwizzle_Blue;
        case RenderSwizzle::A: return DkImageSwizzle_Alpha;
        case RenderSwizzle::IDENTITY:
        default: return identity;
        }
    }

    static DkMsMode toDkMsMode(RenderSampleCounts count) {
        switch (count) {
        case RenderSampleCount::COUNT_2: return DkMsMode_2x;
        case RenderSampleCount::COUNT_4: return DkMsMode_4x;
        case RenderSampleCount::COUNT_8: return DkMsMode_8x;
        default: return DkMsMode_1x;
        }
    }

    // deko3d 0.5.0's tile height bug (SwitchWakerHD, memory.cpp image_tile_size_fix): dkImageLayoutInitialize picks the
    // tile from 1.5 x the height and its layout shrinks it for level 0, but the engines and the descriptor use the
    // unshrunk one. When the two differ, the shrunk tile is given as a custom tile size.
    static void tileSizeFix(DkImageLayoutMaker &m, uint32_t rows) {
        if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo))
            return;
        if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D || m.type == DkImageType_Buffer)
            return;
        if ((m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) && (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x))
            rows *= 2;
        const uint32_t gobs = (rows + rows / 2 + 7) / 8;
        uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
        uint32_t shrunk = tile;
        while (shrunk && (8u << (shrunk - 1)) >= rows)
            shrunk--;
        if (shrunk == tile)
            return;
        m.flags |= DkImageFlags_CustomTileSize;
        m.tileSize = DkTileSize(shrunk);
    }

    // ---- Deko3DBuffer

    Deko3DBuffer::Deko3DBuffer(Deko3DDevice *device, const RenderBufferDesc &desc) : device(device), desc(desc) {
        const uint64_t size = std::max<uint64_t>(alignUp64(desc.size, DK_MEMBLOCK_ALIGNMENT), DK_MEMBLOCK_ALIGNMENT);
        if (size > 0xFFFFFFFFull) {
            logf("buffer of %" PRIu64 " bytes is too large", desc.size);
            return;
        }
        uint32_t flags = DkMemBlockFlags_GpuCached;
        // Uploads written at cache speed and published by unmap(writtenRange) (HOST_CACHED, as the Vulkan back end);
        // everything else uncached for the CPU, so the GPU sees the CPU's writes and the CPU the GPU's after a fence.
        // tw2de (2026-10-24): and readbacks asked for HOST_CACHED (the memexport results, tens of MB a frame while
        // the game loads: read through the uncached mapping they were most of the render thread's busy time in the
        // hitches), whose lines map() invalidates before the CPU reads them.
        // tw2de (2026-10-24): and DEFAULT buffers asked for HOST_CACHED (the memexport buffers, which the CPU seeds
        // and reads the results from directly: no readback copy, no upload of the seed): unmap(writtenRange) cleans
        // what the CPU wrote, map(readRange) invalidates what the GPU wrote.
        cpuCached = (desc.heapType == RenderHeapType::UPLOAD || desc.heapType == RenderHeapType::READBACK ||
                     desc.heapType == RenderHeapType::DEFAULT) &&
                    (desc.flags & RenderBufferFlag::HOST_CACHED);
        flags |= cpuCached ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached;
        block = device->makeBlock(uint32_t(size), flags,
                                  desc.heapType == RenderHeapType::UPLOAD     ? "upload buffer"
                                  : desc.heapType == RenderHeapType::READBACK ? "readback buffer"
                                                                              : "buffer");
        if (!block)
            return;
        gpu = dkMemBlockGetGpuAddr(block);
        cpu = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(block));
        g_deko3dStats.bufferBytes += size;
        {
            std::lock_guard<std::mutex> lock(device->bufferMutex);
            device->bufferRanges[gpu] = {uint32_t(size), cpu};
        }
    }

    // tw2de (2026-10-30): Deko3DCreateBufferOverMemory's block, whose storage is the caller's (destroyBlock finds
    // no guarded entry for it and frees nothing).
    Deko3DBuffer::Deko3DBuffer(Deko3DDevice *device, DkMemBlock block, const RenderBufferDesc &desc, bool cpuCached)
        : device(device), desc(desc), block(block), cpuCached(cpuCached) {
        gpu = dkMemBlockGetGpuAddr(block);
        cpu = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(block));
        g_deko3dStats.bufferBytes += dkMemBlockGetSize(block);
        std::lock_guard<std::mutex> lock(device->bufferMutex);
        device->bufferRanges[gpu] = {dkMemBlockGetSize(block), cpu};
    }

    std::unique_ptr<RenderBuffer> Deko3DCreateBufferOverMemory(RenderDevice *device, void *storage, uint64_t size,
                                                               bool gpuCached, const char *name) {
        Deko3DDevice *dev = static_cast<Deko3DDevice *>(device);
        if (!dev || !dev->dk || !storage || size == 0 || size > 0xFFFFFFFFull || (size & (DK_MEMBLOCK_ALIGNMENT - 1)) ||
            (reinterpret_cast<uintptr_t>(storage) & (DK_MEMBLOCK_ALIGNMENT - 1))) {
            logf("buffer over memory '%s': bad storage %p or size %" PRIu64, name ? name : "", storage, size);
            return nullptr;
        }
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, dev->dk, uint32_t(size));
        m.flags = (gpuCached ? DkMemBlockFlags_GpuCached : DkMemBlockFlags_GpuUncached) | DkMemBlockFlags_CpuCached;
        m.storage = storage;
        const auto start = std::chrono::steady_clock::now();
        DkMemBlock block = dkMemBlockCreate(&m);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!block) {
            logf("buffer over memory '%s' (%p, %" PRIu64 " KiB, GPU %s): dkMemBlockCreate failed after %.2f ms",
                 name ? name : "", storage, size >> 10, gpuCached ? "cached" : "uncached", ms);
            return nullptr;
        }
        logf("buffer over memory '%s' (%p, %" PRIu64 " KiB, GPU %s): block made in %.2f ms, GPU address %" PRIx64,
             name ? name : "", storage, size >> 10, gpuCached ? "cached" : "uncached", ms, uint64_t(dkMemBlockGetGpuAddr(block)));
        RenderBufferDesc desc = RenderBufferDesc::DefaultBuffer(size, RenderBufferFlag::HOST_CACHED);
        return std::make_unique<Deko3DBuffer>(dev, block, desc, true);
    }

    Deko3DBuffer::~Deko3DBuffer() {
        if (block) {
            {
                std::lock_guard<std::mutex> lock(device->bufferMutex);
                device->bufferRanges.erase(gpu);
            }
            g_deko3dStats.bufferBytes -= dkMemBlockGetSize(block);
            device->retire({}, block);
        }
    }

    // tw2de (2026-10-28): the time of a make or free, into one of Deko3DStats' counters.
    struct Deko3DTimed {
        std::atomic<uint64_t> &counter;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit Deko3DTimed(std::atomic<uint64_t> &c) : counter(c) {}
        ~Deko3DTimed() {
            counter.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - start).count()),
                              std::memory_order_relaxed);
        }
    };

    void *Deko3DBuffer::map(uint32_t, const RenderRange *readRange) {
        // tw2de (2026-10-24): a cached readback's lines (of readRange, or the whole buffer) cleaned and invalidated
        // (dc civac: the CPU never writes them, so nothing is written back), so the reads after the fence see what the
        // GPU wrote, not lines kept from an earlier read.
        if (cpuCached && block && desc.heapType != RenderHeapType::UPLOAD) {
            const uint64_t begin = readRange ? readRange->begin : 0;
            const uint64_t end = readRange ? std::min<uint64_t>(readRange->end, desc.size) : desc.size;
            if (end > begin)
                armDCacheFlush(cpu + begin, size_t(end - begin));
        }
        return cpu;
    }

    void Deko3DBuffer::unmap(uint32_t, const RenderRange *writtenRange) {
        if (!cpuCached || !block || desc.heapType == RenderHeapType::READBACK)
            return;
        uint64_t begin = 0, end = desc.size;
        if (writtenRange != nullptr) {
            begin = writtenRange->begin;
            end = writtenRange->end;
        }
        // tw2de (2026-10-19, CPU proposal 14): a clean (dc cvac) of the written lines, not deko3d's clean and
        // invalidate (dc civac): only upload buffers are CPU-cached (written by the CPU, read by the GPU, never read
        // back), so the lines may stay valid for the ring's next pass.
        if (end > begin)
            armDCacheClean(static_cast<uint8_t *>(cpu) + begin, size_t(end - begin));
    }

    std::unique_ptr<RenderBufferFormattedView> Deko3DBuffer::createBufferFormattedView(RenderFormat format) {
        auto view = std::make_unique<Deko3DBufferFormattedView>();
        view->format = format;
        view->buffer = this;
        return view;
    }

    void Deko3DBuffer::setName(const std::string &name) {
        if (block)
            NameSlowBlock(block, name);  // tw2de (2026-10-28)
    }

    uint64_t Deko3DBuffer::getDeviceAddress() const { return gpu; }

    // ---- Deko3DTexture

    Deko3DTexture::Deko3DTexture(Deko3DDevice *device, const RenderTextureDesc &desc, const Deko3DTexture *rowsOf) : device(device), desc(desc) {
        const bool depth = (desc.flags & RenderTextureFlag::DEPTH_TARGET) != 0;
        format = depth ? depthTargetFormat(desc.format) : toDkFormat(desc.format);
        if (format == DkImageFormat_None) {
            logf("texture format %d has no deko3d format", int(desc.format));
            return;
        }
        const uint32_t layers = std::max<uint32_t>(desc.arraySize, 1);
        const bool multisampled = desc.multisampling.sampleCount > RenderSampleCount::COUNT_1;
        switch (desc.dimension) {
        case RenderTextureDimension::TEXTURE_1D:
            type = layers > 1 ? DkImageType_1DArray : DkImageType_1D;
            break;
        case RenderTextureDimension::TEXTURE_3D:
            type = DkImageType_3D;
            break;
        case RenderTextureDimension::TEXTURE_2D:
        default:
            if (multisampled)
                type = layers > 1 ? DkImageType_2DMSArray : DkImageType_2DMS;
            else if ((desc.flags & RenderTextureFlag::CUBE) && layers % 6 == 0)
                type = layers > 6 ? DkImageType_CubemapArray : DkImageType_Cubemap;
            else
                type = layers > 1 ? DkImageType_2DArray : DkImageType_2D;
            break;
        }

        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, device->dk);
        m.type = type;
        m.format = format;
        m.msMode = toDkMsMode(desc.multisampling.sampleCount);
        m.dimensions[0] = std::max<uint32_t>(desc.width, 1);
        m.dimensions[1] = std::max<uint32_t>(desc.height, 1);
        // deko3d's layer counts (dk_image.cpp): a cube map's six faces are implied, a cube map array gives cubes (it
        // multiplies by six), a 1D array gives its layers as the height.
        m.dimensions[2] = type == DkImageType_3D ? std::max<uint32_t>(desc.depth, 1)
                          : type == DkImageType_Cubemap ? 6
                          : type == DkImageType_CubemapArray ? layers / 6 : layers;
        if (type == DkImageType_1D)
            m.dimensions[1] = m.dimensions[2] = 0;
        else if (type == DkImageType_1DArray) {
            m.dimensions[1] = layers;
            m.dimensions[2] = 0;
        }
        else if (type == DkImageType_2D || type == DkImageType_2DMS)
            m.dimensions[2] = 0;
        m.mipLevels = std::max<uint32_t>(desc.mipLevels, 1);
        uint32_t flags = 0;
        const bool render = (desc.flags & (RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::DEPTH_TARGET)) != 0;
        if (render && type != DkImageType_3D) {
            flags |= DkImageFlags_UsageRender;
            // Colour targets compressed; depth too with Deko3DOptions::depthCompression (SwitchWakerHD's defaults left it
            // off as not verified).
            if ((!depth || g_deko3dOptions.depthCompression) && (!multisampled || g_deko3dOptions.msCompression))
                flags |= DkImageFlags_HwCompression;
        }
        if (desc.flags & (RenderTextureFlag::UNORDERED_ACCESS | RenderTextureFlag::STORAGE))
            flags |= DkImageFlags_UsageLoadStore;
        if (type != DkImageType_3D && canUse2DEngine(format))
            flags |= DkImageFlags_Usage2DEngine;
        m.flags = flags;
        uint32_t rows = m.dimensions[1] ? m.dimensions[1] : 1;
        if (isCompressedFormat(format))
            rows = (rows + 3) / 4;
        tileSizeFix(m, rows);
        dkImageLayoutInitialize(&layout, &m);
        const uint64_t size = dkImageLayoutGetSize(&layout);
        const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
        if (rowsOf) {
            // TW2 (2026-11-13, Deko3DCreateTextureRows): over rowsOf's memory.
            if (!rowsOf->valid || size > rowsOf->alloc.size || alignment != dkImageLayoutGetAlignment(&rowsOf->layout))
                return;
            alloc = rowsOf->alloc;
            ownsMemory = false;
            dkImageInitialize(&image, &layout, alloc.block, alloc.offset);
            valid = true;
            return;
        }
        if (size > 0xFFFFFFFFull || !device->imageAlloc(uint32_t(size), alignment, alloc)) {
            logf("texture %ux%ux%u format %d: no memory for %" PRIu64 " bytes", desc.width, desc.height, desc.depth,
                 int(desc.format), size);
            return;
        }
        dkImageInitialize(&image, &layout, alloc.block, alloc.offset);
        g_deko3dStats.imageBytes += alloc.size;
        valid = true;
    }

    Deko3DTexture::~Deko3DTexture() {
        if (valid && ownsMemory) {
            g_deko3dStats.imageBytes -= alloc.size;
            device->retire(alloc, nullptr);
        }
    }

    // TW2 (2026-11-13): see plume_deko3d.h.
    std::unique_ptr<RenderTexture> Deko3DCreateTextureRows(const RenderTexture *source, uint32_t height) {
        const Deko3DTexture *s = static_cast<const Deko3DTexture *>(source);
        if (!s || !s->valid || s->type != DkImageType_2D || !height || height >= s->desc.height ||
            std::max<uint32_t>(s->desc.mipLevels, 1) != 1 || isCompressedFormat(s->format))
            return nullptr;
        // The tile deko3d gives each (tileSizeFix's arithmetic: the tile from 1.5 x the rows, shrunk while the rows fit
        // in half of it): the same for both, or their texels lie apart.
        auto tileOf = [](uint32_t rows) {
            const uint32_t gobs = (rows + rows / 2 + 7) / 8;
            uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
            while (tile && (8u << (tile - 1)) >= rows)
                tile--;
            return tile;
        };
        if (tileOf(height) != tileOf(s->desc.height))
            return nullptr;
        RenderTextureDesc desc = s->desc;
        desc.height = height;
        auto t = std::make_unique<Deko3DTexture>(s->device, desc, s);
        if (!t->valid)
            return nullptr;
        return t;
    }

    DkImageView Deko3DTexture::defaultView() const {
        DkImageView v;
        dkImageViewDefaults(&v, &image);
        return v;
    }

    std::unique_ptr<RenderTextureView> Deko3DTexture::createTextureView(const RenderTextureViewDesc &viewDesc) const {
        auto view = std::make_unique<Deko3DTextureView>();
        view->texture = this;
        view->desc = viewDesc;
        dkImageViewDefaults(&view->view, &image);
        // A different format of the same size (sRGB, typeless, integer views). A depth image keeps its own: deko3d
        // samples depth formats as depth (the red channel), which is what Renut's R32 views of depth read.
        if (viewDesc.format != RenderFormat::UNKNOWN && !isDepthFormat(format)) {
            const DkImageFormat f = toDkFormat(viewDesc.format);
            if (f != DkImageFormat_None && f != format)
                view->view.format = f;
        }
        // tw2de (2026-10-18): an S8_UINT view of a combined depth/stencil image samples its stencil (an unsigned
        // integer in the red channel), for Renut's depth resolve words.
        if (viewDesc.format == RenderFormat::S8_UINT && (format == DkImageFormat_ZF32_X24S8 || format == DkImageFormat_Z24S8))
            view->view.dsSource = DkDsSource_Stencil;
        switch (viewDesc.dimension) {
        case RenderTextureViewDimension::TEXTURE_1D:
            view->view.type = DkImageType_1D;
            break;
        case RenderTextureViewDimension::TEXTURE_2D:
            // tw2de (2026-10-20, from SASRT's port): a 2D view of a layered 2D image that does not pick one layer
            // stays a 2D array, as plume's Vulkan back end makes it (VK_IMAGE_VIEW_TYPE_2D_ARRAY when arraySize > 1).
            // Renut's stacked textures are such views, sampled through sampler2DArray; a single-layer 2D view read
            // layer 0 only.
            if (type == DkImageType_2DArray && desc.arraySize > 1 && viewDesc.arrayIndex == 0 &&
                (viewDesc.arraySize == UINT32_MAX || viewDesc.arraySize > 1))
                view->view.type = DkImageType_2DArray;
            else if (type == DkImageType_2DArray || type == DkImageType_Cubemap || type == DkImageType_CubemapArray)
                view->view.type = DkImageType_2D;
            break;
        case RenderTextureViewDimension::TEXTURE_3D:
            view->view.type = DkImageType_3D;
            break;
        case RenderTextureViewDimension::TEXTURE_CUBE:
            view->view.type = DkImageType_Cubemap;
            break;
        default:
            break;
        }
        if (viewDesc.arrayIndex != 0 || (viewDesc.arraySize != UINT32_MAX && viewDesc.arraySize != 0)) {
            view->view.layerOffset = uint16_t(viewDesc.arrayIndex);
            if (viewDesc.arraySize != UINT32_MAX)
                view->view.layerCount = uint16_t(viewDesc.arraySize);
        }
        if (viewDesc.mipSlice != 0 || viewDesc.mipLevels != UINT32_MAX) {
            view->view.mipLevelOffset = uint8_t(viewDesc.mipSlice);
            if (viewDesc.mipLevels != UINT32_MAX)
                view->view.mipLevelCount = uint8_t(viewDesc.mipLevels);
        }
        const RenderComponentMapping &c = viewDesc.componentMapping;
        view->view.swizzle[0] = toDk(c.r, DkImageSwizzle_Red);
        view->view.swizzle[1] = toDk(c.g, DkImageSwizzle_Green);
        view->view.swizzle[2] = toDk(c.b, DkImageSwizzle_Blue);
        view->view.swizzle[3] = toDk(c.a, DkImageSwizzle_Alpha);
        return view;
    }

    void Deko3DTexture::setName(const std::string &) {}

    // ---- Deko3DDescriptorSet

    Deko3DDescriptorSet::Deko3DDescriptorSet(Deko3DDevice *device, const RenderDescriptorSetDesc &desc) : device(device) {
        uint32_t index = 0;
        uint32_t images = 0, samplers = 0, buffers = 0;
        bool imageStarted = false, samplerStarted = false;
        for (uint32_t i = 0; i < desc.descriptorRangesCount; i++) {
            const RenderDescriptorRange &r = desc.descriptorRanges[i];
            uint32_t count = r.count;
            if (desc.lastRangeIsBoundless && i + 1 == desc.descriptorRangesCount)
                count = std::max<uint32_t>(count, desc.boundlessRangeSize);
            Range range;
            range.type = r.type;
            range.binding = r.binding;
            range.first = index;
            range.count = count;
            ranges.push_back(range);
            switch (r.type) {
            case RenderDescriptorRangeType::TEXTURE:
            case RenderDescriptorRangeType::READ_WRITE_TEXTURE:
                if (!imageStarted) {
                    imageFirst = index;
                    imageStarted = true;
                }
                images = index + count - imageFirst;
                break;
            case RenderDescriptorRangeType::SAMPLER:
                if (!samplerStarted) {
                    samplerFirst = index;
                    samplerStarted = true;
                }
                samplers = index + count - samplerFirst;
                break;
            default:
                buffers = index + count;
                break;
            }
            index += count;
        }
        if (images) {
            imageCount = images;
            imageBlock = device->makeBlock(alignUp(images * 32, DK_MEMBLOCK_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "image descriptors");
            if (imageBlock)
                memset(dkMemBlockGetCpuAddr(imageBlock), 0, images * 32);
        }
        if (samplers) {
            samplerCount = samplers;
            samplerBlock = device->makeBlock(alignUp(samplers * 32, DK_MEMBLOCK_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "sampler descriptors");
            if (samplerBlock)
                memset(dkMemBlockGetCpuAddr(samplerBlock), 0, samplers * 32);
        }
        this->buffers.resize(buffers ? index : 0);
    }

    Deko3DDescriptorSet::~Deko3DDescriptorSet() {
        if (imageBlock)
            device->retire({}, imageBlock);
        if (samplerBlock)
            device->retire({}, samplerBlock);
    }

    const Deko3DDescriptorSet::Range *Deko3DDescriptorSet::rangeOf(uint32_t descriptorIndex) const {
        for (const Range &r : ranges) {
            if (descriptorIndex >= r.first && descriptorIndex < r.first + r.count)
                return &r;
        }
        return nullptr;
    }

    void Deko3DDescriptorSet::setBuffer(uint32_t descriptorIndex, const RenderBuffer *buffer, uint64_t bufferSize, const RenderBufferStructuredView *structuredView, const RenderBufferFormattedView *) {
        if (descriptorIndex >= buffers.size())
            return;
        const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(buffer);
        DkBufExtents &e = buffers[descriptorIndex];
        if (b == nullptr || b->block == nullptr) {
            e = {};
            return;
        }
        uint64_t offset = 0;
        if (structuredView != nullptr)
            offset = uint64_t(structuredView->firstElement) * structuredView->structureByteStride;
        e.addr = b->gpu + offset;
        uint64_t size = bufferSize ? bufferSize : (b->desc.size - offset);
        const Range *r = rangeOf(descriptorIndex);
        if (r && (r->type == RenderDescriptorRangeType::CONSTANT_BUFFER || r->type == RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC))
            size = std::min<uint64_t>(size, DK_UNIFORM_BUF_MAX_SIZE);
        e.size = uint32_t(std::min<uint64_t>(size, 0xFFFFFFFFull));
    }

    void Deko3DDescriptorSet::setTexture(uint32_t descriptorIndex, const RenderTexture *texture, RenderTextureLayout, const RenderTextureView *textureView) {
        if (!imageBlock || descriptorIndex < imageFirst || descriptorIndex - imageFirst >= imageCount)
            return;
        DkImageDescriptor *d = static_cast<DkImageDescriptor *>(dkMemBlockGetCpuAddr(imageBlock)) + (descriptorIndex - imageFirst);
        const Deko3DTexture *t = static_cast<const Deko3DTexture *>(texture);
        if (t == nullptr || !t->valid) {
            memset(d, 0, sizeof(*d));
        }
        else {
            const Range *r = rangeOf(descriptorIndex);
            const bool loadStore = r && r->type == RenderDescriptorRangeType::READ_WRITE_TEXTURE;
            DkImageView view = textureView ? static_cast<const Deko3DTextureView *>(textureView)->view : t->defaultView();
            // Written into a local copy first: the GPU may read the descriptor block (other slots) while the CPU
            // writes it, so a slot is written whole, with one copy.
            DkImageDescriptor local;
            // tw2de (2026-10-22): a multisampled image as deko3d's "decayed" 2D image of its samples. Renut's
            // shaders that read one (the MSAA resolves) address the samples themselves (ms_sample_deko3d.hlsli):
            // the GPU's own multisampled fetch through the multisampled descriptor returned zeros.
            const DkImageType viewType = view.type != DkImageType_None ? view.type : t->type;
            const bool decayMS = viewType == DkImageType_2DMS || viewType == DkImageType_2DMSArray;
            dkImageDescriptorInitialize(&local, &view, loadStore, decayMS);
            memcpy(d, &local, sizeof(local));
            // tw2de (2026-10-19, GPU, txq): the size a texture query of this view returns at level 0: the image's
            // size at the view's first level; the depth of a 3D image there, else the view's layers (all from its first
            // when it names no count). tools/deko3d_glsl_opt.py's txq verify (constant 908 = 2) checks it on the GPU.
            const uint32_t id = descriptorIndex - imageFirst;
            if (imageSizes.size() <= id)
                imageSizes.resize(id + 1);
            const uint32_t m = view.mipLevelOffset;
            const uint32_t w = std::max(1u, t->desc.width >> m), h = std::max(1u, t->desc.height >> m);
            const DkImageType type = view.type != DkImageType_None ? view.type : t->type;
            uint32_t z;
            if (type == DkImageType_3D)
                z = std::max(1u, t->desc.depth >> m);
            else
                z = view.layerCount ? view.layerCount
                                    : (t->desc.arraySize > view.layerOffset ? t->desc.arraySize - view.layerOffset : 1u);
            imageSizes[id] = {w, h, z};
        }
        g_deko3dStats.descriptorWrites++;
        device->descriptorEpoch.fetch_add(1, std::memory_order_release);
    }

    // tw2de (2026-11-06): see plume_deko3d.h.
    uint32_t Deko3DWriteStorageDescriptor(RenderDescriptorSet *setBase, uint32_t descriptorIndex, const RenderTexture *texture,
                                          const RenderTextureView *textureView) {
        Deko3DDescriptorSet *set = static_cast<Deko3DDescriptorSet *>(setBase);
        const Deko3DTexture *t = static_cast<const Deko3DTexture *>(texture);
        if (!set || !set->imageBlock || descriptorIndex < set->imageFirst || descriptorIndex - set->imageFirst >= set->imageCount ||
            !t || !t->valid)
            return ~0u;
        DkImageDescriptor *d = static_cast<DkImageDescriptor *>(dkMemBlockGetCpuAddr(set->imageBlock)) + (descriptorIndex - set->imageFirst);
        DkImageView view = textureView ? static_cast<const Deko3DTextureView *>(textureView)->view : t->defaultView();
        DkImageDescriptor local;
        dkImageDescriptorInitialize(&local, &view, true, false);
        memcpy(d, &local, sizeof(local));
        g_deko3dStats.descriptorWrites++;
        set->device->descriptorEpoch.fetch_add(1, std::memory_order_release);
        return descriptorIndex - set->imageFirst;
    }

    uint32_t Deko3DImageId(const RenderDescriptorSet *setBase, uint32_t descriptorIndex) {
        const Deko3DDescriptorSet *set = static_cast<const Deko3DDescriptorSet *>(setBase);
        if (!set || descriptorIndex < set->imageFirst || descriptorIndex - set->imageFirst >= set->imageCount)
            return ~0u;
        return descriptorIndex - set->imageFirst;
    }

    uint32_t Deko3DSamplerId(const RenderDescriptorSet *setBase, uint32_t descriptorIndex) {
        const Deko3DDescriptorSet *set = static_cast<const Deko3DDescriptorSet *>(setBase);
        if (!set || descriptorIndex < set->samplerFirst || descriptorIndex - set->samplerFirst >= set->samplerCount)
            return ~0u;
        return descriptorIndex - set->samplerFirst;
    }

    void Deko3DDescriptorSet::setSampler(uint32_t descriptorIndex, const RenderSampler *sampler) {
        if (!samplerBlock || descriptorIndex < samplerFirst || descriptorIndex - samplerFirst >= samplerCount)
            return;
        DkSamplerDescriptor *d = static_cast<DkSamplerDescriptor *>(dkMemBlockGetCpuAddr(samplerBlock)) + (descriptorIndex - samplerFirst);
        if (sampler == nullptr)
            memset(d, 0, sizeof(*d));
        else
            memcpy(d, &static_cast<const Deko3DSampler *>(sampler)->descriptor, sizeof(*d));
        g_deko3dStats.descriptorWrites++;
        device->descriptorEpoch.fetch_add(1, std::memory_order_release);
    }

    void Deko3DDescriptorSet::setAccelerationStructure(uint32_t, const RenderAccelerationStructure *) {}

    // ---- Deko3DQueryPool

    Deko3DQueryPool::Deko3DQueryPool(Deko3DDevice *device, uint32_t count) : device(device) {
        results.resize(count);
        // A counter report is 16 bytes: the counter, then the GPU timestamp.
        block = device->makeBlock(alignUp(std::max<uint32_t>(count, 1) * 16, DK_MEMBLOCK_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "query pool");
        if (block)
            memset(dkMemBlockGetCpuAddr(block), 0, count * 16);
    }

    Deko3DQueryPool::~Deko3DQueryPool() {
        if (block)
            device->retire({}, block);
    }

    void Deko3DQueryPool::queryResults() { queryResults(uint32_t(results.size())); }

    bool Deko3DQueryPool::queryResults(uint32_t count) {
        count = std::min<uint32_t>(count, uint32_t(results.size()));
        if (!block)
            return true;
        const volatile uint64_t *reports = static_cast<const volatile uint64_t *>(dkMemBlockGetCpuAddr(block));
        for (uint32_t i = 0; i < count; i++)
            results[i] = dkTimestampToNs(reports[i * 2 + 1]);
        return true;
    }

    // ---- Deko3DDevice

    static void debugCallback(void *, const char *context, DkResult result, const char *message) {
        logf("deko3d says (%s, result %d): %s", context ? context : "", int(result), message ? message : "");
    }

    Deko3DDevice::Deko3DDevice() {}

    static void *compileThreadEntry(void *arg) {
        static_cast<Deko3DDevice *>(arg)->compileWorker();
        return nullptr;
    }

    // tw2de (2026-10-21): Deko3DOptions::threadAllocBegin and co. for a uam thread.
    struct UamThreadAlloc {
        bool on = false;
        void begin() {
            if (!on && g_deko3dOptions.threadAllocBegin) {
                g_deko3dOptions.threadAllocBegin();
                on = true;
            }
        }
        void end() {
            if (on) {
                g_deko3dOptions.threadAllocEnd();
                on = false;
            }
        }
        // A compile: its allocations from the thread's slabs; the result copied out to newlib's heap after (the
        // render thread keeps and frees it).
        uam::Result compile(uam::Stage stage, const char *glsl) {
            begin();
            uam::Result r;
            uam::Result out;
            if (on && g_deko3dOptions.threadAllocActive)
                g_deko3dOptions.threadAllocActive(true);
            r = uam::compile(stage, glsl);
            if (on && g_deko3dOptions.threadAllocActive)
                g_deko3dOptions.threadAllocActive(false);
            out.ok = r.ok;
            out.dksh.assign(r.dksh.begin(), r.dksh.end());
            out.log.assign(r.log.begin(), r.log.end());
            return out;
        }
        // tw2de (2026-10-23): the late path (Deko3DLateCompile) the same way: its allocations from the slabs, its
        // result copied out.
        bool late(DkStage stage, uint64_t textKey, const std::vector<std::pair<uint32_t, uint32_t>> &values,
                  uam::Result &out) {
            begin();
            std::vector<uint8_t> dksh;
            std::string log;
            if (on && g_deko3dOptions.threadAllocActive)
                g_deko3dOptions.threadAllocActive(true);
            const bool ok = g_lateCompile(stage, textKey, values, dksh, log);
            if (on && g_deko3dOptions.threadAllocActive)
                g_deko3dOptions.threadAllocActive(false);
            out.ok = ok;
            out.dksh.assign(dksh.begin(), dksh.end());
            out.log.assign(log.begin(), log.end());
            return ok;
        }
    };

    static void *upgradeThreadEntry(void *arg) {
        static_cast<Deko3DDevice *>(arg)->upgradeWorker();
        return nullptr;
    }

    bool Deko3DDevice::initialize() {
        {
            uint64_t heapFree = 0, processFree = 0;
            memoryFigures(heapFree, processFree);
            logf("device: creating (heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)", heapFree >> 20, processFree >> 20);
        }
        DkDeviceMaker dm;
        dkDeviceMakerDefaults(&dm);
        dm.cbDebug = debugCallback;
        // Upper-left origin and depth from 0 to 1 (Vulkan's, D3D's); with the upper-left origin deko3d 0.5.0 maps clip
        // space y = +1 to the top row, D3D's convention: the deko3d shaders are compiled without -fvk-invert-y.
        dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
        dk = dkDeviceCreate(&dm);
        if (!dk) {
            logf("dkDeviceCreate failed");
            return false;
        }
        DkQueueMaker qm;
        dkQueueMakerDefaults(&qm, dk);
        // Zcull off unless Deko3DOptions::zcull (SwitchWakerHD: with it deko3d dropped depth-biased decals in whole 4x8
        // tiles).
        qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_MediumPrio |
                   (g_deko3dOptions.zcull ? 0u : uint32_t(DkQueueFlags_DisableZcull));
        qm.commandMemorySize = 1u << 20;
        qm.flushThreshold = qm.commandMemorySize / 8;
        // Scratch (local) memory per warp: Renut's biggest vertex shaders need 32 KB (uam's DKSH headers; 81 of the 4,881
        // need some), variants maybe more: 64 KB a warp (about 8 MB for the GPU's warps).
        qm.perWarpScratchMemorySize = 64u << 10;
        perWarpScratch = qm.perWarpScratchMemorySize;
        queue = dkQueueCreate(&qm);
        if (!queue) {
            logf("dkQueueCreate failed");
            return false;
        }
        pushBlock = makeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "push constants");
        if (!pushBlock)
            return false;
        memset(dkMemBlockGetCpuAddr(pushBlock), 0, DK_MEMBLOCK_ALIGNMENT);
        pushAddr = dkMemBlockGetGpuAddr(pushBlock);
        texSizeBlock = makeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "texture sizes");
        if (!texSizeBlock)
            return false;
        memset(dkMemBlockGetCpuAddr(texSizeBlock), 0, DK_MEMBLOCK_ALIGNMENT);
        texSizeAddr = dkMemBlockGetGpuAddr(texSizeBlock);
        zeroBlock = makeBlock(ZeroSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "zero vertex buffer");
        if (!zeroBlock)
            return false;
        memset(dkMemBlockGetCpuAddr(zeroBlock), 0, ZeroSize);
        zeroAddr = dkMemBlockGetGpuAddr(zeroBlock);

        // tw2de (2026-10-22): the image heap's first chunks now, before the heap fragments (imageReserveChunks).
        {
            std::lock_guard<std::mutex> lock(heapMutex);
            for (uint32_t i = 0; i < g_deko3dOptions.imageReserveChunks; i++) {
                Chunk c;
                c.block = makeBlock(ImageChunkSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "image heap chunk (reserved)");
                if (!c.block)
                    break;
                c.size = ImageChunkSize;
                c.freeList.push_back({0, ImageChunkSize});
                chunks.push_back(std::move(c));
            }
            if (!chunks.empty())
                logf("image heap: %zu chunks of %u MiB reserved with the device", chunks.size(), ImageChunkSize >> 20);
        }
        // tw2de (2026-10-24): the buffer arena, while the heap is still in one piece (bufferArenaMB).
        if (g_deko3dOptions.bufferArenaMB) {
            if (g_bufferArena.init(g_deko3dOptions.bufferArenaMB)) {
                uint64_t heapFree = 0, processFree = 0;
                memoryFigures(heapFree, processFree);
                logf("buffer arena: %u MiB reserved with the device for buffers of %u MiB and up (heap %" PRIu64
                     " MB free)", g_deko3dOptions.bufferArenaMB, g_deko3dOptions.bufferArenaMinMB, heapFree >> 20);
            } else {
                logf("buffer arena: %u MiB could not be reserved; the heap, as before", g_deko3dOptions.bufferArenaMB);
            }
        }

        capabilities.geometryShader = true;
        capabilities.descriptorIndexing = true;
        capabilities.scalarBlockLayout = false;
        capabilities.bufferDeviceAddress = true;
        capabilities.samplerMirrorClampToEdge = true;
        capabilities.maxTextureSize = 16384;
        capabilities.triangleFan = true;
        capabilities.dynamicDepthBias = true;
        capabilities.uma = true;
        capabilities.queryPools = true;
        capabilities.resolveRegion = false;
        description.name = "deko3d (Tegra X1, Maxwell GM20B)";
        description.type = RenderDeviceType::INTEGRATED;
        description.vendor = RenderDeviceVendor::NVIDIA;
        description.driverVersion = 0x000500;

        // The uam worker: one thread (uam is not reentrant), 8 MB of stack (Mesa's parser and nv50_ir recurse deeply).
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t thread;
        if (pthread_create(&thread, &attr, compileThreadEntry, this) != 0) {
            logf("the shader compile thread did not start");
            pthread_attr_destroy(&attr);
            return false;
        }
        pthread_attr_destroy(&attr);
        pthread_detach(thread);
        logf("device ready: queue with zcull %s, depth targets %scompressed, vertex shader outputs %s, 1 MB of queue "
             "command memory; uam compile thread started", g_deko3dOptions.zcull ? "on" : "off",
             g_deko3dOptions.depthCompression ? "" : "not ", g_deko3dOptions.vsOutputs ? "pruned per pipeline" : "all written");
        return true;
    }

    Deko3DDevice::~Deko3DDevice() {
        if (queue)
            dkQueueWaitIdle(queue);
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            compileStop = true;
        }
        wakeCompileThread();
        {
            std::unique_lock<std::mutex> lock(compileMutex);
            compileDone.wait_for(lock, std::chrono::seconds(5), [this] { return !compilerReady; });  // the background compile's 3 s too
        }
        collectRetired();
        for (Retired &r : retired) {
            if (r.block)
                destroyBlock(r.block);
        }
        retired.clear();
        for (auto &c : chunks)
            destroyBlock(c.block);
        for (auto &c : codeBlocks)
            destroyBlock(c.first);
        if (pushBlock)
            destroyBlock(pushBlock);
        if (zeroBlock)
            destroyBlock(zeroBlock);
        if (queue)
            dkQueueDestroy(queue);
        if (dk)
            dkDeviceDestroy(dk);
    }


    // tw2de (2026-10-27): the last block creations and destructions (each an nvmap handle and a GPU mapping made or
    // undone), for the fault report (blockCensus): did a burst of them come right before a refused kickoff?
    namespace {
    struct BlockEvent { uint64_t tick; uint32_t size; bool made; const char *what; };
    std::mutex g_blockEventMutex;
    BlockEvent g_blockEvents[24];
    uint64_t g_blockEventCount = 0, g_blocksMade = 0, g_blocksDestroyed = 0;
    void noteBlock(uint32_t size, bool made, const char *what) {
        std::lock_guard<std::mutex> lock(g_blockEventMutex);
        g_blockEvents[g_blockEventCount++ % 24] = {armGetSystemTick(), size, made, what};
        (made ? g_blocksMade : g_blocksDestroyed)++;
    }
    }  // namespace

    // tw2de (2026-10-28): memory blocks that took 2 ms or more to make, by part (the storage: arena or heap; the
    // block: deko3d's dkMemBlockCreate), with the buffer's name once the renderer gives it (setName); the renderer's
    // [burst] line lists them (Deko3DTakeSlowBlocks).
    struct SlowBlock {
        DkMemBlock block = nullptr;
        const char *what = "";
        std::string name;
        uint32_t size = 0, flags = 0;
        double storageMs = 0.0, createMs = 0.0;
        bool arena = false;
    };
    std::mutex g_slowBlocksMutex;
    std::vector<SlowBlock> g_slowBlocks;
    uint32_t g_slowBlocksDropped = 0;

    void NoteSlowBlock(DkMemBlock block, const char *what, uint32_t size, uint32_t flags, double storageMs,
                       double createMs, bool arena) {
        if (storageMs + createMs < 2.0)
            return;
        std::lock_guard<std::mutex> lock(g_slowBlocksMutex);
        if (g_slowBlocks.size() >= 64) {
            ++g_slowBlocksDropped;
            return;
        }
        g_slowBlocks.push_back({block, what, {}, size, flags, storageMs, createMs, arena});
    }

    void NameSlowBlock(DkMemBlock block, const std::string &name) {
        std::lock_guard<std::mutex> lock(g_slowBlocksMutex);
        for (SlowBlock &b : g_slowBlocks)
            if (b.block == block)
                b.name = name;
    }

    std::string Deko3DTakeSlowBlocks() {
        std::vector<SlowBlock> blocks;
        uint32_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(g_slowBlocksMutex);
            blocks.swap(g_slowBlocks);
            dropped = g_slowBlocksDropped;
            g_slowBlocksDropped = 0;
        }
        std::string out;
        char line[256];
        for (const SlowBlock &b : blocks) {
            snprintf(line, sizeof(line), "%s%s%s%s %u KiB: storage %.1f ms (%s), block %.1f ms (%s)", out.empty() ? "" : "; ",
                     b.what, b.name.empty() ? "" : " ", b.name.c_str(), b.size >> 10, b.storageMs,
                     b.arena ? "arena" : "heap", b.createMs,
                     (b.flags & DkMemBlockFlags_CpuCached) ? "CPU-cached" : "CPU-uncached");
            out += line;
        }
        if (dropped) {
            snprintf(line, sizeof(line), "; %u more", dropped);
            out += line;
        }
        return out;
    }

    DkMemBlock Deko3DDevice::makeBlock(uint32_t size, uint32_t flags, const char *what) {
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, dk, alignUp(size, DK_MEMBLOCK_ALIGNMENT));
        m.flags = flags;
        // deko3d takes the block's memory from the heap (aligned_alloc) and aborts the process when a creation fails:
        // big ones, and every 64 MB of them, are logged with the heap's free bytes.
        static std::atomic<uint64_t> total{0};
        static std::atomic<uint32_t> logged{0};
        const uint64_t before = total.fetch_add(m.size);
        // tw2de (2026-10-19, render-thread proposal 18): 24 lines, not 96 (each walks the heap and writes the SD card,
        // on the thread making the block: the render thread for the upload ring's chunks).
        if ((size >= (8u << 20) || (before >> 26) != ((before + m.size) >> 26)) && logged.fetch_add(1) < 24) {
            uint64_t heapFree = 0, processFree = 0;
            memoryFiguresQuick(heapFree, processFree);  // tw2de (2026-11-05): no heap walk
            logf("memory block: %s, %u KiB (deko3d blocks %" PRIu64 " MB in all; heap room %" PRIu64 " MB, process %" PRIu64 " MB unused)",
                 what, size >> 10, (before + m.size) >> 20, heapFree >> 20, processFree >> 20);
        }
#if defined(PLUME_DEKO3D_GUARDS)
        // The block's storage with guard bytes after it (deko3d takes storage given to it as it is).
        uint8_t *storage = static_cast<uint8_t *>(memalign(DK_MEMBLOCK_ALIGNMENT, m.size + GuardBytes));
        if (storage) {
            memset(storage + m.size, GuardByte, GuardBytes);
            m.storage = storage;
        }
        DkMemBlock block = dkMemBlockCreate(&m);
        if (storage) {
            std::lock_guard<std::mutex> lock(guardMutex);
            guarded.push_back({block, storage, m.size, what});
        }
        return block;
#else
        // tw2de (2026-10-22): the block's storage from here, not from deko3d, which aborts the process when its
        // own aligned_alloc fails (the game's first gameplay scenes: 776 MB of the heap free in 42,864 pieces, none of
        // them the 64 MB of a new image heap chunk). A failure is now a null block, which the callers handle.
        const auto storageStart = std::chrono::steady_clock::now();  // tw2de (2026-10-28): NoteSlowBlock
        bool fromArena = false;
        uint8_t *storage = nullptr;
        if (!(flags & (DkMemBlockFlags_Image | DkMemBlockFlags_Code)) &&
            m.size >= (uint64_t(g_deko3dOptions.bufferArenaMinMB) << 20))
        {
            storage = g_bufferArena.alloc(m.size);  // tw2de (2026-10-24): the buffer arena first
            fromArena = storage != nullptr;
            static std::atomic<uint32_t> full_logged{0};
            if (!storage && g_bufferArena.base && full_logged.fetch_add(1) < 8)
                logf("buffer arena: no %u KiB block free (%" PRIu64 " of %" PRIu64 " MiB in use, at most %" PRIu64
                     "): %s from the heap", size >> 10, g_bufferArena.usedUnits, g_bufferArena.units,
                     g_bufferArena.peakUnits, what);
        }
        if (!storage)
            storage = static_cast<uint8_t *>(memalign(DK_MEMBLOCK_ALIGNMENT, m.size));
        if (!storage) {
            // tw2de (2026-10-24): the heap's figures (mallinfo walks it, under the allocator's lock) only for a
            // failure that is logged: they were taken for every one.
            static std::atomic<uint32_t> failed{0};
            if (failed.fetch_add(1) < 32) {
                uint64_t heapFree = 0, processFree = 0;
                memoryFigures(heapFree, processFree);
                logf("memory block: %s, %u KiB: NO MEMORY (heap %" PRIu64 " MB free in pieces, process %" PRIu64
                     " MB unused); the caller goes without", what, size >> 10, heapFree >> 20, processFree >> 20);
            }
            total.fetch_sub(m.size);
            return nullptr;
        }
        m.storage = storage;
        const auto createStart = std::chrono::steady_clock::now();
        DkMemBlock block = dkMemBlockCreate(&m);
        if (!block) {
            freeStorage(storage);
            total.fetch_sub(m.size);
            return nullptr;
        }
        {
            const auto end = std::chrono::steady_clock::now();
            NoteSlowBlock(block, what, uint32_t(m.size), flags,
                          std::chrono::duration<double, std::milli>(createStart - storageStart).count(),
                          std::chrono::duration<double, std::milli>(end - createStart).count(), fromArena);
        }
        noteBlock(uint32_t(m.size), true, what);
        std::lock_guard<std::mutex> lock(guardMutex);
        guarded.push_back({block, storage, m.size, what});
        return block;
#endif
    }

    // tw2de (2026-10-26): plume_deko3d.h.
    std::string Deko3DDevice::blockCensus() {
        struct Kind { uint64_t arena = 0, heap = 0; uint32_t count = 0; };
        std::map<std::string, Kind> kinds;
        uint64_t arena = 0, heap = 0;
        {
            std::lock_guard<std::mutex> lock(guardMutex);
            for (const GuardedBlock &g : guarded) {
                Kind &k = kinds[g.what ? g.what : "?"];
                const bool in_arena = g_bufferArena.owns(g.storage);
                (in_arena ? k.arena : k.heap) += g.size;
                (in_arena ? arena : heap) += g.size;
                k.count++;
            }
        }
        char line[200];
        snprintf(line, sizeof(line), "blocks %" PRIu64 " MB from the heap + %" PRIu64 " MB in the arena (arena %" PRIu64
                 " of %" PRIu64 " MB in use, at most %" PRIu64 "):", heap >> 20, arena >> 20, g_bufferArena.usedUnits,
                 g_bufferArena.units, g_bufferArena.peakUnits);
        std::string out = line;
        for (const auto &[what, k] : kinds) {
            snprintf(line, sizeof(line), " %s x%u %" PRIu64 " MB", what.c_str(), k.count, (k.heap + k.arena) >> 20);
            out += line;
            if (k.arena) {
                snprintf(line, sizeof(line), " (%" PRIu64 " in the arena)", k.arena >> 20);
                out += line;
            }
            out += ';';
        }
        {
            std::lock_guard<std::mutex> lock(g_blockEventMutex);
            const uint64_t now = armGetSystemTick();
            snprintf(line, sizeof(line), " blocks made %" PRIu64 ", destroyed %" PRIu64 "; the last ones (+ made, - destroyed, "
                     "ms ago):", g_blocksMade, g_blocksDestroyed);
            out += line;
            const uint64_t n = std::min<uint64_t>(g_blockEventCount, 24);
            for (uint64_t i = g_blockEventCount - n; i < g_blockEventCount; i++) {
                const BlockEvent &e = g_blockEvents[i % 24];
                snprintf(line, sizeof(line), " %c%u KiB %s %" PRIu64 ";", e.made ? '+' : '-', e.size >> 10,
                         e.what ? e.what : "?", armTicksToNs(now - e.tick) / 1000000);
                out += line;
            }
        }
        return out;
    }

    void Deko3DDevice::destroyBlock(DkMemBlock block) {
        if (!block)
            return;
        dkMemBlockDestroy(block);
        {
        // tw2de (2026-10-22): every block's storage is plume's now (makeBlock), freed here.
        std::lock_guard<std::mutex> lock(guardMutex);
        for (size_t i = 0; i < guarded.size(); i++) {
            if (guarded[i].block == block) {
                noteBlock(uint32_t(guarded[i].size), false, guarded[i].what);
                freeStorage(guarded[i].storage);
                guarded[i] = guarded.back();
                guarded.pop_back();
                break;
            }
        }
        }
    }

    void Deko3DDevice::checkGuards() {
#if defined(PLUME_DEKO3D_GUARDS)
        std::lock_guard<std::mutex> lock(guardMutex);
        for (GuardedBlock &g : guarded) {
            const uint8_t *guard = g.storage + g.size;
            for (uint32_t i = 0; i < GuardBytes; i++) {
                if (guard[i] != GuardByte) {
                    uint32_t last = i;
                    for (uint32_t j = i; j < GuardBytes; j++)
                        if (guard[j] != GuardByte)
                            last = j;
                    uint32_t word = 0;
                    memcpy(&word, guard + (i & ~3u), 4);
                    logf("GUARD: written past the end of %s (%u bytes at GPU 0x%" PRIx64 "): guard bytes %u..%u changed "
                         "(first word 0x%08X)", g.what, g.size, dkMemBlockGetGpuAddr(g.block), i, last, word);
                    memset(const_cast<uint8_t *>(guard), GuardByte, GuardBytes);
                    break;
                }
            }
        }
#endif
    }

    bool Deko3DDevice::findBuffer(DkGpuAddr address, DkGpuAddr &base, uint32_t &size, const uint8_t *&cpu) {
        std::lock_guard<std::mutex> lock(bufferMutex);
        auto it = bufferRanges.upper_bound(address);
        if (it == bufferRanges.begin())
            return false;
        --it;
        if (address >= it->first + it->second.first)
            return false;
        base = it->first;
        size = it->second.first;
        cpu = it->second.second;
        return true;
    }

    // Image heap: 64 MB chunks (bigger for a bigger image), first fit.

    static void chunkFree(Deko3DDevice::Chunk &c, uint32_t offset, uint32_t size) {
        auto it = std::lower_bound(c.freeList.begin(), c.freeList.end(), std::make_pair(offset, 0u));
        it = c.freeList.insert(it, {offset, size});
        if (it + 1 != c.freeList.end() && it->first + it->second == (it + 1)->first) {
            it->second += (it + 1)->second;
            c.freeList.erase(it + 1);
        }
        if (it != c.freeList.begin() && (it - 1)->first + (it - 1)->second == it->first) {
            (it - 1)->second += it->second;
            c.freeList.erase(it);
        }
    }

    bool Deko3DDevice::imageAlloc(uint32_t size, uint32_t alignment, Deko3DImageAlloc &out) {
        size = alignUp(size, 256);
        alignment = std::max<uint32_t>(alignment, 256);
        collectRetired();
        std::lock_guard<std::mutex> lock(heapMutex);
        for (int pass = 0; pass < 2; pass++) {
            for (size_t ci = 0; ci < chunks.size(); ci++) {
                Chunk &c = chunks[ci];
                for (size_t i = 0; i < c.freeList.size(); i++) {
                    const auto [off, len] = c.freeList[i];
                    const uint32_t at = alignUp(off, alignment);
                    if (at - off + uint64_t(size) > len)
                        continue;
                    c.freeList.erase(c.freeList.begin() + long(i));
                    if (at > off)
                        chunkFree(c, off, at - off);
                    if (at + size < off + len)
                        chunkFree(c, at + size, off + len - at - size);
                    out.block = c.block;
                    out.offset = at;
                    out.size = size;
                    out.chunk = int32_t(ci);
                    return true;
                }
            }
            if (pass == 0) {
                uint32_t chunkSize = std::max(ImageChunkSize, alignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT));
                Chunk c;
                c.block = makeBlock(chunkSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "image heap chunk");
                // tw2de (2026-10-22): a fragmented heap may still have the image's own size where it has no 64 MB.
                if (!c.block && chunkSize > alignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT)) {
                    chunkSize = alignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT);
                    c.block = makeBlock(chunkSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "image heap chunk (just the image)");
                }
                if (!c.block)
                    return false;
                c.size = chunkSize;
                c.freeList.push_back({0, chunkSize});
                chunks.push_back(std::move(c));
                logf("image heap: chunk %zu of %u MiB", chunks.size(), chunkSize >> 20);
            }
        }
        return false;
    }

    void Deko3DDevice::retire(const Deko3DImageAlloc &image, DkMemBlock block) {
        std::lock_guard<std::mutex> lock(retireMutex);
        retired.push_back({submitSerial.load(std::memory_order_acquire), image, block});
    }

    void Deko3DDevice::noteSubmission() {
        // A fence per submission serial (16 in a ring): what was retired before a submission is freed once its fence
        // signalled.
        std::lock_guard<std::mutex> lock(retireMutex);
        const uint64_t serial = submitSerial.load(std::memory_order_relaxed);
        DkFence &fence = serialFences[serial % SerialFences];
        if (serialFenceValue[serial % SerialFences] != 0) {
            // The ring wrapped: that older submission must be finished before its fence is reused.
            dkFenceWait(&fence, -1);
            completedSerial.store(std::max(completedSerial.load(), serialFenceValue[serial % SerialFences]));
        }
        dkQueueSignalFence(queue, &fence, false);
        serialFenceValue[serial % SerialFences] = serial;
        submitSerial.store(serial + 1, std::memory_order_release);
    }

    void Deko3DDevice::collectRetired() {
        std::lock_guard<std::mutex> lock(retireMutex);
        if (retired.empty())
            return;
        uint64_t done = completedSerial.load();
        for (uint32_t i = 0; i < SerialFences; i++) {
            if (serialFenceValue[i] > done && dkFenceWait(&serialFences[i], 0) == DkResult_Success)
                done = std::max(done, serialFenceValue[i]);
        }
        completedSerial.store(done);
        size_t kept = 0;
        for (size_t i = 0; i < retired.size(); i++) {
            Retired &r = retired[i];
            // Retired while submission r.serial was being recorded: free once that one has completed.
            if (r.serial <= done) {
                if (r.block)
                    destroyBlock(r.block);
                if (r.image.chunk >= 0) {
                    std::lock_guard<std::mutex> heap(heapMutex);
                    chunkFree(chunks[size_t(r.image.chunk)], r.image.offset, r.image.size);
                }
            }
            else {
                retired[kept++] = r;
            }
        }
        retired.resize(kept);
    }

    // ---- shaders

    static Deko3DShaderUsage parseUsage(const std::string &glsl) {
        Deko3DShaderUsage usage;
        const size_t at = glsl.find("// tw2de-dk ");
        if (at == std::string::npos)
            return usage;
        const size_t end = glsl.find('\n', at);
        const std::string line = glsl.substr(at, end == std::string::npos ? std::string::npos : end - at);
        auto field = [&](const char *key) -> std::string {
            const size_t k = line.find(key);
            if (k == std::string::npos)
                return {};
            const size_t v = k + strlen(key);
            const size_t e = line.find(' ', v);
            return line.substr(v, e == std::string::npos ? std::string::npos : e - v);
        };
        usage.uboMask = uint32_t(strtoul(field("ubo=").c_str(), nullptr, 16));
        usage.ssboMask = uint32_t(strtoul(field("ssbo=").c_str(), nullptr, 16));
        // tw2de (2026-10-30): a shader with exports (binding 14) reads the stream's flag mode and may write the page
        // flags' buffer (binding 12, shader_common_glsl.h g_DkMemExportFlags): bound whenever 14 is, whatever the line
        // says (a store written by an older recompiler lists 14 alone).
        if (usage.ssboMask & 0x4000u)
            usage.ssboMask |= 0x1000u;
        usage.textures = field("tex=") == "1";
        usage.txqMask = uint32_t(strtoul(field("txq=").c_str(), nullptr, 16));  // tw2de (2026-10-19)
        // texb=k:textureWord:samplerWord,... in binding order
        const std::string texb = field("texb=");
        size_t q = 0;
        while (q < texb.size()) {
            const size_t comma = texb.find(',', q);
            const std::string item = texb.substr(q, comma == std::string::npos ? std::string::npos : comma - q);
            unsigned k = 0, t = 0, s = 0;
            if (sscanf(item.c_str(), "%u:%u:%u", &k, &t, &s) == 3 && k == usage.texBindings.size())
                usage.texBindings.push_back({uint16_t(t), uint16_t(s)});
            if (comma == std::string::npos)
                break;
            q = comma + 1;
        }
        const std::string vin = field("vin=");
        size_t p = 0;
        while (p < vin.size()) {
            const size_t comma = vin.find(',', p);
            usage.vertexInputs.push_back(uint32_t(strtoul(vin.substr(p, comma - p).c_str(), nullptr, 10)));
            if (comma == std::string::npos)
                break;
            p = comma + 1;
        }
        return usage;
    }

    struct SpecDefault {
        uint32_t id;
        uint32_t value;
    };

    struct Deko3DShaderImpl : Deko3DShader {
        std::vector<SpecDefault> specDefaults;
        // Variants already looked up (spec values -> variant), so a pipeline does not hash the text again.
        std::mutex mutex;
        std::unordered_map<uint64_t, const DkShader *> known;
        // tw2de (2026-10-24, Deko3DCreateShaderFromMeta): made without its text. glsl is filled by loader the
        // first time a variant needs it (glslReady then); textKey is the default variant's key either way.
        uint64_t textKey = 0;
        Deko3DGlslLoader loader;
        Deko3DVariantKeyLookup variantKey;
        std::atomic<bool> glslReady{true};
        std::mutex glslMutex;

        // The text, read now if it was not (false: it could not be).
        bool ensureGlsl() {
            if (glslReady.load(std::memory_order_acquire))
                return true;
            std::lock_guard<std::mutex> lock(glslMutex);
            if (glslReady.load(std::memory_order_relaxed))
                return true;
            std::string text;
            if (!loader || !loader(text)) {
                logf("shader %s: its GLSL could not be read", name.empty() ? "?" : name.c_str());
                return false;
            }
            if (fnv1a64(text.data(), text.size()) != textKey) {
                // The text is not the one the metadata was made from: its keys would not be the cache's.
                logf("shader %s: its GLSL is not the one its metadata was made from", name.empty() ? "?" : name.c_str());
                return false;
            }
            glsl = std::move(text);
            glslReady.store(true, std::memory_order_release);
            return true;
        }
    };

    // tw2de (2026-10-24): everything createShader read from the text, in one place (Deko3DShaderMeta).
    bool Deko3DShaderMetaFromGlsl(const std::string &glsl, Deko3DShaderMeta &out) {
        out = {};
        out.textKey = fnv1a64(glsl.data(), glsl.size());
        out.usage = parseUsage(glsl);
        // tw2de (2026-10-18): the varying locations it declares ("layout(location = N) in / out" lines).
        {
            const char *key = "\nlayout(location = ";
            size_t p = 0;
            while ((p = glsl.find(key, p)) != std::string::npos) {
                p += strlen(key);
                char *end = nullptr;
                const unsigned long loc = strtoul(glsl.c_str() + p, &end, 10);
                if (loc < 32 && end && strncmp(end, ") in ", 5) == 0)
                    out.usage.inputMask |= 1u << loc;
                else if (loc < 32 && end && strncmp(end, ") out ", 6) == 0)
                    out.usage.outputMask |= 1u << loc;
            }
        }
        {
            // tw2de (2026-10-23): the offset from the key's length (a fixed 19 pointed at the 'x' since the line's
            // name grew by a character in the 2026-10-22 rename, which read every mask as 0).
            static constexpr char zkey[] = "\n// tw2de-vszero=0x";
            const size_t z = glsl.find(zkey);
            if (z != std::string::npos)
                out.usage.zeroOutputMask = uint32_t(strtoul(glsl.c_str() + z + sizeof(zkey) - 1, nullptr, 16));
        }
        out.specDefaults = Deko3DParseSpecDefaults(glsl);
        return true;
    }

    std::unique_ptr<RenderShader> Deko3DCreateShaderFromMeta(RenderDevice *device, DkStage stage,
                                                             const Deko3DShaderMeta &meta, Deko3DGlslLoader loader,
                                                             Deko3DVariantKeyLookup variantKey) {
        if (!device || !loader)
            return nullptr;
        auto shader = std::make_unique<Deko3DShaderImpl>();
        shader->device = static_cast<Deko3DDevice *>(device);
        shader->stage = stage;
        shader->usage = meta.usage;
        for (const auto &[id, value] : meta.specDefaults) {
            shader->specDefaults.push_back({id, value});
            shader->specIds.push_back(id);
        }
        shader->textKey = meta.textKey;
        shader->loader = std::move(loader);
        shader->variantKey = std::move(variantKey);
        shader->glslReady.store(false, std::memory_order_relaxed);
        return shader;
    }

    std::unique_ptr<RenderShader> Deko3DDevice::createShader(const void *data, uint64_t size, const char *, RenderShaderFormat format) {
        if (format != RenderShaderFormat::DEKO3D || data == nullptr || size < sizeof(Deko3DShaderBlob)) {
            logf("createShader: not a deko3d shader (format %d, %" PRIu64 " bytes)", int(format), size);
            return nullptr;
        }
        Deko3DShaderBlob header;
        memcpy(&header, data, sizeof(header));
        if (header.magic != Deko3DShaderBlob::Magic || sizeof(header) + uint64_t(header.glslSize) + header.dkshSize > size) {
            logf("createShader: bad deko3d shader blob");
            return nullptr;
        }
        auto shader = std::make_unique<Deko3DShaderImpl>();
        shader->device = this;
        shader->stage = DkStage(header.stage & ~(Deko3DShaderBlob::StagePlainUam | Deko3DShaderBlob::StageMetaOnly |
                                                 Deko3DShaderBlob::StageNamed));
        shader->plainUam = (header.stage & Deko3DShaderBlob::StagePlainUam) != 0;  // tw2de (2026-10-24)
        const char *glsl = static_cast<const char *>(data) + sizeof(header);
        // tw2de (2026-11-06): its name (StageNamed), for its DKSH made at build time.
        std::string builtinName;
        if (header.stage & Deko3DShaderBlob::StageNamed) {
            const char *n = glsl + header.glslSize + header.dkshSize;
            const char *end = static_cast<const char *>(data) + size;
            const char *z = static_cast<const char *>(memchr(n, 0, size_t(end - n)));
            if (z)
                builtinName.assign(n, z);
        }
        if (header.stage & Deko3DShaderBlob::StageMetaOnly) {
            // tw2de (2026-11-06): the record app/cmake/tw2de_glsl_blob.cmake writes (META_ONLY), checked there
            // against Deko3DShaderMetaFromGlsl for every such shader; the default variant only (no constants).
            const uint8_t *p = reinterpret_cast<const uint8_t *>(glsl), *end = p + header.glslSize;
            bool ok = true;
            auto rd = [&]() -> uint32_t {
                if (end - p < 4) {
                    ok = false;
                    return 0;
                }
                const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
                p += 4;
                return v;
            };
            const uint32_t version = rd();  // 2 (2026-11-06: the name after the record, StageNamed)
            const uint32_t keyLo = rd(), keyHi = rd();
            Deko3DShaderUsage &u = shader->usage;
            u.uboMask = rd();
            u.ssboMask = rd();
            u.textures = rd() != 0;
            u.txqMask = rd();
            u.inputMask = rd();
            u.outputMask = rd();
            u.zeroOutputMask = rd();
            for (uint32_t n = rd(); ok && n; --n) {
                const uint32_t w = rd();
                u.texBindings.push_back({uint16_t(w & 0xFFFF), uint16_t(w >> 16)});
            }
            for (uint32_t n = rd(); ok && n; --n)
                u.vertexInputs.push_back(rd());
            if (!ok || version != 2 || builtinName.empty()) {
                logf("createShader: bad deko3d metadata blob");
                return nullptr;
            }
            shader->textKey = uint64_t(keyHi) << 32 | keyLo;
            shader->glslReady.store(false, std::memory_order_relaxed);  // no text, no loader: only the default variant
            if (!g_builtinDksh || !g_builtinDksh(builtinName, shader->defaultDksh) || shader->defaultDksh.empty()) {
                logf("createShader: the DKSH of host shader %s is not in the NRO", builtinName.c_str());
                return nullptr;
            }
            return shader;
        }
        shader->glsl.assign(glsl, header.glslSize);
        if (header.dkshSize)
            shader->defaultDksh.assign(reinterpret_cast<const uint8_t *>(glsl) + header.glslSize,
                                       reinterpret_cast<const uint8_t *>(glsl) + header.glslSize + header.dkshSize);
        else if (shader->plainUam && g_builtinDksh && !builtinName.empty())
            g_builtinDksh(builtinName, shader->defaultDksh);  // tw2de (2026-11-06): made at build time
        Deko3DShaderMeta meta;
        Deko3DShaderMetaFromGlsl(shader->glsl, meta);
        shader->usage = std::move(meta.usage);
        shader->textKey = meta.textKey;
        for (const auto &[id, value] : meta.specDefaults) {
            shader->specDefaults.push_back({id, value});
            shader->specIds.push_back(id);
        }
        return shader;
    }

    // tw2de (2026-10-23): the fast pass of a run-time compile: without 16 (one backend run for a fragment shader)
    // and with 1048576 (Mesa's optimisation loops once each, uam glsl_frontend.cpp): the full pass replaces it.
    static unsigned fastPassFlags() { return (g_deko3dOptions.uamFlags & ~16u) | 1048576u; }

    void Deko3DDevice::compileWorker() {
        uint64_t heapFree = 0, processFree = 0;
        memoryFigures(heapFree, processFree);
        logf("uam thread: starting (heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)", heapFree >> 20, processFree >> 20);
        const uint64_t i0 = armTicksToNs(armGetSystemTick());
        uam::setFNXF_XIII_LRFlags(g_deko3dOptions.uamFlags);  // tw2de (2026-10-18)
        // tw2de (2026-10-18): off the render thread's core and below the game's threads (package 33's first run
        // compiled 231 variants during play: a core busy for minutes, every thread slower).
        {
            std::lock_guard<std::mutex> lock(compileMutex);  // compile() reads it under the lock
            compileThreadHandle = threadGetCurHandle();
        }
        if (g_deko3dOptions.compileCore >= 0)
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_deko3dOptions.compileCore, 1u << g_deko3dOptions.compileCore);
        if (g_deko3dOptions.compilePriority > 0)
            svcSetThreadPriority(CUR_THREAD_HANDLE, u32(g_deko3dOptions.compilePriority));
        logf("uam thread: core %d, priority 0x%X (fixed; sleeps on an event between compiles)",
             g_deko3dOptions.compileCore, g_deko3dOptions.compilePriority);
        uam::init(true);
        // Self-test (SwitchWakerHD's): a small shader now, which also builds the resident frontend's tables before the
        // game's first variant.
        static const char test[] = "#version 460\nlayout(location = 0) in vec4 pos;\nlayout(std140, binding = 0) uniform B { vec4 s; };\n"
                                   "void main() { gl_Position = pos * s; }\n";
        const uint64_t i1 = armTicksToNs(armGetSystemTick());
        uam::Result selfTest = uam::compile(uam::Stage::Vertex, test);
        const uint64_t i2 = armTicksToNs(armGetSystemTick());
        memoryFigures(heapFree, processFree);
        logf("uam thread: %s (init %.0f ms, self-test %zu bytes of DKSH in %.0f ms; heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)%s%s",
             selfTest.ok ? "ready" : "SELF-TEST FAILED", double(i1 - i0) / 1e6, selfTest.dksh.size(), double(i2 - i1) / 1e6,
             heapFree >> 20, processFree >> 20, selfTest.ok ? "" : ": ", selfTest.ok ? "" : selfTest.log.c_str());
        // tw2de (2026-10-21): the fast pass for what the game waits for, the full one in the background.
        if (selfTest.ok && g_deko3dOptions.fastRuntimeCompiles && (g_deko3dOptions.uamFlags & 16) &&
            !g_deko3dOptions.backgroundFullPass) {
            // tw2de (2026-10-21): fast passes only; the next start's pre-pass compiles them in full.
            uam::setThreadFNXF_XIII_LRFlags(fastPassFlags());
            fastActive = true;
            logf("uam thread: fast pass for run-time compiles (flags %u); the next start compiles them in full (flags %u)",
                 fastPassFlags(), g_deko3dOptions.uamFlags);
        }
        else if (selfTest.ok && g_deko3dOptions.fastRuntimeCompiles && (g_deko3dOptions.uamFlags & 16)) {
            uam::setThreadFNXF_XIII_LRFlags(fastPassFlags());
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 8u << 20);
            pthread_t thread;
            {
                std::lock_guard<std::mutex> lock(compileMutex);
                upgradeRunning = true;
            }
            if (pthread_create(&thread, &attr, upgradeThreadEntry, this) == 0) {
                pthread_detach(thread);
                fastActive = true;
                logf("uam thread: fast pass for run-time compiles (flags %u), the full pass (flags %u) in the background",
                     fastPassFlags(), g_deko3dOptions.uamFlags);
            }
            else {
                uam::setThreadFNXF_XIII_LRFlags(g_deko3dOptions.uamFlags);
                std::lock_guard<std::mutex> lock(compileMutex);
                upgradeRunning = false;
                logf("uam thread: the background compile thread did not start; run-time compiles are full passes");
            }
            pthread_attr_destroy(&attr);
        }
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            compilerReady = true;
        }
        UamThreadAlloc alloc;
        for (;;) {
            CompileJob *job = nullptr;
            {
                std::unique_lock<std::mutex> lock(compileMutex);
                // tw2de (2026-10-22): asleep in the kernel until a job is queued (svcWaitForAddress on compileSeq,
                // read here under the lock while the queue is empty: a job queued after it bumps the value, so the
                // wait returns at once or is woken). Idle a while first, then the allocator's memory back until the
                // next burst. Every return (woken, timed out, value already changed) checks the queue again.
                bool trimmed = false;
                const uint64_t idleStart = armGetSystemTick();
                const uint64_t idleNs = uint64_t(g_deko3dOptions.compileIdleMs) * 1000000ull;
                while (!compileStop && compileQueue.empty()) {
                    const uint32_t seq = compileSeq.load(std::memory_order_acquire);
                    lock.unlock();
                    if (trimmed) {
                        svcWaitForAddress(&compileSeq, ArbitrationType_WaitIfEqual, s32(seq), -1);
                    }
                    else {
                        const uint64_t spent = armTicksToNs(armGetSystemTick() - idleStart);
                        if (spent >= idleNs) {
                            alloc.end();
                            trimmed = true;
                        }
                        else {
                            svcWaitForAddress(&compileSeq, ArbitrationType_WaitIfEqual, s32(seq), s64(idleNs - spent));
                        }
                    }
                    lock.lock();
                }
                if (compileStop)
                    break;
                job = compileQueue.front();
                compileQueue.erase(compileQueue.begin());
                job->startTick = armGetSystemTick();
                compiling = true;
            }
            // tw2de (2026-10-24): a job without text (the late path only): the logs below read an empty one.
            static const std::string kNoSource;
            const bool lateOnly = job->source == nullptr;
            if (lateOnly)
                job->source = &kNoSource;
            static const uam::Stage stages[] = {uam::Stage::Vertex, uam::Stage::TessCtrl, uam::Stage::TessEval,
                                                uam::Stage::Geometry, uam::Stage::Fragment, uam::Stage::Compute};
            const uint64_t t0 = armTicksToNs(armGetSystemTick());
            {
                static uint32_t logged = 0;
                if (logged < 4) {
                    logged++;
                    uint64_t hf = 0, pf = 0;
                    memoryFiguresQuick(hf, pf);  // tw2de (2026-11-05): no heap walk
                    logf("uam: compile %u (stage %u, %zu bytes of GLSL; heap room %" PRIu64 " MB, process %" PRIu64 " MB unused)",
                         logged, uint32_t(job->stage), job->source->size(), hf >> 20, pf >> 20);
                }
                // tw2de (2026-10-20): every run-time compile's specialization (its constants), so the variants
                // a cache lacks can be found (tools/deko3d_variant_cache.py rebuilds their keys from these).
                static uint32_t named = 0;
                if (named < 64) {
                    named++;
                    // The variant's own defines: the lines plume inserted after the first one (2026-10-20: only those;
                    // the shader's defaults further down are not logged). And which shader: the text without them,
                    // its size and FNV-1a 64, which tools/deko3d_variant_cache.py --runtime-log matches to a GLSL file
                    // (so a console log alone completes the next cache).
                    std::string defines;
                    const std::string &src = *job->source;
                    const char *key = "#define SPIRV_CROSS_CONSTANT_ID_";
                    size_t body = src.find('\n');
                    body = body == std::string::npos ? src.size() : body + 1;
                    size_t rest = body;
                    while (src.compare(rest, strlen(key), key) == 0) {
                        const size_t e = src.find('\n', rest);
                        const size_t next = e == std::string::npos ? src.size() : e + 1;
                        if (defines.size() < 900) {
                            defines += src.substr(rest + strlen(key), (e == std::string::npos ? src.size() : e) - rest - strlen(key));
                            defines += ';';
                        }
                        rest = next;
                    }
                    uint64_t base = 0xcbf29ce484222325ull;
                    auto mix = [&](size_t from, size_t to) {
                        for (size_t i = from; i < to; ++i)
                            base = (base ^ uint8_t(src[i])) * 0x100000001b3ull;
                    };
                    mix(0, body);
                    mix(rest, src.size());
                    logf("uam: run-time variant (stage %u, %zu bytes, base %zu bytes %016" PRIx64 "): %s",
                         uint32_t(job->stage), src.size(), body + (src.size() - rest), base, defines.c_str());
                }
            }
            // tw2de (2026-10-22): what is being compiled, for the stall watchdog (switch_perf.cpp, rex_stall.log).
            {
                uint64_t h = 0xcbf29ce484222325ull;
                for (unsigned char ch : *job->source)
                    h = (h ^ ch) * 0x100000001b3ull;
                g_rex_uam_compile_hash = h;
                g_rex_uam_compile_stage = uint32_t(job->stage);
                g_rex_uam_compile_size = uint32_t(job->source->size());
                g_rex_uam_compiles = g_rex_uam_compiles + 1;
                g_rex_uam_compile_start = armGetSystemTick();
            }
            // tw2de (2026-10-23): a fast pass from the shader's kept front end when there is one (late_spec.h), else
            // from the GLSL. Only fast passes: the full pass (the background thread, the next pre-pass) is the GLSL's.
            uam::Result r;
            bool late = false;
            if (!job->plainUam && fastActive && g_lateCompile && job->baseKey && job->values && !job->values->empty()) {
                late = alloc.late(job->stage, job->baseKey, *job->values, r);
                if (!late)
                    r = uam::Result();
            }
            if (!late && lateOnly) {
                job->needsSource = true;  // tw2de (2026-10-24): the caller reads the text and queues it again
                r = uam::Result();
            }
            else if (!late && job->plainUam) {
                // tw2de (2026-10-24): a host shader with uam's tw2de flags at 0 (Deko3DShaderBlob::StagePlainUam),
                // then this thread's flags back.
                uam::setThreadFNXF_XIII_LRFlags(0);
                r = alloc.compile(stages[std::min<uint32_t>(job->stage, 5)], job->source->c_str());
                uam::setThreadFNXF_XIII_LRFlags(fastActive ? fastPassFlags() : g_deko3dOptions.uamFlags);
            }
            else if (!late)
                r = alloc.compile(stages[std::min<uint32_t>(job->stage, 5)], job->source->c_str());
            g_rex_uam_compile_end = armGetSystemTick();  // tw2de: the "Compiling shaders..." notice
            g_rex_uam_compile_start = 0;
            g_deko3dStats.shaderCompileNs += armTicksToNs(armGetSystemTick()) - t0;
            g_deko3dStats.shaderCompiles++;
            {
                std::lock_guard<std::mutex> lock(compileMutex);
                job->ok = r.ok;
                job->late = late;
                job->endTick = armGetSystemTick();
                compiling = false;
                job->fast = fastActive && !job->plainUam;  // tw2de (2026-10-23): flag 1048576 changes vertex shaders too (Mesa's loops once)
                *job->dksh = std::move(r.dksh);
                *job->log = std::move(r.log);
                job->done = true;
            }
            compileDone.notify_all();
            upgradeWake.notify_one();  // the queue may be empty now: the background compiles may go on
        }
        alloc.end();
        // The background thread first (it compiles with the same uam): it stops after the compile it is in.
        bool upgradeStopped;
        {
            std::unique_lock<std::mutex> lock(compileMutex);
            upgradeWake.notify_all();
            upgradeStopped = compileDone.wait_for(lock, std::chrono::seconds(3), [this] { return !upgradeRunning; });
        }
        if (upgradeStopped)
            uam::shutdown();  // else the process is ending with a compile in progress: left as it is
        std::lock_guard<std::mutex> lock(compileMutex);
        compilerReady = false;
        compileDone.notify_all();
    }

    // compileMutex not held. Once a session per key.
    void Deko3DDevice::queueUpgrade(DkStage stage, const std::string &source, uint64_t key) {
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            if (!upgradeRunning || !upgradeKeys.insert(key).second)
                return;
            upgradeQueue.push_back({stage, source, key});
        }
        upgradeWake.notify_one();
    }

    // tw2de (2026-10-21): the full pass of the variants compiled (or cached) as fast passes, on the compile core at
    // the lowest priority, one at a time and only while nothing waits for a compile.
    void Deko3DDevice::upgradeWorker() {
        if (g_deko3dOptions.compileCore >= 0)
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_deko3dOptions.compileCore, 1u << g_deko3dOptions.compileCore);
        const bool prio = g_deko3dOptions.upgradePriority > 0 &&
                          R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, u32(g_deko3dOptions.upgradePriority)));
        uam::setThreadFNXF_XIII_LRFlags(g_deko3dOptions.uamFlags);
        logf("uam background thread: core %d, priority 0x%X%s", g_deko3dOptions.compileCore,
             g_deko3dOptions.upgradePriority, prio ? "" : " NOT accepted");
        UamThreadAlloc alloc;
        for (;;) {
            UpgradeJob job;
            {
                std::unique_lock<std::mutex> lock(compileMutex);
                auto ready = [this] { return compileStop || (!upgradeQueue.empty() && compileQueue.empty()); };
                if (!upgradeWake.wait_for(lock, std::chrono::milliseconds(g_deko3dOptions.compileIdleMs), ready)) {
                    lock.unlock();
                    alloc.end();
                    lock.lock();
                    upgradeWake.wait(lock, ready);
                }
                if (compileStop)
                    break;
                job = std::move(upgradeQueue.front());
                upgradeQueue.pop_front();
            }
            const uint64_t t0 = armTicksToNs(armGetSystemTick());
            uam::Result r = alloc.compile(job.stage == DkStage_Fragment ? uam::Stage::Fragment : uam::Stage::Vertex, job.source.c_str());
            const uint64_t ms = (armTicksToNs(armGetSystemTick()) - t0) / 1000000;
            uint64_t done, failed;
            size_t queued;
            {
                std::lock_guard<std::mutex> lock(compileMutex);
                done = r.ok ? ++upgradesDone : upgradesDone;
                failed = r.ok ? upgradesFailed : ++upgradesFailed;
                queued = upgradeQueue.size();
            }
            if (r.ok && g_cacheStore)
                g_cacheStore(job.key, r.dksh, false);
            if (done + failed <= 8 || (done + failed) % 50 == 0)
                logf("uam background: variant %016" PRIx64 " %s in %" PRIu64 " ms (%" PRIu64 " done, %" PRIu64 " failed, %zu queued)",
                     job.key, r.ok ? "compiled in full" : "FAILED", ms, done, failed, queued);
        }
        alloc.end();
        std::lock_guard<std::mutex> lock(compileMutex);
        upgradeRunning = false;
        compileDone.notify_all();
    }

    // tw2de (2026-10-22): after a job or the stop was queued under compileMutex (see compileSeq).
    void Deko3DDevice::wakeCompileThread() {
        compileSeq.fetch_add(1, std::memory_order_acq_rel);
        svcSignalToAddress(&compileSeq, SignalType_Signal, 0, -1);
    }

    bool Deko3DDevice::upgradesOn() {
        std::lock_guard<std::mutex> lock(compileMutex);
        return upgradeRunning;
    }

    // tw2de (2026-10-24): source may be null: the late path only (needsSource says it could not give the variant).
    bool Deko3DDevice::compile(DkStage stage, const std::string *source, std::vector<uint8_t> &dksh, std::string &log, bool *fast,
                               uint64_t baseKey, const std::vector<std::pair<uint32_t, uint32_t>> *values,
                               bool plainUam, bool *needsSource) {
        CompileJob job;
        job.stage = stage;
        job.plainUam = plainUam;
        job.source = source;
        job.baseKey = baseKey;
        job.values = values;
        job.dksh = &dksh;
        job.log = &log;
        // tw2de (2026-10-22): the uam thread runs at a fixed priority (compilePriority, 0x2C) and sleeps on
        // compileSeq between compiles: queued, then woken (wakeCompileThread). No priority lending any more.
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            job.queuedTick = armGetSystemTick();
            job.ahead = uint32_t(compileQueue.size()) + (compiling ? 1u : 0u);
            compileQueue.push_back(&job);
        }
        wakeCompileThread();
        if (g_compileNotice)
            g_compileNotice();  // tw2de (2026-10-22): while the uam thread compiles
        std::unique_lock<std::mutex> lock(compileMutex);
        compileDone.wait(lock, [&] { return job.done; });
        lock.unlock();
        if (job.needsSource) {
            if (needsSource)
                *needsSource = true;
            return false;  // not a compile: nothing logged or counted as failed
        }
        logCompileQueue(job);
        if (fast)
            *fast = job.fast;
        return job.ok;
    }

    // tw2de (2026-10-23): what a run-time compile cost the thread that asked for it, and whether compiles queue
    // behind each other (then a second compile thread would shorten the wait; alone, it would not). Each of the first
    // 32, then a summary every 50.
    void Deko3DDevice::logCompileQueue(const CompileJob &job) {
        const double tickMs = 1000.0 / double(armGetSystemTickFreq());
        const double waited = double(job.startTick - job.queuedTick) * tickMs;
        const double compiled = double(job.endTick - job.startTick) * tickMs;
        const double total = double(armGetSystemTick() - job.queuedTick) * tickMs;
        struct Totals {
            uint64_t n = 0, behind = 0, vs = 0, ps = 0, late = 0;
            uint32_t maxAhead = 0;
            double waited = 0, compiled = 0, total = 0, worst = 0;
        };
        static std::mutex m;
        static Totals all, window;
        std::lock_guard<std::mutex> lock(m);
        for (Totals *t : {&all, &window}) {
            t->n++;
            t->behind += job.ahead ? 1 : 0;
            t->vs += job.stage == DkStage_Vertex ? 1 : 0;
            t->ps += job.stage == DkStage_Fragment ? 1 : 0;
            t->late += job.late ? 1 : 0;
            t->maxAhead = std::max(t->maxAhead, job.ahead);
            t->waited += waited;
            t->compiled += compiled;
            t->total += total;
            t->worst = std::max(t->worst, total);
        }
        if (all.n <= 32)
            logf("uam queue: compile %" PRIu64 " (%s, %zu bytes of GLSL, %s): %u ahead of it, waited %.1f ms, compiled in %.1f ms, "
                 "%.1f ms for the thread that asked (core %u)",
                 all.n, job.stage == DkStage_Fragment ? "pixel" : job.stage == DkStage_Vertex ? "vertex" : "other",
                 job.source->size(), job.late ? "fast pass from its front end" : job.fast ? "fast pass" : "full pass", job.ahead, waited, compiled, total,
                 svcGetCurrentProcessorNumber());
        if (window.n == 50) {
            logf("uam queue: the last 50 compiles (%" PRIu64 " vertex, %" PRIu64 " pixel, %" PRIu64 " from front ends): %" PRIu64 " queued behind another "
                 "(at most %u ahead), %.0f ms waiting in the queue, %.0f ms compiling, %.0f ms asked in all (worst %.0f); "
                 "since the start %" PRIu64 " compiles, %" PRIu64 " behind another, %.0f ms waiting, %.0f ms compiling",
                 window.vs, window.ps, window.late, window.behind, window.maxAhead, window.waited, window.compiled, window.total,
                 window.worst, all.n, all.behind, all.waited, all.compiled);
            window = Totals();
        }
    }

    // Shader code: 16 MB blocks, bump allocated (deko3d maps every code block into its one code segment).
    static constexpr uint32_t CodeBlockSize = 16u << 20;

    bool Deko3DDevice::loadCode(const std::vector<uint8_t> &dksh, DkShader &shader, const char *what) {
        if (dksh.size() < 64)
            return false;
        {
            // The DKSH header (uam's dksh.h): programs_off at byte 16; the program's per_warp_scratch_sz at byte 20.
            uint32_t programsOff = 0, scratch = 0;
            memcpy(&programsOff, dksh.data() + 16, 4);
            if (uint64_t(programsOff) + 24 <= dksh.size())
                memcpy(&scratch, dksh.data() + programsOff + 20, 4);
            if (scratch > perWarpScratch) {
                logf("shader %s needs %u bytes of scratch a warp, the queue has %u: not loaded", what, scratch, perWarpScratch);
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(codeMutex);
        const uint32_t size = uint32_t(dksh.size());
        if (codeBlocks.empty() || alignUp(codeBlocks.back().second, DK_SHADER_CODE_ALIGNMENT) + size > CodeBlockSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
            if (size > CodeBlockSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
                logf("shader %s: %u bytes of DKSH do not fit a code block", what, size);
                return false;
            }
            DkMemBlock block = makeBlock(CodeBlockSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, "shader code");
            if (!block)
                return false;
            codeBlocks.push_back({block, 0});
            logf("shader code: block %zu (16 MiB)", codeBlocks.size());
        }
        auto &cb = codeBlocks.back();
        const uint32_t at = alignUp(cb.second, DK_SHADER_CODE_ALIGNMENT);
        memcpy(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(cb.first)) + at, dksh.data(), size);
        DkShaderMaker m;
        dkShaderMakerDefaults(&m, cb.first, at);
        dkShaderInitialize(&shader, &m);
        if (!dkShaderIsValid(&shader)) {
            logf("shader %s: %u bytes of DKSH are not valid", what, size);
            return false;
        }
        cb.second = at + size;
        g_deko3dStats.codeBytes += size;
        // Command lists invalidate the GPU's shader cache before their next draw (deko3d does it only at a queue
        // flush).
        codeEpoch.fetch_add(1, std::memory_order_release);
        return true;
    }

    const DkShader *Deko3DDevice::loadVariant(const Deko3DShader *base, const RenderSpecConstant *specConstants, uint32_t specConstantsCount) {
        Deko3DShaderImpl *shader = const_cast<Deko3DShaderImpl *>(static_cast<const Deko3DShaderImpl *>(base));
        // The constants this shader has whose value differs from its default, by id.
        std::string defines;
        std::vector<std::pair<uint32_t, uint32_t>> values;  // tw2de: for the variant recorder
        uint64_t specKey = 0xcbf29ce484222325ull;
        for (const SpecDefault &d : shader->specDefaults) {
            for (uint32_t i = 0; i < specConstantsCount; i++) {
                if (specConstants[i].index == d.id && specConstants[i].value != d.value) {
                    values.emplace_back(d.id, specConstants[i].value);
                    char line[64];
                    snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u %uu\n", d.id, specConstants[i].value);
                    defines += line;
                    specKey = mixHash(specKey, &d.id, sizeof(d.id));
                    specKey = mixHash(specKey, &specConstants[i].value, sizeof(uint32_t));
                    break;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(shader->mutex);
            auto it = shader->known.find(specKey);
            if (it != shader->known.end())
                return it->second;
        }
        // The variant's text: the GLSL with the constants defined after its first line (#version). tw2de
        // (2026-10-24): made only when it is needed (a shader made from its metadata reads its text then).
        std::string source;
        bool haveSource = false;
        auto makeSource = [&]() -> bool {
            if (haveSource)
                return true;
            if (!shader->ensureGlsl())
                return false;
            const size_t nl = shader->glsl.find('\n');
            if (defines.empty() || nl == std::string::npos) {
                source = shader->glsl;
            }
            else {
                source.reserve(shader->glsl.size() + defines.size());
                source.append(shader->glsl, 0, nl + 1);
                source += defines;
                source.append(shader->glsl, nl + 1, std::string::npos);
            }
            haveSource = true;
            return true;
        };
        // The key: the default variant's is the text's (textKey); another's is the renderer's when it knows it (the
        // variant list), or the variant text's.
        uint64_t key = 0;
        if (shader->glslReady.load(std::memory_order_acquire) && !shader->loader) {
            makeSource();
            key = fnv1a64(source.data(), source.size());
        }
        else if (defines.empty()) {
            key = shader->textKey;
        }
        else if (!(shader->variantKey && shader->variantKey(values, key))) {
            if (!makeSource())
                return nullptr;  // no text: no variant (looked up again next time)
            key = fnv1a64(source.data(), source.size());
        }
        // tw2de (2026-10-24): a host shader's DKSH under a key of its own (its flags differ from what the cache held
        // for the same text before: those records are never read again).
        if (shader->plainUam) {
            static constexpr char kPlainTag[] = "uam tw2de flags 0";
            key = mixHash(key, kPlainTag, sizeof(kPlainTag) - 1);
        }
        Deko3DShaderVariant *variant = nullptr;
        {
            std::lock_guard<std::mutex> lock(variantMutex);
            auto it = variants.find(key);
            if (it != variants.end())
                variant = it->second.get();
        }
        if (variant == nullptr) {
            std::vector<uint8_t> dksh;
            bool have = false;
            if (defines.empty() && !shader->defaultDksh.empty()) {
                dksh = shader->defaultDksh;
                have = true;
            }
            bool fast = false;
            if (!have && g_cacheLookup && g_cacheLookup(key, dksh, fast)) {
                g_deko3dStats.shaderCacheHits++;
                have = true;
                // a fast pass an earlier session did not get to upgrade (tw2de: the text read only when the
                // background full passes run)
                if (fast && upgradesOn() && makeSource())
                    queueUpgrade(shader->stage, source, key);
            }
            // tw2de (2026-10-24): a shader made from its metadata: its variant from the shader's front end (the late
            // path, keyed by the text's key) before its text is read; the text only when that cannot give it.
            bool lateTried = false;
            if (!have && !haveSource && !shader->plainUam && !values.empty() && g_lateCompile) {
                lateTried = true;
                std::string log;
                bool needsSource = false;
                if (compile(shader->stage, nullptr, dksh, log, &fast, shader->textKey, &values, false, &needsSource)) {
                    have = true;
                    if (g_cacheStore)
                        g_cacheStore(key, dksh, fast);
                    if (fast && upgradesOn() && makeSource())
                        queueUpgrade(shader->stage, source, key);
                }
                else if (!needsSource) {
                    g_deko3dStats.shaderCompileFailures++;
                    logf("late compile failed for %s (variant %016" PRIx64 "): %s",
                         shader->name.empty() ? "a shader" : shader->name.c_str(), key, log.c_str());
                }
            }
            if (!have) {
                std::string log;
                if (!makeSource()) {
                    return nullptr;  // no text to compile: looked up again next time
                }
                else if (!compile(shader->stage, &source, dksh, log, &fast, lateTried ? 0 : shader->textKey, &values,
                                  shader->plainUam)) {
                    g_deko3dStats.shaderCompileFailures++;
                    static std::atomic<uint32_t> logged{0};
                    if (logged.fetch_add(1) < 32)
                        logf("uam failed for %s (variant %016" PRIx64 "): %s", shader->name.empty() ? "a shader" : shader->name.c_str(), key, log.c_str());
                }
                else {
                    have = true;
                    if (g_cacheStore)
                        g_cacheStore(key, dksh, fast);
                    if (fast && upgradesOn())
                        queueUpgrade(shader->stage, source, key);
                }
            }
            auto fresh = std::make_unique<Deko3DShaderVariant>();
            if (have)
                fresh->valid = loadCode(dksh, fresh->shader, shader->name.c_str());
            if (have && g_variantRecorder)
                g_variantRecorder(shader->name, uint32_t(shader->stage), values, key, fast);
            std::lock_guard<std::mutex> lock(variantMutex);
            auto &slot = variants[key];
            if (!slot)
                slot = std::move(fresh);
            variant = slot.get();
        }
        const DkShader *out = variant->valid ? &variant->shader : nullptr;
        std::lock_guard<std::mutex> lock(shader->mutex);
        shader->known[specKey] = out;
        return out;
    }

    // ---- samplers

    std::unique_ptr<RenderSampler> Deko3DDevice::createSampler(const RenderSamplerDesc &desc) {
        auto sampler = std::make_unique<Deko3DSampler>();
        sampler->desc = desc;
        DkSampler s;
        dkSamplerDefaults(&s);
        s.minFilter = desc.minFilter == RenderFilter::NEAREST ? DkFilter_Nearest : DkFilter_Linear;
        s.magFilter = desc.magFilter == RenderFilter::NEAREST ? DkFilter_Nearest : DkFilter_Linear;
        s.mipFilter = desc.mipmapMode == RenderMipmapMode::NEAREST ? DkMipFilter_Nearest : DkMipFilter_Linear;
        s.wrapMode[0] = toDk(desc.addressU);
        s.wrapMode[1] = toDk(desc.addressV);
        s.wrapMode[2] = toDk(desc.addressW);
        s.lodClampMin = desc.minLOD;
        s.lodClampMax = std::min(desc.maxLOD, 1000.0f);
        s.lodBias = desc.mipLODBias;
        s.compareEnable = desc.comparisonEnabled;
        s.compareOp = toDk(desc.comparisonFunc);
        const float border = desc.borderColor == RenderBorderColor::OPAQUE_WHITE ? 1.0f : 0.0f;
        const float borderAlpha = desc.borderColor == RenderBorderColor::TRANSPARENT_BLACK ? 0.0f : 1.0f;
        s.borderColor[0].value_f = border;
        s.borderColor[1].value_f = border;
        s.borderColor[2].value_f = border;
        s.borderColor[3].value_f = borderAlpha;
        s.maxAnisotropy = desc.anisotropyEnabled ? float(std::max<uint32_t>(desc.maxAnisotropy, 1)) : 1.0f;
        dkSamplerDescriptorInitialize(&sampler->descriptor, &s);
        return sampler;
    }

    // ---- pipeline layouts

    std::unique_ptr<RenderPipelineLayout> Deko3DDevice::createPipelineLayout(const RenderPipelineLayoutDesc &desc) {
        auto layout = std::make_unique<Deko3DPipelineLayout>();
        // Constant buffers: the k-th one of the layout (set order, binding order) is UBO binding k; storage buffers
        // likewise (an array takes one binding per element). tools/deko3d_glsl.py numbers the shaders' the same way.
        uint32_t ubo = 0, ssbo = 0;
        for (uint32_t s = 0; s < desc.descriptorSetDescsCount; s++) {
            const RenderDescriptorSetDesc &sd = desc.descriptorSetDescs[s];
            Deko3DPipelineLayout::Set set;
            uint32_t index = 0;
            for (uint32_t r = 0; r < sd.descriptorRangesCount; r++) {
                const RenderDescriptorRange &dr = sd.descriptorRanges[r];
                Deko3DPipelineLayout::Range range;
                range.type = dr.type;
                range.binding = dr.binding;
                range.count = dr.count;
                if (sd.lastRangeIsBoundless && r + 1 == sd.descriptorRangesCount)
                    range.count = std::max<uint32_t>(range.count, sd.boundlessRangeSize);
                range.descriptorIndex = index;
                switch (dr.type) {
                case RenderDescriptorRangeType::CONSTANT_BUFFER:
                case RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC:
                    range.slot = ubo;
                    ubo += range.count;
                    break;
                case RenderDescriptorRangeType::STRUCTURED_BUFFER:
                case RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER:
                case RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER:
                case RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER:
                case RenderDescriptorRangeType::FORMATTED_BUFFER:
                case RenderDescriptorRangeType::READ_WRITE_FORMATTED_BUFFER:
                    range.slot = ssbo;
                    ssbo += range.count;
                    break;
                default:
                    break;
                }
                index += range.count;
                set.ranges.push_back(range);
            }
            layout->sets.push_back(std::move(set));
        }
        if (ubo > 5)
            logf("pipeline layout with %u constant buffers: the push constants' UBO binding 5 overlaps them", ubo);
        for (uint32_t i = 0; i < desc.pushConstantRangesCount; i++) {
            layout->pushRanges.push_back(desc.pushConstantRanges[i]);
            layout->pushSize = std::max(layout->pushSize, desc.pushConstantRanges[i].offset + desc.pushConstantRanges[i].size);
        }
        if (layout->pushSize > PushSize)
            logf("push constants of %u bytes (more than %u)", layout->pushSize, PushSize);
        return layout;
    }

    // ---- pipelines

    static DkPrimitive toDk(RenderPrimitiveTopology t) {
        switch (t) {
        case RenderPrimitiveTopology::POINT_LIST: return DkPrimitive_Points;
        case RenderPrimitiveTopology::LINE_LIST: return DkPrimitive_Lines;
        case RenderPrimitiveTopology::LINE_STRIP: return DkPrimitive_LineStrip;
        case RenderPrimitiveTopology::TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
        case RenderPrimitiveTopology::TRIANGLE_FAN: return DkPrimitive_TriangleFan;
        case RenderPrimitiveTopology::TRIANGLE_LIST:
        default: return DkPrimitive_Triangles;
        }
    }

    // deko3d 0.5.0 writes the colour destination factor into FuncAlphaDst (SwitchWakerHD's deko3d patch 1, Tenkaichi-3-NX's
    // deko_blend_state.h): the target's IndependentBlend registers (method 0x781 + 8 * target, six of them) are
    // written again with the right alpha factor after dkCmdBufBindBlendStates.
    static uint32_t blendFactorWord(DkBlendFactor f) { return (uint32_t(f) & 0x1Fu) | ((uint32_t(f) & 0x20u) ? 0xC000u : 0u); }

    static void blendWords(uint32_t words[7], uint32_t target, const DkBlendState &s) {
        words[0] = (1u << 29) | (6u << 16) | (0u << 13) | (0x781u + 8u * target); // increasing, 6 words, 3D subchannel
        words[1] = s.colorBlendOp;
        words[2] = blendFactorWord(s.srcColorBlendFactor);
        words[3] = blendFactorWord(s.dstColorBlendFactor);
        words[4] = s.alphaBlendOp;
        words[5] = blendFactorWord(s.srcAlphaBlendFactor);
        words[6] = blendFactorWord(s.dstAlphaBlendFactor);
    }

    std::unique_ptr<RenderPipeline> Deko3DDevice::createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) {
        const Deko3DTimed timed(GetDeko3DStats().pipelineCreateNs);  // tw2de (2026-10-28)
        auto p = std::make_unique<Deko3DGraphicsPipeline>();
        p->type = Deko3DPipeline::Type::Graphics;
        p->device = this;
        p->layout = static_cast<const Deko3DPipelineLayout *>(desc.pipelineLayout);
        const Deko3DShader *stages[3] = {static_cast<const Deko3DShader *>(desc.vertexShader),
                                         static_cast<const Deko3DShader *>(desc.geometryShader),
                                         static_cast<const Deko3DShader *>(desc.pixelShader)};
        static const DkStage dkStages[3] = {DkStage_Vertex, DkStage_Geometry, DkStage_Fragment};
        bool ok = stages[0] != nullptr;
        // tw2de (2026-10-18, Deko3DOptions::vsOutputs): the vertex shader's constant 905, the locations the pixel
        // shader reads of the ones it writes, when that is not all of them (tools/deko3d_variant_cache.py's stage_spec
        // gives the DKSH cache the same).
        // tw2de (2026-10-27): with a geometry shader too (Renut's rectangle_list_gs and point_sprite_gs pass a varying
        // through only where the pixel shader reads it, constant 905 below): the pixel shader's locations and the point
        // size (24), which point_sprite_gs always reads. The game's memexport kernels are point lists: their vertex shader
        // wrote every output for a geometry shader that dropped them all.
        std::vector<RenderSpecConstant> vsSpec;
        if (g_deko3dOptions.vsOutputs && stages[0] &&
            std::find(stages[0]->specIds.begin(), stages[0]->specIds.end(), 905u) != stages[0]->specIds.end()) {
            const uint32_t outs = stages[0]->usage.outputMask;
            const uint32_t gsReads = stages[1] ? (1u << 24) : 0u;
            const uint32_t mask = ((stages[2] ? stages[2]->usage.inputMask : 0u) | gsReads) & outs;
            if (mask != outs) {
                vsSpec.assign(desc.specConstants, desc.specConstants + desc.specConstantsCount);
                vsSpec.push_back(RenderSpecConstant(905, mask));
            }
        }
        // tw2de (2026-10-18): the pixel shader's constant 906, its inputs the vertex shader writes as zero (not with a
        // geometry shader, which writes them).
        std::vector<RenderSpecConstant> psSpec;
        if (g_deko3dOptions.vsOutputs && stages[0] && stages[2] && !stages[1] &&
            std::find(stages[2]->specIds.begin(), stages[2]->specIds.end(), 906u) != stages[2]->specIds.end()) {
            const uint32_t zero = stages[0]->usage.zeroOutputMask & stages[2]->usage.inputMask;
            if (zero) {
                psSpec.assign(desc.specConstants, desc.specConstants + desc.specConstantsCount);
                psSpec.push_back(RenderSpecConstant(906, zero));
            }
        }
        // tw2de (2026-10-27): a geometry shader's constant 905 too (Renut's rectangle_list_gs): the locations the
        // pixel shader reads of the ones it writes. It holds a vertex's outputs in registers until EmitVertex: all 25
        // took 108 registers, a pixel shader's few 17-29.
        std::vector<RenderSpecConstant> gsSpec;
        if (g_deko3dOptions.vsOutputs && stages[1] &&
            std::find(stages[1]->specIds.begin(), stages[1]->specIds.end(), 905u) != stages[1]->specIds.end()) {
            const uint32_t outs = stages[1]->usage.outputMask;
            const uint32_t mask = (stages[2] ? stages[2]->usage.inputMask : 0u) & outs;
            if (mask != outs) {
                gsSpec.assign(desc.specConstants, desc.specConstants + desc.specConstantsCount);
                gsSpec.push_back(RenderSpecConstant(905, mask));
            }
        }
        for (int i = 0; i < 3; i++) {
            if (!stages[i])
                continue;
            const bool own = i == 0 && !vsSpec.empty();
            const bool ownGs = i == 1 && !gsSpec.empty();
            const bool ownPs = i == 2 && !psSpec.empty();
            const DkShader *s = loadVariant(stages[i], own ? vsSpec.data() : ownGs ? gsSpec.data()
                                                       : ownPs ? psSpec.data() : desc.specConstants,
                                            own ? uint32_t(vsSpec.size()) : ownGs ? uint32_t(gsSpec.size())
                                            : ownPs ? uint32_t(psSpec.size()) : desc.specConstantsCount);
            if (!s) {
                ok = false;
                continue;
            }
            p->shaders[p->shaderCount++] = s;
            p->stageShaders[i] = s;  // tw2de (2026-10-19)
            p->stageMask |= 1u << dkStages[i];
            p->uboMask[i] = stages[i]->usage.uboMask;
            p->ssboMask[i] = stages[i]->usage.ssboMask;
            p->texBindings[i] = stages[i]->usage.texBindings;
            p->txqMask[i] = stages[i]->usage.txqMask;
            p->boundTextures = p->boundTextures || !p->texBindings[i].empty();
            p->textures = p->textures || stages[i]->usage.textures;
        }
        p->valid = ok;
        p->primitive = toDk(desc.primitiveTopology);

        dkRasterizerStateDefaults(&p->rasterizer);
        p->rasterizer.cullMode = desc.cullMode == RenderCullMode::FRONT ? DkFace_Front : desc.cullMode == RenderCullMode::BACK ? DkFace_Back : DkFace_None;
        p->rasterizer.frontFace = desc.frontFace == RenderFrontFace::COUNTER_CLOCKWISE ? DkFrontFace_CCW : DkFrontFace_CW;
        // D3D's (and Vulkan's) provoking vertex is the first one.
        p->rasterizer.provokingVertex = DkProvokingVertex_First;
        p->rasterizer.depthClampEnable = desc.depthClipEnabled ? 0 : 1;
        p->depthBias = float(desc.depthBias);
        p->depthBiasClamp = desc.depthBiasClamp;
        p->slopeScaledDepthBias = desc.slopeScaledDepthBias;
        p->dynamicDepthBias = desc.dynamicDepthBiasEnabled;
        if (desc.dynamicDepthBiasEnabled || desc.depthBias != 0 || desc.slopeScaledDepthBias != 0.0f)
            p->rasterizer.depthBiasEnableMask = DkPolygonFlag_All;

        dkMultisampleStateDefaults(&p->multisample);
        p->multisample.mode = toDkMsMode(desc.multisampling.sampleCount);
        p->multisample.rasterizerMode = p->multisample.mode;
        p->multisample.alphaToCoverageEnable = desc.alphaToCoverageEnabled;
        p->multisample.alphaToCoverageDither = g_deko3dOptions.alphaToCoverageDither;  // tw2de (2026-10-22)
        p->sampleMask = desc.sampleMask;  // tw2de

        dkColorStateDefaults(&p->color);
        if (desc.logicOpEnabled)
            p->color.logicOp = toDk(desc.logicOp);
        dkColorWriteStateDefaults(&p->colorWrite);
        p->blendCount = std::min<uint32_t>(desc.renderTargetCount, 8);
        for (uint32_t i = 0; i < p->blendCount; i++) {
            const RenderBlendDesc &b = desc.renderTargetBlend[i];
            dkColorStateSetBlendEnable(&p->color, i, b.blendEnabled);
            dkColorWriteStateSetMask(&p->colorWrite, i, b.renderTargetWriteMask & 0xF);
            DkBlendState &s = p->blend[i];
            dkBlendStateDefaults(&s);
            if (b.blendEnabled) {
                s.colorBlendOp = toDk(b.blendOp);
                s.srcColorBlendFactor = toDk(b.srcBlend);
                s.dstColorBlendFactor = toDk(b.dstBlend);
                s.alphaBlendOp = toDk(b.blendOpAlpha);
                s.srcAlphaBlendFactor = toDkAlpha(b.srcBlendAlpha);
                s.dstAlphaBlendFactor = toDkAlpha(b.dstBlendAlpha);
                for (RenderBlend f : {b.srcBlend, b.dstBlend, b.srcBlendAlpha, b.dstBlendAlpha})
                    p->usesBlendConstant = p->usesBlendConstant || f == RenderBlend::BLEND_FACTOR || f == RenderBlend::INV_BLEND_FACTOR;
            }
            blendWords(p->blendWords[i], i, s);
        }
        for (uint32_t i = p->blendCount; i < 8; i++)
            dkColorWriteStateSetMask(&p->colorWrite, i, 0);

        dkDepthStencilStateDefaults(&p->depthStencil);
        p->depthStencil.depthTestEnable = desc.depthEnabled;
        p->depthStencil.depthWriteEnable = desc.depthWriteEnabled;
        p->depthStencil.depthCompareOp = toDk(desc.depthFunction);
        p->depthStencil.stencilTestEnable = desc.stencilEnabled;
        p->depthStencil.stencilFrontFailOp = toDk(desc.stencilFrontFace.failOp);
        p->depthStencil.stencilFrontPassOp = toDk(desc.stencilFrontFace.passOp);
        p->depthStencil.stencilFrontDepthFailOp = toDk(desc.stencilFrontFace.depthFailOp);
        p->depthStencil.stencilFrontCompareOp = toDk(desc.stencilFrontFace.compareFunction);
        p->depthStencil.stencilBackFailOp = toDk(desc.stencilBackFace.failOp);
        p->depthStencil.stencilBackPassOp = toDk(desc.stencilBackFace.passOp);
        p->depthStencil.stencilBackDepthFailOp = toDk(desc.stencilBackFace.depthFailOp);
        p->depthStencil.stencilBackCompareOp = toDk(desc.stencilBackFace.compareFunction);
        p->stencilEnabled = desc.stencilEnabled;
        p->stencilReadMask = uint8_t(desc.stencilReadMask);
        p->stencilWriteMask = uint8_t(desc.stencilWriteMask);
        p->stencilReference = uint8_t(desc.stencilReference);
        p->dynamicStencilReference = desc.dynamicStencilReferenceEnabled;

        // Vertex input: the vertex shader's attribute i reads the element at its original location vin[i]
        // (tools/deko3d_glsl.py renumbered them to fit uam's 16).
        const std::vector<uint32_t> &vin = stages[0] ? stages[0]->usage.vertexInputs : std::vector<uint32_t>();
        p->attribCount = uint32_t(vin.size());
        for (uint32_t a = 0; a < p->attribCount; a++) {
            DkVtxAttribState &st = p->attribs[a];
            memset(&st, 0, sizeof(st));
            st.isFixed = 1; // a constant zero unless an element feeds it
            st.size = DkVtxAttribSize_1x32;
            st.type = DkVtxAttribType_Float;
            for (uint32_t e = 0; e < desc.inputElementsCount; e++) {
                const RenderInputElement &el = desc.inputElements[e];
                if (el.location != vin[a])
                    continue;
                DkVtxAttribSize size;
                DkVtxAttribType type;
                bool bgra;
                if (!vertexFormat(el.format, size, type, bgra)) {
                    logf("vertex element format %d has no deko3d form", int(el.format));
                    break;
                }
                st.isFixed = 0;
                st.bufferId = el.slotIndex;
                st.offset = el.alignedByteOffset;
                st.size = size;
                st.type = type;
                st.isBgra = bgra ? 1 : 0;
                break;
            }
        }
        for (uint32_t s = 0; s < desc.inputSlotsCount; s++) {
            const RenderInputSlot &slot = desc.inputSlots[s];
            if (slot.index >= 16)
                continue;
            p->vtxBuffers[slot.index].stride = slot.stride;
            p->vtxBuffers[slot.index].divisor = slot.classification == RenderInputSlotClassification::PER_INSTANCE_DATA ? 1 : 0;
            p->vtxBufferCount = std::max(p->vtxBufferCount, slot.index + 1);
        }

        uint64_t h = 0xcbf29ce484222325ull;
        h = mixHash(h, &p->primitive, sizeof(p->primitive));
        h = mixHash(h, &p->rasterizer, sizeof(p->rasterizer));
        h = mixHash(h, &p->multisample, sizeof(p->multisample));
        // tw2de (2026-10-22): and the sample mask. Two pipelines that differed only in it (the game's per-sample depth
        // restores: sample 0, then sample 1) had one state hash, so the second skipped setPipeline's state binds and
        // drew with the first's mask: sample 1's depth was never restored, and the effects after it were depth-tested
        // per sample against different depths (a grid over them with 2x MSAA).
        h = mixHash(h, &p->sampleMask, sizeof(p->sampleMask));
        h = mixHash(h, &p->color, sizeof(p->color));
        h = mixHash(h, &p->colorWrite, sizeof(p->colorWrite));
        h = mixHash(h, p->blendWords, sizeof(p->blendWords));
        h = mixHash(h, &p->depthStencil, sizeof(p->depthStencil));
        h = mixHash(h, &p->stencilReadMask, 3);
        h = mixHash(h, &p->depthBias, sizeof(float) * 3);
        h = mixHash(h, p->attribs, sizeof(DkVtxAttribState) * p->attribCount);
        h = mixHash(h, &p->attribCount, sizeof(p->attribCount));
        h = mixHash(h, p->vtxBuffers, sizeof(DkVtxBufferState) * p->vtxBufferCount);
        h = mixHash(h, &p->vtxBufferCount, sizeof(p->vtxBufferCount));
        p->stateHash = h | 1;
        // tw2de (2026-10-19, CPU proposal 13): each group of setPipeline's state binds, hashed apart (never 0).
        auto group = [](std::initializer_list<std::pair<const void *, size_t>> parts) {
            uint64_t g = 0xcbf29ce484222325ull;
            for (const auto &[data, size] : parts)
                g = mixHash(g, data, size);
            return g | 1;
        };
        p->groupHash[0] = group({{&p->rasterizer, sizeof(p->rasterizer)}});
        p->groupHash[1] = group({{&p->multisample, sizeof(p->multisample)}, {&p->sampleMask, sizeof(p->sampleMask)}});  // tw2de: and the mask
        p->groupHash[2] = group({{&p->color, sizeof(p->color)}});
        p->groupHash[3] = group({{&p->colorWrite, sizeof(p->colorWrite)}});
        p->groupHash[4] = group({{&p->blendCount, sizeof(p->blendCount)},
                                 {p->blend, sizeof(DkBlendState) * p->blendCount},
                                 {p->blendWords, sizeof(p->blendWords[0]) * p->blendCount}});
        p->groupHash[5] = group({{&p->depthStencil, sizeof(p->depthStencil)}});
        p->groupHash[6] = group({{&p->stencilReadMask, 1}, {&p->stencilWriteMask, 1}, {&p->stencilReference, 1}});
        p->groupHash[7] = group({{&p->depthBias, sizeof(float)}, {&p->depthBiasClamp, sizeof(float)},
                                 {&p->slopeScaledDepthBias, sizeof(float)}});
        p->groupHash[8] = group({{&p->attribCount, sizeof(p->attribCount)},
                                 {p->attribs, sizeof(DkVtxAttribState) * p->attribCount},
                                 {&p->vtxBufferCount, sizeof(p->vtxBufferCount)},
                                 {p->vtxBuffers, sizeof(DkVtxBufferState) * p->vtxBufferCount}});
        return p;
    }

    std::unique_ptr<RenderPipeline> Deko3DDevice::createComputePipeline(const RenderComputePipelineDesc &desc) {
        const Deko3DTimed timed(GetDeko3DStats().pipelineCreateNs);  // tw2de (2026-10-28)
        auto p = std::make_unique<Deko3DComputePipeline>();
        p->type = Deko3DPipeline::Type::Compute;
        p->device = this;
        p->layout = static_cast<const Deko3DPipelineLayout *>(desc.pipelineLayout);
        const Deko3DShader *shader = static_cast<const Deko3DShader *>(desc.computeShader);
        if (shader) {
            p->shader = loadVariant(shader, desc.specConstants, desc.specConstantsCount);
            p->uboMask = shader->usage.uboMask;
            p->ssboMask = shader->usage.ssboMask;
        }
        p->valid = p->shader != nullptr;
        return p;
    }

    std::unique_ptr<RenderPipeline> Deko3DDevice::createRaytracingPipeline(const RenderRaytracingPipelineDesc &, const RenderPipeline *) { return nullptr; }

    // ---- the rest of the device

    std::unique_ptr<RenderDescriptorSet> Deko3DDevice::createDescriptorSet(const RenderDescriptorSetDesc &desc) {
        return std::make_unique<Deko3DDescriptorSet>(this, desc);
    }

    std::unique_ptr<RenderCommandQueue> Deko3DDevice::createCommandQueue(RenderCommandListType type) {
        return std::make_unique<Deko3DCommandQueue>(this, type);
    }

    std::unique_ptr<RenderBuffer> Deko3DDevice::createBuffer(const RenderBufferDesc &desc) {
        const Deko3DTimed timed(GetDeko3DStats().bufferCreateNs);  // tw2de (2026-10-28)
        auto buffer = std::make_unique<Deko3DBuffer>(this, desc);
        if (!buffer->block)
            return nullptr;
        return buffer;
    }

    std::unique_ptr<RenderTexture> Deko3DDevice::createTexture(const RenderTextureDesc &desc) {
        const Deko3DTimed timed(GetDeko3DStats().textureCreateNs);  // tw2de (2026-10-28)
        return std::make_unique<Deko3DTexture>(this, desc);
    }

    std::unique_ptr<RenderAccelerationStructure> Deko3DDevice::createAccelerationStructure(const RenderAccelerationStructureDesc &) { return nullptr; }

    std::unique_ptr<RenderPool> Deko3DDevice::createPool(const RenderPoolDesc &) { return nullptr; }

    std::unique_ptr<RenderCommandFence> Deko3DDevice::createCommandFence() { return std::make_unique<Deko3DCommandFence>(); }

    std::unique_ptr<RenderCommandSemaphore> Deko3DDevice::createCommandSemaphore() { return std::make_unique<Deko3DCommandSemaphore>(); }

    std::unique_ptr<RenderFramebuffer> Deko3DDevice::createFramebuffer(const RenderFramebufferDesc &desc) {
        auto fb = std::make_unique<Deko3DFramebuffer>();
        fb->colorCount = std::min<uint32_t>(desc.colorAttachmentsCount, 8);
        for (uint32_t i = 0; i < fb->colorCount; i++) {
            const Deko3DTexture *t = static_cast<const Deko3DTexture *>(desc.colorAttachments[i]);
            const Deko3DTextureView *v = desc.colorAttachmentViews ? static_cast<const Deko3DTextureView *>(desc.colorAttachmentViews[i]) : nullptr;
            fb->colors[i] = v ? v->view : t->defaultView();
            fb->srgb[i] = isSrgbFormat(fb->colors[i].format != DkImageFormat_None ? fb->colors[i].format : t->format);
            if (i == 0) {
                // The view's level (mip chains rendered level by level).
                fb->width = std::max<uint32_t>(t->desc.width >> fb->colors[i].mipLevelOffset, 1);
                fb->height = std::max<uint32_t>(t->desc.height >> fb->colors[i].mipLevelOffset, 1);
            }
        }
        if (desc.depthAttachment) {
            const Deko3DTexture *t = static_cast<const Deko3DTexture *>(desc.depthAttachment);
            const Deko3DTextureView *v = static_cast<const Deko3DTextureView *>(desc.depthAttachmentView);
            fb->depth = v ? v->view : t->defaultView();
            fb->hasDepth = true;
            if (fb->colorCount == 0) {
                fb->width = t->desc.width;
                fb->height = t->desc.height;
            }
        }
        return fb;
    }

    std::unique_ptr<RenderQueryPool> Deko3DDevice::createQueryPool(uint32_t queryCount) {
        return std::make_unique<Deko3DQueryPool>(this, queryCount);
    }

    void Deko3DDevice::setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &, const RenderBottomLevelASMesh *, uint32_t, bool, bool) {}
    void Deko3DDevice::setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &, const RenderTopLevelASInstance *, uint32_t, bool, bool) {}
    void Deko3DDevice::setShaderBindingTableInfo(RenderShaderBindingTableInfo &, const RenderShaderBindingGroups &, const RenderPipeline *, RenderDescriptorSet **, uint32_t) {}

    RenderSampleCounts Deko3DDevice::getSampleCountsSupported(RenderFormat) const {
        return RenderSampleCount::COUNT_1 | RenderSampleCount::COUNT_2 | RenderSampleCount::COUNT_4 | RenderSampleCount::COUNT_8;
    }

    // ---- Deko3DInterface

    Deko3DInterface::Deko3DInterface() {
        capabilities.shaderFormat = RenderShaderFormat::DEKO3D;
        deviceNames.push_back("deko3d");
    }

    std::unique_ptr<RenderDevice> Deko3DInterface::createDevice(const std::string &) {
        auto device = std::make_unique<Deko3DDevice>();
        if (!device->initialize())
            return nullptr;
        return device;
    }

    std::unique_ptr<RenderInterface> CreateDeko3DInterface() {
        return std::make_unique<Deko3DInterface>();
    }

    Deko3DFenceCopy Deko3DCopyFence(const RenderCommandFence *fence) {
        Deko3DFenceCopy copy;
        if (const auto *f = static_cast<const Deko3DCommandFence *>(fence)) {
            if (f->pending) {
                copy.fence = f->fence;
                copy.valid = true;
            } else {
                copy.done = true;
            }
        }
        return copy;
    }

    bool Deko3DFenceCopyDone(Deko3DFenceCopy &copy) {
        if (!copy.done && copy.valid && dkFenceWait(&copy.fence, 0) == DkResult_Success)
            copy.done = true;  // signalled once is signalled for good: no second wait
        return copy.done;
    }

    bool Deko3DFenceCopyWait(Deko3DFenceCopy &copy, RenderDevice *device) {
        if (copy.done || !copy.valid)
            return copy.done;
        Deko3DDevice *d = static_cast<Deko3DDevice *>(device);
        for (;;) {
            if (dkFenceWait(&copy.fence, 2'000'000'000ll) == DkResult_Success) {
                copy.done = true;
                return true;
            }
            if (!d || !d->queue || dkQueueIsInErrorState(d->queue))
                return false;
        }
    }

    Deko3DFenceCopy Deko3DSignalNow(RenderDevice *device) {
        Deko3DFenceCopy copy;
        Deko3DDevice *d = static_cast<Deko3DDevice *>(device);
        if (!d || !d->queue || dkQueueIsInErrorState(d->queue))
            return copy;
        dkQueueSignalFence(d->queue, &copy.fence, true);
        copy.valid = true;
        return copy;
    }

    bool Deko3DDeviceLost(RenderDevice *device) {
        Deko3DDevice *d = static_cast<Deko3DDevice *>(device);
        return d && d->queue && dkQueueIsInErrorState(d->queue);
    }
};
