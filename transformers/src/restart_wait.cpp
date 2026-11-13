#include "restart_wait.h"

#if defined(_WIN32)
// Before windows.h, whose macros clash with names in the runtime headers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <vector>

void tfHoldExitWatchdog(); // (profiler.cpp: the watchdog that ends a process 5 s after the stop request is told to leave this one)

namespace
{
    // The other threads of this process (the game's, the renderer's, the sound's, the loaders', the drivers' and the watchdogs'),
    // as handles that can suspend them. Collected before any is suspended: a suspended thread may hold the heap's lock.
    std::vector<HANDLE> otherThreads()
    {
        std::vector<HANDLE> threads;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0u);
        if (snapshot == INVALID_HANDLE_VALUE)
            return threads;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD me = GetCurrentProcessId(), self = GetCurrentThreadId();
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != me || entry.th32ThreadID == self)
                continue;
            if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID))
                threads.push_back(thread);
        }
        CloseHandle(snapshot);
        return threads;
    }
}

// Only a launcher that keeps the game's place by the process it started needs this: Steam (it puts SteamGameId / SteamAppId in the
// game's environment, on Windows and under Proton) and Wine itself. A plain Windows launch has nothing to keep: the first process
// exits at once, as it did before.
static bool launcherTracksTheProcess()
{
    wchar_t value[8];
    if (GetEnvironmentVariableW(L"SteamGameId", value, 8) != 0u || GetEnvironmentVariableW(L"SteamAppId", value, 8) != 0u)
        return true;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

void restart_wait::wait()
{
    if (!launcherTracksTheProcess())
        return;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0u)
        return;
    const wchar_t *slash = std::wcsrchr(path, L'\\');
    const wchar_t *name = slash ? slash + 1 : path;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    // The processes this one started from this program: the game, started again.
    std::vector<HANDLE> children;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD me = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (entry.th32ParentProcessID != me || entry.th32ProcessID == me || _wcsicmp(entry.szExeFile, name) != 0)
            continue;
        if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID))
            children.push_back(process);
        if (children.size() >= MAXIMUM_WAIT_OBJECTS)
            break;
    }
    CloseHandle(snapshot);
    if (children.empty())
        return;
    tfHoldExitWatchdog();
    std::fprintf(stderr, "[restart] the game started again (%zu process): this one waits for it to end, so the launcher keeps its place\n",
                 children.size());
    std::fflush(stderr);
    // Nothing of this process is needed while it waits, and nothing should run (the game, the renderer, the sound, the loaders and the
    // drivers' threads would else keep a core and the memory they use busy next to the new game): every other thread is suspended, and
    // this one only waits. Nothing is allocated or logged from here on, and the process is ended by TerminateProcess, which takes
    // no lock a suspended thread may hold (ExitProcess takes the loader's).
    const std::vector<HANDLE> threads = otherThreads();
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1)); // (its memory is not needed: given back)
    for (const HANDLE thread : threads)
        SuspendThread(thread);
    WaitForMultipleObjects(static_cast<DWORD>(children.size()), children.data(), TRUE, INFINITE);
    TerminateProcess(GetCurrentProcess(), 0u);
}
#else
void restart_wait::wait() {}
#endif
