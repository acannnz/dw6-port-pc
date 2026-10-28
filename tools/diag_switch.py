"""Diagnosa error "Switch case ... is trying to jump outside function" dari XenonRecomp.

Untuk tiap switch yang error, bandingkan:
  - fungsi menurut recompiler (dari ppc_func_mapping.cpp: awal fungsi s/d awal fungsi berikutnya)
  - fungsi menurut tabel .pdata (kalau ada)
  - rentang label switch (dari file switch table TOML)

Pakai:
    python tools/diag_switch.py <image.bin> <recomp.log> <switch_tables.toml> <ppc_func_mapping.cpp>
"""
import re
import struct
import sys

BASE = 0x82000000
PDATA_BASE, PDATA_SIZE = 0x820C7A00, 0xF228


def load_pdata(data):
    funcs = []
    for i in range(0, PDATA_SIZE, 8):
        start, packed = struct.unpack_from(">II", data, PDATA_BASE - BASE + i)
        if start:
            funcs.append((start, ((packed >> 8) & 0x3FFFFF) * 4))
    return sorted(funcs)


def load_switches(path):
    switches, cur = {}, None
    for line in open(path):
        m = re.match(r"\s*base = (0x[0-9A-F]+)", line)
        if m:
            cur = int(m.group(1), 16)
            switches[cur] = []
            continue
        for h in re.findall(r"0x[0-9A-F]+", line):
            if cur is not None and not line.strip().startswith(("r =", "default")):
                switches[cur].append(int(h, 16))
    return switches


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    failing = sorted({int(m, 16) for m in re.findall(r"Switch case at ([0-9A-F]+)", open(sys.argv[2]).read())})
    switches = load_switches(sys.argv[3])
    starts = sorted(int(h, 16) for h in re.findall(r"\{ (0x[0-9A-F]+), ", open(sys.argv[4]).read()))
    pdata = load_pdata(data)

    for sw in failing:
        # alamat di log = instruksi bctr; `base` di TOML = awal urutan instruksi switch (sedikit sebelumnya)
        labels = switches[max(b for b in switches if b <= sw)]
        lo, hi = min(labels), max(labels)
        rstart = max(s for s in starts if s <= sw)
        rend = min((s for s in starts if s > sw), default=0)
        p = next(((s, n) for s, n in pdata if s <= sw < s + n), None)
        pinfo = f"pdata 0x{p[0]:08X}-0x{p[0] + p[1]:08X}" if p else "pdata -"
        # fungsi recompiler yang memotong rentang label
        cut = [s for s in starts if lo <= s <= hi and s != rstart]
        print(f"switch 0x{sw:08X} label 0x{lo:08X}-0x{hi:08X} | recomp 0x{rstart:08X}-0x{rend:08X} | {pinfo} | dipotong oleh {len(cut)} fungsi: {', '.join(f'0x{c:08X}' for c in cut[:4])}")


if __name__ == "__main__":
    main()
