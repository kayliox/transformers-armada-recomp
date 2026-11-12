// "I have HD textures (PCSX2 pack)" - see hd_pack.h.
#include "hd_pack.h"
#include "hd_progress.h"

#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace hd_pack
{
    namespace
    {
        namespace fs = std::filesystem;
        std::mutex g_mutex;
        std::string g_status;
        std::atomic<bool> g_busy{false};

        void say(const std::string &text)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = text;
            std::fprintf(stderr, "[hd pack] %s\n", text.c_str());
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // The table's PCSX2 names (lower case), from tools\hd_textures\pcsx2_names.json where the game starts.
        std::unordered_set<std::string> tableNames()
        {
            std::unordered_set<std::string> out;
            std::ifstream f(fs::path("tools") / "hd_textures" / "pcsx2_names.json", std::ios::binary);
            std::string line;
            while (std::getline(f, line))
            {
                const size_t a = line.find('"'), b = a == std::string::npos ? a : line.find('"', a + 1);
                if (b != std::string::npos && line.compare(a + 1, 1, "_") != 0 && line.find("\": \"", b) == b)
                    out.insert(line.substr(a + 1, b - a - 1));
            }
            return out;
        }

#if defined(_WIN32)
        // Windows' own folder picker; "" when cancelled.
        std::wstring pickFolder()
        {
            std::wstring out;
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            IFileOpenDialog *dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                DWORD options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                dialog->SetTitle(L"The folder of your PCSX2 HD texture pack for Transformers");
                if (SUCCEEDED(dialog->Show(nullptr)))
                {
                    IShellItem *item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)))
                    {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                        {
                            out = path;
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            if (SUCCEEDED(init))
                CoUninitialize();
            return out;
        }
#endif

        // The folder checked: a PCSX2 pack (its images counted against the table) or our own layout (TFA\ ... PNGs).
        void check(const fs::path &folder)
        {
            std::error_code ec;
            const auto names = tableNames();
            if (names.empty())
            {
                say("The name table (tools\\hd_textures\\pcsx2_names.json) is missing: update the game.");
                return;
            }
            size_t images = 0, matched = 0;
            for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const std::string ext = lower(it->path().extension().string());
                if (ext != ".dds" && ext != ".png")
                    continue;
                ++images;
                if (names.count(lower(it->path().stem().string())))
                    ++matched;
            }
            const std::string where = folder.string();
            if (matched > 0)
            {
                ps2_settings::noteChanged("TF_HD_PACK", where);
                ps2_settings::noteChanged("TF_HIRES", ""); // (On: the HD textures row's own value; "1" showed as a third choice)
                // (the game's own textures, which tell same-named ones apart, are prepared first when they are not there)
                if (!fs::is_directory(fs::path("hd_textures") / "converted" / "textures", ec))
                    hd_progress::prepareOriginals();
                say("HD texture pack found: " + std::to_string(matched) + " of its " + std::to_string(images) +
                    " images are textures of this game. Restart the game to use it.");
                return;
            }
            // Our own layout: an hd_textures\upscaled tree from another PC (TFA\... and TFA2\... PNGs).
            if (fs::is_directory(folder / "TFA", ec) && fs::is_directory(folder / "TFA2", ec))
            {
                ps2_settings::noteChanged("TF_HIRES_DIR", where);
                ps2_settings::noteChanged("TF_HIRES", "");
                say("HD textures in this port's own layout found. Restart the game to use them.");
                return;
            }
            say(images ? "This folder is not a Transformers PCSX2 pack: none of its " + std::to_string(images) +
                             " images is a texture of this game."
                       : "This folder is not a Transformers PCSX2 pack: it has no .dds or .png images.");
        }
    }

    void choose()
    {
#if defined(_WIN32)
        if (g_busy.exchange(true))
            return;
        say("Choose the folder of your pack (the one with SLUS-20668 or replacements in it, or any folder above).");
        std::thread([] {
            const std::wstring folder = pickFolder();
            if (folder.empty())
                say("No folder chosen.");
            else
                check(fs::path(folder));
            g_busy = false;
        }).detach();
#else
        say("Set TF_HD_PACK=<the pack's folder> in settings.ini.");
#endif
    }

    std::string status()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    std::string current()
    {
        return ps2_settings::value("TF_HD_PACK");
    }

    void addSettings()
    {
        // (Found on the Vostro, TODO 290 (17): without these noteChanged wrote nothing - it only writes settings it
        // knows - and the pack chosen in F1 was gone after the restart.)
        for (const char *key : {"TF_HD_PACK", "TF_HIRES_DIR"})
        {
            ps2_settings::Setting s;
            s.key = key;
            s.section = "Graphics";
            s.label = key;
            s.defaultValue = "";
            s.apply = [](const std::string &) { return false; };
            s.available = []() { return false; };
            ps2_settings::add(std::move(s));
        }
    }

    void forget()
    {
        ps2_settings::noteChanged("TF_HD_PACK", "");
        say("The pack is no longer used (after a restart): the game's own textures.");
    }
}
