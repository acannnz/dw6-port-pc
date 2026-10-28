"""Daftar isi / ekstrak file dari ISO disc Xbox 360 (filesystem XDVDFS).

Pengganti sederhana XGDTool, tanpa perlu download apa pun.

Pakai:
    python tools/xiso_extract.py list    <file.iso>
    python tools/xiso_extract.py extract <file.iso> <path/di/disc> <file_output>

Contoh:
    python tools/xiso_extract.py extract private/game.iso default.xex private/default.xex
"""
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Offset awal partisi game untuk tiap jenis disc (XGD1 / XGD2 / XGD3).
PARTITION_OFFSETS = [0x0, 0xFD90000, 0x2080000, 0x18300000]
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("Bukan ISO Xbox: tanda MICROSOFT*XBOX*MEDIA tidak ditemukan.")


def read_root(f, base):
    f.seek(base + 32 * SECTOR + len(MAGIC))
    root_sector, root_size = struct.unpack("<II", f.read(8))
    return root_sector, root_size


def walk_dir(f, base, sector, size, prefix=""):
    """Direktori XDVDFS = pohon biner; tiap entri punya offset anak kiri & kanan."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, name_len = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF:  # padding, bukan entri
            continue
        name = data[off + 14 : off + 14 + name_len].decode("latin-1")
        path = prefix + name
        is_dir = bool(attr & ATTR_DIRECTORY)
        yield path, start, length, is_dir
        if is_dir:
            yield from walk_dir(f, base, start, length, path + "/")
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        raise SystemExit(__doc__)
    cmd, iso = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_partition(f)
        entries = list(walk_dir(f, base, *read_root(f, base)))

        if cmd == "list":
            for path, _, length, is_dir in sorted(entries):
                print(f"{'<DIR>' if is_dir else length:>12}  {path}")
            return

        if len(sys.argv) != 5:
            raise SystemExit(__doc__)
        wanted, out_path = sys.argv[3].replace("\\", "/").lower(), sys.argv[4]
        for path, start, length, is_dir in entries:
            if path.lower() == wanted and not is_dir:
                f.seek(base + start * SECTOR)
                with open(out_path, "wb") as out:
                    remaining = length
                    while remaining:
                        chunk = f.read(min(remaining, 1 << 20))
                        out.write(chunk)
                        remaining -= len(chunk)
                print(f"OK: {path} ({length} byte) -> {out_path}")
                return
        raise SystemExit(f"File '{sys.argv[3]}' tidak ada di disc.")


if __name__ == "__main__":
    main()
