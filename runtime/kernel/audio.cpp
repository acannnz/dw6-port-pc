// Driver audio XAudio (sisi kernel). Game mendaftarkan "render driver client": sebuah callback
// yang harus dipanggil berkala; setiap panggilan game mengirim 1 frame audio (256 sampel x 6 kanal,
// float big-endian, 48 kHz) lewat XAudioSubmitRenderDriverFrame.
// TODO: putar suaranya ke speaker; untuk sekarang frame audio dibuang.
#include <atomic>
#include <chrono>
#include <thread>
#include "../kernel.h"

constexpr uint32_t DRIVER_HANDLE = 0x41550000; // nilai bebas, cukup bukan 0

static std::atomic<uint32_t> g_callback{ 0 };
static std::atomic<uint32_t> g_callbackContext{ 0 };
static std::atomic<uint64_t> g_framesSubmitted{ 0 };

static void AudioThread(uint8_t* base)
{
    PPCContext ctx{};
    InitHostThreadAsGuest(base, ctx, 0x40000);
    auto next = std::chrono::steady_clock::now();
    while (true)
    {
        // 256 sampel / 48000 Hz = 5.333 ms per frame
        next += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(next);
        if (const uint32_t callback = g_callback)
        {
            ctx.r3.u64 = g_callbackContext;
            (PPC_LOOKUP_FUNC(base, callback))(ctx, base);
        }
    }
}

// NTSTATUS XAudioRegisterRenderDriverClient(uint32_t* Callback, uint32_t* Driver)
// Callback menunjuk ke struct { uint32_t Function; uint32_t Context; }.
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    const uint32_t callbackInfo = ctx.r3.u32;
    g_callback = GuestLoadU32(base, callbackInfo + 0);
    g_callbackContext = GuestLoadU32(base, callbackInfo + 4);
    GuestStoreU32(base, ctx.r4.u32, DRIVER_HANDLE);

    static bool started = false;
    if (!started)
    {
        started = true;
        std::thread(AudioThread, base).detach();
        printf("[audio] render driver terdaftar: callback 0x%X\n", g_callback.load());
    }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS XAudioUnregisterRenderDriverClient(uint32_t Driver)
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS XAudioSubmitRenderDriverFrame(uint32_t Driver, float* Samples)
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    const uint64_t frames = ++g_framesSubmitted;
    if (frames == 1 || frames % 10000 == 0)
        printf("[audio] %llu frame audio diterima (belum diputar)\n", (unsigned long long)frames);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
