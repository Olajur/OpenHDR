"""Generates OpenHDR's icons (standard library only):

    python tools/make_icon.py

assets/openhdr.ico      orange "H" badge: the app, its shortcuts, the tray icon while enabled
assets/openhdr-off.ico  grey badge: the tray icon while disabled

Each .ico holds PNG images at 16-256 px, drawn with 4x4 supersampling.
"""
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
ORANGE = (255, 166, 48)
GREY = (110, 110, 118)
INK = (26, 26, 26)
SS = 4  # supersampling factor


def inside_rounded(x, y, size, radius, margin):
    lo, hi = margin, size - margin
    if x < lo or x >= hi or y < lo or y >= hi:
        return False
    cx = min(max(x, lo + radius), hi - radius)
    cy = min(max(y, lo + radius), hi - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def inside_h(x, y, size):
    # Geometric "H": two stems and a crossbar, centred.
    u, v = x / size, y / size
    if not 0.27 <= v <= 0.73:
        return False
    left = 0.30 <= u <= 0.43
    right = 0.57 <= u <= 0.70
    bar = 0.43 < u < 0.57 and 0.455 <= v <= 0.545
    return left or right or bar


def render(size, colour):
    big = size * SS
    margin = big * 0.04
    radius = big * 0.22
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    x, y = px * SS + sx + 0.5, py * SS + sy + 0.5
                    if not inside_rounded(x, y, big, radius, margin):
                        continue
                    c = INK if inside_h(x, y, big) else colour
                    r, g, b, a = r + c[0], g + c[1], b + c[2], a + 1
            n = SS * SS
            if a:
                row += bytes((r // a, g // a, b // a, 255 * a // n))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(path, colour):
    images = [png(s, render(s, colour)) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for s, data in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + entries + b"".join(images))
    print(f"wrote {path}")


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
    os.makedirs(out, exist_ok=True)
    ico(os.path.join(out, "openhdr.ico"), ORANGE)
    ico(os.path.join(out, "openhdr-off.ico"), GREY)
