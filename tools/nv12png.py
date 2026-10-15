#!/usr/bin/env python3
# -*- coding: latin-1 -*-
# Prova le geometrie possibili di un buffer NV12 e salva un PNG per ciascuna
# (cosi' si guarda direttamente cos'e').
#
#   python -I nv12png.py <file.bin> <out.png> [W H STRIDEY STRIDEUV]
#
import struct
import sys
import zlib


def png_gray(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def png_rgb(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def main():
    path = sys.argv[1]
    out = sys.argv[2]
    data = open(path, 'rb').read()
    if len(sys.argv) > 6:
        w, h, sy, suv = (int(x) for x in sys.argv[3:7])
    else:
        w, h, sy, suv = 1920, 1080, 1920, 1920
    y_plane = data[:sy * h]
    uv_off = sy * h
    uv = data[uv_off:uv_off + suv * (h // 2)]

    rr = []
    for yy in range(h):
        row = bytearray()
        ybase = yy * sy
        uvrow = (yy // 2) * suv
        for xx in range(w):
            Y = y_plane[ybase + xx]
            if uv_off and len(uv) >= uvrow + (xx // 2) * 2 + 2:
                U = uv[uvrow + (xx // 2) * 2] - 128
                V = uv[uvrow + (xx // 2) * 2 + 1] - 128
            else:
                U = V = 0
            c = Y - 16
            if c < 0:
                c = 0
            R = clamp((298 * c + 409 * V + 128) >> 8)
            G = clamp((298 * c - 100 * U - 208 * V + 128) >> 8)
            B = clamp((298 * c + 516 * U + 128) >> 8)
            row += bytes((R, G, B))
        rr.append(bytes(row))
    png_rgb(out, w, h, rr)
    print('scritto %s (%dx%d, strideY %d, strideUV %d)' % (out, w, h, sy, suv))


if __name__ == '__main__':
    main()
