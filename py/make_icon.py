# -*- coding: utf-8 -*-
"""生成 NCM 转 MP3 的图标：红色圆角方块 + 白色音符。无第三方依赖。"""
import struct
import zlib

BG = (232, 71, 75)
SIZES = [16, 32, 48, 64, 128, 256]


def png(size):
    cx = cy = (size - 1) / 2.0
    radius = size * 0.22
    half = size * 0.5 - size * 0.06
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            dx = max(abs(x - cx) - (half - radius), 0.0)
            dy = max(abs(y - cy) - (half - radius), 0.0)
            if (dx * dx + dy * dy) ** 0.5 > radius:
                row += bytes((0, 0, 0, 0))
                continue

            white = False
            # 音符的“头”：左下角的实心圆
            hx, hy, hr = cx - size * 0.10, cy + size * 0.14, size * 0.115
            if (x - hx) ** 2 + (y - hy) ** 2 <= hr * hr:
                white = True
            # 音符的“杆”：从头部向上的竖条
            if abs(x - (cx - size * 0.015)) <= size * 0.032 and (cy - size * 0.24) <= y <= (cy + size * 0.20):
                white = True
            # 音符的“旗”：杆顶部向右的横条
            if (cy - size * 0.24) <= y <= (cy - size * 0.15) and (cx - size * 0.015) <= x <= (cx + size * 0.16):
                white = True

            row += bytes((255, 255, 255, 255)) if white else bytes((BG[0], BG[1], BG[2], 255))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    images = [(s, png(s)) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    data = b""
    for s, blob in images:
        w = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        data += blob
    with open("icon.ico", "wb") as f:
        f.write(header + entries + data)
    print("icon.ico written:", SIZES)


if __name__ == "__main__":
    main()
