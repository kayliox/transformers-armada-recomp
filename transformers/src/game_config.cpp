// The game's own config settings in the settings panel (game_config.h has the long form).

#include "game_config.h"

#include "photo_mode.h"
#include "start_screens.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace game_config
{
    namespace
    {
        constexpr uint32_t kTableBegin = 0x00715598u; // clData** (utGameConfig's vector)
        constexpr uint32_t kTableCount = 0x00715594u; // its length
        constexpr uint32_t kLabel = 0x04u;            // the label: a pointer to a std::string (labelAt)
        constexpr uint32_t kValue = 0x14u;            // bool (a byte), enum (its index), integer, float

        enum class Kind
        {
            Bool,
            Enum,
        };

        struct Item
        {
            const char *label;   // the setting's name in 0_DEFS.TMD
            const char *section; // the panel's tab
            const char *title;
            const char *help;
            Kind kind;
            std::vector<const char *> values; // Enum: the values in the file's order (stored as the index)
            int order;
        };

        // Settings retail's code still reads (the research's settings.csv). The help says
        // when a change shows. The core's own (the HUD: photo mode turns it off), then the rows extensions add
        // (addRow: the Cheats and the developers' Debug rows).
        std::vector<Item> &items()
        {
            static std::vector<Item> list = {
                {"showHUD", "Debug", "HUD", "The in-game HUD. Off: a clean picture, for screenshots.", Kind::Bool, {}, 10},
                // ("runUIIntro", the old "Intro screens" switch: replaced by the Gameplay page's "When the game starts",
                // start_screens.h - TODO 110; a value left in settings.ini is not read)
            };
            return list;
        }

        std::string key(const Item &item) { return std::string("TF_CFG_") + item.label; }

        // The choices, by key ("" = the game's own value: not touched). Set from the panel
        // (the window thread), read by frame() (the game thread).
        std::mutex g_lock;
        std::map<std::string, std::string> g_chosen;

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        std::string readString(const uint8_t *rdram, uint32_t address, size_t limit = 64u)
        {
            std::string out;
            for (size_t i = 0; address != 0u && i < limit; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
                if (!c || *c == 0u)
                    break;
                out.push_back(static_cast<char>(*c));
            }
            return out;
        }

        // The game's std::string (its compare at 0x101270): the first byte's low bit set = the
        // characters elsewhere, their address at +8; clear = held in the string, from +1.
        std::string labelAt(const uint8_t *rdram, uint32_t string)
        {
            const uint8_t *flag = string ? getConstMemPtr(rdram, string) : nullptr;
            if (!flag)
                return {};
            return readString(rdram, (*flag & 1u) ? read32(rdram, string + 8u) : string + 1u);
        }

        std::map<std::string, uint32_t> g_entries; // label -> clData* (frame, on the game thread)

        // (TODO 228) The HUD's own flag (taHUD +0x74) from the setting: -1 the game's own, 0 Off, 1 On (frame keeps it).
        std::atomic<int> g_hudWant{-1};
        constexpr uint32_t kHudDraw = 0x002FB190u;   // taHUD's draw (vtable slot 3): nothing of it while +0x74 is 0
        constexpr uint32_t kHudShown = 0x74u;        // the flag taPlayerHUD's constructor sets from showHUD
        PS2Runtime::RecompiledFunction g_hudDraw = nullptr;
        uint32_t g_hudForced = 0u; // the HUD whose flag was set here (put back as the game had it for "Game's own")
        uint8_t g_hudGameValue = 1u;

        void hudDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kHudDraw) // (not a resumed call)
            {
                const uint32_t hud = getRegU32(ctx, 4);
                const int want = photo_mode::hudHidden() ? 0 : g_hudWant.load(std::memory_order_relaxed);
                if (uint8_t *flag = hud ? getMemPtr(rdram, hud + kHudShown) : nullptr)
                {
                    if (photo_mode::takeHudRestore() && want < 0)
                        *flag = 1u;
                    if (want >= 0)
                    {
                        if (g_hudForced != hud)
                        {
                            g_hudForced = hud;
                            g_hudGameValue = *flag;
                        }
                        *flag = static_cast<uint8_t>(want);
                    }
                    else if (g_hudForced == hud)
                    {
                        *flag = g_hudGameValue;
                        g_hudForced = 0u;
                    }
                }
            }
            g_hudDraw(rdram, ctx, runtime);
        }

        // A plausible table: a count of a few hundred at most and a pointer into main memory.
        bool tableReady(const uint8_t *rdram, uint32_t &begin, uint32_t &count)
        {
            begin = read32(rdram, kTableBegin);
            count = read32(rdram, kTableCount);
            return begin >= 0x00100000u && begin < PS2_RAM_SIZE && count > 0u && count < 512u;
        }
    }

    namespace
    {
        std::vector<Choice> choicesOf(const Item &item);
    }

    void addRow(const Row &row)
    {
        items().push_back(Item{row.label, row.section, row.title, row.help, row.isEnum ? Kind::Enum : Kind::Bool, row.values, row.order});
    }

    void addSettings()
    {
        for (const Item &item : items())
        {
            ps2_settings::Setting s;
            s.key = key(item);
            s.section = item.section;
            s.label = item.title;
            s.help = item.help;
            s.order = item.order;
            for (const Choice &c : choicesOf(item))
                s.choices.push_back({c.value, c.label});
            s.defaultValue = "";
            const std::string k = s.key;
            s.apply = [k](const std::string &value)
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_chosen[k] = value;
                return true; // (the game takes it at its next frame)
            };
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_chosen[k] = ps2_settings::value(k);
            }
            ps2_settings::add(s);
        }
    }

    namespace
    {
        std::vector<Choice> choicesOf(const Item &item)
        {
            std::vector<Choice> list{{"", "Game's own"}};
            if (item.kind == Kind::Bool)
            {
                list.push_back({"1", "On"});
                list.push_back({"0", "Off"});
            }
            else
                for (size_t i = 0; i < item.values.size(); ++i)
                    list.push_back({std::to_string(i), item.values[i]});
            return list;
        }
    }

    std::vector<View> views()
    {
        std::vector<View> out;
        std::lock_guard<std::mutex> guard(g_lock);
        for (const Item &item : items())
            out.push_back({item.section, item.title, item.help, choicesOf(item), g_chosen[key(item)]});
        return out;
    }

    void choose(size_t index, const std::string &value)
    {
        if (index >= items().size())
            return;
        const std::string k = key(items()[index]);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_chosen[k] = value;
        }
        ps2_settings::noteChanged(k, value); // (into settings.ini, and the F5 panel shows it)
    }

    void frame(uint8_t *rdram)
    {
        uint32_t begin = 0u, count = 0u;
        if (!tableReady(rdram, begin, count))
            return;

        // Where each setting is in the table (looked up again when the table moves).
        static uint32_t seenBegin = 0u, seenCount = 0u;
        std::map<std::string, uint32_t> &entries = g_entries; // label -> clData*
        if (begin != seenBegin || count != seenCount)
        {
            seenBegin = begin;
            seenCount = count;
            entries.clear();
            static const bool dump = std::getenv("TF_CONFIG_DUMP") != nullptr;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t entry = read32(rdram, begin + 4u * i);
                const std::string label = labelAt(rdram, read32(rdram, entry + kLabel));
                if (label.empty())
                    continue;
                entries[label] = entry;
                if (dump)
                    std::fprintf(stderr, "[config] %3u %-36s vt=%08x value=%08x\n", i, label.c_str(), read32(rdram, entry),
                                 read32(rdram, entry + kValue));
            }
            std::fprintf(stderr, "[config] the game's settings table: %u entries at %08x\n", count, begin);
        }

        std::map<std::string, std::string> chosen;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            chosen = g_chosen;
        }
        {
            const std::string &hud = chosen["TF_CFG_showHUD"];
            g_hudWant.store(hud == "0" ? 0 : hud == "1" ? 1 : -1, std::memory_order_relaxed);
        }
        for (const Item &item : items())
        {
            const std::string &value = chosen[key(item)];
            if (value.empty())
                continue;
            // (never the game's runUIIntro: Off there skips the title's whole flow - no profile, and
            // the first level stops the game (TODO 80); the switch skips the start's movies instead)
            if (std::string(item.label) == "runUIIntro")
                continue;
            const auto it = entries.find(item.label);
            if (it == entries.end())
                continue;
            if (item.kind == Kind::Bool)
            {
                if (uint8_t *p = getMemPtr(rdram, it->second + kValue))
                    *p = value == "1" ? 1u : 0u;
            }
            else if (uint8_t *p = getMemPtr(rdram, it->second + kValue))
            {
                const uint32_t index = static_cast<uint32_t>(std::atoi(value.c_str()));
                std::memcpy(p, &index, sizeof(index));
            }
        }
    }

    void installHud(PS2Runtime &runtime)
    {
        if ((g_hudDraw = runtime.lookupFunction(kHudDraw)) != nullptr)
            runtime.registerFunction(kHudDraw, hudDraw);
    }

    bool hudOff() { return g_hudWant.load(std::memory_order_relaxed) == 0; }

    bool skipIntroMovie(const std::string &movieKey) { return start_screens::skipsMovie(movieKey.c_str()); }

    uint32_t valueAddress(const char *label)
    {
        const auto it = g_entries.find(label);
        return it != g_entries.end() ? it->second + kValue : 0u;
    }
}
