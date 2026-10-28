// Sampler debug: setiap beberapa detik, intip fungsi yang sedang dijalankan setiap thread
// (lewat simbol PDB, mis. "sub_822EE040") lalu cetak ke log. Berguna untuk menemukan thread
// yang berputar menunggu sesuatu tanpa memanggil kernel.
// Aktif kalau variabel lingkungan DW6_SAMPLE diisi jumlah detik antar-sampel, mis. DW6_SAMPLE=10.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

void DumpGuestWaits(uint8_t* base); // kernel/objects.cpp
extern uint8_t* g_sampleBase;
uint8_t* g_sampleBase = nullptr;

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
                // Telusuri beberapa tingkat call stack; fungsi guest bernama sub_XXXXXXXX.
                STACKFRAME64 frame{};
                frame.AddrPC = { context.Rip, 0, AddrModeFlat };
                frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
                frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
                printf("[sample] thread %5lu:", entry.th32ThreadID);
                for (int depth = 0; depth < 8; depth++)
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
                        printf(" %s", symbol->Name);
                    else
                        printf(" 0x%llX", frame.AddrPC.Offset);
                }
                printf("\n");
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

void StartSamplerIfRequested(uint8_t* base)
{
    g_sampleBase = base;
    const char* value = getenv("DW6_SAMPLE");
    const int seconds = value ? atoi(value) : 0;
    if (seconds <= 0)
        return;
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::thread([seconds] {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            PrintThreadSymbols();
        }
    }).detach();
    printf("[sample] sampler aktif tiap %d detik\n", seconds);
}
