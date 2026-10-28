// Objek dispatcher kernel (event, semaphore, thread), handle, dan fungsi tunggu.
//
// Status objek (SignalState) disimpan langsung di struktur guest, bukan di tabel host,
// karena game juga membuat objek sendiri (mis. KEVENT di stack) tanpa memanggil kernel.
// Semua akses ke SignalState dilindungi satu kunci global; thread yang menunggu
// dibangunkan lewat satu condition variable. Sederhana, cukup untuk tahap awal.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "../kernel.h"

static std::mutex g_dispatcherLock;
static std::condition_variable g_dispatcherCv;

static std::mutex g_handleLock;
static std::unordered_map<uint32_t, uint32_t> g_handles; // handle -> alamat objek guest
static uint32_t g_nextHandle = 0xF8000004;

constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;

uint32_t CreateGuestHandle(uint32_t object)
{
    std::lock_guard lock(g_handleLock);
    const uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    g_handles[handle] = object;
    return handle;
}

uint32_t GuestHandleToObject(uint32_t handle)
{
    if (handle == CURRENT_THREAD_HANDLE)
        return CurrentGuestThread().threadObject;
    std::lock_guard lock(g_handleLock);
    const auto it = g_handles.find(handle);
    return it != g_handles.end() ? it->second : 0;
}

static int32_t LoadSignal(uint8_t* base, uint32_t object)
{
    return int32_t(GuestLoadU32(base, object + 4));
}

static void StoreSignal(uint8_t* base, uint32_t object, int32_t state)
{
    GuestStoreU32(base, object + 4, uint32_t(state));
}

void SignalGuestObject(uint8_t* base, uint32_t object, int32_t state)
{
    {
        std::lock_guard lock(g_dispatcherLock);
        StoreSignal(base, object, state);
    }
    g_dispatcherCv.notify_all();
}

// Apakah objek siap? Kalau `consume`, ambil sinyalnya (event sinkronisasi reset, semaphore berkurang).
// Dipanggil dengan g_dispatcherLock terkunci.
static bool TryAcquire(uint8_t* base, uint32_t object, bool consume)
{
    const uint8_t type = base[object];
    const int32_t signal = LoadSignal(base, object);
    if (signal <= 0)
        return false;
    if (consume)
    {
        if (type == DISPATCHER_EVENT_SYNCHRONIZATION)
            StoreSignal(base, object, 0);
        else if (type == DISPATCHER_SEMAPHORE)
            StoreSignal(base, object, signal - 1);
    }
    return true;
}

// Ubah timeout guest (pointer ke int64 dalam satuan 100 ns; negatif = relatif, 0 = cek saja,
// pointer null = tunggu selamanya) jadi batas waktu host.
static bool DeadlineFromTimeout(uint8_t* base, uint32_t pTimeout, std::chrono::steady_clock::time_point& deadline)
{
    if (pTimeout == 0)
        return false; // tanpa batas
    int64_t value = int64_t(GuestLoadU64(base, pTimeout));
    // TODO: nilai positif = waktu absolut (FILETIME); untuk sementara diperlakukan sebagai relatif.
    if (value < 0)
        value = -value;
    deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(value / 10);
    return true;
}

// Tunggu satu atau banyak objek. waitAll = semua harus siap; selain itu cukup salah satu.
// Hasil: STATUS_WAIT_0 + indeks objek (waitAny), X_STATUS_SUCCESS (waitAll), atau X_STATUS_TIMEOUT.
// --- Catatan debug: siapa sedang menunggu apa (dicetak oleh sampler) --------------
struct WaitRecord
{
    uint32_t objects[4];
    size_t count;
    uint64_t lr;
    std::chrono::steady_clock::time_point since;
};
static std::mutex g_waitRecordsLock;
static std::unordered_map<uint32_t, WaitRecord> g_waitRecords; // id thread guest -> tunggu aktif

struct WaitRecordScope
{
    uint32_t threadId;
    WaitRecordScope(const uint32_t* objects, size_t count, uint64_t lr) : threadId(CurrentGuestThread().id)
    {
        WaitRecord record{ {}, std::min<size_t>(count, 4), lr, std::chrono::steady_clock::now() };
        std::copy_n(objects, record.count, record.objects);
        std::lock_guard lock(g_waitRecordsLock);
        g_waitRecords[threadId] = record;
    }
    ~WaitRecordScope()
    {
        std::lock_guard lock(g_waitRecordsLock);
        g_waitRecords.erase(threadId);
    }
};

void DumpGuestWaits(uint8_t* base)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_waitRecordsLock);
    for (const auto& [threadId, record] : g_waitRecords)
    {
        const auto seconds = std::chrono::duration<double>(now - record.since).count();
        if (seconds < 1.0)
            continue;
        printf("[wait] thread guest %u menunggu %.1f s dari lr=0x%llX:", threadId, seconds, (unsigned long long)record.lr);
        for (size_t i = 0; i < record.count; i++)
            printf(" obj 0x%X(type %u, signal %d)", record.objects[i], base[record.objects[i]], LoadSignal(base, record.objects[i]));
        putchar('\n');
    }
    fflush(stdout);
}

static uint32_t WaitForObjects(uint8_t* base, const uint32_t* objects, size_t count, bool waitAll, uint32_t pTimeout, uint64_t lr)
{
    const WaitRecordScope record(objects, count, lr);
    std::chrono::steady_clock::time_point deadline;
    const bool hasDeadline = DeadlineFromTimeout(base, pTimeout, deadline);

    std::unique_lock lock(g_dispatcherLock);
    while (true)
    {
        if (waitAll)
        {
            bool ready = true;
            for (size_t i = 0; i < count && ready; i++)
                ready = TryAcquire(base, objects[i], false);
            if (ready)
            {
                for (size_t i = 0; i < count; i++)
                    TryAcquire(base, objects[i], true);
                return X_STATUS_SUCCESS;
            }
        }
        else
        {
            for (size_t i = 0; i < count; i++)
            {
                if (TryAcquire(base, objects[i], true))
                    return X_STATUS_SUCCESS + uint32_t(i);
            }
        }

        if (hasDeadline)
        {
            if (g_dispatcherCv.wait_until(lock, deadline) == std::cv_status::timeout)
                return X_STATUS_TIMEOUT;
        }
        else
        {
            // Bangun berkala juga, untuk berjaga-jaga kalau game mengubah SignalState sendiri.
            g_dispatcherCv.wait_for(lock, std::chrono::milliseconds(50));
        }
    }
}

// --- Event -------------------------------------------------------------------

// NTSTATUS NtCreateEvent(HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, uint32_t EventType, bool InitialState)
// EventType: 0 = notification (manual reset), 1 = synchronization (auto reset).
PPC_FUNC(__imp__NtCreateEvent)
{
    const uint32_t object = GuestKernelAlloc(base, 16);
    base[object] = ctx.r5.u32 == 0 ? DISPATCHER_EVENT_NOTIFICATION : DISPATCHER_EVENT_SYNCHRONIZATION;
    StoreSignal(base, object, ctx.r6.u32 ? 1 : 0);
    GuestStoreU32(base, ctx.r3.u32, CreateGuestHandle(object));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS NtSetEvent(HANDLE Handle, int32_t* PreviousState)
PPC_FUNC(__imp__NtSetEvent)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r4.u32 != 0)
        GuestStoreU32(base, ctx.r4.u32, uint32_t(LoadSignal(base, object)));
    SignalGuestObject(base, object, 1);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS NtClearEvent(HANDLE Handle)
PPC_FUNC(__imp__NtClearEvent)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    if (object != 0)
        SignalGuestObject(base, object, 0);
    ctx.r3.u64 = object != 0 ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// int32_t KeSetEvent(KEVENT* Event, int32_t Increment, bool Wait) -> state sebelumnya
PPC_FUNC(__imp__KeSetEvent)
{
    const int32_t previous = LoadSignal(base, ctx.r3.u32);
    SignalGuestObject(base, ctx.r3.u32, 1);
    ctx.r3.u64 = uint32_t(previous);
}

// int32_t KeResetEvent(KEVENT* Event) -> state sebelumnya
PPC_FUNC(__imp__KeResetEvent)
{
    const int32_t previous = LoadSignal(base, ctx.r3.u32);
    SignalGuestObject(base, ctx.r3.u32, 0);
    ctx.r3.u64 = uint32_t(previous);
}

// --- Semaphore ---------------------------------------------------------------

// NTSTATUS NtCreateSemaphore(HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, int32_t Initial, int32_t Maximum)
PPC_FUNC(__imp__NtCreateSemaphore)
{
    const uint32_t object = GuestKernelAlloc(base, 20);
    base[object] = DISPATCHER_SEMAPHORE;
    StoreSignal(base, object, ctx.r5.s32);
    GuestStoreU32(base, object + 16, ctx.r6.u32);
    GuestStoreU32(base, ctx.r3.u32, CreateGuestHandle(object));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// void KeInitializeSemaphore(KSEMAPHORE* Semaphore, int32_t Count, int32_t Limit)
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    const uint32_t object = ctx.r3.u32;
    memset(base + object, 0, 20);
    base[object] = DISPATCHER_SEMAPHORE;
    StoreSignal(base, object, ctx.r4.s32);
    GuestStoreU32(base, object + 16, ctx.r5.u32);
}

static int32_t ReleaseSemaphore(uint8_t* base, uint32_t object, int32_t count)
{
    int32_t previous;
    {
        std::lock_guard lock(g_dispatcherLock);
        previous = LoadSignal(base, object);
        const int32_t limit = int32_t(GuestLoadU32(base, object + 16));
        StoreSignal(base, object, limit > 0 ? std::min(previous + count, limit) : previous + count);
    }
    g_dispatcherCv.notify_all();
    return previous;
}

// NTSTATUS NtReleaseSemaphore(HANDLE Handle, int32_t ReleaseCount, int32_t* PreviousCount)
PPC_FUNC(__imp__NtReleaseSemaphore)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    const int32_t previous = ReleaseSemaphore(base, object, ctx.r4.s32);
    if (ctx.r5.u32 != 0)
        GuestStoreU32(base, ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// int32_t KeReleaseSemaphore(KSEMAPHORE* Semaphore, int32_t Increment, int32_t Adjustment, bool Wait)
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    ctx.r3.u64 = uint32_t(ReleaseSemaphore(base, ctx.r3.u32, ctx.r5.s32));
}

// --- Tunggu ------------------------------------------------------------------

// NTSTATUS NtWaitForSingleObjectEx(HANDLE Handle, uint8_t WaitMode, bool Alertable, int64_t* Timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    ctx.r3.u64 = WaitForObjects(base, &object, 1, false, ctx.r6.u32, ctx.lr);
}

// NTSTATUS KeWaitForSingleObject(void* Object, uint32_t Reason, uint8_t Mode, bool Alertable, int64_t* Timeout)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    const uint32_t object = ctx.r3.u32;
    ctx.r3.u64 = WaitForObjects(base, &object, 1, false, ctx.r7.u32, ctx.lr);
}

// NTSTATUS KeWaitForMultipleObjects(uint32_t Count, void** Objects, uint32_t WaitType, uint32_t Reason,
//     uint8_t Mode, bool Alertable, int64_t* Timeout, KWAIT_BLOCK* WaitBlocks)
// WaitType: 0 = WaitAll, 1 = WaitAny.
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    const uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64);
    uint32_t objects[64];
    for (uint32_t i = 0; i < count; i++)
        objects[i] = GuestLoadU32(base, ctx.r4.u32 + i * 4);
    ctx.r3.u64 = WaitForObjects(base, objects, count, ctx.r5.u32 == 0, ctx.r9.u32, ctx.lr);
}

// NTSTATUS KeDelayExecutionThread(uint8_t Mode, bool Alertable, int64_t* Interval)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    int64_t interval = int64_t(GuestLoadU64(base, ctx.r5.u32));
    if (interval < 0)
        interval = -interval;
    if (interval == 0)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::microseconds(interval / 10));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// --- Handle ------------------------------------------------------------------

// NTSTATUS NtClose(HANDLE Handle)
PPC_FUNC(__imp__NtClose)
{
    std::lock_guard lock(g_handleLock);
    ctx.r3.u64 = g_handles.erase(ctx.r3.u32) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

// NTSTATUS NtDuplicateObject(HANDLE Source, HANDLE* Target, uint32_t Options)
PPC_FUNC(__imp__NtDuplicateObject)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r4.u32 != 0)
        GuestStoreU32(base, ctx.r4.u32, CreateGuestHandle(object));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, void* ObjectType, void** Object)
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    const uint32_t object = GuestHandleToObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r5.u32 != 0)
        GuestStoreU32(base, ctx.r5.u32, object);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

PPC_FUNC(__imp__ObDereferenceObject)
{
    // Objek tidak pernah dihapus untuk sementara, jadi tidak perlu hitungan referensi.
}
