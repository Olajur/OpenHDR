"""PFM reading and a preview PNG writer for OpenHDR frame dumps (standard library only)."""
import struct
import zlib


def read_pfm(path):
    """Returns (width, height, rows) with rows top first, each a flat RGB float tuple."""
    with open(path, "rb") as f:
        assert f.readline().strip() == b"PF", "not an RGB PFM"
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        endian = "<" if scale < 0 else ">"
        data = struct.unpack(endian + "f" * (w * h * 3), f.read(w * h * 3 * 4))
    rows = [data[(h - 1 - y) * w * 3:(h - y) * w * 3] for y in range(h)]
    return w, h, rows


def write_png(path, w, h, rows, to_byte):
    """rows: top-first flat RGB floats; to_byte maps one float to 0..255."""
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter: none
        raw.extend(to_byte(v) for v in row)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    # python tools/pfm.py output.pfm preview.png [peak nits]: scRGB dump -> SDR preview (peak maps to white)
    import sys

    src, dst = sys.argv[1], sys.argv[2]
    peak = float(sys.argv[3]) if len(sys.argv) > 3 else 400.0
    w, h, rows = read_pfm(src)
    write_png(dst, w, h, rows, lambda v: int(255 * min(max(v * 80 / peak, 0.0), 1.0) ** (1 / 2.2) + 0.5))
