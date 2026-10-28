// DW6 runtime -- tahap 1: siapkan memori guest, muat default.xex, panggil titik awal game.
//
// Istilah:
//   guest = dunia Xbox 360 (alamat 32-bit, big-endian)
//   host  = PC ini (Windows x64)
//   base  = pointer host ke awal 4 GB memori guest; alamat guest X ada di base + X

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#define NOMINMAX
#include <windows.h>

#include <file.h>
#include <image.h>

#include "kernel.h"

// Tata letak memori guest yang kita pilih sendiri (di luar area image game 0x82000000+).
constexpr uint32_t GUEST_STACK_BASE = 0x70000000;
constexpr uint32_t GUEST_STACK_SIZE = 0x100000; // 1 MB
constexpr uint32_t GUEST_PCR = 0x7FF00000;      // Processor Control Region, ditunjuk register r13
constexpr uint32_t GUEST_THREAD = 0x7FF01000;   // objek thread palsu
constexpr uint32_t GUEST_TLS = 0x7FF02000;      // Thread Local Storage

static uint8_t* g_base = nullptr;

static void StoreGuestU32(uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(g_base + address) = __builtin_bswap32(value);
}

void LogStubCall(const char* name, const PPCContext& ctx)
{
    // Cetak maksimal 5 kali per fungsi supaya log tidak banjir.
    static std::unordered_map<const char*, int> counts;
    if (++counts[name] <= 5)
    {
        printf("[stub] %s(r3=0x%llX, r4=0x%llX, r5=0x%llX) dari lr=0x%llX\n",
            name, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.lr);
        fflush(stdout);
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    const auto* record = info->ExceptionRecord;
    printf("\n[crash] exception 0x%08lX di host 0x%p\n", record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_base != nullptr)
    {
        const auto address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
        const auto guest = address - reinterpret_cast<uintptr_t>(g_base);
        printf("[crash] akses %s alamat host 0x%llX (guest 0x%llX)\n",
            record->ExceptionInformation[0] ? "tulis" : "baca", (unsigned long long)address, (unsigned long long)guest);
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Commit otomatis: kalau game menyentuh halaman guest yang belum di-commit, commit halaman itu
// (isinya nol) lalu lanjutkan. Alamatnya dicatat supaya akses "liar" tetap kelihatan di log.
static LONG WINAPI CommitOnTouchHandler(EXCEPTION_POINTERS* info)
{
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_base == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    const auto start = reinterpret_cast<uintptr_t>(g_base);
    if (address < start || address >= start + PPC_MEMORY_SIZE)
        return EXCEPTION_CONTINUE_SEARCH; // bukan memori guest: crash sungguhan

    const auto guestPage = uint32_t(address - start) & ~0xFFFu;
    if (!GuestCommit(g_base, guestPage, 0x1000))
        return EXCEPTION_CONTINUE_SEARCH;

    static int logged = 0;
    if (++logged <= 20)
        printf("[mem] commit otomatis halaman guest 0x%08X (%s)\n", guestPage, record->ExceptionInformation[0] ? "tulis" : "baca");
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool LoadXex(const char* path, size_t& entryPoint)
{
    const auto file = LoadFile(path);
    if (file.empty())
    {
        printf("Gagal membaca %s\n", path);
        return false;
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        printf("Gagal mem-parse %s\n", path);
        return false;
    }

    // Salin sampai akhir section terakhir (lihat catatan bug image.size di tools/XexDump).
    size_t size = 0;
    for (const auto& section : image.sections)
        size = std::max(size, section.base + section.size - image.base);

    // Commit seluruh ukuran image dari header (bisa lebih besar dari data, mis. untuk .bss).
    if (!GuestCommit(g_base, uint32_t(image.base), std::max(uint32_t(size), image.size)))
        return false;
    memcpy(g_base + image.base, image.data.get(), size);
    entryPoint = image.entry_point;
    printf("Image dimuat: 0x%zX-0x%zX, entry 0x%zX\n", image.base, image.base + size, entryPoint);
    return true;
}

static size_t RegisterFunctions()
{
    // Tabel lookup untuk pemanggilan tidak langsung (pointer fungsi, vtable C++).
    // Letaknya tepat setelah image: 8 byte pointer host per 4 byte kode guest.
    GuestCommit(g_base, uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE), uint32_t(PPC_CODE_SIZE * 2 + sizeof(PPCFunc*)));
    size_t count = 0;
    for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++, count++)
        PPC_LOOKUP_FUNC(g_base, mapping->guest) = mapping->host;
    return count;
}

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "private/default.xex";
    SetUnhandledExceptionFilter(CrashHandler);
    AddVectoredExceptionHandler(1, CommitOnTouchHandler);

    // 4 GB ruang alamat guest, hanya di-reserve (tidak memakan RAM/pagefile).
    // Bagian yang dipakai di-commit lewat GuestCommit (di sini dan di kernel/memory.cpp).
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS));
    if (g_base == nullptr)
    {
        printf("Gagal me-reserve 4 GB ruang alamat guest (error %lu)\n", GetLastError());
        return 1;
    }
    GuestCommit(g_base, GUEST_STACK_BASE, GUEST_STACK_SIZE);
    GuestCommit(g_base, GUEST_PCR, 0x3000); // PCR + thread + TLS

    size_t entryPoint = 0;
    if (!LoadXex(xexPath, entryPoint))
        return 1;
    printf("%zu fungsi didaftarkan\n", RegisterFunctions());

    // r13 menunjuk ke PCR; kode game membaca thread aktif di PCR+0x100 dan TLS di PCR+0x0.
    StoreGuestU32(GUEST_PCR + 0x0, GUEST_TLS);
    StoreGuestU32(GUEST_PCR + 0x100, GUEST_THREAD);

    PPCContext ctx{};
    ctx.r1.u64 = GUEST_STACK_BASE + GUEST_STACK_SIZE - 0x100; // stack tumbuh ke bawah
    ctx.r13.u64 = GUEST_PCR;
    ctx.fpscr.loadFromHost();

    printf("Memanggil entry point 0x%zX...\n", entryPoint);
    fflush(stdout);
    (PPC_LOOKUP_FUNC(g_base, entryPoint))(ctx, g_base);
    printf("Entry point selesai (r3=0x%llX)\n", ctx.r3.u64);
    return 0;
}
