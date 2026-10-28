// Alat debug berbasis simbol PDB (nama fungsi guest = sub_XXXXXXXX):
//  - Sampler: setiap beberapa detik, cetak call stack semua thread + daftar tunggu guest.
//    Aktif kalau variabel lingkungan DW6_SAMPLE diisi jumlah detik antar-sampel, mis. DW6_SAMPLE=10.
//  - PrintCallStack: dipakai crash handler untuk mencetak call stack saat crash.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

void DumpGuestWaits(uint8_t* base); // kernel/objects.cpp
static uint8_t* g_sampleBase = nullptr;

// Cetak sampai 24 tingkat call stack mulai dari `context` (yang boleh diubah oleh StackWalk64).
void PrintCallStack(HANDLE thread, CONTEXT& context)
{
    STACKFRAME64 frame{};
    frame.AddrPC = { context.Rip, 0, AddrModeFlat };
    frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
    frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
    for (int depth = 0; depth < 24; depth++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
            break;
        char buffer[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol))
        {
            // Baris sumber C++ hasil recompile -> komentar instruksi PPC di dekatnya bisa dicari.
            IMAGEHLP_LINE64 line{ sizeof(line) };
            DWORD lineDisplacement = 0;
            if (depth == 0 && SymGetLineFromAddr64(GetCurrentProcess(), frame.AddrPC.Offset, &lineDisplacement, &line))
                printf(" %s(%s:%lu)", symbol->Name, strrchr(line.FileName, '\\') ? strrchr(line.FileName, '\\') + 1 : line.FileName, line.LineNumber);
            else
                printf(" %s", symbol->Name);
        }
        else
            printf(" 0x%llX", frame.AddrPC.Offset);
    }
    printf("\n");
}

static void PrintThreadSymbols()
{
    const DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{ sizeof(entry) };
    printf("[sample] ---\n");
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self)
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr)
            continue;

        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(thread) != DWORD(-1))
        {
            if (GetThreadContext(thread, &context))
            {
                printf("[sample] thread %5lu:", entry.th32ThreadID);
                PrintCallStack(thread, context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    if (g_sampleBase != nullptr)
        DumpGuestWaits(g_sampleBase);
    fflush(stdout);
}

void InitDebugSymbols()
{
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

void StartSamplerIfRequested(uint8_t* base)
{
    g_sampleBase = base;
    const char* value = getenv("DW6_SAMPLE");
    const int seconds = value ? atoi(value) : 0;
    if (seconds <= 0)
        return;
    std::thread([seconds] {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            PrintThreadSymbols();
        }
    }).detach();
    printf("[sample] sampler aktif tiap %d detik\n", seconds);
}
