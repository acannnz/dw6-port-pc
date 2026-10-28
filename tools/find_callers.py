"""Cari fungsi yang memanggil alamat tertentu (instruksi `bl`), memakai tabel .pdata.

Berguna untuk mencari longjmp (fungsi yang memanggil RtlUnwind) dan setjmp.

Pakai:
    python tools/find_callers.py <image.bin> <pdata_base> <pdata_size> <alamat_target>
Contoh:
    python tools/find_callers.py private/default_image.bin 0x820C7A00 0xF228 0x825A0100
"""
import struct
import sys

BASE = 0x82000000


def load_pdata(data, base, size):
    """Tiap entri .pdata = 8 byte: alamat awal fungsi + panjang (dalam jumlah instruksi)."""
    funcs = []
    off = base - BASE
    for i in range(0, size, 8):
        start, packed = struct.unpack_from(">II", data, off + i)
        if start == 0:
            continue
        length = ((packed >> 8) & 0x3FFFFF) * 4
        funcs.append((start, length))
    return sorted(funcs)


def containing(funcs, addr):
    for start, length in funcs:
        if start <= addr < start + length:
            return start, length
    return None


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    pdata_base, pdata_size, target = (int(x, 16) for x in sys.argv[2:5])
    funcs = load_pdata(data, pdata_base, pdata_size)
    print(f"{len(funcs)} fungsi di .pdata")

    for off in range(0, len(data) - 3, 4):
        insn = struct.unpack_from(">I", data, off)[0]
        # bl = opcode 18, AA=0, LK=1
        if insn >> 26 != 18 or insn & 3 != 1:
            continue
        disp = insn & 0x03FFFFFC
        if disp & 0x02000000:
            disp -= 0x04000000
        addr = BASE + off
        if addr + disp != target:
            continue
        func = containing(funcs, addr)
        where = f"di fungsi 0x{func[0]:08X} (size 0x{func[1]:X})" if func else "di luar .pdata"
        print(f"bl dari 0x{addr:08X} {where}")
        if func:
            idx = funcs.index(func)
            for nstart, nlen in funcs[idx + 1 : idx + 4]:
                print(f"    fungsi sesudahnya: 0x{nstart:08X} (size 0x{nlen:X})")


if __name__ == "__main__":
    main()
