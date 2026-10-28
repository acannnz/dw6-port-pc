// Alokasi memori guest.
//
// Seluruh 4 GB memori guest sudah dialokasikan di main.cpp, jadi di sini kita cukup
// "membagikan" rentang alamat. RangeAllocator menyimpan daftar rentang kosong (first-fit)
// supaya memori yang di-free bisa dipakai lagi -- game memang sering alloc+free untuk
// mencari ukuran terbesar yang tersedia.
#include <cstring>
#include <map>
#include <mutex>
#define NOMINMAX
#include <windows.h>
#include "../kernel.h"

static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool GuestCommit(uint8_t* base, uint32_t address, uint32_t size)
{
    if (VirtualAlloc(base + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr)
        return true;
    printf("[mem] commit gagal: 0x%X (size 0x%X), error %lu\n", address, size, GetLastError());
    return false;
}

void GuestDecommit(uint8_t* base, uint32_t address, uint32_t size)
{
    VirtualFree(base + address, size, MEM_DECOMMIT);
}

class RangeAllocator
{
public:
    RangeAllocator(uint32_t start, uint32_t end) { m_free[start] = end - start; }

    // Kembalikan alamat awal, atau 0 kalau tidak ada rentang kosong yang cukup.
    uint32_t Allocate(uint32_t size, uint32_t alignment)
    {
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            const uint32_t blockStart = it->first;
            const uint32_t blockEnd = it->first + it->second;
            const uint32_t address = AlignUp(blockStart, alignment);
            if (address < blockStart || address >= blockEnd || blockEnd - address < size)
                continue;

            m_free.erase(it);
            if (address > blockStart)
                m_free[blockStart] = address - blockStart; // sisa di depan
            if (address + size < blockEnd)
                m_free[address + size] = blockEnd - (address + size); // sisa di belakang
            m_used[address] = size;
            return address;
        }
        return 0;
    }

    // Kembalikan rentang yang dimulai di `address`; gabungkan dengan tetangga yang kosong.
    // Hasil: ukuran rentang yang dibebaskan, atau 0 kalau `address` bukan hasil Allocate.
    uint32_t Free(uint32_t address)
    {
        const auto used = m_used.find(address);
        if (used == m_used.end())
            return 0;
        const uint32_t freedSize = used->second;
        uint32_t start = address;
        uint32_t size = used->second;
        m_used.erase(used);

        auto next = m_free.find(start + size);
        if (next != m_free.end())
        {
            size += next->second;
            m_free.erase(next);
        }
        auto prev = m_free.lower_bound(start);
        if (prev != m_free.begin())
        {
            --prev;
            if (prev->first + prev->second == start)
            {
                start = prev->first;
                size += prev->second;
                m_free.erase(prev);
            }
        }
        m_free[start] = size;
        return freedSize;
    }

    bool Contains(uint32_t address) const { return m_used.count(address) != 0; }

private:
    std::map<uint32_t, uint32_t> m_free; // alamat -> ukuran rentang kosong
    std::map<uint32_t, uint32_t> m_used; // alamat -> ukuran rentang terpakai
};

// Rentang alamat seperti di Xbox 360:
//   0x40000000-0x6FFFFFFF  memori virtual (NtAllocateVirtualMemory); 0x70000000-0x70FFFFFF cadangan
//   0xA0000000-0xBFFFFFFF  memori fisik 512 MB (MmAllocatePhysicalMemoryEx), mis. untuk GPU
//   0x71000000-0x7FEFFFFF  struktur kernel buatan runtime (GuestKernelAlloc)
static RangeAllocator g_virtual(0x40000000, 0x70000000);
static RangeAllocator g_physical(0xA0000000, 0xC0000000);
static RangeAllocator g_kernel(0x71000000, 0x7FF00000);

// Semua allocator dipakai dari banyak thread guest.
static std::mutex g_memoryLock;

uint32_t GuestKernelAlloc(uint8_t* base, uint32_t size, uint32_t alignment)
{
    std::lock_guard lock(g_memoryLock);
    const uint32_t allocSize = AlignUp(size, 0x10);
    const uint32_t address = g_kernel.Allocate(allocSize, alignment);
    if (address == 0)
    {
        printf("[mem] GuestKernelAlloc gagal: size=0x%X\n", size);
        return 0;
    }
    // Commit per halaman penuh; halaman yang sudah di-commit tidak masalah di-commit lagi.
    const uint32_t first = address & ~0xFFFu;
    const uint32_t last = AlignUp(address + allocSize, 0x1000);
    GuestCommit(base, first, last - first);
    memset(base + address, 0, allocSize);
    return address;
}

constexpr uint32_t X_MEM_RELEASE = 0x8000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000; // halaman 64 KB
constexpr uint32_t X_MEM_16MB_PAGES = 0x40000000;

static uint32_t PageSize(uint32_t flags)
{
    if (flags & X_MEM_16MB_PAGES)
        return 0x1000000;
    if (flags & X_MEM_LARGE_PAGES)
        return 0x10000;
    return 0x1000;
}

// NTSTATUS NtAllocateVirtualMemory(void** BaseAddress, size_t* RegionSize,
//     uint32_t AllocationType, uint32_t Protect, bool DebugMemory)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    std::lock_guard lock(g_memoryLock);
    const uint32_t pBase = ctx.r3.u32;
    const uint32_t pSize = ctx.r4.u32;
    const uint32_t pageSize = PageSize(ctx.r5.u32);
    uint32_t address = GuestLoadU32(base, pBase);
    const uint32_t size = AlignUp(GuestLoadU32(base, pSize), pageSize);

    // Alamat sudah ditentukan game (mis. commit di area yang sebelumnya di-reserve): memori sudah ada.
    if (address == 0)
    {
        address = g_virtual.Allocate(size, pageSize > 0x10000 ? pageSize : 0x10000);
        if (address == 0)
        {
            printf("[mem] NtAllocateVirtualMemory gagal: size=0x%X\n", size);
            ctx.r3.u64 = X_STATUS_NO_MEMORY;
            return;
        }
    }

    // Hanya MEM_COMMIT yang benar-benar memakai memori. MEM_RESERVE saja cukup mencatat rentangnya;
    // kalau game menyentuhnya tanpa commit, handler commit-otomatis di main.cpp menanganinya.
    constexpr uint32_t X_MEM_COMMIT = 0x1000;
    if ((ctx.r5.u32 & X_MEM_COMMIT) && !GuestCommit(base, address, size))
    {
        ctx.r3.u64 = X_STATUS_NO_MEMORY;
        return;
    }
    GuestStoreU32(base, pBase, address);
    GuestStoreU32(base, pSize, size);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS NtFreeVirtualMemory(void** BaseAddress, size_t* RegionSize, uint32_t FreeType, bool DebugMemory)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    std::lock_guard lock(g_memoryLock);
    // MEM_DECOMMIT cukup diabaikan (memori tetap ada); X_MEM_RELEASE mengembalikan rentangnya.
    if (ctx.r5.u32 & X_MEM_RELEASE)
    {
        const uint32_t address = GuestLoadU32(base, ctx.r3.u32);
        const uint32_t size = g_virtual.Free(address);
        if (size != 0)
            GuestDecommit(base, address, size);
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// void* MmAllocatePhysicalMemoryEx(uint32_t Flags, size_t Size, uint32_t Protect,
//     uint32_t MinAddress, uint32_t MaxAddress, uint32_t Alignment)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    std::lock_guard lock(g_memoryLock);
    const uint32_t pageSize = PageSize(ctx.r5.u32);
    const uint32_t size = AlignUp(ctx.r4.u32, pageSize);
    const uint32_t alignment = ctx.r8.u32 > pageSize ? ctx.r8.u32 : pageSize;
    const uint32_t address = g_physical.Allocate(size, alignment);
    if (address != 0 && !GuestCommit(base, address, size))
    {
        g_physical.Free(address);
        ctx.r3.u64 = 0;
        return;
    }
    if (address == 0)
        printf("[mem] MmAllocatePhysicalMemoryEx gagal: size=0x%X protect=0x%X align=0x%X\n", size, ctx.r5.u32, ctx.r8.u32);
    ctx.r3.u64 = address;
}

// void MmFreePhysicalMemory(uint32_t Type, void* Address)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    std::lock_guard lock(g_memoryLock);
    const uint32_t size = g_physical.Free(ctx.r4.u32);
    if (size != 0)
        GuestDecommit(base, ctx.r4.u32, size);
}

// uint32_t MmQueryAddressProtect(void* Address)
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    ctx.r3.u64 = 0x04; // PAGE_READWRITE
}

// uint32_t MmGetPhysicalAddress(void* Address)
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; // alamat fisik = 512 MB RAM Xbox 360
}
