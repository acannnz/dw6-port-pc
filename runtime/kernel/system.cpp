// Fungsi sistem dasar: info proses, hak akses, error fatal, string.
#include <cstring>
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

// uint32_t RtlNtStatusToDosError(NTSTATUS Status) -- kode NT -> kode error Win32
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    switch (ctx.r3.u32)
    {
    case 0x00000000: ctx.r3.u64 = 0; break;    // ERROR_SUCCESS
    case 0x00000102: ctx.r3.u64 = 1460; break; // ERROR_TIMEOUT
    case 0x00000103: ctx.r3.u64 = 997; break;  // ERROR_IO_PENDING
    case 0xC0000008: ctx.r3.u64 = 6; break;    // ERROR_INVALID_HANDLE
    case 0xC000000D: ctx.r3.u64 = 87; break;   // ERROR_INVALID_PARAMETER
    case 0xC0000011: ctx.r3.u64 = 38; break;   // ERROR_HANDLE_EOF
    case 0xC0000017: ctx.r3.u64 = 8; break;    // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000034: ctx.r3.u64 = 2; break;    // ERROR_FILE_NOT_FOUND
    case 0xC000003A: ctx.r3.u64 = 3; break;    // ERROR_PATH_NOT_FOUND
    default: ctx.r3.u64 = 31; break;           // ERROR_GEN_FAILURE
    }
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
