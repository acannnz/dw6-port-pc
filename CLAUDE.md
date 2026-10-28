# Proyek: Port Dynasty Warriors (static recompilation)

## Tujuan
Membuat **native port** sebuah game Dynasty Warriors (Shin Sangoku Musou) agar bisa
dimainkan di luar konsol aslinya, dengan pendekatan **static recompilation** —
meniru model proyek **RanggaCasper/Black-Android**.

## Status: BARU MULAI
Belum ada kode. Langkah sekarang: menyiapkan *build environment* + recompiler tool.

## Pendekatan (BUKAN emulator)
Recompiler menerjemahkan executable game → kode **C++**, lalu dibangun jadi aplikasi native.
Tool-nya beda per konsol asal game:
- **Xbox 360** → **XenonRecomp** (+ XenosRecomp untuk shader). Referensi matang: **Unleashed Recompiled** (hedge-dev).
- **Xbox original** → **XboxXBE-Recomp**. Referensi: **Black-Android** (RanggaCasper).
- **PS2** → **PS2Recomp** (paling eksperimental).

## Keputusan (sudah fix, 2026-10-28)
- **Game: Dynasty Warriors 6 — versi Xbox 360.**
- **Target tahap 1: PC (Windows)** via XenonRecomp, meniru Unleashed Recompiled.
- **Target jangka panjang: Android** (keinginan utama pengguna) — dikerjakan setelah port PC jalan.
- Alternatif yang dicatat: DW3/DW4/DW5 **ada** di Xbox original → jalur Black-Android
  (XboxXBE-Recomp → Android). Opsi cadangan kalau 360 → Android terlalu berat.

## Aturan & ekspektasi (penting, jangan dilewati)
- File game **WAJIB hasil dump dari salinan milik sendiri**. Proyek tidak membundel aset game.
- Recompiler hanya menghasilkan **C++ mentah yang belum jalan**. Bagian **runtime**
  (kernel, Direct3D → Vulkan, input, audio) **ditulis tangan** — ini ~80% kerjaan & paling berat.
- Ini **proyek jangka panjang & bertahap** (ratusan commit kecil), BUKAN sekali jadi.
  Jangan janjikan build playable instan.

## Prasyarat build (install dulu)
- Windows 64-bit, Git, CMake 3.20+, **Clang 18+**, Visual Studio 2022 (toolset clang-cl)
- (kalau target Android, à la Black-Android) Android Studio + NDK + CMake
- Ekstrak XEX dari ISO (untuk 360): **XGDTool**

## Langkah berikutnya
1. ~~Pastikan konsol + target~~ → sudah (lihat "Keputusan").
2. ~~Install prasyarat~~ → sudah (2026-10-28). **Visual Studio 2026** (bukan 2022) di
   `D:\Projek\Visual Studio` (disk C pengguna sempit — install hal besar ke D).
   clang-cl 22.1.3, CMake 4.3.1, Ninja: semua bawaan VS, dipakai lewat `VsDevCmd.bat`.
3. ~~Clone & build recompiler~~ → sudah. `tools/XenonRecomp`, build ulang: `tools/build_xenonrecomp.bat`
   (Ninja + clang-cl, Release; perlu `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` karena CMake 4).
   Hasil: `tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse.exe` & `.../XenonRecomp/XenonRecomp.exe`.
   Ringkasan README (Indonesia): `docs/XenonRecomp-ringkasan.md`.
4. ~~Dump/extract game~~ → sudah (2026-10-28). ISO dump milik pengguna (DW6 Europe, XGD2) di
   `private/`. `default.xex` (XEX2, 5.6 MB) diekstrak dengan `tools/xiso_extract.py`
   (pengganti XGDTool, Python murni). Folder `private/` = file game, **jangan dibagikan/commit**.
   Isi disc: `default.xex`, `LINKDATA.BIN/.IDX` (aset utama ~1.9 GB), `bgm/`, `movie/`, `contents/`.
5. ~~Output C++ pertama~~ → sudah (2026-10-28). Config: `config/DW6.toml` (+ `config/DW6_switch_tables.toml`,
   174 tabel dari XenonAnalyse). Output: `private/ppc/` (47 file, ~75 MB, 10.899 fungsi), log: `private/recomp.log`.
   Alamat ditemukan dengan tool buatan sendiri:
   - `tools/XexDump` (C++, pakai XenonUtils) → `private/default_image.bin` (image terdekripsi, base 0x82000000)
   - `tools/find_patterns.py` → 8 fungsi save/restore register (masing-masing tepat 1 hasil)
   - `tools/find_callers.py` → longjmp 0x82469880 (memanggil RtlUnwind), setjmp 0x82469BA0 (+0x320, sama dgn SWA)
   - Semua tool dibangun lewat `tools/CMakeLists.txt` (induk) dengan `tools/build_xenonrecomp.bat` → `tools/build/`.
   Bug upstream: untuk XEX kompresi "basic", `image.size` > buffer asli → jangan baca sampai `image.size`.

## Masalah terbuka dari recompile pertama (lihat `private/recomp.log`)
- ~~6.029 instruksi belum didukung (28 jenis)~~ → **sudah ditambahkan** (2026-10-28) langsung di
  `tools/XenonRecomp/XenonRecomp/recompiler.cpp` (perubahan lokal, lihat `git -C tools/XenonRecomp diff`;
  belum di-commit/fork). Juga perbaikan RC bit `vcmpgtuh.`. Recompile ulang: 0 unrecognized, 0 RC warning.
  Semua 44 file `private/ppc/*.cpp` lolos `tools/check_ppc_compile.bat` (clang-cl `-fsyntax-only`).
  **Belum diuji kebenaran hasilnya** (perlu unit test per instruksi, mis. via XenonTests/Xenia).
- ~~748 error switch case (30 tabel)~~ → **sudah** (2026-10-28). Penyebab: fungsi tanpa .pdata dipotong
  analyzer di `bctr`. Batas fungsi benar dihitung `tools/fix_switch_functions.py` → `functions = [...]`
  di DW6.toml (29 entri; 3 "fungsi dalam" sengaja dibuang karena lanjutan fungsi luar).
  Diagnosa: `tools/diag_switch.py`.
- ~~2 jump table tak terdeteksi~~ → ditambah manual di akhir `config/DW6_switch_tables.toml`
  (tipe byte-offset, ada 2 `nop` sebelum `bctr` sehingga pola XenonAnalyse gagal). Jumlah kasus (9 & 16)
  ditebak dari isi tabel karena tidak ada `cmplwi` pembatas — 16 mungkin termasuk 2 byte padding.
- **Status sekarang: log XenonRecomp 0 peringatan, 43 file C++ lolos cek compile.**

## Runtime (mulai 2026-10-28) — folder `runtime/`
- Build: `runtime/build_runtime.bat` → `runtime/build/DW6.exe` (clang-cl, Ninja; kode ppc pakai `-O1`).
  Jalankan dari root proyek: `runtime/build/DW6.exe private/default.xex`.
- Memori guest: 4 GB hanya **di-reserve**; commit lewat `GuestCommit` (image, tabel fungsi, stack, PCR,
  setiap alokasi kernel) + **commit otomatis saat disentuh** (vectored exception handler, dicatat di log).
  Alasan: commit 4 GB sekaligus memaksa Windows membesarkan pagefile (disk pengguna sempit).
  Commit turun 4.105 MB → 230 MB. Akses otomatis yang terlihat: baca 0x93010000, tulis 0xBE010000 (selidiki).
- `main.cpp`: memori guest (lihat atas), muat XEX via XenonUtils, isi tabel fungsi
  (`PPC_LOOKUP_FUNC`), r1 = stack guest 0x70000000 (1 MB), r13 = PCR palsu 0x7FF00000
  (PCR+0 → TLS 0x7FF02000, PCR+0x100 → thread 0x7FF01000), lalu panggil entry 0x8236C8C0.
  Crash handler mencetak exception + alamat guest.
- `kernel/import_stubs.cpp` DIBUAT OTOMATIS (`tools/gen_import_stubs.py`): 156 import kernel/XAM,
  semua stub (log 5x per fungsi, r3=0). Implementasi asli → tulis `PPC_FUNC(__imp__X)` di file lain
  di `runtime/kernel/`, lalu jalankan ulang script supaya stub-nya hilang.
- Sudah diimplementasi (2026-10-28):
  - `kernel/memory.cpp`: RangeAllocator first-fit dengan free. Virtual 0x40000000-0x6FFFFFFF,
    fisik 0xA0000000-0xBFFFFFFF (512 MB). Page size dari flag (0x20000000 = 64 KB, 0x40000000 = 16 MB).
    PENTING: game mencari ukuran fisik maksimum dengan alloc+free berulang — free WAJIB bekerja.
  - `kernel/system.cpp`: KeGetCurrentProcessType=1, XexCheckExecutablePrivilege=0, TLS (1 thread),
    critical section no-op (1 thread), RtlInitAnsiString, KeBugCheck/HalReturnToFirmware → `GuestFatal`.
  - Konstanta status pakai prefix `X_STATUS_` (nama `STATUS_*` bentrok dengan makro winnt.h).
- Sudah diimplementasi juga (2026-10-28, lanjutan):
  - `kernel/threads.cpp`: **thread sungguhan** (ExCreateThread → std::thread + PPCContext/stack/PCR
    sendiri; XapiThreadStartup dipanggil kalau ada; CREATE_SUSPENDED + NtResumeThread/KeResumeThread;
    ExTerminateThread lewat exception C++). TLS per thread, critical section sungguhan (CAS di
    OwningThread +0x18), waktu (KeQueryPerformanceFrequency = frekuensi rdtsc terukur, karena `mftb`
    diterjemahkan ke `__rdtsc()`), KeQuerySystemTime.
  - **Affinity → PCR+0x10C (nomor hardware thread 0-5)**: dari CreationFlags>>24 dan KeSetAffinityThread.
    WAJIB: game punya barrier multi-core (sub_8233E830) yang menunggu tiap core menandai byte-nya.
  - **PENTING:** setiap PPCContext baru harus `ctx.fpscr.loadFromHost()`; kalau `csr` = 0, game mematikan
    semua masker exception FP → crash 0xC000008F. Ada juga jaring pengaman di vectored handler.
  - `kernel/objects.cpp`: event/semaphore/thread sebagai DISPATCHER_HEADER di memori guest (SignalState
    +4 dibaca langsung), satu lock + condition variable global; handle table; Nt/Ke wait (single/multiple,
    timeout). Catatan "siapa menunggu apa" dicetak sampler (`[wait] ...`).
  - `kernel/gpu.cpp`: **GPU headless** — command processor membaca ring buffer sampai CP_RB_WPTR
    (MMIO 0x7FC80000 + 0x1C5*4), menjalankan MEM_WRITE / EVENT_WRITE_SHD / INDIRECT_BUFFER / INTERRUPT,
    RPtr writeback; vblank 60 Hz memanggil callback interrupt game; VdSwap = NOP + batas 60 FPS;
    VdQueryVideoMode 1280x720. Spinlock & IRQL juga di sini. Alamat fisik → 0xA0000000 | phys.
  - `kernel/files.cpp`: **filesystem langsung dari ISO** (XDVDFS, 207 entri), path `game:\...`:
    NtCreateFile/NtOpenFile (baca saja), NtReadFile, NtQuery/SetInformationFile.
  - `kernel/audio.cpp`: XAudio render driver — callback dipanggil tiap 5.33 ms; frame audio dibuang.
  - `tools/find_missing_switches.py`: jump table absolut tanpa `cmplwi` yang terlewat XenonAnalyse
    (2 ditemukan & ditambahkan). Tabel fungsi diisi `UnmappedFunction` → panggilan ke alamat tak dikenal
    tercetak (`[fatal] ... ctr=`).
  - `sampler.cpp`: set `DW6_SAMPLE=<detik>` → cetak call stack semua thread (simbol sub_XXXXXXXX dari PDB,
    build pakai `-gline-tables-only` + `/DEBUG`) + daftar tunggu guest.
- Lanjutan (2026-10-28, sesi 3):
  - NtSuspendThread sungguhan (suspend diri = tidur; thread lain berhenti di awal fungsi tunggu).
  - `patches.cpp`: fungsi guest bisa ditimpa (`PPC_FUNC(sub_X)` strong vs weak hasil recompile;
    asli via `extern "C" PPC_FUNC(__imp__sub_X)`). **Video intro dilewati**: sub_82382B00 (ambil frame
    video) → 0x16660026 "video habis". Pipeline WMV macet karena worker (thread 17/19) suspend menunggu
    Start() (vtable 0x8202B9C8 index 21) yang tak pernah dipanggil; `DW6_PLAY_MOVIES=1` untuk mencoba.
  - XenonRecomp: + `vcmpbfp(128)` dan `vpkd3d128` tipe 3 (FLOAT16_2) → 0 `__builtin_debugtrap` tersisa.
  - Crash handler & sampler mencetak call stack + file:baris C++ hasil recompile (cari komentar PPC di situ).
  - `kernel/input.cpp`: XamInputGetState/Capabilities/SetState via XInput (layout tombol sama), cadangan
    keyboard (WASD/panah/JKUI/Enter...). NtAllocateVirtualMemory hanya commit bila MEM_COMMIT.
  - GPU: register file + SCRATCH_REG fence writeback (UMSK 0x1DC, ADDR 0x1DD, REG0 0x578), REG_RMW,
    COND_WRITE, RPTR di MMIO 0x1C4.
- **Memori fisik di-alias (2026-10-28):** 0xA0000000/0xC0000000/0xE0000000 = 3 view dari SATU file mapping
  512 MB (`ReserveGuestMemory` di main.cpp). Game menulis lewat satu jendela & membaca lewat jendela lain.
  Ini yang dulu membuat state block D3D terbaca nol → loop sub_823253B8 tanpa akhir. GuestCommit/Decommit
  mengabaikan alamat >= 0xA0000000.
- **Status sekarang:** 3 video dilewati, game loop berjalan stabil (3900+ frame / 90 s, ~260 MB), membaca
  BGM, membuat konteks XMA. **Belum ada jendela/gambar** (GPU headless), **belum ada suara**.
  Tanda masalah tersisa: 1x tulis ke guest 0x00000000 (null pointer) setelah BGM dibuka.
- **Berikutnya (urut):** (1) RENDER — keputusan arsitektur: pakai ulang GPU Xenia (BSD-3: command processor
  + backend D3D12/Vulkan + penerjemah shader) atau tulis renderer sendiri à la Unleashed (XenosRecomp);
  (2) audio: XMA decoder + output (XAudio2/WASAPI); (3) save data (NtCreateFile tulis, XamContent*);
  (4) pemutar video (worker WMV menunggu Start()).

## Catatan tentang pengguna (untuk kalibrasi penjelasan)
Developer web (PHP/Laravel/CI3, JavaScript). Baru di **C/C++, assembly, dan low-level** —
jelaskan konsep low-level pelan-pelan. Bahasa Inggris masih dasar; boleh bantu
menerjemahkan dokumen/README berbahasa Inggris.
## Proyek lain yang ditemukan (2026-10-28) — CEK INI DULU sebelum lanjut
Pengguna memutuskan BERHENTI dulu dan menunggu port yang sudah jadi. Kalau dilanjutkan:
- **DW6 belum punya port siap pakai** (per 2026-10-28).
- **ReXGlue** (github.com/rexglue/rexglue-sdk): runtime + toolkit recomp Xbox 360 berbasis Xenia
  (kernel, GPU D3D12/Vulkan "xenos" plugin, audio). Windows/Linux, belum Android. Jalur tercepat untuk DW6:
  pindahkan proyek ini ke ReXGlue, pakai **dw5-empires-recomp** sebagai cetakan.
- **dw5-empires-recomp** (github.com/TheSlackbringer/dw5-empires-recomp): DW5 Empires Xbox 360 PAL → PC via
  ReXGlue; sudah ada render, BGM/suara, pemutar WMV. Game Koei/Omega Force seangkatan DW6.
- **DW5-Android** (github.com/datfooldive/DW5-Android): DW5 Xbox ORIGINAL (USA) → Android via XboxXBE-Recomp.
  ISO DW5 milik pengguna adalah versi PS2 Europe → tidak bisa dipakai.
- **dw5xl-recomp**: DW5 Xtreme Legends PS2 → PC via PS2Recomp (game berbeda dari DW5 biasa).
- Pelajaran: Black-Android cepat karena runtime XboxXBE-Recomp + port PC Black sudah jadi (TGP482);
  XenonRecomp TIDAK menyediakan runtime. Selalu cari proyek/runtime yang sudah ada sebelum membangun dari nol.
