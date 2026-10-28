"""Hitung batas fungsi yang benar untuk switch yang error, untuk `functions = [...]` di DW6.toml.

Masalah: fungsi tanpa entri .pdata dianalisis XenonAnalyse sampai instruksi `bctr` pertama,
jadi label-label switch-nya jatuh "di luar fungsi". Script ini menelusuri alur kode
(cabang + label switch) mulai dari awal fungsi untuk mencari ujung fungsi yang sebenarnya.

Pakai:
    python tools/fix_switch_functions.py <image.bin> <recomp.log> <switch_tables.toml> <ppc_func_mapping.cpp>
"""
import re
import struct
import sys

BASE = 0x82000000
PDATA_BASE, PDATA_SIZE = 0x820C7A00, 0xF228
BLR = 0x4E800020
BCTR = 0x4E800420


def word(data, addr):
    return struct.unpack_from(">I", data, addr - BASE)[0]


def branch_target(insn, addr):
    op = insn >> 26
    if op == 18:  # b / bl
        disp = insn & 0x03FFFFFC
        if disp & 0x02000000:
            disp -= 0x04000000
    elif op == 16:  # bc
        disp = insn & 0xFFFC
        if disp & 0x8000:
            disp -= 0x10000
    else:
        return None
    return disp if insn & 2 else addr + disp  # AA bit = absolute


def is_unconditional_bc(insn):
    # BO = 1z1zz -> branch always
    return (insn >> 26) == 16 and ((insn >> 21) & 0x14) == 0x14


def load_switches(path):
    switches, cur = {}, None
    for line in open(path):
        m = re.match(r"\s*base = (0x[0-9A-F]+)", line)
        if m:
            cur = int(m.group(1), 16)
            switches[cur] = []
        elif cur is not None and not line.strip().startswith(("r =", "default")):
            switches[cur] += [int(h, 16) for h in re.findall(r"0x[0-9A-F]+", line)]
    return switches


def load_pdata_starts(data):
    starts = set()
    for i in range(0, PDATA_SIZE, 8):
        start = struct.unpack_from(">I", data, PDATA_BASE - BASE + i)[0]
        if start:
            starts.add(start)
    return starts


def load_bl_targets(data, text_start, text_end):
    targets = set()
    for addr in range(text_start, text_end, 4):
        insn = word(data, addr)
        if insn >> 26 == 18 and insn & 1:
            targets.add(branch_target(insn, addr))
    return targets


def analyze(data, start, switch_by_bctr, hard_limit):
    """Telusuri blok kode dari `start`; kembalikan alamat setelah instruksi terakhir yang terjangkau."""
    todo, seen, end = [start], set(), start
    while todo:
        addr = todo.pop()
        while start <= addr < hard_limit and addr not in seen:
            seen.add(addr)
            end = max(end, addr + 4)
            insn = word(data, addr)
            if insn == BLR:
                break
            if insn == BCTR:
                todo += switch_by_bctr.get(addr, [])
                break
            target = branch_target(insn, addr)
            if target is not None and not insn & 1:  # cabang tanpa link (bukan pemanggilan fungsi)
                if insn >> 26 == 18 or is_unconditional_bc(insn):
                    if start <= target < hard_limit:
                        todo.append(target)  # lompatan di dalam fungsi
                    break  # selain itu: tail call ke fungsi lain
                if start <= target < hard_limit:
                    todo.append(target)
            addr += 4
    return end


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    failing = sorted({int(m, 16) for m in re.findall(r"Switch case at ([0-9A-F]+)", open(sys.argv[2]).read())})
    switches = load_switches(sys.argv[3])
    starts = sorted(int(h, 16) for h in re.findall(r"\{ (0x[0-9A-F]+), ", open(sys.argv[4]).read()))
    pdata_starts = load_pdata_starts(data)
    bl_targets = load_bl_targets(data, 0x820E0000, 0x820E0000 + 0x397294)

    # Peta: alamat bctr -> label. Instruksi bctr ada sesudah `base` switch di TOML.
    switch_by_bctr = {}
    for base, labels in switches.items():
        addr = base
        while word(data, addr) != BCTR:
            addr += 4
        switch_by_bctr[addr] = labels

    # Fungsi "asli" yang pasti tidak boleh ditelan: punya .pdata atau dipanggil dengan bl.
    real_starts = sorted(pdata_starts | bl_targets)

    results = []
    for sw in failing:
        fn_start = max(s for s in starts if s <= sw)
        hard_limit = min((s for s in real_starts if s > fn_start), default=fn_start + 0x10000)
        end = analyze(data, fn_start, switch_by_bctr, hard_limit)
        labels = switch_by_bctr[sw]
        ok = all(fn_start <= l < end for l in labels)
        results.append((fn_start, end, ok, hard_limit))

    # Gabungkan hasil yang sama (beberapa switch dalam satu fungsi).
    print("functions = [")
    for fn_start, end, ok, hard_limit in sorted(set(results)):
        note = "" if ok else f"  # PERIKSA: label tidak tercakup (batas 0x{hard_limit:08X})"
        print(f"    {{ address = 0x{fn_start:08X}, size = 0x{end - fn_start:X} }},{note}")
    print("]")


if __name__ == "__main__":
    main()
