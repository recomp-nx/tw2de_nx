#version 460
// tw2de-dk ubo=0x20 ssbo=0x7 tex=0
#if defined(GL_EXT_control_flow_attributes)
#extension GL_EXT_control_flow_attributes : require
#define SPIRV_CROSS_FLATTEN [[flatten]]
#define SPIRV_CROSS_BRANCH [[dont_flatten]]
#define SPIRV_CROSS_UNROLL [[unroll]]
#define SPIRV_CROSS_LOOP [[dont_unroll]]
#else
#define SPIRV_CROSS_FLATTEN
#define SPIRV_CROSS_BRANCH
#define SPIRV_CROSS_UNROLL
#define SPIRV_CROSS_LOOP
#endif
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(binding = 0, std430) buffer type_RWByteAddressBuffer
{
    uint _m0[];
} g_Buffers[3];

layout(std140, binding = 5) uniform type_ConstantBuffer_StreamConvertPush
{
    uint source;
    uint sourceOffset;
    uint destOffset;
    uint vertices;
    uint guestStrideWords;
    uint hostStrideWords;
    uint elementCount;
    uint pad;
    uvec4 elements[4];
} g_Push;

// tw2de (2026-10-27): the converted values' six slots (see main) by an index known at run time.
#define STREAM_PUT(i, v) { uint _pi = (i); float _pv = (v); _94_0 = _pi == 0u ? _pv : _94_0; _94_1 = _pi == 1u ? _pv : _94_1; \
    _94_2 = _pi == 2u ? _pv : _94_2; _94_3 = _pi == 3u ? _pv : _94_3; _94_4 = _pi == 4u ? _pv : _94_4; \
    _94_5 = _pi == 5u ? _pv : _94_5; }
#define STREAM_GET(i) ((i) == 0u ? _94_0 : (i) == 1u ? _94_1 : (i) == 2u ? _94_2 : (i) == 3u ? _94_3 : \
    (i) == 5u ? _94_5 : _94_4)

// tw2de (2026-10-27): the texture mode (elementCount bit 31; draw.cpp UntileFromExportLocked): one block of a
// tiled 2D texture level a thread, from an export window (the slot's export buffer, 'source') to rows of host blocks in
// the destination buffer, byte-swapped by the fetch's endian. The game's texture kernels make DXT textures in memexport
// windows; their draws read them as the GPU wrote them, no wait for the CPU (the 30-90 ms texture waits of its area
// loads). Fields: vertices = blocks, guestStrideWords = the tiling pitch in blocks, hostStrideWords = blocks a row,
// elementCount = bit 31 | log2(block bytes) << 8 | endian, pad = the packed tail's x | y << 16 (blocks),
// elements[0].x = the destination's row pitch (bytes).
// tw2de (2026-10-30): elementCount bit 16: a linear level (texture_layout.cpp's rows of pitch_blocks blocks): the
// blocks are copied and swapped, not untiled (the loading screen's 320x176 DXT1 icons, read from guest memory on the
// GPU with tw2de_memexport_zero_copy instead of a wait and a CPU upload each frame).
uint UntileSwap(uint v, uint endian)
{
    if (endian == 1u)
        return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
    if (endian == 2u)
        return (v << 24u) | ((v << 8u) & 0x00FF0000u) | ((v >> 8u) & 0x0000FF00u) | (v >> 24u);
    if (endian == 3u)
        return (v << 16u) | (v >> 16u);
    return v;
}

// texture_layout.cpp TiledOffset2D (XGAddress2DTiledOffset, in bytes).
uint UntileOffset2D(uint x, uint y, uint pitch_blocks, uint log2_block_bytes)
{
    uint tiles_per_row = ((pitch_blocks + 31u) & ~31u) >> 5u;
    uint shift = log2_block_bytes + 6u;
    uint tile = (((x >> 5u) + (y >> 5u) * tiles_per_row) << shift) & 0x1FFFFFFFu;
    uint micro = (((x & 7u) + ((y << 2u) & 0x18u)) << shift) >> 6u;
    uint a = 2u * (tile + (micro & ~0xFu)) + (micro & 0xFu) + (((y >> 3u) & 1u) << shift) + ((y & 1u) << 4u);
    uint b = ((2u * ((x >> 3u) + ((y >> 2u) & 2u))) & 6u) + ((y >> 4u) & 1u);
    return ((b & 1u) << 11u) + (((a >> 6u) & 7u) << 8u) + ((b & ~1u) << 5u) + ((a & ~0x1FFu) << 3u) + (a & 0x3Fu);
}

void UntileBlocks()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= g_Push.vertices)
        return;
    uint bw = g_Push.hostStrideWords;
    uint y = uint(float(i) / float(bw));
    if (y * bw > i)
        y--;
    if ((y + 1u) * bw <= i)
        y++;
    uint x = i - y * bw;
    uint log2b = (g_Push.elementCount >> 8u) & 31u;
    uint endian = g_Push.elementCount & 3u;
    uint offset = (g_Push.elementCount & 0x10000u) != 0u
        ? ((y + (g_Push.pad >> 16u)) * g_Push.guestStrideWords + x + (g_Push.pad & 0xFFFFu)) << log2b
        : UntileOffset2D(x + (g_Push.pad & 0xFFFFu), y + (g_Push.pad >> 16u), g_Push.guestStrideWords, log2b);
    uint src = (g_Push.sourceOffset + offset) >> 2u;
    uint dst = (g_Push.destOffset + y * g_Push.elements[0].x + (x << log2b)) >> 2u;
    uint words = 1u << (log2b - 2u);
    for (uint w = 0u; w < 4u; w++)
    {
        if (w < words)
            g_Buffers[2u]._m0[dst + w] = UntileSwap(g_Buffers[g_Push.source]._m0[src + w], endian);
    }
}

// tw2de (2026-10-27): the seed mode (elementCount bit 30; draw.cpp SeedFromPendingExportsLocked): one word a thread
// from an older slot's pending window ('source', at sourceOffset) to this slot's fresh window (export buffer
// elements[0].x, at destOffset), only on the pages that window's kernels wrote (its page flags, at byte pad of the
// source buffer; hostStrideWords = the piece's first byte from its window's start). The other pages keep the fresh
// window's seed from guest memory, which has the game's newer writes there: the whole older window, copied, put its
// one-frame-old seed back over them (the minimap: a map the CPU loaded into the game's texture pool between two
// compaction passes was lost, and the next pass moved stale data into its place).
// tw2de (2026-10-29, tw2de_memexport_write_mask): with elements[0].y (the write mask's first byte in the source
// buffer) the word goes by its bit in the mask, not by its page's flag: only the dwords that window's kernels wrote, so
// the rest of a written page keeps the fresh seed's bytes (guest memory's, with the game's writes since that window's
// seed) instead of that window's older seed.
void SeedWrittenPages()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= g_Push.vertices)
        return;
    uint src = g_Push.source;
    uint at = (g_Push.sourceOffset >> 2u) + i;
    if (g_Push.elements[0].y != 0u)
    {
        uint mw = (g_Push.elements[0].y >> 2u) + (at >> 5u);
        uint m = src == 0u ? g_Buffers[0]._m0[mw] : g_Buffers[1]._m0[mw];
        if ((m & (1u << (at & 31u))) == 0u)
            return;
    }
    else
    {
        uint page = (g_Push.hostStrideWords + i * 4u) >> 12u;
        uint flag = src == 0u ? g_Buffers[0]._m0[(g_Push.pad >> 2u) + page] : g_Buffers[1]._m0[(g_Push.pad >> 2u) + page];
        if (flag == 0u)
            return;
    }
    uint v = src == 0u ? g_Buffers[0]._m0[at] : g_Buffers[1]._m0[at];
    uint to = (g_Push.destOffset >> 2u) + i;
    if (g_Push.elements[0].x == 0u)
        g_Buffers[0]._m0[to] = v;
    else
        g_Buffers[1]._m0[to] = v;
}

// tw2de (2026-10-29): the mask mode (elementCount bit 29; draw.cpp FlushExportMaskRangesLocked): the export
// buffer's write mask (shader_common_glsl.h memExportMark: a bit a dword, the words from byte 'pad' of the buffer) gets
// the dwords [sourceOffset, sourceOffset + vertices) that a buffer copy wrote in place of the game's copy kernel
// (CopyKernelAsCopyLocked), one mask word a thread (hostStrideWords = the first word's index), OR-ed into what the
// kernels marked: export buffer elements[0].x (0 or 1, the slot).
void OrMaskRange()
{
    uint w = g_Push.hostStrideWords + gl_GlobalInvocationID.x;
    uint first = g_Push.sourceOffset, end = g_Push.sourceOffset + g_Push.vertices;
    uint lo = max(first, w << 5u), hi = min(end, (w << 5u) + 32u);
    if (lo >= hi)
        return;
    uint n = hi - lo;
    uint bits = (n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u)) << (lo & 31u);
    uint at = (g_Push.pad >> 2u) + w;
    if (g_Push.elements[0].x == 0u)
        atomicOr(g_Buffers[0]._m0[at], bits);
    else
        atomicOr(g_Buffers[1]._m0[at], bits);
}

void main()
{
    if ((g_Push.elementCount & 0x80000000u) != 0u)
    {
        UntileBlocks();
        return;
    }
    if ((g_Push.elementCount & 0x40000000u) != 0u)
    {
        SeedWrittenPages();
        return;
    }
    if ((g_Push.elementCount & 0x20000000u) != 0u)
    {
        OrMaskRange();
        return;
    }
    do
    {
        bool _110;
        if (!(g_Push.elementCount == 0u))
        {
            _110 = gl_GlobalInvocationID.x >= (g_Push.vertices * g_Push.elementCount);
        }
        else
        {
            _110 = true;
        }
        if (_110)
        {
            break;
        }
        float _115 = float(g_Push.elementCount);
        uint _118 = uint((float(gl_GlobalInvocationID.x) / _115) * 0.999989986419677734375);
        uint _125 = _118 + uint((float(gl_GlobalInvocationID.x - (_118 * g_Push.elementCount)) / _115) * 0.999989986419677734375);
        uint _127 = gl_GlobalInvocationID.x - (_125 * g_Push.elementCount);
        uint _129;
        _129 = _125;
        SPIRV_CROSS_LOOP
        for (uint _132 = _127; _132 >= g_Push.elementCount; )
        {
            _129++;
            _132 -= g_Push.elementCount;
            continue;
        }
        uint _137 = gl_GlobalInvocationID.x - (_129 * g_Push.elementCount);
        uint _138 = _137 >> 1u;
        bool _142 = (_137 & 1u) != 0u;
        uint _148;
        if (_142)
        {
            _148 = g_Push.elements[_138].z;
        }
        else
        {
            _148 = g_Push.elements[_138].x;
        }
        uint _154;
        if (_142)
        {
            _154 = g_Push.elements[_138].w;
        }
        else
        {
            _154 = g_Push.elements[_138].y;
        }
        uint _159 = (_148 >> 16u) & 3u;
        uint _167 = g_Push.sourceOffset + (((_129 * g_Push.guestStrideWords) + (_148 & 255u)) * 4u);
        uint _175 = g_Push.destOffset + (((_129 * g_Push.hostStrideWords) + ((_148 >> 8u) & 255u)) * 4u);
        if (!(((_148 >> 18u) & 1u) != 0u))
        {
            uint _183 = (_148 >> 19u) & 7u;
            for (uint _185 = 0u; _185 < _183; _185++)
            {
                uint _191 = _185 * 4u;
                uint _197 = (_167 + _191) >> 2u;
                uint _228;
                do
                {
                    if (_159 == 1u)
                    {
                        _228 = ((g_Buffers[g_Push.source]._m0[_197] & 16711935u) << 8u) | ((g_Buffers[g_Push.source]._m0[_197] >> 8u) & 16711935u);
                        break;
                    }
                    if (_159 == 2u)
                    {
                        _228 = (((g_Buffers[g_Push.source]._m0[_197] << 24u) | ((g_Buffers[g_Push.source]._m0[_197] << 8u) & 16711680u)) | ((g_Buffers[g_Push.source]._m0[_197] >> 8u) & 65280u)) | (g_Buffers[g_Push.source]._m0[_197] >> 24u);
                        break;
                    }
                    if (_159 == 3u)
                    {
                        _228 = (g_Buffers[g_Push.source]._m0[_197] << 16u) | (g_Buffers[g_Push.source]._m0[_197] >> 16u);
                        break;
                    }
                    _228 = g_Buffers[g_Push.source]._m0[_197];
                    break;
                } while(false);
                g_Buffers[2u]._m0[(_175 + _191) >> 2u] = _228;
            }
            break;
        }
        bool _244;
        uint _230 = _154 & 63u;
        bool _233 = ((_154 >> 8u) & 1u) != 0u;
        bool _236 = ((_154 >> 9u) & 1u) != 0u;
        // tw2de (2026-10-27): the six values (x, y, z, w, 0, 1) as six variables, not an array indexed at run time
        // (uam put it in local memory: 5 KB of scratch a warp); STREAM_PUT and STREAM_GET select by the index.
        float _94_0 = 0.0, _94_1 = 0.0, _94_2 = 0.0, _94_3 = 1.0, _94_4 = 0.0, _94_5 = 1.0;
        uint _239 = _167 >> 2u;
        uint _270;
        do
        {
            _244 = _159 == 1u;
            if (_244)
            {
                _270 = ((g_Buffers[g_Push.source]._m0[_239] & 16711935u) << 8u) | ((g_Buffers[g_Push.source]._m0[_239] >> 8u) & 16711935u);
                break;
            }
            if (_159 == 2u)
            {
                _270 = (((g_Buffers[g_Push.source]._m0[_239] << 24u) | ((g_Buffers[g_Push.source]._m0[_239] << 8u) & 16711680u)) | ((g_Buffers[g_Push.source]._m0[_239] >> 8u) & 65280u)) | (g_Buffers[g_Push.source]._m0[_239] >> 24u);
                break;
            }
            if (_159 == 3u)
            {
                _270 = (g_Buffers[g_Push.source]._m0[_239] << 16u) | (g_Buffers[g_Push.source]._m0[_239] >> 16u);
                break;
            }
            _270 = g_Buffers[g_Push.source]._m0[_239];
            break;
        } while(false);
        switch (_230)
        {
            case 6u:
            {
                for (uint _850 = 0u; _850 < 4u; _850++)
                {
                    float _878;
                    do
                    {
                        uint _861 = (_270 >> ((_850 * 8u) & 31u)) & 255u;
                        if (_233)
                        {
                            int _866 = int(_861 << 24u) >> 24;
                            if (_236)
                            {
                                _878 = float(_866);
                                break;
                            }
                            _878 = max(float(_866) * 0.0078740157186985015869140625, -1.0);
                            break;
                        }
                        if (_236)
                        {
                            _878 = float(_861);
                            break;
                        }
                        _878 = float(_861) * 0.0039215688593685626983642578125;
                        break;
                    } while(false);
                    STREAM_PUT(_850, _878);
                }
                break;
            }
            case 7u:
            {
                float _782;
                do
                {
                    uint _765 = _270 & 1023u;
                    if (_233)
                    {
                        int _770 = int(_765 << 22u) >> 22;
                        if (_236)
                        {
                            _782 = float(_770);
                            break;
                        }
                        _782 = max(float(_770) * 0.001956947147846221923828125, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _782 = float(_765);
                        break;
                    }
                    _782 = float(_765) * 0.000977517105638980865478515625;
                    break;
                } while(false);
                _94_0 = _782;
                float _804;
                do
                {
                    uint _787 = (_270 >> 10u) & 1023u;
                    if (_233)
                    {
                        int _792 = int(_787 << 22u) >> 22;
                        if (_236)
                        {
                            _804 = float(_792);
                            break;
                        }
                        _804 = max(float(_792) * 0.001956947147846221923828125, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _804 = float(_787);
                        break;
                    }
                    _804 = float(_787) * 0.000977517105638980865478515625;
                    break;
                } while(false);
                _94_1 = _804;
                float _826;
                do
                {
                    uint _809 = (_270 >> 20u) & 1023u;
                    if (_233)
                    {
                        int _814 = int(_809 << 22u) >> 22;
                        if (_236)
                        {
                            _826 = float(_814);
                            break;
                        }
                        _826 = max(float(_814) * 0.001956947147846221923828125, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _826 = float(_809);
                        break;
                    }
                    _826 = float(_809) * 0.000977517105638980865478515625;
                    break;
                } while(false);
                _94_2 = _826;
                float _847;
                do
                {
                    uint _831 = (_270 >> 30u) & 3u;
                    if (_233)
                    {
                        int _836 = int(_831 << 30u) >> 30;
                        if (_236)
                        {
                            _847 = float(_836);
                            break;
                        }
                        _847 = max(float(_836), -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _847 = float(_831);
                        break;
                    }
                    _847 = float(_831) * 0.3333333432674407958984375;
                    break;
                } while(false);
                _94_3 = _847;
                break;
            }
            case 16u:
            {
                float _717;
                do
                {
                    uint _700 = _270 & 2047u;
                    if (_233)
                    {
                        int _705 = int(_700 << 21u) >> 21;
                        if (_236)
                        {
                            _717 = float(_705);
                            break;
                        }
                        _717 = max(float(_705) * 0.000977517105638980865478515625, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _717 = float(_700);
                        break;
                    }
                    _717 = float(_700) * 0.000488519784994423389434814453125;
                    break;
                } while(false);
                _94_0 = _717;
                float _739;
                do
                {
                    uint _722 = (_270 >> 11u) & 2047u;
                    if (_233)
                    {
                        int _727 = int(_722 << 21u) >> 21;
                        if (_236)
                        {
                            _739 = float(_727);
                            break;
                        }
                        _739 = max(float(_727) * 0.000977517105638980865478515625, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _739 = float(_722);
                        break;
                    }
                    _739 = float(_722) * 0.000488519784994423389434814453125;
                    break;
                } while(false);
                _94_1 = _739;
                float _761;
                do
                {
                    uint _744 = (_270 >> 22u) & 1023u;
                    if (_233)
                    {
                        int _749 = int(_744 << 22u) >> 22;
                        if (_236)
                        {
                            _761 = float(_749);
                            break;
                        }
                        _761 = max(float(_749) * 0.001956947147846221923828125, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _761 = float(_744);
                        break;
                    }
                    _761 = float(_744) * 0.000977517105638980865478515625;
                    break;
                } while(false);
                _94_2 = _761;
                break;
            }
            case 17u:
            {
                float _652;
                do
                {
                    uint _635 = _270 & 1023u;
                    if (_233)
                    {
                        int _640 = int(_635 << 22u) >> 22;
                        if (_236)
                        {
                            _652 = float(_640);
                            break;
                        }
                        _652 = max(float(_640) * 0.001956947147846221923828125, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _652 = float(_635);
                        break;
                    }
                    _652 = float(_635) * 0.000977517105638980865478515625;
                    break;
                } while(false);
                _94_0 = _652;
                float _674;
                do
                {
                    uint _657 = (_270 >> 10u) & 2047u;
                    if (_233)
                    {
                        int _662 = int(_657 << 21u) >> 21;
                        if (_236)
                        {
                            _674 = float(_662);
                            break;
                        }
                        _674 = max(float(_662) * 0.000977517105638980865478515625, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _674 = float(_657);
                        break;
                    }
                    _674 = float(_657) * 0.000488519784994423389434814453125;
                    break;
                } while(false);
                _94_1 = _674;
                float _696;
                do
                {
                    uint _679 = (_270 >> 21u) & 2047u;
                    if (_233)
                    {
                        int _684 = int(_679 << 21u) >> 21;
                        if (_236)
                        {
                            _696 = float(_684);
                            break;
                        }
                        _696 = max(float(_684) * 0.000977517105638980865478515625, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _696 = float(_679);
                        break;
                    }
                    _696 = float(_679) * 0.000488519784994423389434814453125;
                    break;
                } while(false);
                _94_2 = _696;
                break;
            }
            case 25u:
            {
                float _609;
                do
                {
                    uint _592 = _270 & 65535u;
                    if (_233)
                    {
                        int _597 = int(_592 << 16u) >> 16;
                        if (_236)
                        {
                            _609 = float(_597);
                            break;
                        }
                        _609 = max(float(_597) * 3.0518509447574615478515625e-05, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _609 = float(_592);
                        break;
                    }
                    _609 = float(_592) * 1.525902189314365386962890625e-05;
                    break;
                } while(false);
                _94_0 = _609;
                float _631;
                do
                {
                    uint _614 = (_270 >> 16u) & 65535u;
                    if (_233)
                    {
                        int _619 = int(_614 << 16u) >> 16;
                        if (_236)
                        {
                            _631 = float(_619);
                            break;
                        }
                        _631 = max(float(_619) * 3.0518509447574615478515625e-05, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _631 = float(_614);
                        break;
                    }
                    _631 = float(_614) * 1.525902189314365386962890625e-05;
                    break;
                } while(false);
                _94_1 = _631;
                break;
            }
            case 26u:
            {
                uint _473 = (_167 + 4u) >> 2u;
                uint _503;
                do
                {
                    if (_244)
                    {
                        _503 = ((g_Buffers[g_Push.source]._m0[_473] & 16711935u) << 8u) | ((g_Buffers[g_Push.source]._m0[_473] >> 8u) & 16711935u);
                        break;
                    }
                    if (_159 == 2u)
                    {
                        _503 = (((g_Buffers[g_Push.source]._m0[_473] << 24u) | ((g_Buffers[g_Push.source]._m0[_473] << 8u) & 16711680u)) | ((g_Buffers[g_Push.source]._m0[_473] >> 8u) & 65280u)) | (g_Buffers[g_Push.source]._m0[_473] >> 24u);
                        break;
                    }
                    if (_159 == 3u)
                    {
                        _503 = (g_Buffers[g_Push.source]._m0[_473] << 16u) | (g_Buffers[g_Push.source]._m0[_473] >> 16u);
                        break;
                    }
                    _503 = g_Buffers[g_Push.source]._m0[_473];
                    break;
                } while(false);
                float _523;
                do
                {
                    uint _506 = _270 & 65535u;
                    if (_233)
                    {
                        int _511 = int(_506 << 16u) >> 16;
                        if (_236)
                        {
                            _523 = float(_511);
                            break;
                        }
                        _523 = max(float(_511) * 3.0518509447574615478515625e-05, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _523 = float(_506);
                        break;
                    }
                    _523 = float(_506) * 1.525902189314365386962890625e-05;
                    break;
                } while(false);
                _94_0 = _523;
                float _545;
                do
                {
                    uint _528 = (_270 >> 16u) & 65535u;
                    if (_233)
                    {
                        int _533 = int(_528 << 16u) >> 16;
                        if (_236)
                        {
                            _545 = float(_533);
                            break;
                        }
                        _545 = max(float(_533) * 3.0518509447574615478515625e-05, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _545 = float(_528);
                        break;
                    }
                    _545 = float(_528) * 1.525902189314365386962890625e-05;
                    break;
                } while(false);
                _94_1 = _545;
                float _566;
                do
                {
                    uint _549 = _503 & 65535u;
                    if (_233)
                    {
                        int _554 = int(_549 << 16u) >> 16;
                        if (_236)
                        {
                            _566 = float(_554);
                            break;
                        }
                        _566 = max(float(_554) * 3.0518509447574615478515625e-05, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _566 = float(_549);
                        break;
                    }
                    _566 = float(_549) * 1.525902189314365386962890625e-05;
                    break;
                } while(false);
                _94_2 = _566;
                float _588;
                do
                {
                    uint _571 = (_503 >> 16u) & 65535u;
                    if (_233)
                    {
                        int _576 = int(_571 << 16u) >> 16;
                        if (_236)
                        {
                            _588 = float(_576);
                            break;
                        }
                        _588 = max(float(_576) * 3.0518509447574615478515625e-05, -1.0);
                        break;
                    }
                    if (_236)
                    {
                        _588 = float(_571);
                        break;
                    }
                    _588 = float(_571) * 1.525902189314365386962890625e-05;
                    break;
                } while(false);
                _94_3 = _588;
                break;
            }
            case 31u:
            {
                _94_0 = unpackHalf2x16(_270).x;
                _94_1 = unpackHalf2x16(_270 >> 16u).x;
                break;
            }
            case 32u:
            {
                uint _420 = (_167 + 4u) >> 2u;
                uint _450;
                do
                {
                    if (_244)
                    {
                        _450 = ((g_Buffers[g_Push.source]._m0[_420] & 16711935u) << 8u) | ((g_Buffers[g_Push.source]._m0[_420] >> 8u) & 16711935u);
                        break;
                    }
                    if (_159 == 2u)
                    {
                        _450 = (((g_Buffers[g_Push.source]._m0[_420] << 24u) | ((g_Buffers[g_Push.source]._m0[_420] << 8u) & 16711680u)) | ((g_Buffers[g_Push.source]._m0[_420] >> 8u) & 65280u)) | (g_Buffers[g_Push.source]._m0[_420] >> 24u);
                        break;
                    }
                    if (_159 == 3u)
                    {
                        _450 = (g_Buffers[g_Push.source]._m0[_420] << 16u) | (g_Buffers[g_Push.source]._m0[_420] >> 16u);
                        break;
                    }
                    _450 = g_Buffers[g_Push.source]._m0[_420];
                    break;
                } while(false);
                _94_0 = unpackHalf2x16(_270).x;
                _94_1 = unpackHalf2x16(_270 >> 16u).x;
                _94_2 = unpackHalf2x16(_450).x;
                _94_3 = unpackHalf2x16(_450 >> 16u).x;
                break;
            }
            case 33u:
            {
            }
            case 34u:
            {
            }
            case 35u:
            {
                int _354;
                if (_230 == 33u)
                {
                    _354 = 1;
                }
                else
                {
                    _354 = (_230 == 34u) ? 2 : 4;
                }
                uint _355 = uint(_354);
                for (uint _357 = 0u; _357 < _355; _357++)
                {
                    uint _400;
                    if (_357 == 0u)
                    {
                        _400 = _270;
                    }
                    else
                    {
                        uint _369 = (_167 + (_357 * 4u)) >> 2u;
                        uint _399;
                        do
                        {
                            if (_244)
                            {
                                _399 = ((g_Buffers[g_Push.source]._m0[_369] & 16711935u) << 8u) | ((g_Buffers[g_Push.source]._m0[_369] >> 8u) & 16711935u);
                                break;
                            }
                            if (_159 == 2u)
                            {
                                _399 = (((g_Buffers[g_Push.source]._m0[_369] << 24u) | ((g_Buffers[g_Push.source]._m0[_369] << 8u) & 16711680u)) | ((g_Buffers[g_Push.source]._m0[_369] >> 8u) & 65280u)) | (g_Buffers[g_Push.source]._m0[_369] >> 24u);
                                break;
                            }
                            if (_159 == 3u)
                            {
                                _399 = (g_Buffers[g_Push.source]._m0[_369] << 16u) | (g_Buffers[g_Push.source]._m0[_369] >> 16u);
                                break;
                            }
                            _399 = g_Buffers[g_Push.source]._m0[_369];
                            break;
                        } while(false);
                        _400 = _399;
                    }
                    float _417;
                    do
                    {
                        if (_233)
                        {
                            int _405 = int(_400);
                            if (_236)
                            {
                                _417 = float(_405);
                                break;
                            }
                            _417 = max(float(_405) * 4.656612873077392578125e-10, -1.0);
                            break;
                        }
                        if (_236)
                        {
                            _417 = float(_400);
                            break;
                        }
                        _417 = float(_400) * 2.3283064365386962890625e-10;
                        break;
                    } while(false);
                    STREAM_PUT(_357, _417);
                }
                break;
            }
            case 36u:
            {
            }
            case 37u:
            {
            }
            case 38u:
            {
            }
            case 57u:
            {
                int _299;
                if (_230 == 36u)
                {
                    _299 = 1;
                }
                else
                {
                    int _298;
                    if (_230 == 37u)
                    {
                        _298 = 2;
                    }
                    else
                    {
                        _298 = (_230 == 57u) ? 3 : 4;
                    }
                    _299 = _298;
                }
                uint _300 = uint(_299);
                for (uint _302 = 0u; _302 < _300; _302++)
                {
                    uint _345;
                    if (_302 == 0u)
                    {
                        _345 = _270;
                    }
                    else
                    {
                        uint _314 = (_167 + (_302 * 4u)) >> 2u;
                        uint _344;
                        do
                        {
                            if (_244)
                            {
                                _344 = ((g_Buffers[g_Push.source]._m0[_314] & 16711935u) << 8u) | ((g_Buffers[g_Push.source]._m0[_314] >> 8u) & 16711935u);
                                break;
                            }
                            if (_159 == 2u)
                            {
                                _344 = (((g_Buffers[g_Push.source]._m0[_314] << 24u) | ((g_Buffers[g_Push.source]._m0[_314] << 8u) & 16711680u)) | ((g_Buffers[g_Push.source]._m0[_314] >> 8u) & 65280u)) | (g_Buffers[g_Push.source]._m0[_314] >> 24u);
                                break;
                            }
                            if (_159 == 3u)
                            {
                                _344 = (g_Buffers[g_Push.source]._m0[_314] << 16u) | (g_Buffers[g_Push.source]._m0[_314] >> 16u);
                                break;
                            }
                            _344 = g_Buffers[g_Push.source]._m0[_314];
                            break;
                        } while(false);
                        _345 = _344;
                    }
                    STREAM_PUT(_302, uintBitsToFloat(_345));
                }
                break;
            }
            default:
            {
                break;
            }
        }
        // The four outputs by their selectors (3 bits each from bit 10; 6 and 7 read as 4, zero).
        uvec4 _95 = uvec4(floatBitsToUint(STREAM_GET((_154 >> 10u) & 7u)), floatBitsToUint(STREAM_GET((_154 >> 13u) & 7u)),
                          floatBitsToUint(STREAM_GET((_154 >> 16u) & 7u)), floatBitsToUint(STREAM_GET((_154 >> 19u) & 7u)));
        uint _897 = _175 >> 2u;
        g_Buffers[2u]._m0[_897] = _95.x;
        g_Buffers[2u]._m0[_897 + 1u] = _95.y;
        g_Buffers[2u]._m0[_897 + 2u] = _95.z;
        g_Buffers[2u]._m0[_897 + 3u] = _95.w;
        break;
    } while(false);
}

