// Fungsi sistem dasar: info proses, hak akses, error fatal, TLS, critical section, string.
#include <cstring>
#include <vector>
#include "../kernel.h"

PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    ctx.r3.u64 = 1; // proses game (bukan sistem)
}

// bool XexCheckExecutablePrivilege(uint32_t Privilege)
PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    ctx.r3.u64 = 0; // tidak punya hak istimewa apa pun
}

PPC_FUNC(__imp__KeBugCheck)
{
    GuestFatal("KeBugCheck", ctx);
}

PPC_FUNC(__imp__KeBugCheckEx)
{
    GuestFatal("KeBugCheckEx", ctx);
}

PPC_FUNC(__imp__HalReturnToFirmware)
{
    GuestFatal("HalReturnToFirmware (game minta keluar/restart)", ctx);
}

// --- Thread Local Storage -------------------------------------------------
// TODO: per-thread setelah ExCreateThread diimplementasikan; sekarang hanya 1 thread.
static std::vector<uint32_t> g_tlsValues;

PPC_FUNC(__imp__KeTlsAlloc)
{
    g_tlsValues.push_back(0);
    ctx.r3.u64 = g_tlsValues.size() - 1;
}

PPC_FUNC(__imp__KeTlsFree)
{
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    const uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = index < g_tlsValues.size() ? g_tlsValues[index] : 0;
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    const uint32_t index = ctx.r3.u32;
    if (index < g_tlsValues.size())
        g_tlsValues[index] = ctx.r4.u32;
    ctx.r3.u64 = index < g_tlsValues.size();
}

// --- Critical section -----------------------------------------------------
// TODO: kunci sungguhan setelah ada multi-thread; selama baru 1 thread, cukup tanpa kunci.
PPC_FUNC(__imp__RtlInitializeCriticalSection) {}
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) { ctx.r3.u64 = X_STATUS_SUCCESS; }
PPC_FUNC(__imp__RtlEnterCriticalSection) {}
PPC_FUNC(__imp__RtlLeaveCriticalSection) {}
PPC_FUNC(__imp__RtlTryEnterCriticalSection) { ctx.r3.u64 = 1; }

// --- String ---------------------------------------------------------------
// struct ANSI_STRING { uint16_t Length; uint16_t MaximumLength; char* Buffer; }
PPC_FUNC(__imp__RtlInitAnsiString)
{
    const uint32_t pString = ctx.r3.u32;
    const uint32_t pSource = ctx.r4.u32;
    const auto length = pSource ? uint16_t(strlen(reinterpret_cast<char*>(base + pSource))) : uint16_t(0);
    GuestStoreU16(base, pString + 0, length);
    GuestStoreU16(base, pString + 2, pSource ? length + 1 : 0);
    GuestStoreU32(base, pString + 4, pSource);
}
