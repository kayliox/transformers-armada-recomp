// (TODO 279, release A) The installed layout, old and new, read side by side: install_layout.h has the long form.

#include "install_layout.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace install_layout
{
    namespace
    {
        namespace fs = std::filesystem;

        bool g_new = false;
        fs::path g_root;
        std::string g_used; // (the places taken from the new layout, for the log)

        // {"layout": 2} - the number after the word, nothing more is read from the file.
        int markedLayout(const fs::path &file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return 0;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            size_t at = text.find("\"layout\"");
            if (at == std::string::npos)
                return 0;
            at = text.find(':', at);
            if (at == std::string::npos)
                return 0;
            ++at;
            while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
                ++at;
            int n = 0;
            bool any = false;
            while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) && n < 1000)
            {
                n = n * 10 + (text[at++] - '0');
                any = true;
            }
            return any ? n : 0;
        }

        bool isSet(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v;
        }

        void set(const char *name, const fs::path &value, const char *what)
        {
            if (isSet(name))
                return; // (the player's own or a test's switch wins)
            const std::string text = value.string();
#if defined(_WIN32)
            _putenv_s(name, text.c_str());
#else
            setenv(name, text.c_str(), 1);
#endif
            g_used += g_used.empty() ? "" : ", ";
            g_used += what;
        }

        fs::path place(const char *a, const char *b = nullptr, const char *c = nullptr)
        {
            fs::path p = g_root / a;
            if (b)
                p /= b;
            if (c)
                p /= c;
            return p;
        }
    }

    void find()
    {
        g_new = false;
        g_root.clear();
        g_used.clear();
        std::error_code ec;
        const fs::path here = fs::current_path(ec);
        if (ec || here.empty())
            return;
        // The folder above the game folder; for a copy whose game folder is its port\, the one above the copy.
        std::vector<fs::path> above;
        if (here.has_parent_path() && here.parent_path() != here)
        {
            above.push_back(here.parent_path());
            if (here.filename() == "port" && above[0].has_parent_path() && above[0].parent_path() != above[0])
                above.push_back(above[0].parent_path());
        }
        for (const fs::path &dir : above)
        {
            if (markedLayout(dir / "layout.json") == 2)
            {
                g_root = dir;
                g_new = true;
                return;
            }
        }
    }

    void apply()
    {
        if (!g_new)
            return;
        std::error_code ec;
        if (fs::is_regular_file(place("settings.ini"), ec))
            set("PS2X_SETTINGS_FILE", place("settings.ini"), "settings");
        if (fs::is_directory(place("saves", "mc0"), ec))
            set("TF_MC_ROOT", place("saves", "mc0"), "memory cards");
        if (fs::is_directory(place("saves", "states"), ec))
            set("PS2X_STATE_FILE", place("saves", "states", "quick.ps2state"), "save states");
        // (the USA disc's executable or the European one's: an install of either moves the same way)
        if (fs::is_regular_file(place("data", "game", "SLUS_206.68"), ec) || fs::is_regular_file(place("data", "game", "SLES_523.88"), ec))
        {
            set("TRANSFORMERS_GAME_DIR", place("data", "game"), "the disc's files");
            // (release B) The mods stay in the game folder for now, beside the tools that make them: found there by name,
            // since the loader's own default is the folder above the disc's files (data\ once the disc moved).
            if (fs::is_directory(fs::current_path(ec) / "Mods", ec))
                set("TF_MODS_DIR", fs::current_path(ec) / "Mods", "mods (in the game folder)");
        }
        if (fs::is_directory(place("data", "movies_hd"), ec))
            set("TF_HD_MOVIES", place("data", "movies_hd"), "HD movies");
        // (only a cache: made again when it is missing - but never the first thing made in a folder that is not there)
        if (fs::is_directory(place("data"), ec))
            set("TF_SHADER_CACHE", place("data", "shader cache"), "shader cache");
        if (fs::is_directory(place("logs"), ec))
            g_used += std::string(g_used.empty() ? "" : ", ") + "logs";
        if (fs::is_directory(place("saves"), ec))
            g_used += std::string(g_used.empty() ? "" : ", ") + "backups and exports";
        std::fprintf(stderr, "[layout] %s\n", summary().c_str());
    }

    bool isNew() { return g_new; }

    fs::path root() { return g_root; }

    fs::path settingsFile()
    {
        std::error_code ec;
        if (g_new && fs::is_regular_file(place("settings.ini"), ec))
            return place("settings.ini");
        return {};
    }

    fs::path logsDir()
    {
        std::error_code ec;
        if (g_new && fs::is_directory(place("logs"), ec))
            return place("logs");
        const fs::path dir = fs::current_path(ec) / "runs";
        fs::create_directories(dir, ec);
        return dir;
    }

    fs::path savesDir(const fs::path &today)
    {
        std::error_code ec;
        if (g_new && fs::is_directory(place("saves"), ec))
            return place("saves");
        return today;
    }

    bool callerCardRoot()
    {
        const char *set = std::getenv("TF_MC_ROOT");
        if (!set || !*set)
            return false;
        if (!g_new)
            return true;
        std::error_code ec;
        // (not the same folder, or one that cannot be compared: the caller's own)
        return !fs::equivalent(fs::path(set), place("saves", "mc0"), ec);
    }

    std::string summary()
    {
        if (!g_new)
            return "the game folder holds everything (no layout.json above it)";
        return "new folder layout in " + g_root.string() + ": " + (g_used.empty() ? std::string("nothing there yet, all from the game folder") : "from there: " + g_used);
    }
}
