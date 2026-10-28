// DW6 runtime -- tahap 1: siapkan memori guest, muat default.xex, panggil titik awal game.
//
// Istilah:
//   guest = dunia Xbox 360 (alamat 32-bit, big-endian)
//   host  = PC ini (Windows x64)
//   base  = pointer host ke awal 4 GB memori guest; alamat guest X ada di base + X

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#define NOMINMAX
#include <windows.h>

#include <file.h>
#include <image.h>

#include "kernel.h"

static uint8_t* g_base = nullptr;

// sampler.cpp
void StartSamplerIfRequested(uint8_t* base);
void InitDebugSymbols();
void PrintCallStack(HANDLE thread, CONTEXT& context);

void LogStubCall(const char* name, const PPCContext& ctx)
{
    // Cetak maksimal 5 kali per fungsi supaya log tidak banjir.
    static std::mutex lock;
    static std::unordered_map<const char*, int> counts;
    std::lock_guard guard(lock);
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
    printf("[crash] call stack:");
    CONTEXT context = *info->ContextRecord;
    PrintCallStack(GetCurrentThread(), context);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Commit otomatis: kalau game menyentuh halaman guest yang belum di-commit, commit halaman itu
// (isinya nol) lalu lanjutkan. Alamatnya dicatat supaya akses "liar" tetap kelihatan di log.
static LONG WINAPI CommitOnTouchHandler(EXCEPTION_POINTERS* info)
{
    const auto* record = info->ExceptionRecord;

    // Jaring pengaman: exception floating-point (pembagian nol, hasil tidak presisi, dll.)
    // tidak pernah terjadi di Xbox untuk game ini. Matikan semua lalu ulangi instruksinya.
    switch (record->ExceptionCode)
    {
    case STATUS_FLOAT_DENORMAL_OPERAND:
    case STATUS_FLOAT_DIVIDE_BY_ZERO:
    case STATUS_FLOAT_INEXACT_RESULT:
    case STATUS_FLOAT_INVALID_OPERATION:
    case STATUS_FLOAT_OVERFLOW:
    case STATUS_FLOAT_UNDERFLOW:
    case STATUS_FLOAT_MULTIPLE_FAULTS:
    case STATUS_FLOAT_MULTIPLE_TRAPS:
        info->ContextRecord->MxCsr = (info->ContextRecord->MxCsr | 0x1F80) & ~0x3Fu;
        return EXCEPTION_CONTINUE_EXECUTION;
    default:
        break;
    }

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_base == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    const auto start = reinterpret_cast<uintptr_t>(g_base);
    if (address < start || address >= start + PPC_MEMORY_SIZE)
        return EXCEPTION_CONTINUE_SEARCH; // bukan memori guest: crash sungguhan

    const auto guestPage = uint32_t(address - start) & ~0xFFFu;
    // >= 0xA0000000 = jendela memori fisik (selalu ada, lihat ReserveGuestMemory): crash sungguhan.
    if (guestPage >= 0xA0000000 || !GuestCommit(g_base, guestPage, 0x1000))
        return EXCEPTION_CONTINUE_SEARCH;

    static int logged = 0;
    if (++logged <= 20)
        printf("[mem] commit otomatis halaman guest 0x%08X (%s)\n", guestPage, record->ExceptionInformation[0] ? "tulis" : "baca");
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Tata letak 4 GB memori guest:
//   0x00000000-0x9FFFFFFF  hanya di-reserve; di-commit sesuai kebutuhan (GuestCommit / commit otomatis)
//   0xA0000000, 0xC0000000, 0xE0000000 (masing-masing 512 MB) = TIGA jendela ke RAM fisik yang SAMA.
// Di Xbox 360 game bebas menulis lewat satu jendela lalu membaca lewat jendela lain (beda ukuran
// halaman saja), jadi ketiganya dipetakan ke satu "file mapping" Windows yang sama.
constexpr uint32_t PHYSICAL_VIEW_SIZE = 0x20000000;
constexpr uint32_t PHYSICAL_VIEWS[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

static uint8_t* ReserveGuestMemory()
{
    HANDLE physical = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, PHYSICAL_VIEW_SIZE, nullptr);
    if (physical == nullptr)
        return nullptr;

    // Cari 4 GB kosong, lepas lagi, lalu isi dengan reservasi + 3 view di alamat tetap.
    // Bisa kalah cepat dengan alokasi lain di proses ini, jadi dicoba beberapa kali.
    for (int attempt = 0; attempt < 8; attempt++)
    {
        auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS));
        if (base == nullptr)
            return nullptr;
        VirtualFree(base, 0, MEM_RELEASE);

        bool ok = VirtualAlloc(base, PHYSICAL_VIEWS[0], MEM_RESERVE, PAGE_NOACCESS) == base;
        for (const uint32_t view : PHYSICAL_VIEWS)
            ok = ok && MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0, PHYSICAL_VIEW_SIZE, base + view) == base + view;
        if (ok)
            return base;

        VirtualFree(base, 0, MEM_RELEASE);
        for (const uint32_t view : PHYSICAL_VIEWS)
            UnmapViewOfFile(base + view);
    }
    return nullptr;
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

// Dipasang di semua slot tabel fungsi yang kosong: pemanggilan tidak langsung ke alamat guest
// yang bukan awal fungsi hasil recompile (biasanya karena batas fungsi salah) langsung ketahuan.
static void UnmappedFunction(PPCContext& __restrict ctx, uint8_t* base)
{
    printf("[fatal] panggilan ke alamat guest yang tidak dikenal: ctr=0x%08X (lr=0x%llX)\n", ctx.ctr.u32, ctx.lr);
    fflush(stdout);
    exit(3);
}

static size_t RegisterFunctions()
{
    // Tabel lookup untuk pemanggilan tidak langsung (pointer fungsi, vtable C++).
    // Letaknya tepat setelah image: 8 byte pointer host per 4 byte kode guest.
    GuestCommit(g_base, uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE), uint32_t(PPC_CODE_SIZE * 2 + sizeof(PPCFunc*)));
    for (uint32_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
        PPC_LOOKUP_FUNC(g_base, address) = UnmappedFunction;
    size_t count = 0;
    for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++, count++)
        PPC_LOOKUP_FUNC(g_base, mapping->guest) = mapping->host;
    return count;
}

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "private/default.xex";
    const char* isoPath = argc > 2 ? argv[2] : "private/Dynasty Warriors 6 (Europe).iso";
    InitDebugSymbols();
    SetUnhandledExceptionFilter(CrashHandler);
    AddVectoredExceptionHandler(1, CommitOnTouchHandler);

    g_base = ReserveGuestMemory();
    if (g_base == nullptr)
    {
        printf("Gagal menyiapkan 4 GB ruang alamat guest (error %lu)\n", GetLastError());
        return 1;
    }

    size_t entryPoint = 0;
    if (!LoadXex(xexPath, entryPoint))
        return 1;
    printf("%zu fungsi didaftarkan\n", RegisterFunctions());
    if (!MountIso(isoPath))
        return 1;

    // Thread utama: stack, PCR (r13) dan objek thread sendiri (lihat kernel/threads.cpp).
    PPCContext ctx{};
    InitMainGuestThread(g_base, ctx);
    ctx.fpscr.loadFromHost();

    StartSamplerIfRequested(g_base);
    printf("Memanggil entry point 0x%zX...\n", entryPoint);
    fflush(stdout);
    (PPC_LOOKUP_FUNC(g_base, entryPoint))(ctx, g_base);
    printf("Entry point selesai (r3=0x%llX)\n", ctx.r3.u64);
    return 0;
}
