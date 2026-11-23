// The game's console when it is started from a command window (console_attach.h).
//
// On Windows the game is a windows program, not a console one (transformers\CMakeLists.txt): started from the setup, the
// launcher, a shortcut, Steam or a double click, Windows opened an empty command window beside the game, as its output
// goes to logs\play_*.err / .out (self_log). Now no window is made. Started from a command window (Play.bat, a script, a
// developer's prompt) with its output not already sent to a file or a pipe, it writes into that window as the console
// program did: stdout, stderr and stdin reopened on it. self_log then decides where the log goes, exactly as before (a
// console or nothing: logs\play_*; a file or a pipe: kept). TF_ATTACH_CONSOLE=0: never attach.

#include "console_attach.h"

#if defined(_WIN32)

#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>

namespace console_attach
{
    namespace
    {
        bool redirected(DWORD which)
        {
            const HANDLE h = GetStdHandle(which);
            if (h == nullptr || h == INVALID_HANDLE_VALUE)
                return false;
            const DWORD type = GetFileType(h);
            return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
        }
    }

    void attachParent()
    {
        if (const char *e = std::getenv("TF_ATTACH_CONSOLE"); e && *e == '0')
            return;
        if (redirected(STD_ERROR_HANDLE) || redirected(STD_OUTPUT_HANDLE))
            return; // (a file or a pipe given: the C runtime already writes there)
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            return; // (no parent console: nothing, and no window)
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    }
}

#else

namespace console_attach
{
    void attachParent() {}
}

#endif
