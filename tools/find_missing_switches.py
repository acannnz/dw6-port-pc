"""Cari jump table "absolut" yang terlewat XenonAnalyse dan tambahkan ke file switch table.

XenonAnalyse hanya mengenali switch yang diawali pengecekan batas (cmplwi + bgt). Ada switch
tanpa pengecekan itu; bctr-nya lalu diterjemahkan sebagai pemanggilan fungsi tidak langsung
dan crash saat jalan. Pola yang dicari (alamat tabel di r12):
    lis   r12, HI
    addi  r12, r12, LO
    rlwinm rT, rIdx, 2, 0, 29      (indeks * 4)
    lwzx  rX, r12, rT
    mtctr rX
    bctr
Jumlah kasus: dari cmplwi rIdx,N sebelumnya kalau ada; kalau tidak, dihitung selama isi tabel
masih alamat kode di dalam fungsi yang sama (.pdata) dan tabel belum menabrak label pertamanya.

Pakai:
    python tools/find_missing_switches.py <image.bin> <switch_tables.toml> [--write]
"""
import re
import struct
import sys

BASE = 0x82000000
TEXT_START, TEXT_END = 0x820E0000, 0x820E0000 + 0x397294
PDATA_BASE, PDATA_SIZE = 0x820C7A00, 0xF228
BCTR = 0x4E800420


def word(data, addr):
    return struct.unpack_from(">I", data, addr - BASE)[0]


def load_pdata(data):
    funcs = []
    for i in range(0, PDATA_SIZE, 8):
        start, packed = struct.unpack_from(">II", data, PDATA_BASE - BASE + i)
        if start:
            funcs.append((start, start + ((packed >> 8) & 0x3FFFFF) * 4))
    return sorted(funcs)


def containing(funcs, addr):
    for start, end in funcs:
        if start <= addr < end:
            return start, end
    return None


def match_absolute(data, bctr):
    """Kembalikan (alamat_lis, alamat_tabel, reg_indeks) kalau pola cocok, selain itu None."""
    mtctr, lwzx, rlwinm, addi, lis = (word(data, bctr - 4 * k) for k in range(1, 6))
    if mtctr & 0xFC1FFFFF != 0x7C0903A6:
        return None
    rx = (mtctr >> 21) & 31
    # lwzx rX, r12, rT  (opcode 31, xo 23)
    if lwzx >> 26 != 31 or (lwzx >> 1) & 0x3FF != 23 or (lwzx >> 21) & 31 != rx or (lwzx >> 16) & 31 != 12:
        return None
    rt = (lwzx >> 11) & 31
    # rlwinm rT, rIdx, 2, 0, 29
    if rlwinm >> 26 != 21 or (rlwinm >> 16) & 31 != rt or (rlwinm >> 11) & 31 != 2 or (rlwinm >> 1) & 0x3FF != (0 << 5 | 29):
        return None
    index_reg = (rlwinm >> 21) & 31
    # addi r12, r12, LO ; lis r12, HI
    if addi >> 16 != 0x398C or lis >> 16 != 0x3D80:
        return None
    lo = addi & 0xFFFF
    lo = lo - 0x10000 if lo & 0x8000 else lo
    table = ((lis & 0xFFFF) << 16) + lo
    return bctr - 20, table & 0xFFFFFFFF, index_reg


def bound_from_cmplwi(data, lis_addr, index_reg):
    for k in range(1, 12):
        insn = word(data, lis_addr - 4 * k)
        # cmplwi crN, rIdx, N  (opcode 10, L=0)
        if insn >> 26 == 10 and (insn >> 16) & 31 == index_reg and not (insn >> 21) & 1:
            return (insn & 0xFFFF) + 1
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    toml_path = sys.argv[2]
    known = {int(h, 16) for h in re.findall(r"base = (0x[0-9A-Fa-f]+)", open(toml_path).read())}
    funcs = load_pdata(data)

    found = []
    for bctr in range(TEXT_START + 20, TEXT_END, 4):
        if word(data, bctr) != BCTR:
            continue
        m = match_absolute(data, bctr)
        if m is None or m[0] in known:
            continue
        lis_addr, table, index_reg = m
        fn = containing(funcs, bctr)
        count = bound_from_cmplwi(data, lis_addr, index_reg)
        labels = []
        limit = count if count else 512
        while len(labels) < limit:
            entry = word(data, table + len(labels) * 4)
            if count is None:
                inside = fn and fn[0] <= entry < fn[1]
                if not inside or (labels and table + len(labels) * 4 >= min(labels)):
                    break
            labels.append(entry)
        if not labels:
            print(f"# PERIKSA 0x{bctr:08X}: tabel kosong/tidak terbaca")
            continue
        found.append((lis_addr, index_reg, labels, count is not None))

    lines = ["", "# ---- DITAMBAHKAN tools/find_missing_switches.py (switch absolut tanpa cmplwi) ----"]
    for lis_addr, index_reg, labels, bounded in found:
        note = "" if bounded else f"  # jumlah kasus ditebak dari isi tabel ({len(labels)})"
        lines += ["[[switch]]", f"base = 0x{lis_addr:08X}{note}", f"r = {index_reg}", "labels = ["]
        lines += [f"    0x{label:08X}," for label in labels]
        lines += ["]", ""]

    print(f"{len(found)} switch absolut belum terdaftar ditemukan")
    for lis_addr, index_reg, labels, bounded in found[:10]:
        print(f"  0x{lis_addr:08X} r{index_reg} {len(labels)} kasus{'' if bounded else ' (ditebak)'}")
    if "--write" in sys.argv and found:
        with open(toml_path, "a") as f:
            f.write("\n".join(lines))
        print(f"ditulis ke {toml_path}")


if __name__ == "__main__":
    main()
