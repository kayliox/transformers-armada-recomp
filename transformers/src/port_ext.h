#pragma once
// Extensions: optional parts of the port that live outside its core add themselves here, and
// the core calls them at fixed points. With none registered every hook does nothing and every
// query gives the core's own answer, so the core builds and runs the same without them.
//
// An extension is a plain struct of optional function pointers, registered once at static
// initialisation:
//
//     static const ext::Extension kMine = [] {
//         ext::Extension e;
//         e.name = "mine";
//         e.phase = ext::Phase::BeforeGuestCode;
//         e.install = [](PS2Runtime &rt) { mine::install(rt); return true; };
//         return e;
//     }();
//     TF_EXTENSION(kMine);
//
// Installs run in (phase, order) and frames in (frameOrder), each in registration order when
// those are equal. A query asks the extensions in (phase, order) and the first that answers
// wins; none answering gives the core's default.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class PS2Runtime;
namespace nr
{
    struct Draw;
    struct Frame;
}

namespace ext
{
    // Where in the game hook's install (overrides.cpp applyTransformers) an extension installs. The
    // order is load-bearing: added guest code is laid out in install order, and an extension's
    // allocations come after the core's.
    enum class Phase : int
    {
        Early = 0,           // first, before any of the core's installs
        AfterPhotoMode = 1,  // among the core's installs, right after photo mode's (the jet bike's place: keeps the guest
                             // layout every existing save state was made with)
        BeforeGuestCode = 2, // after the core's installs, before the added code's install and self-test
        AfterGuestCode = 3,  // right after the added code's install (no guest allocations of its own)
        AfterRender = 4,     // right after the native renderer's install (nr::install), before its N5 step's
        BeforeSettings = 5,  // after the core's debugging hooks, right before the core's settings are added
        AfterSettings = 6,   // right after the core's settings are added, before the HQ animation tween's install
        Late = 7,            // after everything else, before the added code is written to guest memory
    };

    // What a frame hook is told about the moment (the game hook's frame, overrides.cpp).
    struct FrameFacts
    {
        uint64_t tick = 0;              // the VBlank count
        bool inLevel = false;           // a level runs (the taGame is there)
        bool inHq = false;              // the HQ's executor runs
        int levelRate = 0;              // tf::port::levelRate()'s figure for this frame
        uint32_t playerPosition = 0u;   // the player's position in guest memory, 0 when none
        bool moviePlaying = false;      // a movie streams (an FMV is live)
    };

    // The native renderer's capture of a frame (nr_capture.cpp), for an extension that draws the player its own way.
    // Set once at static initialisation (setRenderHooks); none set, or `active` false: the renderer runs as without it.
    struct RenderHooks
    {
        bool (*active)() = nullptr;                                               // checked before every hook below
        void (*noteTexture)(uint32_t guestTexture, const std::string &name) = nullptr; // game thread, each texture converted
        void (*beginCapture)(const uint8_t *rdram, uint64_t tick, bool gameplay, double ticksPerSecond) = nullptr; // game thread
        void (*tagDraw)(nr::Draw &draw) = nullptr;                                // a freshly captured draw, before its enqueue
        void (*sealCapture)(nr::Frame &frame) = nullptr;                          // game thread, before the frame is submitted
        bool (*replaceFrame)(nr::Frame &frame) = nullptr;                         // render thread, after the frame is expanded
    };

    struct Extension
    {
        const char *name = "";
        Phase phase = Phase::BeforeGuestCode;
        int order = 0;      // within the phase
        int frameOrder = 0; // among the frame hooks

        // Setting rows (ps2_settings::add); run where the core adds its own.
        void (*addSettings)() = nullptr;
        // The install; false stops the game (the core's own install failures do the same).
        bool (*install)(PS2Runtime &runtime) = nullptr;
        // Once a frame, after the added code's frame work.
        void (*gameFrame)(uint8_t *rdram, PS2Runtime &runtime, const FrameFacts &facts) = nullptr;

        // Queries (nullptr: no answer).
        bool (*hideCrosshair)(const uint8_t *rdram) = nullptr;           // the game's crosshair is left out
        bool (*hideDistance)(const uint8_t *rdram) = nullptr;            // no target distance to show
        bool (*aimPoint)(const uint8_t *rdram, float point[3]) = nullptr; // where the crosshair goes instead
        uint32_t (*moviePath)(uint8_t *rdram, std::string_view movie) = nullptr; // another path string (guest address) for a movie, 0 = none
        bool (*ownsMouseScreen)(const uint8_t *rdram, uint32_t menu) = nullptr; // a list screen whose rows it lays out
        bool (*rowBounds)(const uint8_t *rdram, uint32_t menu, unsigned row, float &start, float &end) = nullptr;
        std::string (*notification)() = nullptr;                         // a message for the menu's notice line
        std::filesystem::path (*prepareDisc)(const std::filesystem::path &gameDir) = nullptr; // another disc root, empty = none
        // (TODO 316) Another memory card root than `normalRoot` (the save profile's own card), empty = none. Asked once
        // at the start, after the ELF loaded; not when the caller gave the card itself (TF_MC_ROOT: tests).
        std::filesystem::path (*cardRoot)(const std::filesystem::path &normalRoot) = nullptr;
        std::vector<std::string> (*loadoutTags)() = nullptr;             // what it changes, kept in recordings and states
        // Its own controls (buttons) in the F1 menu, drawn where the menu shows `section` (mod_menu.cpp).
        void (*menuSection)(const char *section) = nullptr;
        // A change of its that takes a fresh start of the game (the menu offers "Restart game").
        bool (*restartPending)() = nullptr;
        // That restart carried out by the extension itself (it asks first): the menu calls it each frame it is drawn,
        // clicked = its "Restart game" button was pressed this frame (then true: the extension took it; none taking it,
        // the menu restarts at once).
        bool (*restartGame)(bool clicked) = nullptr;
        // Functions of its own for the decomp layer's registry, added when that installs (decomp::install, before
        // it sorts and switches them; not with TF_DECOMP=0).
        void (*addDecompFunctions)() = nullptr;
        // Its last words before a test run's quit (TF_QUIT_VSYNC ends the process without exit handlers).
        void (*beforeQuit)() = nullptr;
        // The character the player plays when it is its own (Discord status, discord_presence.h): its name and the
        // art asset key of its small image; false: none of its own (the Autobot is shown).
        bool (*presenceCharacter)(const uint8_t *rdram, std::string &name, std::string &imageKey) = nullptr;
        // A save state was loaded, before the core adopts it (overrides.cpp); false: the extension could not adopt it
        // and the runtime is stopping (the core then leaves the state alone).
        bool (*stateLoaded)(PS2Runtime &runtime) = nullptr;
        // A movie (its key, hd_movies::movieKey) skipped as the player would skip it: true skips it.
        bool (*skipMovie)(std::string_view movieKey) = nullptr;
    };

    // Registers an extension (kept by pointer: give it static storage).
    void add(const Extension &extension);
    // The registered extensions, sorted by (phase, order).
    const std::vector<const Extension *> &all();

    // --- Called by the core ---------------------------------------------------------------------
    void addSettings();
    // Runs one phase's installs; false when one failed (the caller stops the game).
    bool install(Phase phase, PS2Runtime &runtime);
    void frame(uint8_t *rdram, PS2Runtime &runtime, const FrameFacts &facts);

    bool hideCrosshair(const uint8_t *rdram);
    bool hideDistance(const uint8_t *rdram);
    bool aimPoint(const uint8_t *rdram, float point[3]);
    uint32_t moviePath(uint8_t *rdram, std::string_view movie);
    bool ownsMouseScreen(const uint8_t *rdram, uint32_t menu);
    bool rowBounds(const uint8_t *rdram, uint32_t menu, unsigned row, float &start, float &end);
    std::string notification();
    std::filesystem::path prepareDisc(const std::filesystem::path &gameDir);
    // The memory card root an extension moves the card to (TODO 316), empty = the normal one.
    std::filesystem::path cardRoot(const std::filesystem::path &normalRoot);
    // Every extension's tags, in order.
    std::vector<std::string> loadoutTags();
    // Every extension's controls for a menu section, in order.
    void menuSection(const char *section);
    // Any extension waits for a fresh start.
    bool restartPending();
    // The menu's restart for it, each frame the menu is drawn (restartGame above); true when an extension took the click.
    bool restartGame(bool clicked);
    void addDecompFunctions();
    void beforeQuit();
    // The first extension's character (presenceCharacter above); false: none.
    bool presenceCharacter(const uint8_t *rdram, std::string &name, std::string &imageKey);
    // Every extension's stateLoaded; false when one could not adopt the state.
    bool stateLoaded(PS2Runtime &runtime);
    // Any extension skips this movie.
    bool skipMovie(std::string_view movieKey);
    // The renderer's hooks (RenderHooks above): set once; null = none.
    void setRenderHooks(const RenderHooks *hooks);
    const RenderHooks *renderHooks();
}

#define TF_EXT_CAT2(a, b) a##b
#define TF_EXT_CAT(a, b) TF_EXT_CAT2(a, b)
// Registers `extension` (an ext::Extension with static storage) before main runs.
#define TF_EXTENSION(extension) \
    static const bool TF_EXT_CAT(tfExtensionAdded_, __LINE__) = (::ext::add(extension), true)
