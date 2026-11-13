// The extension registry (port_ext.h). Filled during static initialisation and read-only from
// then on, so the hooks need no locks.

#include "port_ext.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace ext
{
    namespace
    {
        // (a function-local static: extensions register from other files' static initialisers)
        std::vector<const Extension *> &registry()
        {
            static std::vector<const Extension *> list;
            return list;
        }

        std::vector<const Extension *> &framesInOrder()
        {
            static std::vector<const Extension *> list;
            return list;
        }

        bool before(const Extension *a, const Extension *b)
        {
            if (a->phase != b->phase)
                return static_cast<int>(a->phase) < static_cast<int>(b->phase);
            return a->order < b->order;
        }
    }

    void add(const Extension &extension)
    {
        auto &list = registry();
        // (stable: equal keys keep their registration order)
        list.insert(std::upper_bound(list.begin(), list.end(), &extension, before), &extension);
        if (extension.gameFrame)
        {
            auto &frames = framesInOrder();
            frames.insert(std::upper_bound(frames.begin(), frames.end(), &extension,
                                           [](const Extension *a, const Extension *b) { return a->frameOrder < b->frameOrder; }),
                          &extension);
        }
    }

    const std::vector<const Extension *> &all() { return registry(); }

    void addSettings()
    {
        for (const Extension *e : registry())
            if (e->addSettings)
                e->addSettings();
    }

    bool install(Phase phase, PS2Runtime &runtime)
    {
        bool ok = true;
        for (const Extension *e : registry())
            if (e->phase == phase && e->install && !e->install(runtime))
            {
                // (TODO 256) Said in the log: the game is stopped for it (overrides.cpp), and nothing else names it.
                std::fprintf(stderr, "[ext] %s could not install (phase %d): the game stops\n", e->name ? e->name : "?",
                             static_cast<int>(phase));
                std::fflush(stderr);
                ok = false;
            }
        return ok;
    }

    void frame(uint8_t *rdram, PS2Runtime &runtime, const FrameFacts &facts)
    {
        for (const Extension *e : framesInOrder())
            e->gameFrame(rdram, runtime, facts);
    }

    bool hideCrosshair(const uint8_t *rdram)
    {
        for (const Extension *e : registry())
            if (e->hideCrosshair && e->hideCrosshair(rdram))
                return true;
        return false;
    }

    bool hideDistance(const uint8_t *rdram)
    {
        for (const Extension *e : registry())
            if (e->hideDistance && e->hideDistance(rdram))
                return true;
        return false;
    }

    bool aimPoint(const uint8_t *rdram, float point[3])
    {
        for (const Extension *e : registry())
            if (e->aimPoint && e->aimPoint(rdram, point))
                return true;
        return false;
    }

    uint32_t moviePath(uint8_t *rdram, std::string_view movie)
    {
        for (const Extension *e : registry())
            if (e->moviePath)
                if (const uint32_t path = e->moviePath(rdram, movie))
                    return path;
        return 0u;
    }

    bool ownsMouseScreen(const uint8_t *rdram, uint32_t menu)
    {
        for (const Extension *e : registry())
            if (e->ownsMouseScreen && e->ownsMouseScreen(rdram, menu))
                return true;
        return false;
    }

    bool rowBounds(const uint8_t *rdram, uint32_t menu, unsigned row, float &start, float &end)
    {
        for (const Extension *e : registry())
            if (e->rowBounds && e->rowBounds(rdram, menu, row, start, end))
                return true;
        return false;
    }

    std::string notification()
    {
        for (const Extension *e : registry())
            if (e->notification)
                if (std::string message = e->notification(); !message.empty())
                    return message;
        return {};
    }

    std::filesystem::path prepareDisc(const std::filesystem::path &gameDir)
    {
        for (const Extension *e : registry())
            if (e->prepareDisc)
                if (std::filesystem::path disc = e->prepareDisc(gameDir); !disc.empty())
                    return disc;
        return {};
    }

    std::filesystem::path cardRoot(const std::filesystem::path &normalRoot)
    {
        for (const Extension *e : registry())
            if (e->cardRoot)
                if (std::filesystem::path root = e->cardRoot(normalRoot); !root.empty())
                    return root;
        return {};
    }

    void menuSection(const char *section)
    {
        for (const Extension *e : registry())
            if (e->menuSection)
                e->menuSection(section);
    }

    bool restartPending()
    {
        for (const Extension *e : registry())
            if (e->restartPending && e->restartPending())
                return true;
        return false;
    }

    bool restartGame(bool clicked)
    {
        bool taken = false;
        for (const Extension *e : registry())
            if (e->restartGame && e->restartGame(clicked))
                taken = true;
        return taken;
    }

    void addDecompFunctions()
    {
        for (const Extension *e : registry())
            if (e->addDecompFunctions)
                e->addDecompFunctions();
    }

    void beforeQuit()
    {
        for (const Extension *e : registry())
            if (e->beforeQuit)
                e->beforeQuit();
    }

    bool presenceCharacter(const uint8_t *rdram, std::string &name, std::string &imageKey)
    {
        for (const Extension *e : registry())
            if (e->presenceCharacter && e->presenceCharacter(rdram, name, imageKey))
                return true;
        return false;
    }

    bool stateLoaded(PS2Runtime &runtime)
    {
        bool ok = true;
        for (const Extension *e : registry())
            if (e->stateLoaded && !e->stateLoaded(runtime))
                ok = false;
        return ok;
    }

    bool skipMovie(std::string_view movieKey)
    {
        for (const Extension *e : registry())
            if (e->skipMovie && e->skipMovie(movieKey))
                return true;
        return false;
    }

    namespace
    {
        const RenderHooks *g_renderHooks = nullptr;
    }

    void setRenderHooks(const RenderHooks *hooks) { g_renderHooks = hooks; }

    const RenderHooks *renderHooks() { return g_renderHooks; }

    std::vector<std::string> loadoutTags()
    {
        std::vector<std::string> tags;
        for (const Extension *e : registry())
            if (e->loadoutTags)
                for (std::string &tag : e->loadoutTags())
                    tags.push_back(std::move(tag));
        return tags;
    }
}
