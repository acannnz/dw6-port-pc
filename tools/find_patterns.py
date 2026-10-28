"""Cari alamat fungsi save/restore register di image hasil XexDump.

Pola byte diambil dari README XenonRecomp (lihat docs/XenonRecomp-ringkasan.md).

Pakai:
    python tools/find_patterns.py private/default_image.bin
"""
import sys

BASE = 0x82000000  # alamat byte ke-0 file image (dicetak XexDump sebagai base=)

PATTERNS = {
    "restgprlr_14_address": "e9 c1 ff 68",
    "savegprlr_14_address": "f9 c1 ff 68",
    "restfpr_14_address": "c9 cc ff 70",
    "savefpr_14_address": "d9 cc ff 70",
    "restvmx_14_address": "39 60 fe e0 7d cb 60 ce",
    "savevmx_14_address": "39 60 fe e0 7d cb 61 ce",
    "restvmx_64_address": "39 60 fc 00 10 0b 60 cb",
    "savevmx_64_address": "39 60 fc 00 10 0b 61 cb",
}


def find_all(data, needle):
    hits, pos = [], data.find(needle)
    while pos != -1:
        if pos % 4 == 0:  # instruksi PowerPC selalu 4 byte & rata 4
            hits.append(BASE + pos)
        pos = data.find(needle, pos + 1)
    return hits


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    for name, pattern in PATTERNS.items():
        hits = find_all(data, bytes.fromhex(pattern))
        found = ", ".join(f"0x{h:08X}" for h in hits) or "TIDAK DITEMUKAN"
        print(f"{name:<22} {found}")


if __name__ == "__main__":
    main()
