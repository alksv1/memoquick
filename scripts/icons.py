"""Generate the small, code-drawn native icon using only Python's standard library."""
import pathlib
import struct
import zlib

directory = pathlib.Path(__file__).resolve().parent.parent / "assets"
size = 64
rows = []
for y in range(size):
    row = bytearray([0])
    for x in range(size):
        line = (18 <= x <= 45 and 17 <= y <= 20) or (18 <= x <= 42 and 28 <= y <= 31) or (18 <= x <= 35 and 39 <= y <= 42)
        row.extend((250, 251, 246, 255) if line else (40, 100, 77, 255) if 8 <= x < 56 and 8 <= y < 56 else (0, 0, 0, 0))
    rows.append(bytes(row))


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
(directory / "icon.png").write_bytes(png)
(directory / "icon.ico").write_bytes(struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(png), 22) + png)
