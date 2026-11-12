// launcher.cpp - "Transformers Armada.exe", the launcher in a player's install folder (TODO 279, release B;
// docs/INSTALL_LAYOUT_PLAN.md section 2.2). build.ps1 copies it beside tfa-setup.exe after a good build.
//
// It starts the real game - <checkout>\out\build\transformers\RelWithDebInfo\TransformersRecomp.exe, with its ~30 DLLs
// where the build put them - in the game folder, and waits for it, so Steam (a non-Steam game), Quiver or a shortcut
// sees one program running for as long as the game runs, a restart from the game's settings included. It shows no
// window of its own.
//
//   - The checkout: <install>\source when it has the built game, else <install>\transformers-armada-recomp, else any
//     folder of the install with the built game in it (a repository that holds the game as port\ included).
//   - The game finds the folder layout itself (install_layout.cpp: layout.json in the install folder): the launcher sets
//     no switch and writes no path anywhere, so a copied install works on another drive or PC (section 2.4).
//   - Layout 2: settings.ini made in the install folder from the example on the first start; before that, in the
//     checkout (what Play.bat does).
//   - No built game: tfa-setup.exe in the install folder is started instead (the setup finishes it), else a message.
//   - Its own command line is passed on to the game.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path builtGame(const fs::path &gameFolder)
    {
        return gameFolder / "out" / "build" / "transformers" / "RelWithDebInfo" / "TransformersRecomp.exe";
    }

    // The game folder in a checkout: the checkout itself, or its port\.
    fs::path gameFolderOf(const fs::path &checkout)
    {
        std::error_code ec;
        if (fs::is_regular_file(builtGame(checkout / "port"), ec))
            return checkout / "port";
        return checkout;
    }

    fs::path findGameFolder(const fs::path &root)
    {
        std::error_code ec;
        for (const wchar_t *name : {L"source", L"transformers-armada-recomp"})
        {
            const fs::path g = gameFolderOf(root / name);
            if (fs::is_regular_file(builtGame(g), ec))
                return g;
        }
        for (const auto &e : fs::directory_iterator(root, ec))
        {
            if (!e.is_directory(ec))
                continue;
            const fs::path g = gameFolderOf(e.path());
            if (fs::is_regular_file(builtGame(g), ec))
                return g;
        }
        return {};
    }

    int layoutOf(const fs::path &root)
    {
        HANDLE f = CreateFileW((root / "layout.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return 1;
        char buf[256] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        const std::string text(buf, got);
        size_t at = text.find("\"layout\"");
        if (at == std::string::npos || (at = text.find(':', at)) == std::string::npos)
            return 1;
        int n = 0;
        for (++at; at < text.size() && (text[at] == ' ' || text[at] == '\t'); ++at)
        {
        }
        for (; at < text.size() && text[at] >= '0' && text[at] <= '9' && n < 1000; ++at)
            n = n * 10 + (text[at] - '0');
        return n ? n : 1;
    }

    // settings.ini made from the example when it is missing (overlay\settings.example.ini first: the private build).
    void settingsFromExample(const fs::path &settings, const fs::path &gameFolder)
    {
        std::error_code ec;
        if (fs::exists(settings, ec))
            return;
        for (const fs::path &example : {gameFolder / "overlay" / "settings.example.ini", gameFolder / "settings.example.ini"})
            if (fs::exists(example, ec) && fs::copy_file(example, settings, ec))
                return;
    }

    // The command line after the program's own name, as it was given.
    std::wstring ownArguments()
    {
        const wchar_t *p = GetCommandLineW();
        bool quoted = false;
        for (; *p; ++p)
        {
            if (*p == L'"')
                quoted = !quoted;
            else if (!quoted && (*p == L' ' || *p == L'\t'))
                break;
        }
        while (*p == L' ' || *p == L'\t')
            ++p;
        return p;
    }

    // Starts `exe` (in `dir`) inside a job and waits until it AND every program it started are gone (the game's
    // settings Restart starts the game again and ends the first one). Returns the first program's exit code.
    int runAndWait(const fs::path &exe, const fs::path &dir, const std::wstring &args)
    {
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        if (!args.empty())
            cmd += L" " + args;
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job)
        {
            // (a game that outlives this launcher - killed from the task manager - is not killed with it)
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(0);
        if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi))
            return -1;
        if (job)
            AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        if (job)
        {
            // A game it left running (the settings Restart: the game started again): waited for too. Only the game's
            // own exe counts - not a window the game opened beside itself (the HD texture maker carrying on).
            const std::wstring gameName = exe.filename().wstring();
            for (;;)
            {
                struct
                {
                    JOBOBJECT_BASIC_PROCESS_ID_LIST list;
                    ULONG_PTR more[255];
                } ids{};
                if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &ids, sizeof(ids), nullptr))
                    break;
                bool game = false;
                for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList && !game; ++i)
                {
                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(ids.list.ProcessIdList[i]));
                    if (!h)
                        continue;
                    wchar_t path[MAX_PATH * 4] = {};
                    DWORD len = static_cast<DWORD>(std::size(path));
                    if (QueryFullProcessImageNameW(h, 0, path, &len))
                        game = _wcsicmp(fs::path(path).filename().c_str(), gameName.c_str()) == 0;
                    CloseHandle(h);
                }
                if (!game)
                    break;
                Sleep(500);
            }
            CloseHandle(job);
        }
        return static_cast<int>(code);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t me[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, me, static_cast<DWORD>(std::size(me)));
    if (n == 0 || n >= std::size(me))
        return 1;
    const fs::path root = fs::path(me).parent_path();
    const fs::path game = findGameFolder(root);
    if (game.empty())
    {
        // Not built yet (or the build failed): the setup finishes it.
        const fs::path setup = root / "tfa-setup.exe";
        std::error_code ec;
        if (fs::is_regular_file(setup, ec))
            return runAndWait(setup, root, L"--setup");
        MessageBoxW(nullptr, L"The game is not built in this folder yet. Run tfa-setup.exe to install it.", L"Transformers Armada", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (layoutOf(root) == 2)
    {
        std::error_code ec;
        for (const wchar_t *d : {L"saves", L"logs", L"data"})
            fs::create_directories(root / d, ec);
        settingsFromExample(root / "settings.ini", game);
    }
    else
        settingsFromExample(game / "settings.ini", game);
    return runAndWait(builtGame(game), game, ownArguments());
}
