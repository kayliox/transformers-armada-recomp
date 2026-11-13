// (TODO 339) The old Ultra preset's forest moved to the new Ultra's, once: see ultra_migrate.h.
//
// History: 2026-11-11 (the user's decision) the old Ultra's forest values were moved to the new Ultra's at every start
// (overrides.cpp migrateUltraForest), with nothing to say it was done, so a player who set the same values by hand had
// them moved back each time. It now happens before settings.ini is read, in the file itself, at most once per file: the
// line TF_ULTRA_MIGRATE_DONE=1 says it was looked at (the way laptop_reset's TF_LAPTOP_RESET_DONE does) - and because
// the file is changed before ps2_settings::loadFile reads it, the mark and the new values are part of what the settings
// keep and write back later.
#include "ultra_migrate.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace ultra_migrate
{
    namespace
    {
        constexpr const char *kMark = "TF_ULTRA_MIGRATE_DONE";

        std::string trim(const std::string &s)
        {
            size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t'))
                ++a;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
                --b;
            return s.substr(a, b - a);
        }

        // A line's key ("" for comments, blanks and lines without '=').
        std::string keyOf(const std::string &line)
        {
            const std::string t = trim(line);
            const size_t eq = t.find('=');
            if (t.empty() || t[0] == '#' || t[0] == ';' || eq == std::string::npos)
                return {};
            return trim(t.substr(0, eq));
        }

        std::string valueOf(const std::string &line)
        {
            const std::string t = trim(line);
            return trim(t.substr(t.find('=') + 1));
        }

        bool write(const std::filesystem::path &file, const std::vector<std::string> &lines)
        {
            std::error_code ec;
            const std::filesystem::path tmp = file.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                for (const std::string &line : lines)
                    out << line << "\r\n";
                if (!out)
                {
                    std::fprintf(stderr, "[tf] the old-Ultra check: could not write %s - nothing changed\n", tmp.string().c_str());
                    return false;
                }
            }
            std::filesystem::rename(tmp, file, ec);
            if (ec)
            {
                std::fprintf(stderr, "[tf] the old-Ultra check: could not replace %s (%s) - nothing changed\n", file.string().c_str(),
                             ec.message().c_str());
                std::filesystem::remove(tmp, ec);
                return false;
            }
            return true;
        }
    }

    void run()
    {
        if (const char *e = std::getenv("TF_ULTRA_MIGRATE"); e && *e == '0')
            return;
        // settings.ini where ps2_settings::loadFile reads it (PS2X_SETTINGS_FILE, else the working folder's)
        std::filesystem::path file = "settings.ini";
        if (const char *env = std::getenv("PS2X_SETTINGS_FILE"))
        {
            const std::string name = trim(env);
            std::string up = name;
            for (char &c : up)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (name.empty() || name == "0" || up == "NONE")
                return; // (no settings file: a test, a replay)
            file = name;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
            return; // (none yet: the first start that has one marks it)
        std::vector<std::string> lines;
        std::map<std::string, std::string> fromFile;
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return;
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const std::string key = keyOf(line);
                if (key == kMark)
                    return; // (looked at once already: the player's values since then stay)
                if (!key.empty() && !fromFile.count(key))
                    fromFile[key] = valueOf(line);
                lines.push_back(line);
            }
        }
        // The file's value, and not a launch's environment that sets the row otherwise (a test run: the environment wins
        // over the file there, ps2_settings.h)
        const auto is = [&](const char *key, const char *value)
        {
            const auto it = fromFile.find(key);
            const char *env = std::getenv(key);
            return it != fromFile.end() && it->second == value && (!env || it->second == env);
        };
        const auto full = fromFile.find("TF_FOREST_FULL_DETAIL");
        const bool oldUltra = is("TF_DRAW_DISTANCE", "4") && is("TF_SCENERY_DISTANCE", "4") && is("TF_FOREST_DISTANCE", "4") &&
                              is("TF_GRASS_DISTANCE", "4") && is("TF_TERRAIN_DETAIL", "4") && is("TF_FOREST_SPRITES", "1") &&
                              is("PS2X_GPU_MSAA", "8") &&
                              (full == fromFile.end() || full->second == "0" || full->second == "1" || full->second.empty()) &&
                              !(std::getenv("TF_FOREST_FULL_DETAIL") && full != fromFile.end() &&
                                full->second != std::getenv("TF_FOREST_FULL_DETAIL"));
        std::vector<std::string> out;
        if (oldUltra)
        {
            // (the new Ultra's: the forest and the far trees Auto - their lines out - and full 3D off)
            bool fullSet = false;
            for (const std::string &line : lines)
            {
                const std::string key = keyOf(line);
                if (key == "TF_FOREST_DISTANCE" || key == "TF_FOREST_SPRITES")
                    continue;
                if (key == "TF_FOREST_FULL_DETAIL")
                {
                    if (!fullSet)
                        out.push_back("TF_FOREST_FULL_DETAIL=0");
                    fullSet = true;
                    continue;
                }
                out.push_back(line);
            }
            if (!fullSet)
                out.push_back("TF_FOREST_FULL_DETAIL=0");
        }
        else
            out = lines;
        out.push_back(std::string(kMark) + "=1");
        if (!write(file, out))
            return;
        if (oldUltra)
            std::fprintf(stderr, "[tf] the settings were the old Ultra preset's: the forest set to the new Ultra's - Auto (2x), far trees "
                                 "Auto, full 3D off (Forest distance and Trees in Graphics > Advanced set them apart); once only (%s)\n",
                         kMark);
        else
            std::fprintf(stderr, "[tf] the old-Ultra check: not the old Ultra preset's settings - nothing moved; marked %s=1, not looked "
                                 "at again\n", kMark);
    }
}
