#pragma once

// Header bersama untuk semua implementasi fungsi kernel/XAM Xbox 360 di runtime.
//
// Konvensi pemanggilan Xbox 360 (PowerPC): argumen ke-1..8 ada di r3..r10,
// nilai kembalian di r3. Pointer guest = alamat 32-bit; isinya big-endian.
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ppc_config.h>
#include <ppc_context.h>

// Kode status NT yang sering dipakai.
constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;

// Mencatat pemanggilan fungsi import yang belum diimplementasikan (lihat kernel/import_stubs.cpp).
void LogStubCall(const char* name, const PPCContext& ctx);

// Memori guest hanya di-reserve (4 GB ruang alamat) di main.cpp; area yang dipakai harus
// di-commit dulu supaya bisa dibaca/ditulis. Akses ke area yang belum di-commit = crash
// dengan alamat guest tercetak (membantu debugging). Didefinisikan di kernel/memory.cpp.
bool GuestCommit(uint8_t* base, uint32_t address, uint32_t size);
void GuestDecommit(uint8_t* base, uint32_t address, uint32_t size);

// Alokasi kecil untuk struktur milik "kernel" (objek thread, event, stack, PCR) di area
// 0x71000000-0x7FEFFFFF. Hasil sudah di-commit dan diisi nol. 0 kalau habis.
uint32_t GuestKernelAlloc(uint8_t* base, uint32_t size, uint32_t alignment = 0x10);

// Baca/tulis memori guest (otomatis membalik byte big-endian <-> little-endian).
inline uint16_t GuestLoadU16(uint8_t* base, uint32_t address)
{
    return __builtin_bswap16(*reinterpret_cast<uint16_t*>(base + address));
}

inline uint32_t GuestLoadU32(uint8_t* base, uint32_t address)
{
    return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + address));
}

inline void GuestStoreU16(uint8_t* base, uint32_t address, uint16_t value)
{
    *reinterpret_cast<uint16_t*>(base + address) = __builtin_bswap16(value);
}

inline void GuestStoreU32(uint8_t* base, uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(base + address) = __builtin_bswap32(value);
}

inline uint64_t GuestLoadU64(uint8_t* base, uint32_t address)
{
    return __builtin_bswap64(*reinterpret_cast<uint64_t*>(base + address));
}

inline void GuestStoreU64(uint8_t* base, uint32_t address, uint64_t value)
{
    *reinterpret_cast<uint64_t*>(base + address) = __builtin_bswap64(value);
}

// --- Thread (kernel/threads.cpp) ---------------------------------------------
struct GuestThreadState
{
    uint32_t id = 0;
    uint32_t threadObject = 0; // KTHREAD di memori guest (juga jadi "pemilik" critical section)
    uint32_t pcr = 0;          // nilai r13 thread ini
    uint32_t tlsValues[64] = {}; // slot KeTlsAlloc
};

// Thread guest yang sedang berjalan di thread host ini.
GuestThreadState& CurrentGuestThread();

// Siapkan PCR/TLS/objek thread + stack untuk thread utama, lalu isi r1/r13 di ctx.
void InitMainGuestThread(uint8_t* base, PPCContext& ctx);
// Sama, untuk thread host milik runtime yang perlu memanggil kode guest (mis. callback interrupt GPU).
void InitHostThreadAsGuest(uint8_t* base, PPCContext& ctx, uint32_t stackSize);

// Tidur selama thread ini di-suspend (NtSuspendThread dari thread lain). Dipanggil di awal fungsi tunggu.
void GuestSuspendCheckpoint();

// Frekuensi penghitung waktu (instruksi mftb diterjemahkan jadi __rdtsc()).
uint64_t TimestampFrequency();

// --- Objek dispatcher (kernel/objects.cpp) -----------------------------------
// Event/semaphore/thread disimpan sebagai DISPATCHER_HEADER di memori guest:
//   +0 u8 Type, +4 s32 SignalState, +8 daftar tunggu (tidak dipakai). Semaphore: +16 s32 Limit.
constexpr uint8_t DISPATCHER_EVENT_NOTIFICATION = 0;
constexpr uint8_t DISPATCHER_EVENT_SYNCHRONIZATION = 1;
constexpr uint8_t DISPATCHER_SEMAPHORE = 5;
constexpr uint8_t DISPATCHER_THREAD = 6;

uint32_t CreateGuestHandle(uint32_t object);
uint32_t GuestHandleToObject(uint32_t handle); // 0 kalau tidak ada
// Set SignalState objek lalu bangunkan semua thread yang sedang menunggu.
void SignalGuestObject(uint8_t* base, uint32_t object, int32_t state);

// --- Sistem file (kernel/files.cpp) -------------------------------------------
// Pasang ISO disc game supaya file "game:\..." bisa dibaca.
bool MountIso(const char* path);

constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;

// Error fatal dari sisi game (mis. KeBugCheck): cetak lalu keluar supaya terlihat jelas di log.
[[noreturn]] inline void GuestFatal(const char* what, const PPCContext& ctx)
{
    printf("[fatal] %s (r3=0x%llX, r4=0x%llX) dari lr=0x%llX\n", what, ctx.r3.u64, ctx.r4.u64, ctx.lr);
    fflush(stdout);
    exit(2);
}
