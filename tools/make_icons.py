"""Generate our geometric Burrow emblem using only Python's standard library."""
import binascii
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1] / "resources" / "icons"
# Polygon coordinates share a 64-unit canvas with the SVG source.
SHAPES = [
    ("#24594e", [(4, 4), (60, 4), (60, 60), (4, 60)]),
    ("#d9dcd5", [(11, 23), (14, 10), (24, 17), (40, 17), (50, 10), (53, 23), (49, 43), (32, 55), (15, 43)]),
    ("#252e32", [(16, 19), (24, 21), (29, 42), (21, 38)]),
    ("#252e32", [(48, 19), (40, 21), (35, 42), (43, 38)]),
    ("#252e32", [(26, 43), (38, 43), (32, 49)]),
]


def contains(x, y, points):
    inside = False
    previous = points[-1]
    for current in points:
        ax, ay = previous
        bx, by = current
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            inside = not inside
        previous = current
    return inside


def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", binascii.crc32(kind + payload) & 0xffffffff)


def png(size):
    raw = bytearray()
    colors = [(bytes.fromhex(color[1:]) + bytes([255]), points) for color, points in SHAPES]
    for row in range(size):
        raw.append(0)
        for column in range(size):
            color = bytes([0, 0, 0, 0])
            for candidate, points in colors:
                if contains((column + 0.5) * 64 / size, (row + 0.5) * 64 / size, points):
                    color = candidate
            raw.extend(color)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    paths = [f'<path fill="{color}" d="M ' + " L ".join(f"{x} {y}" for x, y in points) + ' Z"/>' for color, points in SHAPES]
    (ROOT / "burrow.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">' + "".join(paths) + '</svg>\n')
    images = {size: png(size) for size in [16, 32, 48, 64, 128, 256, 512, 1024]}
    (ROOT / "burrow.png").write_bytes(images[256])
    sizes = [16, 32, 48, 256]
    offset = 6 + 16 * len(sizes)
    entries = bytearray()
    payload = bytearray()
    for size in sizes:
        data = images[size]
        entries.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        payload.extend(data)
        offset += len(data)
    (ROOT / "burrow.ico").write_bytes(struct.pack("<HHH", 0, 1, len(sizes)) + entries + payload)
    payload = b"".join(kind + struct.pack(">I", len(images[size]) + 8) + images[size] for size, kind in [(128, b"ic07"), (256, b"ic08"), (512, b"ic09"), (1024, b"ic10")])
    (ROOT / "burrow.icns").write_bytes(b"icns" + struct.pack(">I", len(payload) + 8) + payload)


if __name__ == "__main__":
    main()
