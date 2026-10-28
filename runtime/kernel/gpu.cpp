// GPU "headless": belum menggambar apa pun, tapi cukup supaya game tidak menunggu selamanya.
//
// Cara GPU Xbox 360 (Xenos) dipakai game:
//   1. Game menulis paket perintah PM4 ke ring buffer di memori fisik.
//   2. Game menaikkan register CP_RB_WPTR (lewat MMIO di 0x7FC80000 + nomor_register * 4).
//   3. GPU membaca perintah sampai WPTR, lalu melaporkan posisi bacanya (RPTR) ke memori
//      ("RPtr writeback") supaya game tahu ruang mana di ring buffer yang sudah bebas.
//   4. Perintah tertentu menulis nilai "fence" ke memori atau memicu interrupt; game menunggu itu.
//   5. Setiap vblank (60 Hz) GPU memanggil callback interrupt milik game.
//
// Di sini langkah 3-5 ditiru. Perintah gambar (draw) diabaikan -- itu tugas backend render nanti.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include "../kernel.h"

constexpr uint32_t GPU_MMIO_BASE = 0x7FC80000;
constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;

static std::atomic<uint32_t> g_interruptCallback{ 0 };
static std::atomic<uint32_t> g_interruptContext{ 0 };
static std::atomic<uint32_t> g_ringBuffer{ 0 };      // alamat virtual guest
static std::atomic<uint32_t> g_ringBufferDwords{ 0 };
static std::atomic<uint32_t> g_rptrWriteBack{ 0 };   // alamat virtual guest, 0 = mati
static std::atomic<uint64_t> g_swapCount{ 0 };
static std::once_flag g_startThreads;
static std::mutex g_interruptLock; // interrupt di Xbox tidak pernah berjalan bersamaan

// Alamat fisik (yang ada di perintah GPU) -> alamat virtual guest. MmAllocatePhysicalMemoryEx
// membagikan memori fisik di 0xA0000000 + alamat_fisik (lihat kernel/memory.cpp).
static uint32_t PhysicalToVirtual(uint32_t physical)
{
    return 0xA0000000 | (physical & 0x1FFFFFFF);
}

static void CallInterrupt(uint8_t* base, PPCContext& ctx, uint32_t source)
{
    const uint32_t callback = g_interruptCallback;
    if (callback == 0)
        return;
    std::lock_guard lock(g_interruptLock);
    ctx.r3.u64 = source; // 0 = vblank, 1 = interrupt dari command processor
    ctx.r4.u64 = g_interruptContext;
    (PPC_LOOKUP_FUNC(base, callback))(ctx, base);
}

// Tukar urutan byte sesuai mode endian di 2 bit terbawah alamat tujuan perintah GPU.
static uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); // 8-in-16
    case 2: return __builtin_bswap32(value);                                     // 8-in-32
    case 3: return (value << 16) | (value >> 16);                                // 16-in-32
    default: return value;
    }
}

static void GpuWrite(uint8_t* base, uint32_t addressWithEndian, uint32_t value)
{
    const uint32_t address = PhysicalToVirtual(addressWithEndian & ~3u);
    GuestCommit(base, address & ~0xFFFu, 0x1000);
    *reinterpret_cast<uint32_t*>(base + address) = GpuSwap(value, addressWithEndian);
}

// Jalankan paket PM4 di [address, address + dwords*4). Hanya perintah yang memengaruhi CPU
// (tulis memori, fence, interrupt, indirect buffer) yang diproses.
static void ExecutePackets(uint8_t* base, PPCContext& ctx, uint32_t address, uint32_t dwords, int depth = 0)
{
    uint32_t i = 0;
    auto read = [&](uint32_t index) { return GuestLoadU32(base, address + index * 4); };
    while (i < dwords)
    {
        const uint32_t header = read(i);
        const uint32_t type = header >> 30;
        if (type == 0) // tulis banyak register berurutan
        {
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            i += 1 + count;
        }
        else if (type == 1) // tulis 2 register
        {
            i += 3;
        }
        else if (type == 2) // NOP
        {
            i += 1;
        }
        else
        {
            const uint32_t opcode = (header >> 8) & 0x7F;
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t data = i + 1;
            switch (opcode)
            {
            case 0x3F: // INDIRECT_BUFFER: lompat ke buffer perintah lain
                if (depth < 4)
                    ExecutePackets(base, ctx, PhysicalToVirtual(read(data)), read(data + 1) & 0xFFFFF, depth + 1);
                break;
            case 0x3D: // MEM_WRITE: tulis count-1 nilai mulai dari alamat
            {
                uint32_t target = read(data);
                for (uint32_t k = 1; k < count; k++, target += 4)
                    GpuWrite(base, target, read(data + k));
                break;
            }
            case 0x58: // EVENT_WRITE_SHD: tulis fence
                GpuWrite(base, read(data + 1), read(data + 2));
                break;
            case 0x54: // INTERRUPT
                CallInterrupt(base, ctx, 1);
                break;
            default:
                break; // draw, state, tunggu register, dll.: diabaikan untuk sementara
            }
            i += 1 + count;
        }
    }
}

static void CommandProcessorThread(uint8_t* base)
{
    PPCContext ctx{};
    InitHostThreadAsGuest(base, ctx, 0x40000);
    GuestCommit(base, GPU_MMIO_BASE, 0x10000);

    uint32_t readPointer = 0;
    while (true)
    {
        const uint32_t ring = g_ringBuffer;
        const uint32_t size = g_ringBufferDwords;
        const uint32_t writePointer = size ? GuestLoadU32(base, GPU_MMIO_BASE + REG_CP_RB_WPTR * 4) % size : 0;
        if (ring == 0 || writePointer == readPointer)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        if (writePointer > readPointer)
        {
            ExecutePackets(base, ctx, ring + readPointer * 4, writePointer - readPointer);
        }
        else // ring buffer berputar ke awal
        {
            ExecutePackets(base, ctx, ring + readPointer * 4, size - readPointer);
            ExecutePackets(base, ctx, ring, writePointer);
        }
        readPointer = writePointer;

        if (const uint32_t writeBack = g_rptrWriteBack)
            GuestStoreU32(base, writeBack, readPointer);
    }
}

static void VblankThread(uint8_t* base)
{
    PPCContext ctx{};
    InitHostThreadAsGuest(base, ctx, 0x40000);
    auto next = std::chrono::steady_clock::now();
    while (true)
    {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        CallInterrupt(base, ctx, 0);
    }
}

static void StartGpuThreads(uint8_t* base)
{
    std::call_once(g_startThreads, [base] {
        std::thread(CommandProcessorThread, base).detach();
        std::thread(VblankThread, base).detach();
        printf("[gpu] thread command processor + vblank dimulai\n");
    });
}

// --- Fungsi kernel Vd* (driver video) -------------------------------------------

// void VdSetGraphicsInterruptCallback(void* Callback, void* Context)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    g_interruptCallback = ctx.r3.u32;
    g_interruptContext = ctx.r4.u32;
}

// void VdInitializeRingBuffer(void* PhysicalAddress, uint32_t SizeLog2) -- ukuran = 2^(SizeLog2+3) byte
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    g_ringBufferDwords = (1u << (ctx.r4.u32 + 3)) / 4;
    g_ringBuffer = PhysicalToVirtual(ctx.r3.u32);
    StartGpuThreads(base);
}

// void VdEnableRingBufferRPtrWriteBack(void* PhysicalAddress, uint32_t BlockSizeLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    g_rptrWriteBack = PhysicalToVirtual(ctx.r3.u32);
}

// void VdSwap(void* BufferPtr, void* Fetch, void* Unknown, void* Frontbuffer, uint32_t* ColorFormat, ...)
// Di Xbox asli, VdSwap menulis paket "tukar frame" ke command buffer di BufferPtr (64 dword).
// Di sini cukup diisi NOP (paket tipe 2) dan frame dihitung.
PPC_FUNC(__imp__VdSwap)
{
    for (uint32_t i = 0; i < 64; i++)
        GuestStoreU32(base, ctx.r3.u32 + i * 4, 0x80000000);
    // Batasi 60 FPS (di Xbox asli, VdSwap menunggu vblank).
    static thread_local auto nextFrame = std::chrono::steady_clock::now();
    nextFrame = std::max(nextFrame + std::chrono::microseconds(16667), std::chrono::steady_clock::now() - std::chrono::milliseconds(100));
    std::this_thread::sleep_until(nextFrame);

    const uint64_t frame = ++g_swapCount;
    if (frame <= 3 || frame % 300 == 0)
        printf("[gpu] VdSwap frame %llu\n", (unsigned long long)frame);
    ctx.r3.u64 = 0;
}

// struct X_VIDEO_MODE (0x30 byte): width, height, interlaced, widescreen, hi_def, refresh (float),
// video_standard, unknown_0x8A, unknown_0x01, reserved[3]
static void FillVideoMode(uint8_t* base, uint32_t mode)
{
    const float refreshRate = 60.0f;
    uint32_t refreshBits;
    memcpy(&refreshBits, &refreshRate, 4);
    const uint32_t fields[12] = { 1280, 720, 0, 1, 1, refreshBits, 1, 0x4A, 0x01, 0, 0, 0 };
    for (uint32_t i = 0; i < 12; i++)
        GuestStoreU32(base, mode + i * 4, fields[i]);
}

PPC_FUNC(__imp__VdQueryVideoMode)
{
    FillVideoMode(base, ctx.r3.u32);
}

PPC_FUNC(__imp__XGetVideoMode)
{
    FillVideoMode(base, ctx.r3.u32);
}

// uint32_t VdQueryVideoFlags() -- bit0 layar lebar, bit1 resolusi >= 1024
PPC_FUNC(__imp__VdQueryVideoFlags)
{
    ctx.r3.u64 = 0x3;
}

// --- Spinlock & IRQL --------------------------------------------------------------
// Spinlock Xbox = 1 word di memori guest; terisi alamat KTHREAD pemilik, 0 = bebas.

static void AcquireSpinLock(uint8_t* base, uint32_t lockAddress)
{
    auto* word = reinterpret_cast<uint32_t*>(base + lockAddress);
    const uint32_t me = __builtin_bswap32(CurrentGuestThread().threadObject);
    while (!__sync_bool_compare_and_swap(word, 0, me))
        std::this_thread::yield();
}

static void ReleaseSpinLock(uint8_t* base, uint32_t lockAddress)
{
    __atomic_store_n(reinterpret_cast<uint32_t*>(base + lockAddress), 0u, __ATOMIC_RELEASE);
}

// IRQL (tingkat prioritas interrupt) tidak ditiru; selalu dianggap 0.
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KfLowerIrql) {}

PPC_FUNC(__imp__KfAcquireSpinLock)
{
    AcquireSpinLock(base, ctx.r3.u32);
    ctx.r3.u64 = 0; // IRQL lama
}

PPC_FUNC(__imp__KfReleaseSpinLock)
{
    ReleaseSpinLock(base, ctx.r3.u32);
}

PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    AcquireSpinLock(base, ctx.r3.u32);
}

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    ReleaseSpinLock(base, ctx.r3.u32);
}

PPC_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql)
{
    auto* word = reinterpret_cast<uint32_t*>(base + ctx.r3.u32);
    ctx.r3.u64 = __sync_bool_compare_and_swap(word, 0, __builtin_bswap32(CurrentGuestThread().threadObject));
}
