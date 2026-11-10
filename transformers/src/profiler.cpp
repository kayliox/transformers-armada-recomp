// Sampling profiler for performance work (Windows only).
//
// Set TF_PROFILE=<seconds>: a background thread samples the instruction
// pointer of every other thread once per millisecond, and after that many
// seconds writes the hottest functions per thread to runs/profile.txt (the
// symbols come from the executable's PDB). TF_PROFILE_AFTER=<seconds> delays
// the start. TF_PROFILE_STACKS=<depth, up to 40> also records call stacks: the report
// lists the innermost frames of the commonest ones, and for every function the share of
// samples that had it anywhere on the stack ("inclusive").

#if defined(_WIN32)

// Before windows.h, whose macros clash with names in the runtime headers.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

// Set by main() once the runtime exists.
PS2Runtime *g_tfRuntime = nullptr;

// The EE thread (the one that runs the game's frames), noted at its first frame: TF_PROFILE_EE_ONLY=1
// samples that thread alone, so the other threads (VIF1, the GS, the renderers) are never stopped and
// the EE does not end up waiting for a thread the sampler held.
static std::atomic<DWORD> g_tfEeThreadId{0};
void tfNoteEeThread()
{
    if (g_tfEeThreadId.load(std::memory_order_relaxed) == 0)
        g_tfEeThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

// Shows the hidden game window at (x, y) without activating it, so test runs on
// another monitor don't take keyboard focus (TF_MONITOR, see main.cpp).
void tfShowWindowWithoutFocus(void *window, int x, int y)
{
    HWND hwnd = static_cast<HWND>(window);
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

// CPU time the calling thread has used, in milliseconds (TF_FRAME_LOG): its cycle count
// (QueryThreadCycleTime counts at the time stamp counter's rate) over that rate, measured
// once against steady_clock.
double tfThreadCpuMs()
{
    static const double cyclesPerMs = []()
    {
        const auto start = std::chrono::steady_clock::now();
        const unsigned long long first = __rdtsc();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(20))
        {
        }
        const unsigned long long last = __rdtsc();
        return static_cast<double>(last - first) /
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }();
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return static_cast<double>(cycles) / cyclesPerMs;
}

namespace
{
    // The symbols from the executable's own folder only: not the build's PDB path written into the
    // executable (a copy of the build - the playtest's - would open and lock the build's PDB, and a
    // rebuild then fails to write it).
    std::string exeDirectory()
    {
        char path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string dir(path, n);
        const size_t slash = dir.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
    }

    const char *statusName(EeThreadStatus s)
    {
        switch (s)
        {
        case EeThreadStatus::Running: return "running";
        case EeThreadStatus::Ready: return "ready";
        case EeThreadStatus::Waiting: return "waiting";
        case EeThreadStatus::WaitingSuspended: return "wait+susp";
        case EeThreadStatus::Suspended: return "suspended";
        default: return "dormant";
        }
    }

    const char *waitName(EeWaitReason w)
    {
        switch (w)
        {
        case EeWaitReason::Sleep: return "sleep";
        case EeWaitReason::Semaphore: return "sema";
        case EeWaitReason::EventFlag: return "evflag";
        case EeWaitReason::VSync: return "vsync";
        case EeWaitReason::External: return "external";
        case EeWaitReason::Mpeg: return "mpeg";
        default: return "-";
        }
    }

    constexpr int kMaxStackDepth = 40;
    constexpr int kPrintedStackDepth = 8;
    using Stack = std::array<DWORD64, kMaxStackDepth>;

    struct ThreadSamples
    {
        uint64_t total = 0;
        std::unordered_map<DWORD64, uint64_t> byAddress;
        std::map<Stack, uint64_t> byStack; // TF_PROFILE_STACKS
    };

    // (TODO 164) TF_PROFILE_SPIKES=<ms>: the EE thread's samples split by the game frame they fell in - those of the
    // frames (from one VBlank wait's end to the next's) in which the thread itself ran over <ms> (its CPU time: the
    // game's code and what runs on the thread inside the wait - not the EE time charged, which stays low in the HQ's
    // slow frames), and the rest - so the report says what a slow frame does that the others do not. The
    // frames' spans are noted by the EE thread (tfNoteFrameWork), the samples' moments by the sampler.
    struct FrameSpan
    {
        int64_t start = 0, end = 0;
        float workMs = 0.0f;
        float cpuMs = 0.0f; // (the EE thread's own time in it: what tells a slow frame - its waits left out)
    };
    constexpr size_t kFrameSpans = 1u << 16;
    FrameSpan g_frameSpans[kFrameSpans];
    std::atomic<uint64_t> g_frameSpanCount{0};
    int64_t g_frameWorkStart = 0;

    double spikeThreshold()
    {
        static const double ms = std::getenv("TF_PROFILE_SPIKES") ? std::atof(std::getenv("TF_PROFILE_SPIKES")) : 0.0;
        return ms;
    }

    // (TF_PROFILE_SPIKES=<ms>,<most>: only frames under <most> ms count as slow - a level's load, hundreds of ms, left out)
    double spikeCeiling()
    {
        static const double ms = [] {
            const char *e = std::getenv("TF_PROFILE_SPIKES");
            const char *comma = e ? std::strchr(e, ',') : nullptr;
            return comma ? std::atof(comma + 1) : 1e30;
        }();
        return ms;
    }

    int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Return addresses of a suspended thread, innermost first, from x64 unwind data.
    template <size_t N>
    void captureStack(CONTEXT context, int depth, std::array<DWORD64, N> &frames)
    {
        frames.fill(0u);
        for (int i = 0; i < depth && i < static_cast<int>(N) && context.Rip != 0u; ++i)
        {
            frames[static_cast<size_t>(i)] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function)
            {
                // Leaf function without unwind data: the return address is at [rsp].
                context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void *handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisherFrame, nullptr);
        }
    }

    std::vector<DWORD> otherThreads(DWORD self)
    {
        std::vector<DWORD> ids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return ids;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
        {
            if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self)
                ids.push_back(entry.th32ThreadID);
        }
        CloseHandle(snap);
        return ids;
    }

    std::string symbolFor(HANDLE process, DWORD64 address)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol))
            return symbol->Name;
        // Outside every module: code made at run time (the VU1 JIT's blocks), as one entry.
        if (SymGetModuleBase64(process, address) == 0)
            return "[generated code]";
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }

    void report(const std::map<DWORD, ThreadSamples> &samples, double seconds,
                const std::map<std::string, uint64_t> &guest, uint64_t guestTotal,
                const std::vector<std::pair<std::string, const ThreadSamples *>> &extra = {})
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymSetOptions(SymGetOptions() | SYMOPT_IGNORE_CVREC); SymInitialize(process, exeDirectory().c_str(), TRUE);

        std::filesystem::create_directories("runs");
        std::ofstream out("runs/profile.txt");
        out << "TF_PROFILE: " << seconds << " s of samples (1 ms interval)\n";
        {
            std::vector<std::pair<std::string, uint64_t>> sorted(guest.begin(), guest.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b)
                      { return a.second > b.second; });
            out << "\nguest threads (" << guestTotal << " snapshots): thread / state / wait / pc / ra\n";
            for (size_t i = 0; i < sorted.size() && i < 40; ++i)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "  %6.2f%%  ",
                              guestTotal ? 100.0 * sorted[i].second / guestTotal : 0.0);
                out << line << sorted[i].first << "\n";
            }
        }
        const auto printThread = [&](const std::string &title, const ThreadSamples &data)
        {
            std::unordered_map<std::string, uint64_t> byFunction;
            for (const auto &[address, count] : data.byAddress)
                byFunction[symbolFor(process, address)] += count;
            std::vector<std::pair<std::string, uint64_t>> sorted(byFunction.begin(), byFunction.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b)
                      { return a.second > b.second; });
            out << "\n" << title << ": " << data.total << " samples\n";
            // TF_PROFILE_TOP=<n>: how many functions each thread lists (default 40)
            static const size_t topCount = []
            {
                const char *top = std::getenv("TF_PROFILE_TOP");
                const int n = top ? std::atoi(top) : 0;
                return n > 0 ? static_cast<size_t>(n) : size_t{40};
            }();
            for (size_t i = 0; i < sorted.size() && i < topCount; ++i)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "  %6.2f%%  %8llu  ", 100.0 * sorted[i].second / data.total,
                              static_cast<unsigned long long>(sorted[i].second));
                out << line << sorted[i].first << "\n";
            }
            // TF_PROFILE_LINES=1: the hottest source lines of the busiest function;
            // TF_PROFILE_LINES=<text>[,<text>...]: of every function whose name contains one
            // of these and that has at least 1% of the thread's samples.
            static const char *const byLine = std::getenv("TF_PROFILE_LINES");
            std::vector<size_t> targets;
            if (byLine && std::strcmp(byLine, "1") == 0)
            {
                if (!sorted.empty() && sorted[0].second * 10u >= data.total)
                    targets.push_back(0u);
            }
            else if (byLine)
            {
                std::vector<std::string> wanted;
                for (const char *p = byLine; *p;)
                {
                    const char *comma = std::strchr(p, ',');
                    const size_t length = comma ? static_cast<size_t>(comma - p) : std::strlen(p);
                    if (length != 0u)
                        wanted.emplace_back(p, length);
                    p += length + (comma ? 1u : 0u);
                }
                for (size_t i = 0; i < sorted.size() && sorted[i].second * 100u >= data.total; ++i)
                {
                    for (const std::string &text : wanted)
                    {
                        if (sorted[i].first.find(text) != std::string::npos)
                        {
                            targets.push_back(i);
                            break;
                        }
                    }
                }
            }
            for (const size_t target : targets)
            {
                std::map<std::string, uint64_t> lines;
                for (const auto &[address, count] : data.byAddress)
                {
                    if (symbolFor(process, address) != sorted[target].first)
                        continue;
                    IMAGEHLP_LINE64 info{};
                    info.SizeOfStruct = sizeof(info);
                    DWORD displacement = 0;
                    std::string key = "?";
                    if (SymGetLineFromAddr64(process, address, &displacement, &info))
                    {
                        const char *file = std::strrchr(info.FileName, '\\');
                        key = std::string(file ? file + 1 : info.FileName) + ":" + std::to_string(info.LineNumber);
                    }
                    lines[key] += count;
                }
                std::vector<std::pair<std::string, uint64_t>> hot(lines.begin(), lines.end());
                std::sort(hot.begin(), hot.end(), [](const auto &a, const auto &b)
                          { return a.second > b.second; });
                out << "  lines of " << sorted[target].first << ":\n";
                for (size_t i = 0; i < hot.size() && i < 30; ++i)
                {
                    char line[64];
                    std::snprintf(line, sizeof(line), "  %6.2f%%  ", 100.0 * hot[i].second / data.total);
                    out << line << hot[i].first << "\n";
                }
            }
            // (n11f, local diagnostic) TF_PROFILE_FILES=1: every sample of the thread by its source file (the
            // inlined runtime parts - leaf calls, checkpoints - by their own header), then the hottest lines
            // outside the generated code.
            static const bool byFile = std::getenv("TF_PROFILE_FILES") != nullptr;
            if (byFile && data.total > 1000u)
            {
                std::map<std::string, uint64_t> files, rtLines;
                for (const auto &[address, count] : data.byAddress)
                {
                    IMAGEHLP_LINE64 info{};
                    info.SizeOfStruct = sizeof(info);
                    DWORD displacement = 0;
                    std::string file;
                    std::string key = "?";
                    if (SymGetLineFromAddr64(process, address, &displacement, &info))
                    {
                        const char *f = std::strrchr(info.FileName, '\\');
                        file = f ? f + 1 : info.FileName;
                        key = file + ":" + std::to_string(info.LineNumber);
                        if (std::strstr(info.FileName, "\\work\\generated\\") || std::strstr(info.FileName, "\\work\\batches\\"))
                            file = "<generated>";
                    }
                    else
                        file = "? " + symbolFor(process, address);
                    files[file] += count;
                    if (file != "<generated>")
                        rtLines[key + "  [" + symbolFor(process, address) + "]"] += count;
                }
                for (auto *table : {&files, &rtLines})
                {
                    std::vector<std::pair<std::string, uint64_t>> hot(table->begin(), table->end());
                    std::sort(hot.begin(), hot.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    out << (table == &files ? "  by file:\n" : "  lines outside the generated code:\n");
                    for (size_t i = 0; i < hot.size() && i < (table == &files ? 60u : 200u); ++i)
                    {
                        char line[64];
                        std::snprintf(line, sizeof(line), "  %6.2f%% %7llu  ", 100.0 * hot[i].second / data.total,
                                      static_cast<unsigned long long>(hot[i].second));
                        out << line << hot[i].first << "\n";
                    }
                }
            }
            if (data.byStack.empty())
                return;
            // Call stacks, innermost first, merged by function name.
            std::map<std::string, uint64_t> byStackName;
            for (const auto &[stack, count] : data.byStack)
            {
                std::string key;
                int printed = 0;
                for (DWORD64 address : stack)
                {
                    if (address == 0u || printed++ == kPrintedStackDepth)
                        break;
                    if (!key.empty())
                        key += " <- ";
                    key += symbolFor(process, address);
                }
                byStackName[key] += count;
            }
            // Inclusive time: samples with the function anywhere on the recorded stack.
            std::map<std::string, uint64_t> inclusive;
            for (const auto &[stack, count] : data.byStack)
            {
                std::vector<std::string> seen;
                for (DWORD64 address : stack)
                {
                    if (address == 0u)
                        break;
                    std::string name = symbolFor(process, address);
                    if (std::find(seen.begin(), seen.end(), name) == seen.end())
                        seen.push_back(std::move(name));
                }
                for (const std::string &name : seen)
                    inclusive[name] += count;
            }
            std::vector<std::pair<std::string, uint64_t>> inclusiveHot(inclusive.begin(), inclusive.end());
            std::sort(inclusiveHot.begin(), inclusiveHot.end(), [](const auto &a, const auto &b)
                      { return a.second > b.second; });
            out << "  inclusive:\n";
            for (size_t i = 0; i < inclusiveHot.size() && i < 40; ++i)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "  %6.2f%%  ", 100.0 * inclusiveHot[i].second / data.total);
                out << line << inclusiveHot[i].first << "\n";
            }
            std::vector<std::pair<std::string, uint64_t>> stacks(byStackName.begin(), byStackName.end());
            std::sort(stacks.begin(), stacks.end(), [](const auto &a, const auto &b)
                      { return a.second > b.second; });
            out << "  stacks:\n";
            for (size_t i = 0; i < stacks.size() && i < 60; ++i)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "  %6.2f%%  ", 100.0 * stacks[i].second / data.total);
                out << line << stacks[i].first << "\n";
            }
        };
        for (const auto &[tid, data] : samples)
            if (data.total >= 50)
                printThread("thread " + std::to_string(tid), data);
        for (const auto &[title, data] : extra)
            if (data && data->total > 0)
                printThread(title, *data);
        SymCleanup(process);
    }

    void samplerMain(double seconds, double delay)
    {
        if (delay > 0.0)
            Sleep(static_cast<DWORD>(delay * 1000.0));
        const DWORD self = GetCurrentThreadId();
        std::map<DWORD, ThreadSamples> samples;
        std::unordered_map<DWORD, HANDLE> handles;
        const auto start = std::chrono::steady_clock::now();
        auto lastEnumerate = start - std::chrono::seconds(10);
        std::vector<DWORD> ids;
        std::map<std::string, uint64_t> guest;
        uint64_t guestTotal = 0;
        const char *stacksEnv = std::getenv("TF_PROFILE_STACKS");
        const int stackDepth = stacksEnv ? std::clamp(std::atoi(stacksEnv), 0, kMaxStackDepth) : 0;
        const char *eeOnlyEnv = std::getenv("TF_PROFILE_EE_ONLY");
        const bool eeOnly = eeOnlyEnv && *eeOnlyEnv && *eeOnlyEnv != '0';
        const char *onlyNamed = std::getenv("TF_PROFILE_THREAD");
        if (onlyNamed && !*onlyNamed)
            onlyNamed = nullptr;
        const bool spikes = spikeThreshold() > 0.0;
        std::vector<std::pair<int64_t, Stack>> eeTimed; // (TF_PROFILE_SPIKES: the EE thread's samples with their moment)
        if (spikes)
            eeTimed.reserve(static_cast<size_t>(seconds * 1100.0) + 1024u);
        timeBeginPeriod(1);
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEnumerate > std::chrono::seconds(1))
            {
                ids = otherThreads(self);
                if (eeOnly)
                {
                    const DWORD ee = g_tfEeThreadId.load(std::memory_order_relaxed);
                    ids.assign(ee != 0 ? 1u : 0u, ee);
                }
                else if (onlyNamed)
                {
                    // TF_PROFILE_THREAD=<text>: only the threads whose description contains it (NativeRender,
                    // Present, VBlankWatch...) - the others never stopped.
                    std::vector<DWORD> named;
                    for (DWORD id : ids)
                    {
                        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
                        if (!th)
                            continue;
                        PWSTR description = nullptr;
                        if (SUCCEEDED(GetThreadDescription(th, &description)) && description)
                        {
                            std::wstring d(description);
                            if (d.find(std::wstring(onlyNamed, onlyNamed + std::strlen(onlyNamed))) != std::wstring::npos)
                                named.push_back(id);
                            LocalFree(description);
                        }
                        CloseHandle(th);
                    }
                    ids = std::move(named);
                }
                lastEnumerate = now;
            }
            for (DWORD id : ids)
            {
                HANDLE &h = handles[id];
                if (!h)
                    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
                const int64_t sampledAt = spikes ? nowNs() : 0;
                if (!h || SuspendThread(h) == static_cast<DWORD>(-1))
                    continue;
                // Nothing is allocated while the thread is stopped: it may hold the heap's lock.
                CONTEXT context{};
                context.ContextFlags = stackDepth > 0 ? CONTEXT_FULL : CONTEXT_CONTROL;
                Stack frames{};
                const bool sampled = GetThreadContext(h, &context) != 0;
                if (sampled && stackDepth > 0)
                    captureStack(context, stackDepth, frames);
                ResumeThread(h);
                if (sampled)
                {
                    ThreadSamples &t = samples[id];
                    ++t.total;
                    ++t.byAddress[context.Rip];
                    if (stackDepth > 0)
                        ++t.byStack[frames];
                    if (spikes && id == g_tfEeThreadId.load(std::memory_order_relaxed) && eeTimed.size() < eeTimed.capacity())
                    {
                        if (stackDepth == 0)
                            frames[0] = context.Rip;
                        eeTimed.emplace_back(sampledAt, frames);
                    }
                }
            }
            if (g_tfRuntime && !eeOnly && !onlyNamed) // (the snapshot takes a lock the EE takes too)
            {
                const EeKernelSnapshot snap = g_tfRuntime->eeScheduler().snapshot();
                ++guestTotal;
                for (const EeThreadSnapshot &t : snap.threads)
                {
                    char key[160];
                    std::snprintf(key, sizeof(key), "t%-3d %-9s %-8s pc=%08x ra=%08x", t.id, statusName(t.status),
                                  t.status == EeThreadStatus::Waiting ? waitName(t.waitReason) : "-", t.pc, t.ra);
                    ++guest[key];
                }
            }
            Sleep(1);
        }
        timeEndPeriod(1);
        for (auto &[id, h] : handles)
            if (h)
                CloseHandle(h);
        // (TF_PROFILE_SPIKES) The EE thread's samples by the frame they fell in: its work span, slow or not.
        ThreadSamples spike, normal;
        std::vector<std::pair<std::string, const ThreadSamples *>> extra;
        if (spikes)
        {
            const uint64_t made = g_frameSpanCount.load(std::memory_order_acquire);
            std::vector<FrameSpan> spans;
            for (uint64_t k = made > kFrameSpans ? made - kFrameSpans : 0u; k < made; ++k)
                spans.push_back(g_frameSpans[k % kFrameSpans]);
            std::sort(spans.begin(), spans.end(), [](const FrameSpan &a, const FrameSpan &b) { return a.start < b.start; });
            const int64_t first = eeTimed.empty() ? 0 : eeTimed.front().first, last = eeTimed.empty() ? 0 : eeTimed.back().first;
            const auto slow = [](const FrameSpan &f) { return f.cpuMs > spikeThreshold() && f.cpuMs < spikeCeiling(); };
            size_t slowFrames = 0, allFrames = 0;
            double slowMs = 0.0, allMs = 0.0, slowEe = 0.0, allEe = 0.0;
            for (const FrameSpan &f : spans)
                if (f.start >= first && f.end <= last && f.cpuMs < spikeCeiling())
                {
                    ++allFrames;
                    allMs += f.cpuMs;
                    allEe += f.workMs;
                    if (slow(f))
                        ++slowFrames, slowMs += f.cpuMs, slowEe += f.workMs;
                }
            for (const auto &[at, stack] : eeTimed)
            {
                auto it = std::upper_bound(spans.begin(), spans.end(), at, [](int64_t t, const FrameSpan &f) { return t < f.start; });
                if (it == spans.begin())
                    continue;
                --it;
                if (at > it->end || it->cpuMs >= spikeCeiling())
                    continue;
                ThreadSamples &b = slow(*it) ? spike : normal;
                ++b.total;
                ++b.byAddress[stack[0]];
                if (stackDepth > 0)
                    ++b.byStack[stack];
            }
            char title[200];
            std::snprintf(title, sizeof(title), "EE thread in the %zu frames of the %zu with over %.1f ms of its own time (%.1f ms on average, %.1f ms of EE time)",
                          slowFrames, allFrames, spikeThreshold(), slowFrames ? slowMs / slowFrames : 0.0, slowFrames ? slowEe / slowFrames : 0.0);
            extra.emplace_back(title, &spike);
            std::snprintf(title, sizeof(title), "EE thread in the other %zu frames (%.1f ms of its own time on average, %.1f ms of EE time)", allFrames - slowFrames,
                          allFrames > slowFrames ? (allMs - slowMs) / (allFrames - slowFrames) : 0.0,
                          allFrames > slowFrames ? (allEe - slowEe) / (allFrames - slowFrames) : 0.0);
            extra.emplace_back(title, &normal);
        }
        report(samples, seconds, guest, guestTotal, extra);
        std::fprintf(stderr, "[tf] profile written to runs/profile.txt\n");
    }

    // TF_WATCH_ARM=<hex value>: once the first watched word holds this value,
    // write-protect the host page behind the watched range. Every write to that
    // page then faults; writes that hit the range are logged with the host
    // call stack (recompiled functions are named after their guest address),
    // and each faulting instruction is single-stepped with the page open.
    struct PageGuard
    {
        std::atomic<bool> armed{false};
        uint8_t *rangeStart = nullptr;
        uint8_t *rangeEnd = nullptr;
        uint8_t *page = nullptr;
        std::atomic<int> hits{0};
        bool handlerInstalled = false;
        bool hasIgnore = false; // writes that leave `ignore` in the first word are not logged
        uint32_t ignore = 0;
        // TF_WATCH_LOG=<offset>,<bytes> (hex): only writes to that part of the range are
        // logged; TF_WATCH_HITS=<n>: how many (40).
        size_t logFrom = 0, logTo = ~size_t(0);
        int maxHits = 40;
    } g_guard;
    thread_local bool t_reprotect = false;
    // A write to the guarded range, kept until the instruction has run and its value is known.
    thread_local int t_pendingFrames = -1;
    thread_local DWORD64 t_pending[16];
    thread_local size_t t_pendingOffset = 0;
    thread_local uint8_t t_before[16]; // the range's first bytes before the write (logged only if they change)

    LONG CALLBACK guardHandler(EXCEPTION_POINTERS *info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP && t_reprotect)
        {
            t_reprotect = false;
            if (t_pendingFrames >= 0)
            {
                uint32_t value = 0;
                std::memcpy(&value, g_guard.rangeStart, sizeof(value));
                const size_t span = std::min<size_t>(sizeof(t_before), static_cast<size_t>(g_guard.rangeEnd - g_guard.rangeStart));
                if (std::memcmp(t_before, g_guard.rangeStart, span) != 0 &&
                    !(g_guard.hasIgnore && value == g_guard.ignore) && t_pendingOffset >= g_guard.logFrom &&
                    t_pendingOffset < g_guard.logTo && g_guard.hits.fetch_add(1) < g_guard.maxHits)
                {
                    HANDLE process = GetCurrentProcess();
                    std::fprintf(stderr, "[tf:guard] write to +%zu (first word now %08x) at %s\n", t_pendingOffset, value,
                                 symbolFor(process, t_pending[0]).c_str());
                    for (int i = 0; i < t_pendingFrames; ++i)
                        std::fprintf(stderr, "[tf:guard]     %s\n", symbolFor(process, t_pending[i]).c_str());
                    std::fflush(stderr);
                }
                t_pendingFrames = -1;
            }
            if (g_guard.armed.load())
            {
                DWORD old = 0;
                VirtualProtect(g_guard.page, 4096, PAGE_READONLY, &old);
            }
            info->ContextRecord->EFlags &= ~0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || !g_guard.armed.load())
            return EXCEPTION_CONTINUE_SEARCH;
        uint8_t *target = reinterpret_cast<uint8_t *>(info->ExceptionRecord->ExceptionInformation[1]);
        if (info->ExceptionRecord->ExceptionInformation[0] != 1 || target < g_guard.page || target >= g_guard.page + 4096)
            return EXCEPTION_CONTINUE_SEARCH;

        // (a wide store - a vector one, up to 64 bytes - that starts just below the range writes it too)
        if (target + 64 > g_guard.rangeStart && target < g_guard.rangeEnd)
        {
            // Logged after the instruction has run (single-step above), with the value it left.
            t_pendingOffset = target >= g_guard.rangeStart ? static_cast<size_t>(target - g_guard.rangeStart) : 0u;
            std::memcpy(t_before, g_guard.rangeStart, std::min<size_t>(sizeof(t_before), static_cast<size_t>(g_guard.rangeEnd - g_guard.rangeStart)));
            t_pendingFrames = 0;
            HANDLE process = GetCurrentProcess();
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = info->ContextRecord->Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = info->ContextRecord->Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = info->ContextRecord->Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            CONTEXT copy = *info->ContextRecord;
            while (t_pendingFrames < 16)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &copy, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                    frame.AddrPC.Offset == 0)
                    break;
                t_pending[t_pendingFrames++] = frame.AddrPC.Offset;
            }
        }
        DWORD old = 0;
        VirtualProtect(g_guard.page, 4096, PAGE_READWRITE, &old);
        t_reprotect = true;
        info->ContextRecord->EFlags |= 0x100u; // trap after this instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // TF_WATCH=<hex address>[,<bytes>]: polls a range of EE RAM and logs every
    // change, with what each host thread was executing right after it (to find
    // who corrupts a guest object).
    void watchMain(uint32_t address, uint32_t length)
    {
        if (const char *logEnv = std::getenv("TF_WATCH_LOG"))
        {
            char *end = nullptr;
            g_guard.logFrom = std::strtoul(logEnv, &end, 16);
            g_guard.logTo = g_guard.logFrom + (end && *end == ',' ? std::strtoul(end + 1, nullptr, 16) : 4u);
        }
        if (const char *hitsEnv = std::getenv("TF_WATCH_HITS"))
            g_guard.maxHits = std::atoi(hitsEnv);
        const char *armEnv = std::getenv("TF_WATCH_ARM");
        const bool armable = armEnv != nullptr;
        const uint32_t armValue = armable ? static_cast<uint32_t>(std::strtoul(armEnv, nullptr, 16)) : 0u;
        while (!g_tfRuntime || !g_tfRuntime->memory().getRDRAM())
            Sleep(10);
        // RAM or scratchpad (0x70000000).
        const uint8_t *watched = getConstMemPtr(g_tfRuntime->memory().getRDRAM(), address);
        std::vector<uint8_t> last(watched, watched + length);

        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymSetOptions(SymGetOptions() | SYMOPT_IGNORE_CVREC); SymInitialize(process, exeDirectory().c_str(), TRUE);
        const DWORD self = GetCurrentThreadId();
        const auto start = std::chrono::steady_clock::now();
        std::unordered_map<DWORD, HANDLE> handles;

        for (int events = 0; events < 400;)
        {
            if (std::memcmp(watched, last.data(), length) == 0)
            {
                YieldProcessor();
                continue;
            }

            // Where every other thread is, as close to the write as we can get.
            std::vector<std::pair<DWORD, DWORD64>> rips;
            for (DWORD id : otherThreads(self))
            {
                HANDLE &h = handles[id];
                if (!h)
                    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
                if (!h || SuspendThread(h) == static_cast<DWORD>(-1))
                    continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &context))
                    rips.emplace_back(id, context.Rip);
                ResumeThread(h);
            }

            std::vector<uint8_t> now(watched, watched + length);
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::string line;
            char text[160];
            std::snprintf(text, sizeof(text), "[tf:watch] %.4fs %08x:", t, address);
            line += text;
            for (uint32_t i = 0; i + 4 <= length; i += 4)
            {
                uint32_t before = 0, after = 0;
                std::memcpy(&before, last.data() + i, 4);
                std::memcpy(&after, now.data() + i, 4);
                std::snprintf(text, sizeof(text), before == after ? " %08x" : " %08x>%08x", before, after);
                line += text;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            if (g_tfRuntime)
            {
                const EeKernelSnapshot snap = g_tfRuntime->eeScheduler().snapshot();
                for (const EeThreadSnapshot &th : snap.threads)
                    if (th.status == EeThreadStatus::Running)
                        std::fprintf(stderr, "[tf:watch]   guest t%d pc=%08x ra=%08x\n", th.id, th.pc, th.ra);
            }
            for (const auto &[id, rip] : rips)
            {
                const std::string name = symbolFor(process, rip);
                if (name.rfind("Nt", 0) == 0 || name.rfind("Zw", 0) == 0)
                    continue; // waiting threads
                std::fprintf(stderr, "[tf:watch]   thread %lu: %s\n", static_cast<unsigned long>(id), name.c_str());
            }
            last.swap(now);
            ++events;

            uint32_t first = 0;
            std::memcpy(&first, last.data(), 4);
            if (armable && !g_guard.armed.load() && first == armValue)
            {
                uint8_t *range = const_cast<uint8_t *>(watched);
                g_guard.rangeStart = range;
                g_guard.rangeEnd = range + length;
                g_guard.page = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(range) & ~uintptr_t(4095));
                if (!g_guard.handlerInstalled)
                {
                    AddVectoredExceptionHandler(1, guardHandler);
                    g_guard.handlerInstalled = true;
                }
                g_guard.armed.store(true);
                DWORD old = 0;
                VirtualProtect(g_guard.page, 4096, PAGE_READONLY, &old);
                std::fprintf(stderr, "[tf:guard] armed on %08x\n", address);
            }
        }
        for (auto &[id, h] : handles)
            if (h)
                CloseHandle(h);
    }

    // Logs the host call stack of every other thread. The frames are collected while a
    // thread is suspended and named afterwards: naming allocates, and the suspended thread
    // may hold the heap lock.
    void logAllThreads(const char *label)
    {
        constexpr int kDepth = 20;
        struct Captured
        {
            DWORD id = 0;
            std::array<DWORD64, kDepth> frames{};
        };
        std::vector<Captured> stacks;
        const std::vector<DWORD> ids = otherThreads(GetCurrentThreadId());
        stacks.reserve(ids.size());
        for (DWORD id : ids)
        {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
            if (!h)
                continue;
            if (SuspendThread(h) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                Captured captured;
                captured.id = id;
                if (GetThreadContext(h, &context))
                    captureStack(context, kDepth, captured.frames);
                ResumeThread(h);
                stacks.push_back(captured);
            }
            CloseHandle(h);
        }
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymSetOptions(SymGetOptions() | SYMOPT_IGNORE_CVREC); SymInitialize(process, exeDirectory().c_str(), TRUE);
        for (const Captured &captured : stacks)
        {
            std::fprintf(stderr, "[tf] %s: thread %lu\n", label, static_cast<unsigned long>(captured.id));
            for (int i = 0; i < kDepth && captured.frames[static_cast<size_t>(i)] != 0u; ++i)
                std::fprintf(stderr, "[tf] %s:   #%d %s\n", label, i,
                             symbolFor(process, captured.frames[static_cast<size_t>(i)]).c_str());
        }
        std::fflush(stderr);
    }

    // (TODO 256) Where the stop request came from - the window closed, a menu's quit, a test's TF_QUIT_VSYNC, the updater,
    // an extension that could not install... - logged once, on the requesting thread, with its caller's stack (the
    // runtime's own line says whether it came before the game started). Repeated by the exit watchdog.
    std::mutex g_stopFromMutex;
    std::string g_stopFrom;

    void noteStopSource(bool beforeRun)
    {
        void *frames[24] = {};
        const USHORT count = CaptureStackBackTrace(2, 24, frames, nullptr); // (past this and PS2Runtime::requestStop)
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymSetOptions(SymGetOptions() | SYMOPT_IGNORE_CVREC);
        SymInitialize(process, exeDirectory().c_str(), TRUE);
        char line[64];
        std::snprintf(line, sizeof(line), "thread %lu%s:", static_cast<unsigned long>(GetCurrentThreadId()),
                      beforeRun ? ", before the game started" : "");
        std::string text = line;
        for (USHORT i = 0; i < count && i < 10; ++i)
        {
            const std::string name = symbolFor(process, reinterpret_cast<DWORD64>(frames[i]));
            if (name == "BaseThreadInitThunk" || name == "RtlUserThreadStart")
                break;
            text += (i ? " <- " : " ") + name;
            // (the requesting function's line too: one function asks for a stop at many places)
            IMAGEHLP_LINE64 where{};
            where.SizeOfStruct = sizeof(where);
            DWORD offset = 0;
            if (i == 0 && SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(frames[i]) - 1, &offset, &where) && where.FileName)
            {
                const char *file = std::strrchr(where.FileName, '\\');
                text += " (" + std::string(file ? file + 1 : where.FileName) + ":" + std::to_string(where.LineNumber) + ")";
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_stopFromMutex);
            g_stopFrom = text;
        }
        std::fprintf(stderr, "[tf] the stop came from %s\n", text.c_str());
        std::fflush(stderr);
    }

    // Shutdown watchdog. Once the runtime was asked to stop (the window was closed, or
    // TF_QUIT_VSYNC was reached) the process should be gone within moments. If it is still
    // here after a few seconds something is stuck: say where, and end the process so that
    // neither a player nor a test run is left with a dead window.
    void exitWatchdogMain()
    {
        while (!g_tfRuntime || !g_tfRuntime->isStopRequested())
            Sleep(250);
        Sleep(5000);
        std::string from;
        {
            std::lock_guard<std::mutex> lock(g_stopFromMutex);
            from = g_stopFrom;
        }
        std::fprintf(stderr, "[tf] exit watchdog: still running 5 s after the stop request (it came from %s)\n",
                     from.empty() ? "an unknown place" : from.c_str());
        logAllThreads("exit watchdog");
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }

    // ---- the hang watchdog and the crash dump (the user, 2026-10-22: see what happened while they
    // played) ------------------------------------------------------------------------------------
    // No game frame for kHangSeconds (the frame hook, tfWatchdogFrame, stopped being called: a black
    // screen, a freeze): the EE kernel's state (every guest thread with its pc, ra and what it waits
    // for, the semaphores and event flags waited on, the scheduler's cycle and next deadline, the
    // VBlank period), every host thread's stack, a minidump (stacks and the memory they point at)
    // and the guest's RAM go to runs\hang_<n>_* - once a freeze, and when the frames come back it
    // says after how long. TF_WATCHDOG=0 turns it off. A crash writes runs\crash_<time>.dmp too.
    // While a level loads (0x6CD324 set) it waits kLoadHangSeconds instead (TODO 185 b).
    constexpr int kHangSeconds = 6;
    constexpr int kLoadHangSeconds = 20; // (the longest load seen: ~5 s; a fast load ~1 s)
    std::atomic<uint64_t> g_gameFrames{0};

    std::filesystem::path runsDir()
    {
        std::error_code e;
        const std::filesystem::path dir = std::filesystem::current_path(e) / "runs";
        std::filesystem::create_directories(dir, e);
        return dir;
    }

    std::string stamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char s[32];
        std::snprintf(s, sizeof(s), "%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return s;
    }

    bool writeMinidump(const std::filesystem::path &path, EXCEPTION_POINTERS *exception)
    {
        HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exception;
        info.ClientPointers = FALSE;
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpScanMemory | MiniDumpWithUnloadedModules);
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception ? &info : nullptr, nullptr,
                                          nullptr);
        CloseHandle(file);
        return ok != FALSE;
    }

    void logKernelState(const char *label)
    {
        if (!g_tfRuntime)
            return;
        EeScheduler &scheduler = g_tfRuntime->eeScheduler();
        const EeKernelSnapshot snap = scheduler.snapshot();
        std::fprintf(stderr, "[tf] %s: vsync tick %llu, VBlank period %.3f ms, EE cycle %llu, slice end %llu, next deadline %llu, running thread %d\n",
                     label, static_cast<unsigned long long>(scheduler.currentVSyncTick()), scheduler.vblankPeriod().count() / 1e6,
                     static_cast<unsigned long long>(snap.eeCycle), static_cast<unsigned long long>(snap.sliceEndCycle),
                     static_cast<unsigned long long>(snap.nextEventCycle), snap.runningThreadId);
        for (const EeThreadSnapshot &t : snap.threads)
            std::fprintf(stderr, "[tf] %s:   guest thread %d %-9s %-8s (id %d) pc %08x ra %08x sp %08x entry %08x prio %d depth %u\n", label, t.id,
                         statusName(t.status), t.status == EeThreadStatus::Waiting || t.status == EeThreadStatus::WaitingSuspended ? waitName(t.waitReason) : "-",
                         t.waitId, t.pc, t.ra, t.sp, t.entry, t.currentPriority, t.invocationDepth);
        for (const EeSemaphoreSnapshot &s : snap.semaphores)
            if (s.waiters)
                std::fprintf(stderr, "[tf] %s:   semaphore %d count %d/%d, %u waiting\n", label, s.id, s.count, s.maxCount, s.waiters);
        for (const EeEventFlagSnapshot &f : snap.eventFlags)
            if (f.waiters)
                std::fprintf(stderr, "[tf] %s:   event flag %d bits %08x, %u waiting\n", label, f.id, f.bits, f.waiters);
        std::fflush(stderr);
    }

    void hangWatchdogMain()
    {
        while (!g_tfRuntime)
            Sleep(250);
        uint64_t last = g_gameFrames.load();
        auto changed = std::chrono::steady_clock::now();
        bool frozen = false;
        int count = 0;
        while (!g_tfRuntime->isStopRequested())
        {
            Sleep(500);
            const uint64_t frames = g_gameFrames.load();
            const auto now = std::chrono::steady_clock::now();
            if (frames != last)
            {
                if (frozen)
                    std::fprintf(stderr, "[tf] watchdog: game frames again after %.1f s\n",
                                 std::chrono::duration<double>(now - changed).count());
                frozen = false;
                last = frames;
                changed = now;
                continue;
            }
            if (frozen || frames == 0u || now - changed < std::chrono::seconds(kHangSeconds))
                continue;
            // (a level loading: the game sets 0x6CD324 while it does - a long load is no freeze; but a load with no
            // frame for kLoadHangSeconds is: TODO 185 b froze in a warp tunnel, where this test kept the dump away)
            const bool loading = [] {
                const uint8_t *rdram = g_tfRuntime->memory().getRDRAM();
                return rdram && rdram[0x006CD324u] != 0u;
            }();
            if (loading && now - changed < std::chrono::seconds(kLoadHangSeconds))
                continue;
            frozen = true;
            ++count;
            const std::string name = "hang_" + std::to_string(count) + "_" + stamp();
            std::fprintf(stderr, "[tf] watchdog: no game frame for %d s%s (frame %llu) - %s\n", loading ? kLoadHangSeconds : kHangSeconds, loading ? ", in a load" : "",
                         static_cast<unsigned long long>(frames), name.c_str());
            logKernelState("watchdog");
            logAllThreads("watchdog");
            const std::filesystem::path dir = runsDir();
            const bool dumped = writeMinidump(dir / (name + ".dmp"), nullptr);
            bool ram = false;
            if (uint8_t *rdram = g_tfRuntime->memory().getRDRAM())
            {
                std::ofstream out(dir / (name + "_ram.bin"), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(rdram), PS2_RAM_SIZE);
                ram = static_cast<bool>(out);
            }
            std::fprintf(stderr, "[tf] watchdog: %s.dmp %s, %s_ram.bin %s\n", name.c_str(), dumped ? "written" : "failed", name.c_str(),
                         ram ? "written" : "failed");
            std::fflush(stderr);
        }
    }

    // (TODO 163) The C runtime's own fatal ends - an invalid parameter to a CRT function, abort(), a pure virtual call -
    // end the process with 0xC0000409 (a fast fail) and never reach crashFilter: the calling thread's stack, every
    // thread's, the kernel's state and a minidump (runs\fail_<time>.dmp) are written first. Then the CRT's way, as before.
    void reportFatal(const char *what)
    {
        static std::atomic<bool> once{false};
        if (once.exchange(true))
            return;
        const std::string name = "fail_" + stamp() + ".dmp";
        std::fprintf(stderr, "[tf] fatal: %s on thread %lu - %s\n", what, GetCurrentThreadId(), name.c_str());
        void *frames[48] = {};
        const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_IGNORE_CVREC);
        SymInitialize(process, exeDirectory().c_str(), TRUE);
        for (USHORT i = 0; i < n; ++i)
        {
            const DWORD64 a = reinterpret_cast<DWORD64>(frames[i]);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD displacement = 0;
            char where[300] = "";
            if (SymGetLineFromAddr64(process, a, &displacement, &line))
            {
                const char *file = std::strrchr(line.FileName, '\\');
                std::snprintf(where, sizeof(where), " (%s:%lu)", file ? file + 1 : line.FileName, line.LineNumber);
            }
            std::fprintf(stderr, "[tf] fatal   #%u %s%s\n", static_cast<unsigned>(i), symbolFor(process, a).c_str(), where);
        }
        std::fflush(stderr);
        logKernelState("fatal");
        logAllThreads("fatal");
        const bool ok = writeMinidump(runsDir() / name, nullptr);
        std::fprintf(stderr, "[tf] fatal: %s %s\n", name.c_str(), ok ? "written" : "failed");
        std::fflush(stderr);
    }

    void __cdecl invalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
    {
        reportFatal("an invalid parameter to a C runtime function");
        __fastfail(FAST_FAIL_INVALID_ARG); // (the CRT's own end, as without the handler)
    }

    void __cdecl pureCall()
    {
        reportFatal("a pure virtual function called");
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }

    void onAbort(int)
    {
        reportFatal("abort()");
    }

    LONG WINAPI crashFilter(EXCEPTION_POINTERS *exception)
    {
        static std::atomic<bool> once{false};
        if (!once.exchange(true))
        {
            const std::string name = "crash_" + stamp() + ".dmp";
            std::fprintf(stderr, "[tf] crash: exception %08lx at %p - %s\n",
                         exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0ul,
                         exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionAddress : nullptr, name.c_str());
            std::fflush(stderr);
            logKernelState("crash");
            const bool ok = writeMinidump(runsDir() / name, exception);
            std::fprintf(stderr, "[tf] crash: %s %s\n", name.c_str(), ok ? "written" : "failed");
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    struct ProfilerStarter
    {
        ProfilerStarter()
        {
            PS2Runtime::setStopObserver(&noteStopSource); // (TODO 256: every stop request's source in the log)
            std::thread(exitWatchdogMain).detach();
            if (const char *env = std::getenv("TF_WATCHDOG"); !env || *env != '0')
            {
                std::thread(hangWatchdogMain).detach();
                SetUnhandledExceptionFilter(crashFilter);
                _set_invalid_parameter_handler(invalidParameter); // (TODO 163: the CRT's fatal ends told first)
                _set_purecall_handler(pureCall);
                std::signal(SIGABRT, onAbort);
            }
            if (const char *watch = std::getenv("TF_WATCH"))
            {
                char *end = nullptr;
                const uint32_t address = static_cast<uint32_t>(std::strtoul(watch, &end, 16));
                const uint32_t length = (end && *end == ',') ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0)) : 16u;
                std::thread(watchMain, address, std::max<uint32_t>(4u, std::min<uint32_t>(length, 64u)) & ~3u).detach();
            }
            const char *env = std::getenv("TF_PROFILE");
            if (!env || !*env)
                return;
            const double seconds = std::max(1.0, std::atof(env));
            const char *after = std::getenv("TF_PROFILE_AFTER"); // start sampling later
            std::thread(samplerMain, seconds, after ? std::atof(after) : 0.0).detach();
        }
    } g_profilerStarter;
}

// Write-protects the host page behind [address, address + length) of EE RAM: every write to
// that range is logged with its host call stack, except writes that leave `expected` in the
// first word (pass useExpected = false to log them all). One range at a time.
void tfGuardGuestWrites(uint32_t address, uint32_t length, uint32_t expected, bool useExpected)
{
    if (!g_tfRuntime || !g_tfRuntime->memory().getRDRAM())
        return;
    HANDLE process = GetCurrentProcess();
    static bool symbolsReady = false;
    if (!symbolsReady)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymSetOptions(SymGetOptions() | SYMOPT_IGNORE_CVREC); SymInitialize(process, exeDirectory().c_str(), TRUE);
        symbolsReady = true;
    }
    uint8_t *range = g_tfRuntime->memory().getRDRAM() + (address & PS2_RAM_MASK);
    DWORD old = 0;
    if (g_guard.armed.load())
        VirtualProtect(g_guard.page, 4096, PAGE_READWRITE, &old);
    g_guard.rangeStart = range;
    g_guard.rangeEnd = range + length;
    g_guard.page = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(range) & ~uintptr_t(4095));
    g_guard.hasIgnore = useExpected;
    g_guard.ignore = expected;
    if (!g_guard.handlerInstalled)
    {
        AddVectoredExceptionHandler(1, guardHandler);
        g_guard.handlerInstalled = true;
    }
    g_guard.armed.store(true);
    VirtualProtect(g_guard.page, 4096, PAGE_READONLY, &old);
}

void tfStopGuestWriteGuard()
{
    if (!g_guard.armed.exchange(false))
        return;
    DWORD old = 0;
    VirtualProtect(g_guard.page, 4096, PAGE_READWRITE, &old);
}

// Logs the calling thread's host call stack. Generated functions carry their guest
// address in their name, so this is the guest call chain as well.
void tfLogCallStack(const char *label)
{
    HANDLE process = GetCurrentProcess();
    static bool symbolsReady = false;
    if (!symbolsReady)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymSetOptions(SymGetOptions() | SYMOPT_IGNORE_CVREC); SymInitialize(process, exeDirectory().c_str(), TRUE);
        symbolsReady = true;
    }
    void *frames[48];
    const USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
        std::fprintf(stderr, "[tf] %s  #%u %s\n", label, static_cast<unsigned>(i),
                     symbolFor(process, reinterpret_cast<DWORD64>(frames[i])).c_str());
}

// (TODO 164, TF_PROFILE_SPIKES) The game's work in a frame begins (the VBlank wait ended) / ends (the next wait begins,
// with its EE time in ms). On the EE thread.
void tfNoteFrameWork(bool begins, double workMs)
{
    if (spikeThreshold() <= 0.0)
        return;
    // (A frame: from one VBlank wait's end to the next's - its game work and the wait with what runs inside it, the
    // VBlank's own work: the native capture's frame end, the presentation. The HQ's slow frames spend their time
    // there, not in the game's work: 15-20 ms of the thread's time, 0.4 ms of EE time.)
    static float lastWorkMs = 0.0f;
    if (!begins)
    {
        lastWorkMs = static_cast<float>(workMs);
        return;
    }
    const int64_t now = nowNs();
    static double cpuBefore = 0.0;
    const double cpu = tfThreadCpuMs();
    if (g_frameWorkStart != 0)
    {
        const uint64_t n = g_frameSpanCount.load(std::memory_order_relaxed);
        g_frameSpans[n % kFrameSpans] = FrameSpan{g_frameWorkStart, now, lastWorkMs, static_cast<float>(cpu - cpuBefore)};
        g_frameSpanCount.store(n + 1u, std::memory_order_release);
    }
    g_frameWorkStart = now;
    cpuBefore = cpu;
}

// Once a game frame (overrides.cpp): the hang watchdog's sign of life.
void tfWatchdogFrame()
{
    const uint64_t frame = g_gameFrames.fetch_add(1u, std::memory_order_relaxed) + 1u;
    tfNoteEeThread(); // (called on the EE thread: TF_PROFILE_EE_ONLY)
    // TF_WATCHDOG_TEST=<frame> (tests): the game thread held 8 s at that frame, a freeze to catch
    static const uint64_t testAt = std::getenv("TF_WATCHDOG_TEST") ? std::strtoull(std::getenv("TF_WATCHDOG_TEST"), nullptr, 10) : 0u;
    if (testAt != 0u && frame == testAt)
        Sleep(8000);
}

// TF_TEST_WINDOW (tests, TODO 117: a window the player has left - minimised, or behind another window - on a test
// machine whose game window is hidden): actions at seconds after the game's first frame, "cover@15,full@16,hide@40,
// uncover@41". Done by a thread of its own, so a game held up does not hold them up. min = minimised without being
// activated (put off every screen first, so that when it is restored it is not seen there); show = restored / shown,
// not activated, off every screen; cover = a black window over the whole of the game window's screen, in front of
// all, never activated (another program's window over the game); full = the game window shown, not activated, the
// size of that screen and behind every other window - under the cover, never seen; hide = hidden again; uncover = the
// cover taken away (after hide: the game window is never on the screen); away / back = the window taken as left by
// the player and back (tfWindowAway, as for a window that lost the focus). Each action and, every 2 s while there are
// any, the window's state (minimised, visible, foreground, covered) and the game's VBlank are logged.
static std::atomic<HWND> g_tfTestWindow{nullptr};
static std::atomic<int> g_tfTestAway{-1}; // (-1: not set by a test)
void tfNoteWindow(void *window) { g_tfTestWindow.store(static_cast<HWND>(window)); }

// (TODO 197: movies stutter with the game alt-tabbed) Windows 11 throttles a process none of whose windows the player is
// using: EcoQoS (its threads on slow, efficient cores, at low clocks) and, for a window minimised or covered, the timer
// resolution it asked for ignored (Sleep and the VBlank clock's waits rounded up to 15.6 ms). The game's clock, its
// movie decoder and its sound stream all wait on short timers, so a game in the background ran late in bursts. The
// process opts out of both (SetProcessInformation, ProcessPowerThrottling: control mask EXECUTION_SPEED and
// IGNORE_TIMER_RESOLUTION, state 0 = never throttled), as a game that keeps playing behind other windows should.
// TF_BACKGROUND_THROTTLE=1: Windows' own handling (as before).
void tfKeepFullSpeedInBackground()
{
    if (const char *v = std::getenv("TF_BACKGROUND_THROTTLE"); v && *v == '1')
        return;
    constexpr ULONG kExecutionSpeed = 0x1u, kIgnoreTimerResolution = 0x4u; // (PROCESS_POWER_THROTTLING_*)
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = kExecutionSpeed | kIgnoreTimerResolution;
    state.StateMask = 0u;
    bool ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
    if (!ok) // (a Windows before 11: the timer flag unknown there - the speed alone)
    {
        state.ControlMask = kExecutionSpeed;
        ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
    }
    std::fprintf(stderr, "[tf] background: %s\n",
                 ok ? (state.ControlMask & kIgnoreTimerResolution ? "full speed and timers kept with the window left (no Windows throttling)"
                                                                 : "full speed kept with the window left (no Windows throttling)")
                    : "Windows' throttling could not be switched off");
}

// (TODO 117) The player has left the window: minimised, or another window has the focus after this one had it (a
// window that never had it - a test machine's hidden one - is not "left"). Cheap enough for every frame.
bool tfWindowAway()
{
    const int test = g_tfTestAway.load(std::memory_order_relaxed);
    if (test >= 0)
        return test != 0;
    const HWND window = g_tfTestWindow.load(std::memory_order_relaxed);
    if (!window)
        return false;
    if (IsIconic(window))
        return true;
    static std::atomic<bool> hadFocus{false};
    const bool focused = GetForegroundWindow() == window;
    if (focused)
        hadFocus.store(true, std::memory_order_relaxed);
    return !focused && hadFocus.load(std::memory_order_relaxed);
}

static std::atomic<uint64_t> g_tfTestVBlank{0};

static void tfTestWindowMain(std::vector<std::pair<double, std::string>> actions)
{
    SetThreadDescription(GetCurrentThread(), L"TestWindow");
    const auto start = std::chrono::steady_clock::now();
    HWND cover = nullptr;
    size_t next = 0;
    auto lastStatus = start;
    for (;;)
    {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const HWND window = g_tfTestWindow.load();
        while (window && next < actions.size() && t >= actions[next].first)
        {
            const std::string what = actions[next].second;
            ++next;
            HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            const RECT r = info.rcMonitor;
            if (what == "min" || what == "show")
                SetWindowPos(window, HWND_BOTTOM, -30000, -30000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            if (what == "min")
                ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
            else if (what == "show")
                ShowWindowAsync(window, SW_SHOWNOACTIVATE);
            else if (what == "hide")
                ShowWindowAsync(window, SW_HIDE);
            else if (what == "close") // (the window's close button: WM_CLOSE from this thread)
                PostMessageW(window, WM_CLOSE, 0, 0);
            else if (what == "full")
                SetWindowPos(window, HWND_BOTTOM, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_SHOWWINDOW);
            else if (what == "cover" && !cover)
            {
                WNDCLASSW wc{};
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                wc.lpszClassName = L"tf_test_cover";
                RegisterClassW(&wc);
                cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"cover", WS_POPUP,
                                        r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
                if (cover)
                    ShowWindow(cover, SW_SHOWNOACTIVATE);
            }
            else if (what == "uncover" && cover)
            {
                DestroyWindow(cover);
                cover = nullptr;
            }
            else if (what == "away" || what == "back")
                g_tfTestAway.store(what == "away" ? 1 : 0);
            std::fprintf(stderr, "[tf] test window: %s at %.1f s (vblank %llu)\n", what.c_str(), t,
                         static_cast<unsigned long long>(g_tfTestVBlank.load()));
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const auto now = std::chrono::steady_clock::now();
        if (window && now - lastStatus >= std::chrono::seconds(2))
        {
            lastStatus = now;
            RECT wr{};
            GetWindowRect(window, &wr);
            std::fprintf(stderr, "[tf] test window: at %.1f s minimised %d visible %d foreground %d covered %d, %ldx%ld at %ld,%ld (vblank %llu)\n",
                         t, IsIconic(window) ? 1 : 0, IsWindowVisible(window) ? 1 : 0, GetForegroundWindow() == window ? 1 : 0,
                         cover ? 1 : 0, wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top,
                         static_cast<unsigned long long>(g_tfTestVBlank.load()));
        }
        if (next >= actions.size() && !cover)
            return;
        Sleep(20);
    }
}

void tfTestWindow(uint64_t vblank)
{
    g_tfTestVBlank.store(vblank, std::memory_order_relaxed);
    static const bool started = []()
    {
        std::vector<std::pair<double, std::string>> out;
        const char *env = std::getenv("TF_TEST_WINDOW");
        std::string all = env ? env : "";
        size_t from = 0;
        while (from < all.size())
        {
            size_t to = all.find(',', from);
            if (to == std::string::npos)
                to = all.size();
            const std::string item = all.substr(from, to - from);
            if (const size_t at = item.find_first_of("@:"); at != std::string::npos) // ("away:15" too: a test script's own escapes)
                out.emplace_back(std::atof(item.c_str() + at + 1), item.substr(0, at));
            from = to + 1;
        }
        if (out.empty())
            return false;
        std::thread(tfTestWindowMain, std::move(out)).detach();
        return true;
    }();
    (void)started;
}

// (TODO 170 / 174) The F1 overlay's typed characters and mouse wheel, taken from the window's own messages. raylib keeps
// only what came in its LAST PollInputEvents (each poll clears its character queue and sets the wheel to the poll's own
// move), and the runtime polls several times between two drawn pictures, so ImGui (rlImGui reads them once a picture)
// lost wheel notches - "the wheel sometimes won't scroll" - and characters. Here every WM_CHAR and WM_MOUSEWHEEL is
// counted as it is dispatched (the window procedure, on the window's thread, before GLFW's) and the overlay takes them
// all once a picture (mod_menu.cpp). TF_OVERLAY_INPUT_LOG=1: each key-down, character and wheel message logged.
static WNDPROC g_overlayProc = nullptr;
static std::mutex g_overlayMutex;
static std::vector<uint32_t> g_overlayChars;
static std::vector<int> g_overlayKeys; // (virtual keys: + down, - up; a key-down's auto-repeats left out)
static float g_overlayWheelX = 0.0f, g_overlayWheelY = 0.0f;
// (TODO 175) The game's own menus' clicks and wheel (menu_mouse.cpp: raylib's IsMouseButtonPressed / wheel see only their
// last poll's - a click between two pictures was lost), and the cursor hidden while a controller is in use.
static int g_menuLeftClicks = 0, g_menuRightClicks = 0;
static float g_menuWheel = 0.0f;
static std::atomic<bool> g_cursorHiddenByPad{false};
static LPARAM g_lastMouseMove = -1;
static LRESULT CALLBACK tfOverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static const bool log = std::getenv("TF_OVERLAY_INPUT_LOG") && *std::getenv("TF_OVERLAY_INPUT_LOG") == '1';
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g_cursorHiddenByPad.load(std::memory_order_relaxed))
    {
        SetCursor(nullptr); // (TODO 175: a controller in use - no cursor over the picture until the mouse moves)
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE)
    {
        if (g_lastMouseMove != -1 && lp != g_lastMouseMove && g_cursorHiddenByPad.exchange(false))
            PostMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        g_lastMouseMove = lp;
    }
    if ((msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN) && g_cursorHiddenByPad.exchange(false)) // (keyboard / mouse use: back)
        PostMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        ++(msg == WM_LBUTTONDOWN ? g_menuLeftClicks : g_menuRightClicks);
    }
    if (msg == WM_MOUSEWHEEL)
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        g_menuWheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
    }
    if (msg == WM_CHAR || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL || msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        if (msg == WM_CHAR)
        {
            if (g_overlayChars.size() < 256u)
                g_overlayChars.push_back(static_cast<uint32_t>(wp));
        }
        else if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            if ((!down || !((lp >> 30) & 1)) && g_overlayKeys.size() < 256u)
                g_overlayKeys.push_back(down ? static_cast<int>(wp) : -static_cast<int>(wp));
        }
        else if (msg == WM_MOUSEWHEEL)
            g_overlayWheelY += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        else
            g_overlayWheelX -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
    }
    if (log && (msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_MOUSEWHEEL))
        std::fprintf(stderr, "[overlay input] %s %llx (repeat %u, was down %u) at %.3f s\n",
                     msg == WM_KEYDOWN ? "key down" : msg == WM_CHAR ? "char" : "wheel", static_cast<unsigned long long>(wp),
                     static_cast<unsigned>(lp & 0xFFFF), static_cast<unsigned>((lp >> 30) & 1), GetTickCount64() / 1000.0);
    return CallWindowProcW(g_overlayProc, hwnd, msg, wp, lp);
}
// Installed once the window exists (the window's thread); false: no window.
bool tfOverlayInputHook()
{
    if (g_overlayProc)
        return true;
    const HWND window = g_tfTestWindow.load();
    if (!window)
        return false;
    g_overlayProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(tfOverlayWndProc)));
    return g_overlayProc != nullptr;
}
// What came since the last call: the characters and key changes (appended), the wheel's notches.
void tfOverlayTakeInput(std::vector<uint32_t> &chars, std::vector<int> &keys, float &wheelX, float &wheelY)
{
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    chars.insert(chars.end(), g_overlayChars.begin(), g_overlayChars.end());
    g_overlayChars.clear();
    keys.insert(keys.end(), g_overlayKeys.begin(), g_overlayKeys.end());
    g_overlayKeys.clear();
    wheelX = g_overlayWheelX, wheelY = g_overlayWheelY;
    g_overlayWheelX = g_overlayWheelY = 0.0f;
}
// (TODO 175) The game menus' clicks and wheel notches since the last call; the cursor hidden (a controller used) or not.
void tfTakeMenuMouse(int &left, int &right, float &wheel)
{
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    left = g_menuLeftClicks, right = g_menuRightClicks, wheel = g_menuWheel;
    g_menuLeftClicks = g_menuRightClicks = 0;
    g_menuWheel = 0.0f;
}
void tfHideCursorForPad()
{
    if (!g_cursorHiddenByPad.exchange(true))
        if (const HWND window = g_tfTestWindow.load())
        {
            POINT p{};
            RECT r{};
            if (GetCursorPos(&p) && ScreenToClient(window, &p) && GetClientRect(window, &r) && PtInRect(&r, p))
                SetCursor(nullptr); // (now, not at the next mouse message)
        }
}
bool tfCursorHiddenForPad() { return g_cursorHiddenByPad.load(std::memory_order_relaxed); }
// Windows' own scroll speed: lines a wheel notch (SPI_GETWHEELSCROLLLINES; 3 by default; WHEEL_PAGESCROLL: -1).
int tfWheelScrollLines()
{
    UINT lines = 3u;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0))
        return 3;
    return lines == WHEEL_PAGESCROLL ? -1 : static_cast<int>(lines);
}
// (Tests: TF_MENU_TYPE_TEST) A key typed into the window as the keyboard would: WM_KEYDOWN then WM_KEYUP posted (the
// window's own message loop translates the key-down into its WM_CHAR) - no focus needed.
void tfPostTestKey(int vk, bool down)
{
    const HWND window = g_tfTestWindow.load();
    if (!window)
        return;
    if (vk < 0) // (-1: the wheel a notch down)
    {
        if (down)
            PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        return;
    }
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    const LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16) | (down ? 0 : (static_cast<LPARAM>(3) << 30));
    PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), lp);
}

#pragma comment(lib, "winmm.lib")

#endif

#if !defined(_WIN32)
// Linux: the same input taps as the window procedure above, through GLFW's callbacks (raylib's own are kept: each of
// these calls the one it replaced first). The windowing parts of the profiler (minidumps, the hang watchdog, the
// test window) stay Windows' alone.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <execinfo.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace
{
    std::atomic<GLFWwindow *> g_window{nullptr};
    std::mutex g_inputMutex;
    std::vector<uint32_t> g_chars;
    std::vector<int> g_keys; // (Windows virtual-key codes, as mod_menu.cpp's imguiKeyOf reads them: + down, - up)
    float g_wheelX = 0.0f, g_wheelY = 0.0f, g_menuWheel = 0.0f;
    int g_menuLeftClicks = 0, g_menuRightClicks = 0;
    std::atomic<bool> g_cursorHiddenByPad{false};
    std::atomic<bool> g_focused{false}, g_hadFocus{false}, g_iconified{false};

    GLFWcharfun g_prevChar = nullptr;
    GLFWkeyfun g_prevKey = nullptr;
    GLFWscrollfun g_prevScroll = nullptr;
    GLFWmousebuttonfun g_prevButton = nullptr;
    GLFWcursorposfun g_prevCursor = nullptr;
    GLFWwindowfocusfun g_prevFocus = nullptr;
    GLFWwindowiconifyfun g_prevIconify = nullptr;

    int virtualKeyOf(int key)
    {
        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
            return key; // ('A'..'Z' in both)
        switch (key)
        {
        case GLFW_KEY_BACKSPACE: return 0x08;
        case GLFW_KEY_TAB: return 0x09;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: return 0x0D;
        case GLFW_KEY_ESCAPE: return 0x1B;
        case GLFW_KEY_PAGE_UP: return 0x21;
        case GLFW_KEY_PAGE_DOWN: return 0x22;
        case GLFW_KEY_END: return 0x23;
        case GLFW_KEY_HOME: return 0x24;
        case GLFW_KEY_LEFT: return 0x25;
        case GLFW_KEY_UP: return 0x26;
        case GLFW_KEY_RIGHT: return 0x27;
        case GLFW_KEY_DOWN: return 0x28;
        case GLFW_KEY_INSERT: return 0x2D;
        case GLFW_KEY_DELETE: return 0x2E;
        default: return 0;
        }
    }

    // The cursor back once the mouse or keyboard is used (a captured mouse - GLFW_CURSOR_DISABLED - is left alone).
    void showCursorAgain(GLFWwindow *window)
    {
        if (g_cursorHiddenByPad.exchange(false) && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_HIDDEN)
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    void onChar(GLFWwindow *window, unsigned int c)
    {
        if (g_prevChar)
            g_prevChar(window, c);
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (g_chars.size() < 256u)
            g_chars.push_back(c);
    }
    void onKey(GLFWwindow *window, int key, int scancode, int action, int mods)
    {
        if (g_prevKey)
            g_prevKey(window, key, scancode, action, mods);
        if (action == GLFW_PRESS)
            showCursorAgain(window);
        const int vk = virtualKeyOf(key);
        if (vk == 0 || action == GLFW_REPEAT) // (a held key's repeats left out, as on Windows)
            return;
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (g_keys.size() < 256u)
            g_keys.push_back(action == GLFW_PRESS ? vk : -vk);
    }
    void onScroll(GLFWwindow *window, double x, double y)
    {
        if (g_prevScroll)
            g_prevScroll(window, x, y);
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_wheelY += static_cast<float>(y);
        g_wheelX -= static_cast<float>(x);
        g_menuWheel += static_cast<float>(y);
    }
    void onButton(GLFWwindow *window, int button, int action, int mods)
    {
        if (g_prevButton)
            g_prevButton(window, button, action, mods);
        if (action != GLFW_PRESS)
            return;
        showCursorAgain(window);
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (button == GLFW_MOUSE_BUTTON_LEFT)
            ++g_menuLeftClicks;
        else if (button == GLFW_MOUSE_BUTTON_RIGHT)
            ++g_menuRightClicks;
    }
    void onCursor(GLFWwindow *window, double x, double y)
    {
        if (g_prevCursor)
            g_prevCursor(window, x, y);
        static double lastX = -1.0, lastY = -1.0;
        if (lastX >= 0.0 && (x != lastX || y != lastY))
            showCursorAgain(window);
        lastX = x, lastY = y;
    }
    void onFocus(GLFWwindow *window, int focused)
    {
        if (g_prevFocus)
            g_prevFocus(window, focused);
        g_focused.store(focused != 0, std::memory_order_relaxed);
        if (focused)
            g_hadFocus.store(true, std::memory_order_relaxed);
    }
    void onIconify(GLFWwindow *window, int iconified)
    {
        if (g_prevIconify)
            g_prevIconify(window, iconified);
        g_iconified.store(iconified != 0, std::memory_order_relaxed);
    }
}

void tfNoteWindow(void *window) { g_window.store(static_cast<GLFWwindow *>(window)); }

// Installed once the window exists (the window's thread); false: no window.
bool tfOverlayInputHook()
{
    static bool installed = false;
    if (installed)
        return true;
    GLFWwindow *window = g_window.load();
    if (!window)
        return false;
    g_prevChar = glfwSetCharCallback(window, onChar);
    g_prevKey = glfwSetKeyCallback(window, onKey);
    g_prevScroll = glfwSetScrollCallback(window, onScroll);
    g_prevButton = glfwSetMouseButtonCallback(window, onButton);
    g_prevCursor = glfwSetCursorPosCallback(window, onCursor);
    g_prevFocus = glfwSetWindowFocusCallback(window, onFocus);
    g_prevIconify = glfwSetWindowIconifyCallback(window, onIconify);
    const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
    g_focused.store(focused);
    if (focused)
        g_hadFocus.store(true);
    installed = true;
    return true;
}

void tfOverlayTakeInput(std::vector<uint32_t> &chars, std::vector<int> &keys, float &wheelX, float &wheelY)
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    chars.insert(chars.end(), g_chars.begin(), g_chars.end());
    g_chars.clear();
    keys.insert(keys.end(), g_keys.begin(), g_keys.end());
    g_keys.clear();
    wheelX = g_wheelX, wheelY = g_wheelY;
    g_wheelX = g_wheelY = 0.0f;
}

void tfTakeMenuMouse(int &left, int &right, float &wheel)
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    left = g_menuLeftClicks, right = g_menuRightClicks, wheel = g_menuWheel;
    g_menuLeftClicks = g_menuRightClicks = 0;
    g_menuWheel = 0.0f;
}

// (the window's thread) Hidden only over a normal cursor: a mouse the game has captured stays as it is.
void tfHideCursorForPad()
{
    GLFWwindow *window = g_window.load();
    if (window && !g_cursorHiddenByPad.exchange(true) && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_NORMAL)
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
}
bool tfCursorHiddenForPad() { return g_cursorHiddenByPad.load(std::memory_order_relaxed); }
int tfWheelScrollLines() { return 3; } // (no desktop-wide setting to read: Windows' default)

// (Tests: TF_MENU_TYPE_TEST) A key typed as the keyboard would: its down and up, and on the down its character
// (unshifted, as Windows translates it); -1: the wheel a notch down.
void tfPostTestKey(int vk, bool down)
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (vk < 0)
    {
        if (down)
            g_wheelY -= 1.0f, g_menuWheel -= 1.0f;
        return;
    }
    g_keys.push_back(down ? vk : -vk);
    if (down && ((vk >= '0' && vk <= '9') || vk == ' '))
        g_chars.push_back(static_cast<uint32_t>(vk));
    else if (down && vk >= 'A' && vk <= 'Z')
        g_chars.push_back(static_cast<uint32_t>(vk - 'A' + 'a'));
}

// (TODO 117) Minimised, or another window has the focus after this one had it - from the callbacks above, any thread.
bool tfWindowAway()
{
    if (!g_window.load(std::memory_order_relaxed))
        return false;
    return g_iconified.load(std::memory_order_relaxed) ||
           (!g_focused.load(std::memory_order_relaxed) && g_hadFocus.load(std::memory_order_relaxed));
}

// The host stack (glibc's backtrace: function names where the executable exports them).
void tfLogCallStack(const char *label)
{
    void *frames[48];
    const int count = backtrace(frames, 48);
    std::fprintf(stderr, "[tf] %s: host stack (%d frames)\n", label, count);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

void tfWatchdogFrame() {}
void tfNoteFrameWork(bool, double) {}
void tfKeepFullSpeedInBackground() {}
void tfTestWindow(uint64_t) {}
#endif
