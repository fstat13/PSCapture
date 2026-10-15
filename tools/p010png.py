#!/usr/bin/env python3
# -*- coding: latin-1 -*-
# Rende un frame P010 (10 bit in contenitori da 16) in PNG.
#
#   python -I p010png.py <file.bin> <off> <out.png> [W H ]
#
import struct
import sys
import zlib


def chunk(tag, data):
    c = tag + data
    return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)


def png_rgb(path, w, h, rows):
    raw = b''.join(b'\x00' + rows[y] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def main():
    path = sys.argv[1]
    off = int(sys.argv[2], 0)
    out = sys.argv[3]
    w = int(sys.argv[4]) if len(sys.argv) > 4 else 1920
    h = int(sys.argv[5]) if len(sys.argv) > 5 else 1080
    d = open(path, 'rb').read()
    y_stride = w * 2
    uv_off = off + y_stride * h
    rgb = []
    for y in range(h):
        yb = off + y * y_stride
        uvrow = uv_off + (y // 2) * (w // 2) * 4
        row = bytearray()
        for x in range(w):
            Y = ((d[yb + x * 2] | (d[yb + x * 2 + 1] << 8)) >> 6) >> 2
            p = uvrow + (x // 2) * 4
            U = ((d[p] | (d[p + 1] << 8)) >> 6) >> 2
            V = ((d[p + 2] | (d[p + 3] << 8)) >> 6) >> 2
            c = Y - 16
            U -= 128
            V -= 128
            R = clamp((298 * c + 409 * V + 128) >> 8)
            G = clamp((298 * c - 100 * U - 208 * V + 128) >> 8)
            B = clamp((298 * c + 516 * U + 128) >> 8)
            row += bytes((R, G, B))
        rgb.append(bytes(row))
    png_rgb(out, w, h, rgb)
    print('scritto ' + out)


if __name__ == '__main__':
    main()
