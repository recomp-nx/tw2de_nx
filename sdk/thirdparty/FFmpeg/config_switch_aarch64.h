/* FFmpeg configuration for Nintendo Switch (Horizon, aarch64, devkitA64).
 *
 * The Xenia FFmpeg fork ships one config header per desktop/mobile OS and none
 * for Horizon. Linux/aarch64 is the closest: same CPU features (NEON, ARMv8,
 * inline assembly) and pthreads, which libnx provides. Only what Horizon lacks
 * is turned off. Verified by compiling every libavutil and libavcodec source
 * the SDK builds (NEON .S files included) with devkitA64. */
#pragma once
#include "config_linux_aarch64.h"

#undef HAVE_SCHED_GETAFFINITY
#define HAVE_SCHED_GETAFFINITY 0
#undef HAVE_MMAP
#define HAVE_MMAP 0
#undef HAVE_SYSCONF
#define HAVE_SYSCONF 0

/* TW2 (2026-11-12): the game's USM movies (Sofdec: MPEG-1 video, ADX audio) decoded natively
 * (app/src/cpu/tw2de_usm_ffmpeg.cpp). The MPEG-1/2 decoders and what configure selects for them
 * (mpegvideo_select: blockdsp h264chroma hpeldsp idctdsp me_cmp mpeg_er videodsp; me_cmp_select:
 * fdctdsp idctdsp pixblockdsp; mpeg_er_select: error_resilience), and the ADX decoder. */
#undef CONFIG_MPEG1VIDEO_DECODER
#define CONFIG_MPEG1VIDEO_DECODER 1
#undef CONFIG_MPEG2VIDEO_DECODER
#define CONFIG_MPEG2VIDEO_DECODER 1
#undef CONFIG_ADPCM_ADX_DECODER
#define CONFIG_ADPCM_ADX_DECODER 1
#undef CONFIG_MPEGVIDEO
#define CONFIG_MPEGVIDEO 1
#undef CONFIG_BLOCKDSP
#define CONFIG_BLOCKDSP 1
#undef CONFIG_H264CHROMA
#define CONFIG_H264CHROMA 1
#undef CONFIG_HPELDSP
#define CONFIG_HPELDSP 1
#undef CONFIG_IDCTDSP
#define CONFIG_IDCTDSP 1
#undef CONFIG_ME_CMP
#define CONFIG_ME_CMP 1
#undef CONFIG_MPEG_ER
#define CONFIG_MPEG_ER 1
#undef CONFIG_ERROR_RESILIENCE
#define CONFIG_ERROR_RESILIENCE 1
#undef CONFIG_VIDEODSP
#define CONFIG_VIDEODSP 1
#undef CONFIG_FDCTDSP
#define CONFIG_FDCTDSP 1
#undef CONFIG_PIXBLOCKDSP
#define CONFIG_PIXBLOCKDSP 1
