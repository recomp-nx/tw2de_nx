# Third-party vendored dependencies for ReXGlue
# tw2de (2026-10-29): only what the Switch build and the code generator use (no SDL3, Vulkan, SPIR-V tools,
# glslang, DXC, Tracy, Catch2, MoltenVK, ImGui or RenderDoc: their sources are gone with the PC build).

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

#=============================================================================
# Verify git submodules are initialized (only vendored ones)
#=============================================================================
set(REQUIRED_SUBMODULES
    cli11
    libmspack
    FFmpeg
    tomlplusplus
    simde
    xxHash
    spdlog
    fmt
    utfcpp
)

foreach(submodule ${REQUIRED_SUBMODULES})
    # Fork change: the third-party sources are fetched by tools/fetch_thirdparty.py instead of
    # being git submodules, so check for a populated directory rather than a .git entry.
    file(GLOB _submodule_files "${CMAKE_CURRENT_SOURCE_DIR}/${submodule}/*")
    if(NOT _submodule_files)
        message(FATAL_ERROR
            "Third-party source '${submodule}' is missing.\n"
            "Please run: python tools/fetch_thirdparty.py\n"
            "from the repository root directory."
        )
    endif()
endforeach()

#=============================================================================
# Header-only libraries (vendored, not in vcpkg)
#=============================================================================

# tomlplusplus - TOML config file parser (header-only)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# simde - SIMD Everywhere (header-only, git submodule)
add_library(simde INTERFACE)
target_include_directories(simde INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/simde>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/simde/simde>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

#=============================================================================
# Compiled libraries (vendored sources)
#=============================================================================

# tiny-AES-c - AES encryption (used for XEX decryption)
add_library(tiny-aes STATIC
    tiny-aes-c/aes.c
)
target_include_directories(tiny-aes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tiny-aes-c)
target_compile_definitions(tiny-aes PUBLIC AES128=1 CBC=1)

# aes_128 - AES-128 encryption/decryption (used by kernel crypt routines)
# From: https://github.com/openluopworld/aes_128
add_library(aes128 STATIC
    aes_128/aes.c
)
target_include_directories(aes128 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Suppress warnings in third-party C code
target_compile_options(aes128 PRIVATE -w)

# libmspack - LZX decompression (only lzxd.c needed for XEX decompression)
# Uses cabextract version which has all needed headers in one place
set(MSPACK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libmspack/cabextract/mspack")
add_library(mspack STATIC
    ${MSPACK_DIR}/lzxd.c
)
target_include_directories(mspack PUBLIC
    $<BUILD_INTERFACE:${MSPACK_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Suppress warnings in third-party C code
target_compile_options(mspack PRIVATE -w)

# o1heap - O(1) heap allocator for real-time systems (used by rexcrt heap)
# NOTE: o1heap.c auto-detects CLZ via __builtin_clzl when __clang__/__GNUC__
# are defined, but on Windows unsigned long is 32-bit even on x64.  We inject
# a config header that provides a correct 64-bit CLZ
add_library(o1heap STATIC
    o1heap/o1heap/o1heap.c
)
target_include_directories(o1heap PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/o1heap/o1heap>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(o1heap PRIVATE
    "O1HEAP_CONFIG_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/../include/rex/o1heap_config.h\"")
target_compile_options(o1heap PRIVATE -w)

# xxHash - extremely fast non-cryptographic hash (git submodule)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
add_subdirectory(xxHash/cmake_unofficial)
target_compile_options(xxhash PRIVATE -w)

# disasm - PowerPC disassembler (existing local library)
add_subdirectory(disasm)
# Suppress warnings in disasm
target_compile_options(disasm PRIVATE -w)

#=============================================================================
# fmt - Formatting library (must come before spdlog)
#=============================================================================
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(fmt)
target_compile_options(fmt PRIVATE -w)

#=============================================================================
# spdlog - Logging library
#=============================================================================
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(spdlog)
target_compile_options(spdlog PRIVATE -w)

#=============================================================================
# utfcpp - UTF-8 string library (header-only)
#=============================================================================
add_subdirectory(utfcpp)

# tw2de (2026-10-23): no FFmpeg in the code generator alone (REXGLUE_CODEGEN_ONLY): the rest of this file.
if(REXGLUE_CODEGEN_ONLY)
    return()
endif()

# =============================================================================
# FFmpeg - Audio/video codec library (for XMA decoding)
# Only builds libavutil and libavcodec needed for WMA Pro decoding
# Fork addition: also the WMV3 decoder (for games with WMV cutscenes) (config_masseffect_wmv3.h)
# =============================================================================

set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg")

# Overlay generated FFmpeg list files
set(FFMPEG_OVERLAY_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-overlay")
set(FFMPEG_OVERLAY_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-overlay")
file(MAKE_DIRECTORY "${FFMPEG_OVERLAY_BUILD_DIR}/libavcodec")
configure_file(
  "${FFMPEG_OVERLAY_SRC_DIR}/codec_list.c" "${FFMPEG_OVERLAY_BUILD_DIR}/libavcodec/codec_list.c"
  COPYONLY
)
configure_file(
  "${FFMPEG_OVERLAY_SRC_DIR}/parser_list.c" "${FFMPEG_OVERLAY_BUILD_DIR}/libavcodec/parser_list.c"
  COPYONLY
)
configure_file(
  "${FFMPEG_OVERLAY_SRC_DIR}/bsf_list.c" "${FFMPEG_OVERLAY_BUILD_DIR}/libavcodec/bsf_list.c"
  COPYONLY
)

# Common defines for both libraries
set(FFMPEG_COMMON_DEFINES
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES  # For M_PI/etc
)

# Include path for atomics (GCC's)
set(FFMPEG_ATOMICS_INCLUDE "${FFMPEG_DIR}/compat/atomics/gcc")

#=============================================================================
# ARM64 setup for FFmpeg (NEON optimizations). tw2de (2026-10-29): FFmpeg is built for the Switch only (the code
# generator returned above), so always aarch64.
#=============================================================================
enable_language(ASM)
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -march=armv8-a")

# libavutil - FFmpeg utility library (must build first)
add_library(libavutil STATIC
    ${FFMPEG_DIR}/libavutil/adler32.c
    ${FFMPEG_DIR}/libavutil/aes.c
    ${FFMPEG_DIR}/libavutil/aes_ctr.c
    ${FFMPEG_DIR}/libavutil/audio_fifo.c
    ${FFMPEG_DIR}/libavutil/avstring.c
    ${FFMPEG_DIR}/libavutil/avsscanf.c
    ${FFMPEG_DIR}/libavutil/base64.c
    ${FFMPEG_DIR}/libavutil/blowfish.c
    ${FFMPEG_DIR}/libavutil/bprint.c
    ${FFMPEG_DIR}/libavutil/buffer.c
    ${FFMPEG_DIR}/libavutil/cast5.c
    ${FFMPEG_DIR}/libavutil/camellia.c
    ${FFMPEG_DIR}/libavutil/channel_layout.c
    ${FFMPEG_DIR}/libavutil/color_utils.c
    ${FFMPEG_DIR}/libavutil/cpu.c
    ${FFMPEG_DIR}/libavutil/crc.c
    ${FFMPEG_DIR}/libavutil/des.c
    ${FFMPEG_DIR}/libavutil/dict.c
    ${FFMPEG_DIR}/libavutil/display.c
    ${FFMPEG_DIR}/libavutil/dovi_meta.c
    ${FFMPEG_DIR}/libavutil/downmix_info.c
    ${FFMPEG_DIR}/libavutil/encryption_info.c
    ${FFMPEG_DIR}/libavutil/error.c
    ${FFMPEG_DIR}/libavutil/eval.c
    ${FFMPEG_DIR}/libavutil/fifo.c
    ${FFMPEG_DIR}/libavutil/file.c
    ${FFMPEG_DIR}/libavutil/file_open.c
    ${FFMPEG_DIR}/libavutil/float_dsp.c
    ${FFMPEG_DIR}/libavutil/fixed_dsp.c
    ${FFMPEG_DIR}/libavutil/frame.c
    ${FFMPEG_DIR}/libavutil/hash.c
    ${FFMPEG_DIR}/libavutil/hdr_dynamic_metadata.c
    ${FFMPEG_DIR}/libavutil/hmac.c
    ${FFMPEG_DIR}/libavutil/hwcontext.c
    ${FFMPEG_DIR}/libavutil/imgutils.c
    ${FFMPEG_DIR}/libavutil/integer.c
    ${FFMPEG_DIR}/libavutil/intmath.c
    ${FFMPEG_DIR}/libavutil/lfg.c
    ${FFMPEG_DIR}/libavutil/lls.c
    ${FFMPEG_DIR}/libavutil/log.c
    ${FFMPEG_DIR}/libavutil/log2_tab.c
    ${FFMPEG_DIR}/libavutil/mathematics.c
    ${FFMPEG_DIR}/libavutil/mastering_display_metadata.c
    ${FFMPEG_DIR}/libavutil/md5.c
    ${FFMPEG_DIR}/libavutil/mem.c
    ${FFMPEG_DIR}/libavutil/murmur3.c
    ${FFMPEG_DIR}/libavutil/opt.c
    ${FFMPEG_DIR}/libavutil/parseutils.c
    ${FFMPEG_DIR}/libavutil/pixdesc.c
    ${FFMPEG_DIR}/libavutil/pixelutils.c
    ${FFMPEG_DIR}/libavutil/random_seed.c
    ${FFMPEG_DIR}/libavutil/rational.c
    ${FFMPEG_DIR}/libavutil/reverse.c
    ${FFMPEG_DIR}/libavutil/rc4.c
    ${FFMPEG_DIR}/libavutil/ripemd.c
    ${FFMPEG_DIR}/libavutil/samplefmt.c
    ${FFMPEG_DIR}/libavutil/sha.c
    ${FFMPEG_DIR}/libavutil/sha512.c
    ${FFMPEG_DIR}/libavutil/slicethread.c
    ${FFMPEG_DIR}/libavutil/spherical.c
    ${FFMPEG_DIR}/libavutil/stereo3d.c
    ${FFMPEG_DIR}/libavutil/threadmessage.c
    ${FFMPEG_DIR}/libavutil/time.c
    ${FFMPEG_DIR}/libavutil/timecode.c
    ${FFMPEG_DIR}/libavutil/tree.c
    ${FFMPEG_DIR}/libavutil/twofish.c
    ${FFMPEG_DIR}/libavutil/utils.c
    ${FFMPEG_DIR}/libavutil/xga_font_data.c
    ${FFMPEG_DIR}/libavutil/xtea.c
    ${FFMPEG_DIR}/libavutil/tea.c
    ${FFMPEG_DIR}/libavutil/tx.c
    ${FFMPEG_DIR}/libavutil/tx_float.c
    ${FFMPEG_DIR}/libavutil/tx_double.c
    ${FFMPEG_DIR}/libavutil/tx_int32.c
    ${FFMPEG_DIR}/libavutil/video_enc_params.c
    ${FFMPEG_DIR}/libavutil/film_grain_params.c
)


# ARM64 optimizations for libavutil
target_sources(libavutil PRIVATE
    ${FFMPEG_DIR}/libavutil/aarch64/cpu.c
    ${FFMPEG_DIR}/libavutil/aarch64/float_dsp_init.c
    ${FFMPEG_DIR}/libavutil/aarch64/float_dsp_neon.S
)
target_compile_definitions(libavutil PRIVATE
    HAVE_NEON=1
    HAVE_ARMV8=1
    HAVE_INLINE_ASM=1
)

target_include_directories(libavutil
    PUBLIC
        $<BUILD_INTERFACE:${FFMPEG_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${FFMPEG_ATOMICS_INCLUDE}
)
target_compile_definitions(libavutil PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(libavutil PRIVATE -w)  # Suppress warnings in third-party code

# libavcodec - FFmpeg codec library (depends on libavutil)
add_library(libavcodec STATIC
    ${FFMPEG_DIR}/libavcodec/ac3_parser.c
    ${FFMPEG_DIR}/libavcodec/adts_parser.c
    ${FFMPEG_DIR}/libavcodec/allcodecs.c
    ${FFMPEG_DIR}/libavcodec/avcodec.c
    ${FFMPEG_DIR}/libavcodec/avdct.c
    ${FFMPEG_DIR}/libavcodec/avpacket.c
    ${FFMPEG_DIR}/libavcodec/avpicture.c
    ${FFMPEG_DIR}/libavcodec/bitstream.c
    ${FFMPEG_DIR}/libavcodec/bitstream_filter.c
    ${FFMPEG_DIR}/libavcodec/bitstream_filters.c
    ${FFMPEG_DIR}/libavcodec/bsf.c
    ${FFMPEG_DIR}/libavcodec/codec_desc.c
    ${FFMPEG_DIR}/libavcodec/codec_par.c
    ${FFMPEG_DIR}/libavcodec/d3d11va.c
    ${FFMPEG_DIR}/libavcodec/decode.c
    ${FFMPEG_DIR}/libavcodec/dirac.c
    ${FFMPEG_DIR}/libavcodec/dv_profile.c
    ${FFMPEG_DIR}/libavcodec/encode.c
    ${FFMPEG_DIR}/libavcodec/imgconvert.c
    ${FFMPEG_DIR}/libavcodec/jni.c
    ${FFMPEG_DIR}/libavcodec/mathtables.c
    ${FFMPEG_DIR}/libavcodec/mediacodec.c
    ${FFMPEG_DIR}/libavcodec/mpeg12framerate.c
    ${FFMPEG_DIR}/libavcodec/options.c
    ${FFMPEG_DIR}/libavcodec/parser.c
    ${FFMPEG_DIR}/libavcodec/parsers.c
    ${FFMPEG_DIR}/libavcodec/profiles.c
    ${FFMPEG_DIR}/libavcodec/qsv_api.c
    ${FFMPEG_DIR}/libavcodec/raw.c
    ${FFMPEG_DIR}/libavcodec/utils.c
    ${FFMPEG_DIR}/libavcodec/vorbis_parser.c
    ${FFMPEG_DIR}/libavcodec/xiph.c
    ${FFMPEG_DIR}/libavcodec/faandct.c
    ${FFMPEG_DIR}/libavcodec/faanidct.c
    ${FFMPEG_DIR}/libavcodec/fdctdsp.c
    ${FFMPEG_DIR}/libavcodec/jfdctfst.c
    ${FFMPEG_DIR}/libavcodec/jfdctint.c
    ${FFMPEG_DIR}/libavcodec/idctdsp.c
    ${FFMPEG_DIR}/libavcodec/simple_idct.c
    ${FFMPEG_DIR}/libavcodec/jrevdct.c
    ${FFMPEG_DIR}/libavcodec/mdct_float.c
    ${FFMPEG_DIR}/libavcodec/mdct_fixed_32.c
    ${FFMPEG_DIR}/libavcodec/sinewin.c
    ${FFMPEG_DIR}/libavcodec/wma_freqs.c
    ${FFMPEG_DIR}/libavcodec/wmaprodec.c
    ${FFMPEG_DIR}/libavcodec/wmadec.c
    ${FFMPEG_DIR}/libavcodec/wma.c
    ${FFMPEG_DIR}/libavcodec/wma_common.c
    ${FFMPEG_DIR}/libavcodec/mpegaudio.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodata.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodecheader.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodec_common.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodec_float.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodec_fixed.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp_data.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp_float.c
    ${FFMPEG_DIR}/libavcodec/mpegaudiodsp_fixed.c
    ${FFMPEG_DIR}/libavcodec/mpegaudio_parser.c
    ${FFMPEG_DIR}/libavcodec/dct.c
    ${FFMPEG_DIR}/libavcodec/dct32_float.c
    ${FFMPEG_DIR}/libavcodec/dct32_fixed.c
    ${FFMPEG_DIR}/libavcodec/rdft.c
    ${FFMPEG_DIR}/libavcodec/aactab.c
    ${FFMPEG_DIR}/libavcodec/null_bsf.c
    ${FFMPEG_DIR}/libavcodec/pthread.c
    ${FFMPEG_DIR}/libavcodec/pthread_slice.c
    ${FFMPEG_DIR}/libavcodec/pthread_frame.c
    ${FFMPEG_DIR}/libavcodec/avfft.c
    ${FFMPEG_DIR}/libavcodec/fft_float.c
    ${FFMPEG_DIR}/libavcodec/fft_fixed_32.c
    ${FFMPEG_DIR}/libavcodec/fft_init_table.c
    # Fork addition: WMV3 (VC-1 main profile) and what it selects
    ${FFMPEG_DIR}/libavcodec/vc1dec.c
    ${FFMPEG_DIR}/libavcodec/vc1_block.c
    ${FFMPEG_DIR}/libavcodec/vc1_loopfilter.c
    ${FFMPEG_DIR}/libavcodec/vc1_mc.c
    ${FFMPEG_DIR}/libavcodec/vc1_pred.c
    ${FFMPEG_DIR}/libavcodec/vc1.c
    ${FFMPEG_DIR}/libavcodec/vc1data.c
    ${FFMPEG_DIR}/libavcodec/vc1dsp.c
    ${FFMPEG_DIR}/libavcodec/msmpeg4dec.c
    ${FFMPEG_DIR}/libavcodec/msmpeg4.c
    ${FFMPEG_DIR}/libavcodec/msmpeg4data.c
    ${FFMPEG_DIR}/libavcodec/wmv2dsp.c
    ${FFMPEG_DIR}/libavcodec/wmv2data.c
    ${FFMPEG_DIR}/libavcodec/h263dec.c
    ${FFMPEG_DIR}/libavcodec/h263.c
    ${FFMPEG_DIR}/libavcodec/ituh263dec.c
    ${FFMPEG_DIR}/libavcodec/mpeg4video.c
    ${FFMPEG_DIR}/libavcodec/mpeg4videodec.c
    ${FFMPEG_DIR}/libavcodec/h263data.c
    ${FFMPEG_DIR}/libavcodec/h263_parser.c
    ${FFMPEG_DIR}/libavcodec/h263dsp.c
    ${FFMPEG_DIR}/libavcodec/mpegvideo.c
    ${FFMPEG_DIR}/libavcodec/mpegvideodsp.c
    ${FFMPEG_DIR}/libavcodec/rl.c
    ${FFMPEG_DIR}/libavcodec/mpegvideo_motion.c
    ${FFMPEG_DIR}/libavcodec/mpegutils.c
    ${FFMPEG_DIR}/libavcodec/mpegvideodata.c
    ${FFMPEG_DIR}/libavcodec/mpegpicture.c
    ${FFMPEG_DIR}/libavcodec/mpeg_er.c
    ${FFMPEG_DIR}/libavcodec/error_resilience.c
    ${FFMPEG_DIR}/libavcodec/qpeldsp.c
    ${FFMPEG_DIR}/libavcodec/blockdsp.c
    ${FFMPEG_DIR}/libavcodec/h264qpel.c
    ${FFMPEG_DIR}/libavcodec/intrax8.c
    ${FFMPEG_DIR}/libavcodec/intrax8dsp.c
    ${FFMPEG_DIR}/libavcodec/h264chroma.c
    ${FFMPEG_DIR}/libavcodec/hpeldsp.c
    ${FFMPEG_DIR}/libavcodec/me_cmp.c
    ${FFMPEG_DIR}/libavcodec/videodsp.c
    ${FFMPEG_DIR}/libavcodec/pixblockdsp.c
    ${FFMPEG_DIR}/libavcodec/startcode.c
    # TW2 (2026-11-12): MPEG-1/2 video and ADX audio, the game's USM movies (FFmpeg 4.4's sources, as the rest)
    ${FFMPEG_DIR}/libavcodec/mpeg12dec.c
    ${FFMPEG_DIR}/libavcodec/mpeg12.c
    ${FFMPEG_DIR}/libavcodec/mpeg12data.c
    ${FFMPEG_DIR}/libavcodec/adx.c
    ${FFMPEG_DIR}/libavcodec/adxdec.c
)



# ARM64 optimizations for libavcodec
target_sources(libavcodec PRIVATE
    ${FFMPEG_DIR}/libavcodec/aarch64/fft_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/aarch64/fft_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/mdct_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/simple_idct_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/videodsp.S
    ${FFMPEG_DIR}/libavcodec/aarch64/neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/mpegaudiodsp_init.c
    ${FFMPEG_DIR}/libavcodec/aarch64/mpegaudiodsp_neon.S
    # Fork addition: NEON for WMV3
    ${FFMPEG_DIR}/libavcodec/aarch64/h264chroma_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/aarch64/h264cmc_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/h264qpel_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/aarch64/h264qpel_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/hpeldsp_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/aarch64/hpeldsp_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/idctdsp_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/aarch64/pixblockdsp_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/aarch64/pixblockdsp_neon.S
    ${FFMPEG_DIR}/libavcodec/aarch64/videodsp_init.c
    ${FFMPEG_DIR}/libavcodec/aarch64/vc1dsp_init_aarch64.c
    ${FFMPEG_DIR}/libavcodec/neon/mpegvideo.c
)
target_compile_definitions(libavcodec PRIVATE
    HAVE_NEON=1
    HAVE_ARMV8=1
    HAVE_INLINE_ASM=1
)

target_include_directories(libavcodec BEFORE PRIVATE "${FFMPEG_OVERLAY_BUILD_DIR}")

target_include_directories(libavcodec
    PUBLIC
        $<BUILD_INTERFACE:${FFMPEG_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${FFMPEG_ATOMICS_INCLUDE}
)
target_compile_definitions(libavcodec PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(libavcodec PRIVATE -w)  # Suppress warnings in third-party code
target_link_libraries(libavcodec PUBLIC libavutil)

# Hidden visibility is required on aarch64. The NEON .S files use ADRP/ADD
# PC-relative addressing (movrel) for symbols like ff_cos_*, which the linker
# only accepts against locally bound symbols when emitting a shared object.
set_target_properties(libavutil libavcodec PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    FOLDER "FFmpeg")
