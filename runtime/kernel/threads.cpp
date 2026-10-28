// Thread guest: setiap thread Xbox 360 dijalankan di thread host (std::thread) sendiri,
// dengan PPCContext, stack, PCR (r13), dan slot TLS sendiri.
// Juga: critical section, TLS, dan fungsi waktu.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <intrin.h>
#define NOMINMAX
#include <windows.h>
#include "../kernel.h"

constexpr uint32_t KTHREAD_SIZE = 0x200;
constexpr uint32_t PCR_BLOCK_SIZE = 0x2000;   // PCR (0x1000) + blok TLS (0x1000)
constexpr uint32_t MIN_STACK_SIZE = 0x40000;  // 256 KB; stack Xbox sering diminta kecil
constexpr uint32_t X_CREATE_SUSPENDED = 0x1;

static thread_local GuestThreadState t_thread;
static std::atomic<uint32_t> g_nextThreadId{ 1 };

GuestThreadState& CurrentGuestThread()
{
    return t_thread;
}

constexpr uint32_t PCR_CURRENT_PROCESSOR = 0x10C; // u8: nomor hardware thread (0-5) tempat thread berjalan

// Peta KTHREAD -> PCR, supaya KeSetAffinityThread bisa mengubah nomor prosesor thread lain.
static std::mutex g_pcrLock;
static std::unordered_map<uint32_t, uint32_t> g_threadPcr;

static void SetProcessorFromAffinity(uint8_t* base, uint32_t pcr, uint32_t affinity)
{
    // Xbox 360: 6 hardware thread. Barrier multi-core di game memakai nomor ini, jadi tiap
    // thread yang di-pin harus melaporkan nomor yang berbeda.
    if (affinity & 0x3F)
        base[pcr + PCR_CURRENT_PROCESSOR] = uint8_t(__builtin_ctz(affinity & 0x3F));
}

// Buat objek thread + PCR untuk thread host yang sedang berjalan, lalu isi r1/r13.
static void SetupGuestThread(uint8_t* base, PPCContext& ctx, uint32_t threadObject, uint32_t stackSize, uint32_t affinity = 0)
{
    t_thread.id = g_nextThreadId++;
    t_thread.threadObject = threadObject;

    // PCR: +0x0 pointer blok TLS, +0x100 pointer KTHREAD thread aktif.
    t_thread.pcr = GuestKernelAlloc(base, PCR_BLOCK_SIZE, 0x1000);
    GuestStoreU32(base, t_thread.pcr + 0x0, t_thread.pcr + 0x1000);
    GuestStoreU32(base, t_thread.pcr + 0x100, threadObject);
    SetProcessorFromAffinity(base, t_thread.pcr, affinity);
    {
        std::lock_guard lock(g_pcrLock);
        g_threadPcr[threadObject] = t_thread.pcr;
    }

    const uint32_t stack = GuestKernelAlloc(base, stackSize, 0x1000);
    ctx.r1.u64 = stack + stackSize - 0x100; // stack tumbuh ke bawah
    ctx.r13.u64 = t_thread.pcr;
    // Wajib: isi salinan MXCSR dari CPU. Kalau dibiarkan 0, perubahan mode pembulatan oleh game
    // ikut mematikan semua masker exception floating-point (crash 0xC000008F dkk).
    ctx.fpscr.loadFromHost();
}

static uint32_t AllocThreadObject(uint8_t* base)
{
    const uint32_t object = GuestKernelAlloc(base, KTHREAD_SIZE);
    base[object] = DISPATCHER_THREAD; // SignalState 0 = masih berjalan
    return object;
}

void InitMainGuestThread(uint8_t* base, PPCContext& ctx)
{
    SetupGuestThread(base, ctx, AllocThreadObject(base), 0x100000);
}

void InitHostThreadAsGuest(uint8_t* base, PPCContext& ctx, uint32_t stackSize)
{
    SetupGuestThread(base, ctx, AllocThreadObject(base), stackSize);
}

// --- Pembuatan thread -------------------------------------------------------

struct HostThread
{
    std::mutex lock;
    std::condition_variable resumed;
    int suspendCount = 0;
};

static std::mutex g_threadsLock;
static std::unordered_map<uint32_t, std::shared_ptr<HostThread>> g_threads; // KTHREAD -> host

struct ThreadExit
{
    uint32_t code;
};

static std::shared_ptr<HostThread> FindThread(uint32_t threadObject)
{
    std::lock_guard lock(g_threadsLock);
    const auto it = g_threads.find(threadObject);
    return it != g_threads.end() ? it->second : nullptr;
}

// NTSTATUS ExCreateThread(HANDLE* Handle, uint32_t StackSize, uint32_t* ThreadId, void* XapiThreadStartup,
//     void* StartAddress, void* StartContext, uint32_t CreationFlags)
PPC_FUNC(__imp__ExCreateThread)
{
    const uint32_t pHandle = ctx.r3.u32;
    const uint32_t stackSize = std::max(ctx.r4.u32, MIN_STACK_SIZE);
    const uint32_t pThreadId = ctx.r5.u32;
    const uint32_t xapiStartup = ctx.r6.u32;
    const uint32_t startAddress = ctx.r7.u32;
    const uint32_t startContext = ctx.r8.u32;
    const uint32_t flags = ctx.r9.u32;

    const uint32_t threadObject = AllocThreadObject(base);
    auto host = std::make_shared<HostThread>();
    host->suspendCount = (flags & X_CREATE_SUSPENDED) ? 1 : 0;
    {
        std::lock_guard lock(g_threadsLock);
        g_threads[threadObject] = host;
    }

    std::promise<uint32_t> idPromise;
    auto idFuture = idPromise.get_future();
    std::thread([=, idPromise = std::move(idPromise)]() mutable {
        PPCContext threadCtx{};
        // Bit 24-29 CreationFlags = affinity (hardware thread mana yang boleh dipakai).
        SetupGuestThread(base, threadCtx, threadObject, stackSize, flags >> 24);
        idPromise.set_value(t_thread.id);

        {
            std::unique_lock lock(host->lock);
            host->resumed.wait(lock, [&] { return host->suspendCount <= 0; });
        }

        printf("[thread] %u mulai di 0x%X (context 0x%X), thread Windows %lu\n", t_thread.id, startAddress, startContext, GetCurrentThreadId());
        fflush(stdout);
        try
        {
            // Thread XAPI dimulai lewat fungsi pembungkus milik game: wrapper(StartAddress, StartContext).
            if (xapiStartup != 0)
            {
                threadCtx.r3.u64 = startAddress;
                threadCtx.r4.u64 = startContext;
                (PPC_LOOKUP_FUNC(base, xapiStartup))(threadCtx, base);
            }
            else
            {
                threadCtx.r3.u64 = startContext;
                (PPC_LOOKUP_FUNC(base, startAddress))(threadCtx, base);
            }
        }
        catch (const ThreadExit&)
        {
        }
        printf("[thread] %u selesai\n", t_thread.id);
        fflush(stdout);
        SignalGuestObject(base, threadObject, 1);
    }).detach();

    // Tunggu sampai thread punya ID supaya bisa dikembalikan ke game.
    const uint32_t threadId = idFuture.get();
    if (pThreadId != 0)
        GuestStoreU32(base, pThreadId, threadId);
    if (pHandle != 0)
        GuestStoreU32(base, pHandle, CreateGuestHandle(threadObject));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// void ExTerminateThread(uint32_t ExitCode)
PPC_FUNC(__imp__ExTerminateThread)
{
    // Lompat keluar dari semua fungsi guest kembali ke awal thread (ditangkap di ExCreateThread).
    throw ThreadExit{ ctx.r3.u32 };
}

static uint32_t ResumeThread(uint32_t threadObject)
{
    const auto host = FindThread(threadObject);
    if (!host)
        return 0;
    std::lock_guard lock(host->lock);
    const int previous = host->suspendCount;
    if (host->suspendCount > 0 && --host->suspendCount == 0)
        host->resumed.notify_all();
    return uint32_t(previous);
}

// NTSTATUS NtResumeThread(HANDLE Handle, uint32_t* SuspendCount)
PPC_FUNC(__imp__NtResumeThread)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    const uint32_t previous = ResumeThread(object);
    if (ctx.r4.u32 != 0)
        GuestStoreU32(base, ctx.r4.u32, previous);
    ctx.r3.u64 = object != 0 ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// uint32_t KeResumeThread(KTHREAD* Thread)
PPC_FUNC(__imp__KeResumeThread)
{
    ctx.r3.u64 = ResumeThread(ctx.r3.u32);
}

// NTSTATUS NtSuspendThread(HANDLE Handle, uint32_t* SuspendCount)
PPC_FUNC(__imp__NtSuspendThread)
{
    // TODO: menghentikan thread lain di tengah jalan tidak bisa dilakukan dengan aman di sini.
    LogStubCall("NtSuspendThread", ctx);
    if (ctx.r4.u32 != 0)
        GuestStoreU32(base, ctx.r4.u32, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// Prioritas & afinitas CPU: diabaikan, biar Windows yang mengatur.
PPC_FUNC(__imp__KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
// uint32_t KeSetAffinityThread(KTHREAD* Thread, uint32_t Affinity) -> affinity lama
PPC_FUNC(__imp__KeSetAffinityThread)
{
    uint32_t pcr = 0;
    {
        std::lock_guard lock(g_pcrLock);
        const auto it = g_threadPcr.find(ctx.r3.u32);
        if (it != g_threadPcr.end())
            pcr = it->second;
    }
    uint32_t previous = 0;
    if (pcr != 0)
    {
        previous = 1u << base[pcr + PCR_CURRENT_PROCESSOR];
        SetProcessorFromAffinity(base, pcr, ctx.r4.u32);
    }
    ctx.r3.u64 = previous;
}

// --- TLS --------------------------------------------------------------------

static std::atomic<uint32_t> g_nextTlsIndex{ 0 };

PPC_FUNC(__imp__KeTlsAlloc)
{
    const uint32_t index = g_nextTlsIndex++;
    ctx.r3.u64 = index < 64 ? index : 0xFFFFFFFF;
}

PPC_FUNC(__imp__KeTlsFree)
{
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    const uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = index < 64 ? t_thread.tlsValues[index] : 0;
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    const uint32_t index = ctx.r3.u32;
    if (index < 64)
        t_thread.tlsValues[index] = ctx.r4.u32;
    ctx.r3.u64 = index < 64;
}

// --- Critical section -------------------------------------------------------
// RTL_CRITICAL_SECTION Xbox: +0x00 header (16 byte), +0x10 LockCount, +0x14 RecursionCount,
// +0x18 OwningThread. Pemilik = alamat KTHREAD thread yang memegang kunci.

static uint32_t* OwnerField(uint8_t* base, uint32_t cs)
{
    return reinterpret_cast<uint32_t*>(base + cs + 0x18);
}

static bool TryEnterCriticalSection(uint8_t* base, uint32_t cs)
{
    const uint32_t me = __builtin_bswap32(t_thread.threadObject);
    uint32_t* owner = OwnerField(base, cs);
    if (__atomic_load_n(owner, __ATOMIC_ACQUIRE) == me)
    {
        GuestStoreU32(base, cs + 0x14, GuestLoadU32(base, cs + 0x14) + 1);
        return true;
    }
    if (__sync_bool_compare_and_swap(owner, 0, me))
    {
        GuestStoreU32(base, cs + 0x14, 1);
        return true;
    }
    return false;
}

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    memset(base + ctx.r3.u32, 0, 0x1C);
    GuestStoreU32(base, ctx.r3.u32 + 0x10, 0xFFFFFFFF); // LockCount -1 = bebas
}

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    __imp__RtlInitializeCriticalSection(ctx, base);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    for (uint32_t spin = 0; !TryEnterCriticalSection(base, ctx.r3.u32); spin++)
    {
        if (spin < 64)
            _mm_pause();
        else
            std::this_thread::yield();
    }
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = TryEnterCriticalSection(base, ctx.r3.u32);
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t recursion = GuestLoadU32(base, cs + 0x14);
    if (recursion > 1)
    {
        GuestStoreU32(base, cs + 0x14, recursion - 1);
        return;
    }
    GuestStoreU32(base, cs + 0x14, 0);
    __atomic_store_n(OwnerField(base, cs), 0u, __ATOMIC_RELEASE);
}

// --- Waktu ------------------------------------------------------------------

uint64_t TimestampFrequency()
{
    // Ukur frekuensi rdtsc sekali (100 ms) saat pertama dipakai.
    static const uint64_t frequency = [] {
        const auto start = std::chrono::steady_clock::now();
        const uint64_t tscStart = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t tscEnd = __rdtsc();
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return uint64_t((tscEnd - tscStart) / seconds);
    }();
    return frequency;
}

// int64_t KeQueryPerformanceFrequency()
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = TimestampFrequency();
}

// void KeQuerySystemTime(int64_t* CurrentTime) -- FILETIME: satuan 100 ns sejak 1 Januari 1601
PPC_FUNC(__imp__KeQuerySystemTime)
{
    constexpr int64_t EPOCH_DIFFERENCE = 116444736000000000LL; // 1601 -> 1970 dalam 100 ns
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t fileTime = std::chrono::duration_cast<std::chrono::microseconds>(now).count() * 10 + EPOCH_DIFFERENCE;
    GuestStoreU64(base, ctx.r3.u32, uint64_t(fileTime));
}
