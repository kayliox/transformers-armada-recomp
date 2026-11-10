// Linux-only, no disc or window: compile with C++17 and -pthread, then run the executable.
#include "../transformers/src/hd_progress.cpp"
#include <cassert>
#include <iterator>
#include <utility>

namespace nr
{
    std::filesystem::path hdTextureFolder() { return std::filesystem::current_path() / "hd_textures" / "upscaled"; }
    bool crispFontsFound() { return std::filesystem::is_regular_file(hdTextureFolder().parent_path() / "crisp_fonts" / "manifest.json"); }
}

namespace
{
    struct Set
    {
        fs::path dir = fs::temp_directory_path() / ("tfa-hd-status-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::path status = dir / "upscale_status.json";
        fs::path colours = dir / "upscaled_precolour" / "colour_gains.json";
        fs::path fonts = dir / "crisp_fonts" / "manifest.json";
        fs::file_time_type at = fs::file_time_type::clock::now() - std::chrono::hours(1);

        Set() { assert(fs::create_directory(dir)); }
        ~Set() { fs::remove_all(dir); }

        void write(const fs::path &path, const std::string &text, int seconds)
        {
            fs::create_directories(path.parent_path());
            std::ofstream out(path);
            out << text;
            out.close();
            assert(out);
            fs::last_write_time(path, at + std::chrono::seconds(seconds));
        }

        void upscale(const char *state = "upscaled", int done = 954, int total = 954)
        {
            write(status, "{\"state\": \"" + std::string(state) + "\", \"done\": " + std::to_string(done) +
                          ", \"total\": " + std::to_string(total) + ", \"quality\": \"best\", \"gpu\": \"Deck\", \"updated\": \"old\"}", 0);
        }

        void finish()
        {
            write(colours, "{}", 1);
            write(fonts, "{\"atlases\": {}}", 2);
        }
    };
}

int main()
{
#if defined(__linux__)
    {
        Set set;
        set.upscale();
        set.finish();
        const auto s = hd_progress::readStatus(set.status);
        assert(s.state == "done" && s.done == 954 && s.total == 954);
        assert(hd_progress::finished(s));
        hd_progress::setLine(s);
        assert(hd_progress::line().empty());
        assert(hd_progress::cornerLine().empty());
        assert(!hd_progress::readyToRestart());
        std::ifstream in(set.status);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(hd_progress::field(text, "state") == "done");
        assert(hd_progress::field(text, "quality") == "best");
        assert(hd_progress::field(text, "gpu") == "Deck");
        assert(hd_progress::field(text, "updated") == "old");
    }
    {
        Set set;
        set.upscale();
        fs::create_directories(set.dir / "upscaled_bc7_cache_colour");
        assert(hd_progress::readStatus(set.status).state == "upscaled");
        set.write(set.colours, "{}", 1);
        assert(hd_progress::readStatus(set.status).state == "upscaled");
        set.write(set.fonts, "", 2);
        assert(hd_progress::readStatus(set.status).state == "upscaled");
        set.write(set.fonts, "{}", 2);
        set.write(set.colours, "{}", -1);
        assert(hd_progress::readStatus(set.status).state == "upscaled");
        set.write(set.colours, "", 1);
        assert(hd_progress::readStatus(set.status).state == "upscaled");
        // A preinstalled font set is already complete; it need not be rebuilt after the upscale.
        set.write(set.fonts, "{}", -1);
        set.write(set.colours, "{}", 1);
        assert(hd_progress::readStatus(set.status).state == "done");
    }
    for (const char *state : {"working", "paused", "checking", "upscaled_with_errors", "done", "card_broken"})
    {
        Set set;
        set.upscale(state);
        set.finish();
        assert(hd_progress::readStatus(set.status).state == state);
        assert(fs::last_write_time(set.status) == set.at);
    }
    for (const auto counts : {std::pair<int, int>{953, 954}, {955, 954}, {0, 0}})
    {
        Set set;
        set.upscale("upscaled", counts.first, counts.second);
        set.finish();
        assert(hd_progress::readStatus(set.status).state == "upscaled");
    }
    {
        Set set;
        set.upscale();
        set.finish();
        set.write(set.dir / "upscale_status.json.finished.tmp" / "held", "x", 3);
        assert(hd_progress::readStatus(set.status).state == "upscaled");
        assert(fs::last_write_time(set.status) == set.at);
    }
#endif
}
