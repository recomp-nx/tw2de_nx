#!/usr/bin/env python3
"""tw2de (2026-11-08): the NRO's icon made at build time (app/CMakeLists.txt).

    make_icon.py <in.png> <out.jpg> [size] [max_bytes]

Reads a PNG (8 or 16 bits a channel, grey / RGB / palette, with or without alpha; not interlaced), puts its
transparent parts over white, scales it to size x size (default 256: what the Switch's NRO icon is) with a Lanczos-3 filter
and writes a baseline JPEG (4:4:4) at the highest quality (100 down to 50) whose file is at most max_bytes (default
102400: 100 kB), failing the build when even quality 50 is larger. The Python standard library only (no Pillow on the build PC needed).
"""
import math
import struct
import sys
import zlib


# ---------------------------------------------------------------------------- PNG

def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('make_icon: %s is not a PNG' % path)
    pos, idat, palette, trns = 8, [], None, None
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
    if interlace:
        raise SystemExit('make_icon: %s is interlaced (save it without interlacing)' % path)
    if depth not in (8, 16) and not (ctype == 3 and depth in (1, 2, 4)):
        raise SystemExit('make_icon: %s: %d bits a channel not handled' % (path, depth))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    bits = channels * depth
    stride = (width * bits + 7) // 8
    bpp = max(1, bits // 8)
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        ftype = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if ftype == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif ftype == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif ftype == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    # RGB over white, floats 0-255.
    pixels = []
    for line in rows:
        out = []
        if ctype == 3:
            per = 8 // depth
            for x in range(width):
                byte = line[x // per]
                index = (byte >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
                r, g, b = palette[index]
                a = trns[index] if trns and index < len(trns) else 255
                out.append((r, g, b, a))
        else:
            step = 2 if depth == 16 else 1  # 16 bits: the high byte
            for x in range(width):
                v = [line[(x * channels + c) * step] for c in range(channels)]
                if ctype == 0:
                    out.append((v[0], v[0], v[0], 255))
                elif ctype == 4:
                    out.append((v[0], v[0], v[0], v[1]))
                elif ctype == 2:
                    out.append((v[0], v[1], v[2], 255))
                else:
                    out.append(tuple(v))
        pixels.append([tuple(c * p[3] / 255.0 + 255.0 - p[3] for c in p[:3]) for p in out])
    return width, height, pixels


# ---------------------------------------------------------------------------- scaling (Lanczos-3)

def lanczos(x):
    if x == 0.0:
        return 1.0
    if abs(x) >= 3.0:
        return 0.0
    px = math.pi * x
    return 3.0 * math.sin(px) * math.sin(px / 3.0) / (px * px)


def weights(src, dst):
    """For each destination index, the (source index, weight) pairs of a Lanczos-3 filter widened by the reduction
    (as Pillow's LANCZOS), weights summing to 1, source indices clamped at the edges."""
    ratio = src / dst
    support = max(ratio, 1.0)
    table = []
    for d in range(dst):
        centre = (d + 0.5) * ratio
        pairs = {}
        for s in range(int(math.floor(centre - 3.0 * support)), int(math.ceil(centre + 3.0 * support)) + 1):
            w = lanczos((s + 0.5 - centre) / support)
            if w:
                k = min(max(s, 0), src - 1)
                pairs[k] = pairs.get(k, 0.0) + w
        total = sum(pairs.values())
        table.append([(k, w / total) for k, w in pairs.items()])
    return table


def scale(width, height, pixels, size):
    wx, wy = weights(width, size), weights(height, size)
    rows = []  # horizontal first
    for line in pixels:
        rows.append([tuple(sum(line[s][c] * w for s, w in pairs) for c in range(3)) for pairs in wx])
    out = []
    for pairs in wy:
        out.append([tuple(min(max(sum(rows[s][x][c] * w for s, w in pairs), 0.0), 255.0) for c in range(3))
                    for x in range(size)])
    return out


# ---------------------------------------------------------------------------- JPEG (baseline, 4:4:4)

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
          21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53,
          60, 61, 54, 47, 55, 62, 63]
LUMA_Q = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22,
          29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103,
          121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
CHROMA_Q = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99,
            99, 99, 99, 99, 99] + [99] * 32
# The standard tables (ITU T.81 annex K.3): code counts by length, then the values.
DC_LUMA = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
DC_CHROMA = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))
AC_LUMA = ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], [
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa])
AC_CHROMA = ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], [
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa])


def huffman_codes(table):
    counts, values = table
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            codes[values[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


def quant(base, quality):
    s = 5000 // quality if quality < 50 else 200 - 2 * quality
    return [min(max((q * s + 50) // 100, 1), 255) for q in base]


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, length):
        self.acc = (self.acc << length) | (value & ((1 << length) - 1))
        self.n += length
        while self.n >= 8:
            byte = (self.acc >> (self.n - 8)) & 255
            self.out.append(byte)
            if byte == 0xFF:
                self.out.append(0)
            self.n -= 8
        self.acc &= (1 << self.n) - 1

    def flush(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)


COS = [[(math.sqrt(0.5) if u == 0 else 1.0) * 0.5 * math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)]
       for u in range(8)]


def fdct(block):
    rows = [[sum(COS[u][x] * block[y * 8 + x] for x in range(8)) for u in range(8)] for y in range(8)]
    return [sum(COS[v][y] * rows[y][u] for y in range(8)) for v in range(8) for u in range(8)]


def magnitude(v):
    n = abs(v).bit_length()
    return n, (v if v >= 0 else v + (1 << n) - 1)


def encode_jpeg(pixels, size, quality):
    qy, qc = quant(LUMA_Q, quality), quant(CHROMA_Q, quality)
    dcy, acy, dcc, acc = (huffman_codes(t) for t in (DC_LUMA, AC_LUMA, DC_CHROMA, AC_CHROMA))
    planes = [[0.0] * (size * size) for _ in range(3)]
    for y in range(size):
        for x in range(size):
            r, g, b = pixels[y][x]
            planes[0][y * size + x] = 0.299 * r + 0.587 * g + 0.114 * b - 128.0
            planes[1][y * size + x] = -0.168736 * r - 0.331264 * g + 0.5 * b
            planes[2][y * size + x] = 0.5 * r - 0.418688 * g - 0.081312 * b
    bits = Bits()
    pred = [0, 0, 0]
    blocks = (size + 7) // 8
    for by in range(blocks):
        for bx in range(blocks):
            for c in range(3):
                plane = planes[c]
                block = [plane[min(by * 8 + j, size - 1) * size + min(bx * 8 + i, size - 1)]
                         for j in range(8) for i in range(8)]
                q = qy if c == 0 else qc
                coeffs = fdct(block)
                zz = [int(round(coeffs[ZIGZAG[k]] / q[ZIGZAG[k]])) for k in range(64)]
                dc_codes, ac_codes = (dcy, acy) if c == 0 else (dcc, acc)
                diff = zz[0] - pred[c]
                pred[c] = zz[0]
                n, v = magnitude(diff)
                bits.put(*dc_codes[n])
                if n:
                    bits.put(v, n)
                run = 0
                for k in range(1, 64):
                    if zz[k] == 0:
                        run += 1
                        continue
                    while run > 15:
                        bits.put(*ac_codes[0xF0])
                        run -= 16
                    n, v = magnitude(zz[k])
                    bits.put(*ac_codes[(run << 4) | n])
                    bits.put(v, n)
                    run = 0
                if run:
                    bits.put(*ac_codes[0x00])
    bits.flush()

    def segment(marker, body):
        return struct.pack('>BBH', 0xFF, marker, len(body) + 2) + body

    def dht(cls, ident, table):
        return bytes([(cls << 4) | ident]) + bytes(table[0]) + bytes(table[1])

    out = bytearray(b'\xff\xd8')
    out += segment(0xE0, b'JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00')
    out += segment(0xDB, bytes([0]) + bytes(qy[ZIGZAG[k]] for k in range(64)) + bytes([1]) + bytes(qc[ZIGZAG[k]] for k in range(64)))
    out += segment(0xC0, struct.pack('>BHHB', 8, size, size, 3) + bytes([1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1]))
    out += segment(0xC4, dht(0, 0, DC_LUMA) + dht(1, 0, AC_LUMA) + dht(0, 1, DC_CHROMA) + dht(1, 1, AC_CHROMA))
    out += segment(0xDA, bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]))
    out += bits.out + b'\xff\xd9'
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    max_bytes = int(sys.argv[4]) if len(sys.argv) > 4 else 102400
    width, height, pixels = read_png(sys.argv[1])
    scaled = scale(width, height, pixels, size)
    for quality in range(100, 49, -1):
        data = encode_jpeg(scaled, size, quality)
        if len(data) <= max_bytes:
            break
    else:
        raise SystemExit('make_icon: %s is %d bytes at quality 50, over the limit of %d' % (sys.argv[2], len(data),
                                                                                           max_bytes))
    open(sys.argv[2], 'wb').write(data)
    print('make_icon: %s (%dx%d) -> %s (%dx%d, quality %d, %d bytes of at most %d)' % (
        sys.argv[1], width, height, sys.argv[2], size, size, quality, len(data), max_bytes))


if __name__ == '__main__':
    main()
