# Ringkasan README XenonRecomp (Bahasa Indonesia)

Sumber: `tools/XenonRecomp/README.md`. Ini ringkasan, bukan terjemahan kata per kata.

## Apa itu
Alat untuk mengubah file program Xbox 360 (`.xex`) jadi kode **C++**, yang lalu bisa
di-compile untuk platform lain (PC, dan secara teori ARM64/Android).

**Peringatan dari pembuatnya:** alat ini **tidak menyediakan runtime**. Hasil C++-nya
tidak akan jalan tanpa runtime buatan kita sendiri. Membuat game-nya jalan = tanggung jawab kita.

## Konsep penting (versi sederhana)
- **CPU Xbox 360 = PowerPC (PPC).** Setiap fungsi game diterjemahkan jadi fungsi C++
  `sub_XXXXXXXX(ctx, base)`:
  - `ctx` = "kotak" berisi semua register CPU (variabel internal CPU).
  - `base` = alamat awal memori game (Xbox 360 pakai pointer 32-bit).
- **Endianness:** Xbox 360 menyimpan angka "terbalik" (big-endian), PC/HP "normal"
  (little-endian). Recompiler otomatis membalik urutan byte tiap kali baca/tulis memori.
- **ARM64 didukung** lewat library SIMDe → kabar baik untuk target Android nanti.
- **Belum didukung:** MMIO (akses hardware langsung, mis. audio XMA) dan exception C++.

## Dua program utama
1. **XenonAnalyse** — memindai `.xex`, mencari *jump table* (tabel lompatan untuk `switch`).
   ```
   XenonAnalyse default.xex jump_tables.toml
   ```
   Deteksinya dibuat untuk Sonic Unleashed; game lain (DW6) **mungkin perlu penyesuaian**.
2. **XenonRecomp** — menerjemahkan `.xex` → C++, dikendalikan file konfigurasi `.toml`.
   ```
   XenonRecomp config.toml XenonUtils/ppc_context.h
   ```

## Isi file konfigurasi `.toml` yang harus kita cari sendiri untuk DW6
| Bagian | Artinya |
|---|---|
| `file_path` | lokasi `default.xex` DW6 |
| `patch_file_path` | file update (`.xexp`), kalau ada title update |
| `out_directory_path` | folder output C++ (harus dibuat dulu) |
| `switch_table_file_path` | hasil dari XenonAnalyse |
| `restgprlr_14_address` dkk. | **alamat 8 fungsi simpan/pulihkan register** di dalam xex DW6. Dicari pakai pola byte (mis. `e9 c1 ff 68`). Wajib diisi. |
| `longjmp_address` / `setjmp_address` | alamat fungsi longjmp/setjmp, kalau game memakainya |
| `functions` | batas fungsi manual, kalau analisis otomatis gagal |
| `invalid_instructions` | data yang bukan kode, supaya dilewati |
| `[[midasm_hook]]` | titik "sadap" di tengah kode untuk menyisipkan kode kita |

Contoh konfigurasi lengkap: `SWA.toml` di repo **UnleashedRecomp**.

## Optimasi
Ada opsi `skip_lr`, `ctr_as_local`, dll. **Biarkan semua `false`** sampai port sudah jalan.

## Build
- CMake 3.20+ dan **Clang 18+** (compiler lain tidak disarankan).
- Di Windows: pakai toolset **clang-cl**, buka folder proyek di Visual Studio (CMake integration).
