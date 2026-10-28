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

// Error fatal dari sisi game (mis. KeBugCheck): cetak lalu keluar supaya terlihat jelas di log.
[[noreturn]] inline void GuestFatal(const char* what, const PPCContext& ctx)
{
    printf("[fatal] %s (r3=0x%llX, r4=0x%llX) dari lr=0x%llX\n", what, ctx.r3.u64, ctx.r4.u64, ctx.lr);
    fflush(stdout);
    exit(2);
}
