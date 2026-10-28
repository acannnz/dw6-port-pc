"""Buat runtime/kernel/import_stubs.cpp: stub untuk setiap fungsi import kernel/XAM
yang dipakai game tapi belum diimplementasikan di runtime/kernel/*.cpp.

Stub hanya mencatat pemanggilan (lewat LogStubCall) lalu mengembalikan r3 = 0.
Jalankan ulang setiap kali menambah implementasi asli atau setelah recompile.

Pakai:
    python tools/gen_import_stubs.py private/ppc/ppc_recomp_shared.h runtime/kernel
"""
import pathlib
import re
import sys

OUTPUT_NAME = "import_stubs.cpp"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    shared_header, kernel_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    imports = re.findall(r"PPC_EXTERN_FUNC\(__imp__(\w+)\);", shared_header.read_text())

    implemented = set()
    for path in kernel_dir.glob("*.cpp"):
        if path.name != OUTPUT_NAME:
            implemented |= set(re.findall(r"PPC_FUNC\(__imp__(\w+)\)", path.read_text()))

    lines = [
        "// FILE INI DIBUAT OTOMATIS oleh tools/gen_import_stubs.py -- jangan diedit manual.",
        "// Implementasi asli ditulis di file lain di folder ini; jalankan ulang script-nya.",
        '#include "../kernel.h"',
        "",
    ]
    stubs = [name for name in imports if name not in implemented]
    for name in stubs:
        lines.append(f'PPC_FUNC(__imp__{name}) {{ LogStubCall("{name}", ctx); ctx.r3.u64 = 0; }}')

    (kernel_dir / OUTPUT_NAME).write_text("\n".join(lines) + "\n")
    print(f"{len(imports)} import, {len(implemented)} sudah diimplementasi, {len(stubs)} stub dibuat")


if __name__ == "__main__":
    main()
