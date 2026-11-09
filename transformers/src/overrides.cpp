// Game-specific runtime hooks for Transformers (USA, SLUS-20668).

#include "controller_settings.h"
#include "game_config.h"
#include "ultrawide.h"
#include "cam_trace.h"
#include "file_trace.h"
#include "camera_mode.h"
#include "discord_presence.h"
#include "dualsense.h"
#include "boss_cheat.h"
#include "distance_meter.h"
#include "hd_movies.h"
#include "nr_movie.h"
#include "speedrun_timer.h"
#include "start_screens.h"
#include "tuning.h"
#include "photo_mode.h"
#include "view_turn.h"
#include "fast_load.h"
#include "hq_rate.h"
#include "hq_animation_tween.h"
#include "port_ext.h"
#include "guest_code.h"
#include "key_text.h"
#include "actor_guard.h"
#include "revisit_music.h"
#include "pool_growth.h"
#include "movement.h"
#include "ragdoll_step.h"
#include "test_place.h"
#include "ai_probe.h"
#include "small_crosshair.h"
#include "crisp_text.h"
#include "sound_nodes.h"
#include "sound_trace.h"
#include "sirens_trace.h"
#include "native_disc.h"
#include "native_sound.h"
#include "vrr_probe.h"
#include "menu_mouse.h"
#include "mod_menu.h"
#include "frustum_native.h"
#include "playrec.h"
#include "decomp.h"
#include "timing/timing.h"
#include "game_overrides.h"
#include "port.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_host_present.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_vif1_thread.h"
#include "nr_install.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <tuple>
#include <vector>

// Hooks exported by the runtime (ps2xRuntime/src/lib/Kernel/Stubs/Pad.cpp, MPEG.cpp, LibC.cpp).
namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
    void setMpegGuestBuffersStream(bool enabled);
    void setVarArgSlotBytes(uint32_t bytes);
}

// The native renderer's counters for the slow-frame report (TODO 112, TF_HITCH_MS; native_render/nr_types.h).
namespace nr
{
    double gameWaitMs();
    void largeImageActivity(unsigned &queued, unsigned &heavy, uint64_t &compressed, uint64_t &pngs);
    uint64_t pipelinesMade();
    namespace d3d
    {
        double slotWaitMs();
    }
    namespace n5
    {
        void install(PS2Runtime &runtime); // (native_render/nr_n5.cpp: N5, the render device native - docs/N5_PLAN.md)
        void dropSummaries();              // (N5 step 1: the geometry sets' packet summaries let go of)
    }
}

#if defined(_WIN32)
// profiler.cpp
void tfLogCallStack(const char *label);
void tfGuardGuestWrites(uint32_t address, uint32_t length, uint32_t expected, bool useExpected);
void tfStopGuestWriteGuard();
std::string ps2xRecentDispatches(); // (the runtime: the thread's last guest dispatches)
void tfWatchdogFrame(); // (profiler.cpp: the hang watchdog's sign of life, once a game frame)
void tfNoteFrameWork(bool begins, double workMs); // (profiler.cpp: TF_PROFILE_SPIKES's frame spans, TODO 164)
void tfTestWindow(uint64_t vblank); // (profiler.cpp: TF_TEST_WINDOW, the window minimised / shown in tests)
bool tfWindowAway(); // (profiler.cpp, TODO 117: the player has left the window - minimised, or another one has the focus)
double tfThreadCpuMs();
#endif

namespace
{
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kElfCrc32 = 0x74C982C0u;

    // The frame rate levels run at (see "The frame rate" below).
    int g_frameRate = 30; // the setting: TF_FRAME_RATE, 60 by default (30: the console's)
    int g_levelRate = 0;  // the rate the running level runs at (0: the console's 30)
    // The vtable of the executor the main loop runs now (executorLoop; 0: not seen yet): fast_load
    // is told whether it is the HQ's (taUIPreGame, 0x6B7360).
    uint32_t g_executorVtable = 0u;
    bool g_inBetween = true; // the setting: TF_IN_BETWEEN, in-between pictures in levels (see "The frame rate")
    // With in-between pictures a frame's two fields are drawn apart, each object twice: field A
    // into the top half of the scene buffer (block 0, rows 0-223), field B - the in-between -
    // into the bottom half (rows 224-447); after the scene both are copied into the display
    // buffer's rows, A's into the even ones, B's into the odd ones (SCANMSK 3, then 2). A frame
    // the GPU cannot draw both for in time leaves B's out, and shows A at both VBlanks: the game
    // goes on at its speed instead of waiting for the GPU (ps2_host_display::setSecondFieldDraws)
    // - two pictures a frame as a cap. TF_IN_BETWEEN_DROP=0: always both, the game waiting (slow
    // motion where the GPU cannot keep up). Not in turbo mode (PS2X_TURBO_VSYNCS).
    bool g_inBetweenDrop = true;
    void setInBetweenDrop(bool inBetween)
    {
        ps2_host_display::setSecondFieldDraws(inBetween && g_inBetweenDrop ? 0 : -1, 224u, 447u);
        ps2_host_display::setFieldsBuffer(inBetween ? 0 : -1); // (both fields' buffer, cap or not)
        // With moment pictures (TF_MOMENTS=1) field B is the frame before's pose (the governor's
        // in-between factor 0): the GPU renderer draws the screen's refreshes between the two.
        ps2_host_display::setMomentFields(inBetween && tf::port::momentPictures() ? 224 : -1, 447u);
    }
    // Pictures a second locked (TF_FPS_LOCK=60 or 120, F4 switches between them while playing):
    // levels at 60 frames, one picture a frame or two, never changed by the host's load - a
    // frame rate cap as a PC game's settings have it. 0: unlocked (120 while the host keeps up).
    int g_pictureLock = 0;
    constexpr int kKeyF4 = 293; // (raylib's code)
    // The frame rate setting itself (TF_FRAME_RATE): g_frameRate is 60 while the pictures are
    // locked (F4, TF_FPS_LOCK), and unlocking goes back to this.
    int g_rateSetting = 60;
    constexpr int kUnlimitedRate = 480; // (Unlimited: the most the port runs a level at)
    // (TODO 161 a, 165) The game's logic follows the frame rate cap (the user, 2026-11-05: every picture a game frame): a level
    // runs at the cap - the screen's refresh at most - a short hitch leaves pictures out and is caught up (nr_thread.cpp,
    // TF_CATCH_UP_MS), and only a PC that cannot hold the rate for long has it lowered (adaptFrameRate: its frames late, or
    // the drawing leaving frames out, for 1.5 s), climbing back when there is room. Not a player's setting: TF_LOGIC_RATE=<rate>
    // is a test switch - the level fixed at that rate, the cap capping the pictures (TODO 161 a's).
    std::atomic<int> g_logicRate{0};
    // The settings panel's choices (ps2_settings.h, the window thread), taken on the game thread at
    // its next frame (takeSettings, applyDrawDistance); and what is in effect, for the panel.
    std::atomic<int> g_wantLock{-1};
    // Pictures at the screen's refresh (moment pictures, docs/RENDERER.md stage 4): TF_FPS_LOCK=refresh
    // or TF_MOMENTS=1 at start, the settings panel's Frame rate while playing (taken at the next frame,
    // takeSettings); F4 goes back to 120 or 60. Read by tf::port::momentPictures() - the governor's
    // in-between factor, the clippers, the renderer's moments (setInBetweenDrop).
    std::atomic<bool> g_moments{false};
    // (N9 stage 2) Pictures between the game's frames with the native renderer (TF_POSES, the settings panel's "Smooth
    // pictures between frames", live; on by default): read by tf::port::posePictures - the governor's in-between factor
    // while a frame draws one field (oneFieldFactor), the native renderer's thread (nr_thread.cpp poses).
    // (TODO 99: taken from the environment on first use, not when the program starts - settings.ini's TF_POSES is put into
    // the environment by main (ps2_settings::loadFile), after the statics are made: read at start, "Off" in settings.ini was
    // never seen and the pictures between frames stayed on. -1: not taken yet.) Not a player's setting since 2026-11-05 (the
    // user: the frame rate cap is the one frame setting): built in, TF_POSES=0 / 1 a test switch.
    std::atomic<int> g_posePictures{-1};
    bool posePicturesWanted()
    {
        int v = g_posePictures.load(std::memory_order_relaxed);
        if (v < 0)
        {
            const char *e = std::getenv("TF_POSES");
            int expected = -1;
            // (TODO 165: off by default - every picture is a game frame; TF_POSES=1, a test switch)
            g_posePictures.compare_exchange_strong(expected, e && *e == '1' ? 1 : 0);
            v = g_posePictures.load(std::memory_order_relaxed);
        }
        return v > 0;
    }
    // The HQ's own moment pictures (whatever the levels' setting): its game rate stays at 30, so
    // pictures at the screen's refresh are its only way past 60 (executorLoop; TF_HQ_MOMENTS=1 - off by
    // default since they tore Optimus's grille there, TODO item 32).
    std::atomic<bool> g_hqMoments{false};
    std::atomic<int> g_wantMoments{-1};
    std::atomic<bool> g_momentsShown{false}; // (the panel's: the choice made, before the game takes it)
    std::atomic<float> g_wantDrawDistance{0.0f};
    std::atomic<int> g_lockShown{0};
    std::atomic<float> g_drawPresetShown{2.0f}, g_sceneryShown{4.0f};
    bool g_levelHooks = false;        // the frame rate's hooks are in (started above 30)
    bool g_drawDistanceHooks = false; // the draw distance's (started at a factor other than 1)
    float framesPerSecond() { return g_levelRate != 0 ? static_cast<float>(g_levelRate) : 30.0f; }
    // The view's top turning speeds with the right stick (see the mouse look below), a second.
    constexpr float kYawPerSecond = 2.35619f; // radians at full deflection
    constexpr float kPitchPerSecond = 0.785398f;

    // int _sceSifLoadModuleBuffer(u32 iopAddr, int argLen, const char *args, int *startResult)
    constexpr uint32_t kSifLoadModuleBuffer = 0x00126D00u;

    uint32_t readLe32(const uint8_t *p)
    {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint16_t readLe16(const uint8_t *p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    // Size of an ELF image from its section and program headers.
    uint32_t elfImageSize(PS2Runtime &runtime, uint32_t iopAddr)
    {
        uint8_t ehdr[52];
        if (!runtime.readIopMemory(iopAddr, ehdr, sizeof(ehdr)) || std::memcmp(ehdr, "\x7F" "ELF", 4) != 0)
            return 0;

        const uint32_t phoff = readLe32(ehdr + 28), shoff = readLe32(ehdr + 32);
        const uint16_t phentsize = readLe16(ehdr + 42), phnum = readLe16(ehdr + 44);
        const uint16_t shentsize = readLe16(ehdr + 46), shnum = readLe16(ehdr + 48);
        uint32_t size = std::max<uint32_t>(sizeof(ehdr), phoff + phentsize * phnum);
        size = std::max<uint32_t>(size, shoff + shentsize * shnum);

        std::vector<uint8_t> hdr(40);
        for (uint16_t i = 0; i < shnum; ++i)
        {
            if (!runtime.readIopMemory(iopAddr + shoff + i * shentsize, hdr.data(), 40))
                return 0;
            if (readLe32(hdr.data() + 4) != 8u) // SHT_NOBITS occupies no file space
                size = std::max(size, readLe32(hdr.data() + 16) + readLe32(hdr.data() + 20));
        }
        for (uint16_t i = 0; i < phnum; ++i)
        {
            if (!runtime.readIopMemory(iopAddr + phoff + i * phentsize, hdr.data(), 32))
                return 0;
            size = std::max(size, readLe32(hdr.data() + 4) + readLe32(hdr.data() + 16));
        }
        return size <= 4u * 1024u * 1024u ? size : 0u;
    }

    // The game DMAs its IOP modules (the TwoFish CD/DVD file server, etc.) into
    // IOP RAM and then loads them from there, as the SDK intends. PS2Recomp's
    // stock handler reads the buffer address from EE RAM instead, so stage the
    // image through EE RAM for the duration of the call.
    void sifLoadModuleBufferFromIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        const uint32_t iopAddr = getRegU32(ctx, 4);
        const uint32_t argLen = getRegU32(ctx, 5);
        const uint32_t argAddr = getRegU32(ctx, 6);
        const uint32_t resultAddr = getRegU32(ctx, 7);

        const uint32_t size = elfImageSize(*runtime, iopAddr);
        if (size == 0u)
        {
            std::cerr << "[tf] LoadModuleBuffer: no IRX image at IOP 0x" << std::hex << iopAddr << std::dec << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        std::vector<uint8_t> image(size);
        (void)runtime->readIopMemory(iopAddr, image.data(), size);

        std::vector<uint8_t> args;
        if (argAddr != 0u && argLen != 0u)
        {
            args.resize(std::min<uint32_t>(argLen, 64u * 1024u));
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = rdram[(argAddr + i) & PS2_RAM_MASK];
        }

        // Borrow EE RAM at the same offset, then put back what was there.
        const uint32_t stage = iopAddr & PS2_RAM_MASK;
        std::vector<uint8_t> saved(rdram + stage, rdram + stage + size);
        std::memcpy(rdram + stage, image.data(), size);
        const auto loaded = runtime->loadIopModuleBuffer(stage, args.empty() ? nullptr : args.data(),
                                                         static_cast<uint32_t>(args.size()));
        std::memcpy(rdram + stage, saved.data(), size);

        if (!loaded.handled || loaded.moduleId <= 0)
        {
            std::cerr << "[tf] failed to load the IOP module at 0x" << std::hex << iopAddr << std::dec << std::endl;
            for (const std::string &line : runtime->iopDebugSnapshot().diagnostics)
                std::cerr << "[tf]   iop: " << line << std::endl;
            setReturnS32(ctx, -1);
            return;
        }

        // On hardware this load is an RPC round trip, and the module's own
        // threads get to run before the EE sees the reply. TwoFish relies on
        // that: it announces itself with a SIF command the EE checks right
        // after the load returns. Let the IOP run for ~10 ms of EE time.
        for (int i = 0; i < 300; ++i)
            runtime->eeScheduler().accountCycles(10000u);

        if (resultAddr != 0u)
        {
            const uint32_t value = static_cast<uint32_t>(loaded.startResult);
            std::memcpy(rdram + (resultAddr & PS2_RAM_MASK), &value, 4);
        }
        setReturnS32(ctx, loaded.moduleId);

        // The EE thread would be blocked waiting for the load RPC's reply,
        // letting SIF command handlers (like TwoFish's "ready") run first.
        // Yield so the scheduler services them before the caller resumes.
        EeScheduler &ee = runtime->eeScheduler();
        (void)ee.rotateReadyQueue(0, false);
        ee.transferIfRequested(false);
    }

    // TF_SKIP_FMV=1 skips the movies the way a player would, to keep scripted test runs
    // short: while an fmvStreamPS2 exists, Start and Cross are pressed briefly once a
    // second.
    constexpr uint32_t kCreateFmvStream = 0x00274D20u; // allocates and constructs an fmvStreamPS2
    constexpr uint32_t kFmvStreamDtor = 0x00275F40u;   // fmvStreamPS2::~fmvStreamPS2(this, flags)
    constexpr uint16_t kPadStart = 1u << 3;
    constexpr uint16_t kPadCross = 1u << 14;
    PS2Runtime::RecompiledFunction g_createFmvStream = nullptr;
    PS2Runtime::RecompiledFunction g_fmvStreamDtor = nullptr;
    int g_liveFmvStreams = 0;
    bool g_liveFmvIsFilm = false; // (the movie playing is a film, not the boot logos or the title's attract loop: Discord status)
    std::vector<uint32_t> g_inSceneStreams; // fmvStreamPS2 objects shown in the scene (the LEVELS previews)
    struct SceneStreamPace // (TODO 113: fmvStreamUpdate, below)
    {
        uint32_t stream;
        double nextTick; // the raw VBlank at which the update is next let through
        double lastTick = -1.0;   // (TODO 266: the raw VBlank of the frame before's update call)
        bool lastLetThrough = false;
    };
    std::vector<SceneStreamPace> g_sceneStreamPace;
    // (TODO 282) A state loaded in the HQ: its preview movie's player is in the state's memory, but this list is the
    // port's own and starts empty - the player then ran unpaced, every frame waited for the movie's next picture and
    // the Drop Zones screen fell to half and a quarter of the screen's refresh (15 a second at 60 Hz). Set by the
    // load; the first player updated in the HQ with no movie of the window's own playing is taken as the preview's.
    bool g_adoptSceneStream = false;
    uint32_t g_fmvFrames = 0;
    bool g_skipFmv = false;
    bool g_skipIntroMovie = false; // (the movie playing is a start movie the "Intro screens" switch skips: game_config)

    // Widescreen. The game has a 16:9 mode of its own: the widescreen switch of its Options
    // menu (option 4, kept at 0x6CF004 for the renderer), on by default when the console's TV
    // setting is 16:9 (sceScfGetAspect, 0x135A00; PS2X_WIDESCREEN=1). It draws the same
    // 512x448 buffer for a TV that stretches it to 16:9: a wider field of view (0x2CB404,
    // times 1.27) and a pixel aspect 3/4 of the 4:3 one, which the camera's projection takes
    // (0x20C530 -> 0x20CA00, and 0x221E70 for VU1's screen transform). The renderer's display
    // mode (0x70F480, set by 0x25E080 at every change of screen) holds the width and the pixel
    // aspect drawn with: 1 / (640 / width x 0.93), times 3/4 in widescreen. The window shows
    // the picture in the shape the game draws for: 16:9 when the mode's pixel aspect is the
    // widescreen one, else 4:3, and 4:3 while a movie plays (the movies are 4:3 pictures, which
    // the game shows unchanged in either mode).
    constexpr uint32_t kDisplayMode = 0x0070F480u; // width, height (u32), ..., pixel aspect (float at +0x10)

    void updateDisplayAspect(const uint8_t *rdram)
    {
        float aspect = 4.0f / 3.0f;
        if (g_liveFmvStreams == 0)
        {
            uint32_t width = 0u;
            float pixelAspect = 0.0f;
            std::memcpy(&width, rdram + kDisplayMode, sizeof(width));
            std::memcpy(&pixelAspect, rdram + kDisplayMode + 0x10u, sizeof(pixelAspect));
            // 1 for a 4:3 TV, 0.75 for a 16:9 one, 0.75 / k at an ultrawide shape (ultrawide.h).
            const float tv = width != 0u ? pixelAspect * (640.0f / static_cast<float>(width)) * 0.93f : 1.0f;
            if (tv > 0.25f && tv < 0.875f)
                aspect = ultrawide::shape();
        }
        static float shown = 0.0f;
        if (aspect != shown)
        {
            std::fprintf(stderr, "[tf] picture shape %.3f:1%s\n", aspect, g_liveFmvStreams != 0 ? " (movie)" : "");
            shown = aspect;
        }
        ps2_host_display::setGameAspect(aspect);
        // The game's 2D (HUD, menus, text) stays drawn for 4:3 in its widescreen mode: narrowed
        // by 3/4 (ps2_host_display::setOverlaySqueeze) it keeps its shape on the 16:9 picture.
        // TF_WIDE_HUD=1 leaves it stretched, as a widescreen TV showed it.
        static const bool stretchHud = []()
        {
            const char *env = std::getenv("TF_WIDE_HUD");
            return env && *env && *env != '0';
        }();
        ps2_host_display::setOverlaySqueeze(aspect > 1.5f && !stretchHud ? (4.0f / 3.0f) / aspect : 1.0f);
    }

    // A saved profile brings its own options, the widescreen switch included: loading one
    // copies them over the current ones and applies them all (0x2780E0, from 0x32F1E4), and
    // the card's settings are copied at boot (0x2780A0, reached by a tail jump from 0x32EE40:
    // that one is hooked, a jump bypasses the function table). With PS2X_WIDESCREEN=1 the game
    // plays in widescreen whatever the save says: the switch is put back on after either (the
    // Options menu still turns it off for the rest of the session).
    constexpr uint32_t kCopyOptions = 0x0032EE40u;
    constexpr uint32_t kApplyProfileOptions = 0x002780E0u;
    constexpr uint32_t kOptionsPointer = 0x006CF244u; // -> the options (byte 4: widescreen)
    constexpr uint32_t kWidescreenFlag = 0x006CF004u; // the renderer's copy of it
    PS2Runtime::RecompiledFunction g_copyOptions = nullptr;
    PS2Runtime::RecompiledFunction g_applyProfileOptions = nullptr;

    void forceWidescreenOption(uint8_t *rdram)
    {
        uint32_t options = 0u;
        std::memcpy(&options, rdram + kOptionsPointer, sizeof(options));
        if (options != 0u && (options & PS2_RAM_MASK) + 4u < PS2_RAM_SIZE)
            rdram[(options & PS2_RAM_MASK) + 4u] = 1u;
        rdram[kWidescreenFlag] = 1u;
    }

    // (TODO 178) Invert Y off by default. The options' two default-setters (0x32DFD0, 0x32E0F0: each option from the
    // game's config, byte +1 = invertYAxis through utGameConfig's GetBoolean - on in retail's CONFIGS/0_DEFS.TMD) run for a
    // new game / no save; the byte is put to 0 after them. A saved profile's options are copied in afterwards (0x32EE40 /
    // 0x2780E0), so a player's own choice still wins. TF_INVERT_DEFAULT=1: the game's default (on).
    constexpr uint32_t kOptionsDefaults[] = {0x0032DFD0u, 0x0032E0F0u};
    PS2Runtime::RecompiledFunction g_optionsDefaults[2] = {nullptr, nullptr};
    template <int I> void optionsDefaults(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t options = getRegU32(ctx, 4);
        g_optionsDefaults[I](rdram, ctx, runtime);
        static const bool gameDefault = std::getenv("TF_INVERT_DEFAULT") && *std::getenv("TF_INVERT_DEFAULT") == '1';
        if (!gameDefault && ctx->pc == getRegU32(ctx, 31))
            if (uint8_t *p = getMemPtr(rdram, options + 1u))
            {
                std::fprintf(stderr, "[tf] the options' defaults (%08x): invert Y off (the game's default %u)\n", kOptionsDefaults[I], *p);
                *p = 0u;
            }
    }
    // (TODO 178, the user's choice) The Invert Y option's meaning flipped for the in-game cameras: NO = stick up looks up
    // (the modern way), YES = the original's (stick up looks down). The game's cameras read the option through 0x278080
    // (options byte by index; index 1 = invert: the third-person turn 0x46D410, gliding, the car, the turret and aiming
    // cameras) - the answer for index 1 is flipped there; the menus show and change the stored byte, so the label stays
    // the game's and NO stays the default. TF_INVERT_FLIP=0: the original meaning.
    constexpr uint32_t kOptionByIndex = 0x00278080u;
    PS2Runtime::RecompiledFunction g_optionByIndex = nullptr;
    bool invertFlipped()
    {
        static const bool off = std::getenv("TF_INVERT_FLIP") && *std::getenv("TF_INVERT_FLIP") == '0';
        return !off;
    }
    // (TODO 153) The robot's slowing where he wades (taTransformerActor's two handlers, 0x28D290 and 0x28D7D0, under
    // their flags at the context's +0x30E / +0x30D): every frame the context's velocity times 0.99 (gp-0x65E0), given to
    // SetVelocity (0x47C830) from 0x28D720 / 0x28DCA0. A frame's factor, not scaled for the rate: the ground speed step
    // (0x476320, dt-scaled) adds its acceleration and this takes 1% back each frame, so the speed settles where the two
    // meet - lower the more frames a second (under water about 26 at the console's 30, 12.5 at 60, 3 at 240 - stuck at the
    // walk's least speed, never a run). At the level's rate the factor is 0.99^(30/R), as the console's frame would have it.
    // TF_WADE_RATE=0: the game's 0.99 a frame.
    constexpr uint32_t kSetVelocity = 0x0047C830u, kWadeSlowA = 0x0028D728u, kWadeSlowB = 0x0028DCA8u;
    // (rate sweep, 2026-11-08) taTransformerStateLook (0x474000 -> 0x473D90): while its timer +0x8C runs (counted down in
    // seconds) the controller's velocity times 0.8 (gp-0x75E0) is given to SetVelocity each frame (0x473E30, return
    // 0x473E38): the same per-frame factor - at 240 the move died away in an eighth of the time. 0.8^(30/R) at R.
    // TF_LOOK_RATE=0: the game's 0.8 a frame.
    constexpr uint32_t kLookSlow = 0x00473E38u;
    PS2Runtime::RecompiledFunction g_setVelocity = nullptr;
    void setVelocityAtRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (ra == kLookSlow && g_levelRate != 0)
        {
            static const bool off = std::getenv("TF_LOOK_RATE") && *std::getenv("TF_LOOK_RATE") == '0';
            if (!off)
            {
                const float scale = std::pow(0.8f, 30.0f / static_cast<float>(g_levelRate)) / 0.8f;
                if (uint8_t *v = getMemPtr(rdram, getRegU32(ctx, 5)))
                    for (int i = 0; i < 3; ++i)
                    {
                        float c = 0.0f;
                        std::memcpy(&c, v + 4 * i, 4);
                        c *= scale;
                        std::memcpy(v + 4 * i, &c, 4);
                    }
            }
        }
        if ((ra == kWadeSlowA || ra == kWadeSlowB) && g_levelRate != 0)
        {
            static const bool off = std::getenv("TF_WADE_RATE") && *std::getenv("TF_WADE_RATE") == '0';
            if (!off)
            {
                // (the game's v x 0.99 made v x 0.99^(30/R))
                const float scale = std::pow(0.99f, 30.0f / static_cast<float>(g_levelRate)) / 0.99f;
                if (uint8_t *v = getMemPtr(rdram, getRegU32(ctx, 5)))
                    for (int i = 0; i < 3; ++i)
                    {
                        float c = 0.0f;
                        std::memcpy(&c, v + 4 * i, 4);
                        c *= scale;
                        std::memcpy(v + 4 * i, &c, 4);
                    }
            }
        }
        g_setVelocity(rdram, ctx, runtime);
    }

    // (rate sweep, 2026-11-08) Lerps and slerps by a share a frame that comes from the object, not from a constant the
    // instruction patches could reach (docs/FRAME_RATE_SITES.md, "Recursive lerps whose share changes over time"): the
    // vector lerp 0x1B0440 (out, from, to, t f12) and the quaternion slerp 0x1AF4C0 (t f12), called once a frame from
    //  - the third-person camera (taThirdPersonCameraScriptThree, 0x2D0B50): +0x50 toward its target by +0xC4 (ramped by
    //    dt up to 1) - return 0x2D1124;
    //  - the glide camera (0x2CE0A0): +0xA0 toward its target by +0xB0 (rising 0.5 a second to 1) - return 0x2CE25C;
    //  - the flight (0x47FF90): the state's +0xE0 toward the wanted orientation by +0xDC (data) - return 0x480BC4.
    // At R the share given is 1 - (1 - t)^(30/R): as far in a 30th of a second as at 30. TF_LERP_RATE=0: as before.
    float shareAtLevelRate(float atThirty); // (below)
    constexpr uint32_t kLerp = 0x001B0440u, kSlerp = 0x001AF4C0u;
    constexpr uint32_t kLerpAtRate[] = {0x002D1124u, 0x002CE25Cu, 0x00480BC4u};
    PS2Runtime::RecompiledFunction g_lerp = nullptr, g_slerp = nullptr;
    void lerpShareAtRate(R5900Context *ctx)
    {
        static const bool off = std::getenv("TF_LERP_RATE") && *std::getenv("TF_LERP_RATE") == '0';
        if (off || g_levelRate == 0)
            return;
        const uint32_t ra = getRegU32(ctx, 31);
        for (uint32_t at : kLerpAtRate)
            if (ra == at)
            {
                ctx->f[12] = shareAtLevelRate(ctx->f[12]);
                return;
            }
    }
    void lerpAtRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kLerp)
            lerpShareAtRate(ctx);
        g_lerp(rdram, ctx, runtime);
    }
    void slerpAtRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kSlerp)
            lerpShareAtRate(ctx);
        g_slerp(rdram, ctx, runtime);
    }

    void optionByIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t index = getRegU32(ctx, 4);
        g_optionByIndex(rdram, ctx, runtime);
        if (index == 1u && invertFlipped() && ctx->pc == getRegU32(ctx, 31))
            SET_GPR_U32(ctx, 2, getRegU32(ctx, 2) == 0u ? 1u : 0u);
    }
    // The mouse turns as the right stick does with the option (the direct third-person turn below applies it itself,
    // the other cameras get the mouse as their stick, which the game inverts by the option): the runtime's own mouse
    // inversion stays off. g_mouseInvertY: the third-person turn's (the camera's effective invert).
    bool g_mouseInvertY = false;
    void mouseFollowsInvertOption(const uint8_t *rdram)
    {
        uint32_t options = 0u;
        std::memcpy(&options, rdram + kOptionsPointer, sizeof(options));
        if (options == 0u || (options & PS2_RAM_MASK) + 1u >= PS2_RAM_SIZE)
            return;
        static bool once = (ps2_host_input::setMouseInvertY(false), true);
        (void)once;
        const bool stored = rdram[(options & PS2_RAM_MASK) + 1u] != 0u;
        // (TODO 187) The mouse in the direct third-person turn follows the stored option itself: NO = moved up looks up, as
        // before 178 (the flip is the stick's - the game's own turn reads it; following the flipped answer turned the mouse
        // upside down with NO).
        g_mouseInvertY = stored;
    }

    void copyOptions(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_copyOptions(rdram, ctx, runtime);
        forceWidescreenOption(rdram);
    }

    void applyProfileOptions(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_applyProfileOptions(rdram, ctx, runtime);
        forceWidescreenOption(rdram);
    }

    void applyVBlankPeriod(PS2Runtime *runtime); // (the frame rate, below: the console's while a movie plays)
    void createFmvStream(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // (a0: the movie's file name, for the HD movies: hd_movies.h)
        // (a0 is a std::string, CodeWarrior's: byte 0's low bit set = the long form, its characters at
        // the pointer at +8; else inline from +1 - "FMV/NATARI.PSS")
        std::string movie;
        if (const uint8_t *s = getConstMemPtr(rdram, getRegU32(ctx, 4)))
        {
            uint32_t chars = getRegU32(ctx, 4) + 1u;
            if ((s[0] & 1u) != 0u)
                std::memcpy(&chars, s + 8, sizeof(chars));
            if (const uint8_t *p = getConstMemPtr(rdram, chars))
                for (int i = 0; i < 160 && p[i] >= 0x20 && p[i] < 0x7F; ++i)
                    movie += static_cast<char>(p[i]);
        }
        if (std::getenv("TF_FMV_ARGS"))
        {
            const auto text = [rdram](uint32_t a)
            {
                std::string t;
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    for (int i = 0; i < 64 && p[i] >= 0x20 && p[i] < 0x7F; ++i)
                        t += static_cast<char>(p[i]);
                return t;
            };
            const auto deref = [rdram](uint32_t a)
            {
                uint32_t v = 0u;
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
                return v;
            };
            for (int r = 4; r <= 7; ++r)
            {
                const uint32_t v = getRegU32(ctx, r);
                std::fprintf(stderr, "[fmv] a%d=%08x '%s' *=%08x '%s' *8='%s'\n", r - 4, v, text(v).c_str(), deref(v),
                             text(deref(v)).c_str(), text(deref(v + 8)).c_str());
            }
        }
        if (const uint32_t path = ext::moviePath(rdram, movie)) // (an extension's movie in its place: port_ext.h)
            SET_GPR_U32(ctx, 4, path);
        g_createFmvStream(rdram, ctx, runtime);
        // The level previews (FMV/LEVELS/N01AMA and the others: the HQ's Drop Zones) play inside
        // the 3D scene, on a screen of the HQ, not as a movie of the window's own. They keep the
        // scene's picture shape and timing, and get no HD overlay and no FMV skip.
        if (getRegU32(ctx, 2) != 0u && hd_movies::movieKey(movie).rfind("LEVELS/", 0) == 0)
        {
            g_inSceneStreams.push_back(getRegU32(ctx, 2));
            g_adoptSceneStream = false; // (a preview begun in this session: nothing left over from a loaded state)
            // (TODO 159) Its upscaled copy, if there is one (movies_hd/LEVELS/<name>.mkv, tools/upscale_previews.py), drawn
            // by the native renderer in place of its pictures. TF_HD_PREVIEWS=0: the game's pictures.
            static const bool hdPreviews = !(std::getenv("TF_HD_PREVIEWS") && *std::getenv("TF_HD_PREVIEWS") == '0');
            const std::string key = hd_movies::movieKey(movie);
            nr::movie::previewStarted(key, hdPreviews ? hd_movies::fileFor(key) : std::string());
            return;
        }
        if (getRegU32(ctx, 2) != 0u)
        {
            if (!movie.empty())
                hd_movies::movieStarted(*runtime, movie);
            g_skipIntroMovie = game_config::skipIntroMovie(hd_movies::movieKey(movie)); // (the "Intro screens" switch Off)
            {
                const std::string key = hd_movies::movieKey(movie);
                g_liveFmvIsFilm = key != "NATARI" && key != "NMH_LOGO" && key != "NATTRACT";
            }
            ++g_liveFmvStreams;
            g_fmvFrames = 0;
            // (TODO 255) The movies are 640x384 pictures that the game draws letterboxed in its 640x448 frame (rows 32 to
            // 416), and the films and logos have 12-row black bars of their own on top: their picture is 640x360, about
            // 1.66:1 on the console's 4:3 TV. Shown as the whole 4:3 frame, a 16:9 screen got a small picture with black
            // all round. Now only that band of rows is shown, as large as the window allows at its own shape (the PS2's
            // 1.66:1, full height on 16:9 and 21:9). The Atari logo has no bars: its band is the 384 rows. With an HD copy
            // (movies_hd) its own overlay draws instead. TF_MOVIE_BAND=0: the whole 4:3 frame, as before.
            static const bool band = !(std::getenv("TF_MOVIE_BAND") && *std::getenv("TF_MOVIE_BAND") == '0');
            if (band)
            {
                const bool bars = hd_movies::movieKey(movie) != "NATARI";
                ps2_host_display::setPictureBand((bars ? 44.0f : 32.0f) / 448.0f, (bars ? 404.0f : 416.0f) / 448.0f);
            }
            updateDisplayAspect(rdram);
            applyVBlankPeriod(runtime);
        }
    }

    void destroyFmvStream(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool object = getRegU32(ctx, 4) != 0u;
        const auto inScene = std::find(g_inSceneStreams.begin(), g_inSceneStreams.end(), getRegU32(ctx, 4));
        if (object && inScene != g_inSceneStreams.end())
        {
            g_inSceneStreams.erase(inScene);
            nr::movie::previewEnded(); // (TODO 159)
            const uint32_t self = getRegU32(ctx, 4);
            g_sceneStreamPace.erase(std::remove_if(g_sceneStreamPace.begin(), g_sceneStreamPace.end(),
                                                   [self](const SceneStreamPace &p) { return p.stream == self; }),
                                    g_sceneStreamPace.end());
            g_fmvStreamDtor(rdram, ctx, runtime);
            return;
        }
        g_fmvStreamDtor(rdram, ctx, runtime);
        if (object && g_liveFmvStreams > 0 && --g_liveFmvStreams == 0)
        {
            hd_movies::movieEnded();
            ps2_host_display::setPictureBand(0.0f, 1.0f); // (TODO 255: the whole picture again)
            ps2_stubs::clearPadOverrideState();
            updateDisplayAspect(rdram);
            applyVBlankPeriod(runtime);
        }
    }

    // (TODO 113) The level previews' player at the movie's own pace, not the frame rate's. The player's update
    // (0x275A00, once a frame from the Drop Zones' screen) asks sceMpegGetPicture for the next picture, which waits
    // until that picture's moment, counted in VBlanks - two a picture, as at the console's 60 VBlanks a second. The
    // HQ at the PC rate R has R VBlanks a second: every frame then waited for the next picture (the game's frames
    // held to two VBlanks each - 20 a second at 40, 60 at 120), the adaptive ladder took the waits for a host that
    // cannot keep up and stepped the HQ down to 40, and the preview played at R/60 of its speed. On the console a
    // frame is two VBlanks, so the player is asked once per picture and never waits: here the update is let through
    // once per picture's worth of VBlanks (R/29.97, at least 2) and left out in the frames in between - the player
    // and its stream run at the console's 30 calls a second, the HQ at the frame rate. TF_HQ_MOVIE_PACE=0: as before.
    constexpr uint32_t kFmvStreamUpdate = 0x00275A00u; // fmvStreamPS2's update (this)
    PS2Runtime::RecompiledFunction g_fmvStreamUpdate = nullptr;
    uint64_t g_sceneStreamSkips = 0u;

    void fmvStreamUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = getRegU32(ctx, 4);
        EeScheduler &ee = runtime->eeScheduler();
        const double vblanksPerSecond = 1e9 / static_cast<double>(std::max<int64_t>(1, ee.vblankPeriod().count()));
        // (only when entered afresh: after a yield inside it the call goes on at another pc; and only while the HQ runs at
        // a PC rate - one VBlank a frame. At the console's 30 (two VBlanks a frame) every frame is a picture already. TODO
        // 266: this was told by the VBlank rate, not 60 a second, and an HQ at 60 - a 60 Hz screen caps it there - has 60
        // VBlanks a second too: its player then went unpaced, every frame waited for the next picture, the HQ fell to 30
        // and, at 30 VBlanks a second, to 15 frames.)
        if (g_adoptSceneStream && ctx->pc == kFmvStreamUpdate && self != 0u && g_executorVtable == 0x006B7360u && g_liveFmvStreams == 0 &&
            std::find(g_inSceneStreams.begin(), g_inSceneStreams.end(), self) == g_inSceneStreams.end())
        {
            g_inSceneStreams.push_back(self);
            std::fprintf(stderr, "[tf] the HQ's preview movie of the loaded state: its player %08x paced again\n", self);
        }
        if (ctx->pc == kFmvStreamUpdate && g_levelRate != 0 &&
            std::find(g_inSceneStreams.begin(), g_inSceneStreams.end(), self) != g_inSceneStreams.end())
        {
            const double tick = static_cast<double>(ee.currentVSyncTick());
            const double perPicture = std::max(2.0, vblanksPerSecond * 1001.0 / 30000.0); // (the player's least: 2)
            auto it = std::find_if(g_sceneStreamPace.begin(), g_sceneStreamPace.end(), [self](const SceneStreamPace &p)
                                   { return p.stream == self; });
            // (TODO 266) An update let through that found its picture not yet due waited for it inside the call: that
            // frame took a VBlank or more beyond its own (a frame is one VBlank here, two with in-between pictures). The
            // grid then lags the movie's pictures by that much - each later update asked a VBlank early and waited again,
            // every frame two VBlanks: the HQ at 30 on a 60 Hz screen, for as long as no hitch moved it. The grid goes to
            // the picture it waited for (a VBlank before now) instead.
            if (it != g_sceneStreamPace.end())
            {
                const double frameVBlanks = std::max(1.0, std::round(vblanksPerSecond / static_cast<double>(g_levelRate)));
                if (it->lastLetThrough && it->lastTick >= 0.0 && tick - it->lastTick > frameVBlanks + 0.5)
                    it->nextTick = std::max(it->nextTick, tick - 1.0 + perPicture);
                it->lastTick = tick;
                it->lastLetThrough = false;
            }
            if (it == g_sceneStreamPace.end())
            {
                g_sceneStreamPace.push_back({self, tick + perPicture, tick, true});
                std::fprintf(stderr, "[tf] level preview movie at its own pace: its player asked once per %.2f VBlanks (TODO 113)\n",
                             perPicture);
            }
            else if (tick + 0.5 < it->nextTick) // (the nearest VBlank to the picture's moment lets it through)
            {
                ++g_sceneStreamSkips;
                ctx->pc = getRegU32(ctx, 31); // (no picture due: as the console's frame between two)
                return;
            }
            else
            {
                // (on the picture grid while it keeps up; from now after a gap - a rate change, a slow frame)
                it->nextTick = tick - it->nextTick < perPicture ? it->nextTick + perPicture : tick + perPicture;
                it->lastLetThrough = true;
            }
            // TF_HQ_MOVIE_TRACE=1: every 2 s, the player's updates let through (its pictures: ~60 at the movie's 30 a
            // second) and left out. (Counted here: a guest call's host function need not return when the call does.)
            static const bool trace = std::getenv("TF_HQ_MOVIE_TRACE") && *std::getenv("TF_HQ_MOVIE_TRACE") == '1';
            if (trace)
            {
                static uint32_t passed = 0u;
                static auto since = std::chrono::steady_clock::now();
                ++passed;
                if (std::chrono::steady_clock::now() - since >= std::chrono::seconds(2))
                {
                    std::fprintf(stderr, "[tf] preview player: %u updates let through, %llu left out (2 s, %.0f VBlanks a second)\n", passed,
                                 static_cast<unsigned long long>(g_sceneStreamSkips), vblanksPerSecond);
                    passed = 0u;
                    g_sceneStreamSkips = 0u;
                    since = std::chrono::steady_clock::now();
                }
            }
        }
        g_fmvStreamUpdate(rdram, ctx, runtime);
    }

    // Buttons (active high) the FMV skip wants pressed this frame.
    uint16_t fmvSkipButtons()
    {
        if (!(g_skipFmv || g_skipIntroMovie) || g_liveFmvStreams == 0)
            return 0u;
        ++g_fmvFrames;
        const bool press = g_fmvFrames >= 15u && (g_fmvFrames % 30u) < 3u;
        return press ? static_cast<uint16_t>(kPadStart | kPadCross) : 0u;
    }

    // The testing aids below count time in the console's VBlanks, 60 a second of game time, at
    // every frame rate: in a level at the PC rate R a VBlank counts 60/R of one (the VBlank
    // period now over the console's). A state loaded starts the count at its VBlank count.
    double g_testTick = 0.0;
    uint64_t g_testTickVBlank = 0u;
    uint64_t g_testTickStates = 0u;

    uint64_t testTick(PS2Runtime *runtime)
    {
        EeScheduler &ee = runtime->eeScheduler();
        const uint64_t vblank = ee.currentVSyncTick();
        if (ee.loadedStateCount() != g_testTickStates || vblank < g_testTickVBlank)
        {
            g_testTickStates = ee.loadedStateCount();
            g_testTick = static_cast<double>(vblank);
        }
        else
        {
            g_testTick += static_cast<double>(vblank - g_testTickVBlank) *
                          (static_cast<double>(ee.vblankPeriod().count()) /
                           static_cast<double>(EeScheduler::kConsoleVBlankPeriod.count()));
        }
        g_testTickVBlank = vblank;
        return static_cast<uint64_t>(g_testTick + 1e-6);
    }

    // Scripted input (testing aid): TF_INPUT="2520:start,2640:cross+up:12" presses those
    // buttons from the first frame at or after the given tick (testTick: 60 a second of game
    // time) for 6 ticks (or the given count) and at least 2 frames. Timed by game time, so
    // runs are repeatable however slowly the host renders, and at any frame rate.
    // TF_QUIT_VSYNC=<tick> exits there.
    struct ScriptedPress
    {
        uint64_t tick = 0u;
        uint64_t hold = 6u;
        uint16_t buttons = 0u;
        uint8_t sticks = 0u;          // left stick up/right/down/left, then the right stick's
        uint8_t stickPercent = 100u;  // how far they are pushed
        uint64_t startTick = 0u;
        uint32_t frames = 0u;
        bool done = false;
        uint64_t afterLevel = 0u; // "+N:...": N ticks after the level's start (updatePadOverride); tick waits till then
    };
    std::vector<ScriptedPress> g_inputScript;
    // Ticks given as "+N" (TF_INPUT, TF_GOTO): N after the moment the player first stands in a level (its position
    // known) - the same moments of the level however long the menus and the load took on the machine (TODO 106's
    // scripted sound runs). Until then they wait (kWaitingForLevel).
    constexpr uint64_t kWaitingForLevel = ~0ull >> 1;
    bool g_levelTicks = false;      // any "+N" given
    uint64_t g_levelStartTick = 0u; // when the player was first seen in a level (0: not yet)
    uint64_t g_quitTick = 0u;
    bool g_padOverridden = false;
    void writeCoverage(); // TF_COVERAGE, below
    void writeCallMap();  // TF_CALLMAP, below

    uint16_t padButton(const std::string &name)
    {
        static const std::pair<const char *, uint16_t> kButtons[] = {
            {"select", 1u << 0}, {"l3", 1u << 1}, {"r3", 1u << 2}, {"start", 1u << 3},
            {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
            {"l2", 1u << 8}, {"r2", 1u << 9}, {"l1", 1u << 10}, {"r1", 1u << 11},
            {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};
        for (const auto &[button, mask] : kButtons)
            if (name == button)
                return mask;
        return 0u;
    }

    // "lup", "lright", "ldown", "lleft" push the left stick fully that way, "rup" ... the right;
    // "rright@30" pushes it 30% of the way.
    uint8_t padStick(const std::string &name)
    {
        static const std::pair<const char *, uint8_t> kSticks[] = {
            {"lup", 1u << 0}, {"lright", 1u << 1}, {"ldown", 1u << 2}, {"lleft", 1u << 3},
            {"rup", 1u << 4}, {"rright", 1u << 5}, {"rdown", 1u << 6}, {"rleft", 1u << 7}};
        for (const auto &[stick, mask] : kSticks)
            if (name == stick)
                return mask;
        return 0u;
    }

    void parseInputScript()
    {
        if (const char *quit = std::getenv("TF_QUIT_VSYNC"))
            g_quitTick = std::strtoull(quit, nullptr, 10);
        const char *env = std::getenv("TF_INPUT");
        if (!env)
            return;
        std::string script(env);
        size_t pos = 0;
        while (pos < script.size())
        {
            size_t end = script.find(',', pos);
            if (end == std::string::npos)
                end = script.size();
            const std::string item = script.substr(pos, end - pos);
            pos = end + 1;
            const size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            ScriptedPress press;
            press.tick = std::strtoull(item.c_str() + (item[0] == '+' ? 1 : 0), nullptr, 10);
            if (item[0] == '+')
            {
                press.afterLevel = press.tick;
                press.tick = kWaitingForLevel;
                g_levelTicks = true;
            }
            std::string buttons = item.substr(colon + 1);
            const size_t holdColon = buttons.find(':');
            if (holdColon != std::string::npos)
            {
                press.hold = std::strtoull(buttons.c_str() + holdColon + 1, nullptr, 10);
                buttons.resize(holdColon);
            }
            for (size_t start = 0; start <= buttons.size();)
            {
                size_t plus = buttons.find('+', start);
                if (plus == std::string::npos)
                    plus = buttons.size();
                std::string name = buttons.substr(start, plus - start);
                if (const size_t at = name.find('@'); at != std::string::npos)
                {
                    press.stickPercent = static_cast<uint8_t>(std::clamp(std::atoi(name.c_str() + at + 1), 0, 100));
                    name.resize(at);
                }
                const uint16_t button = padButton(name);
                const uint8_t stick = padStick(name);
                if (button == 0u && stick == 0u)
                    std::fprintf(stderr, "[tf] TF_INPUT: unknown button '%s'\n", name.c_str());
                press.buttons |= button;
                press.sticks |= stick;
                start = plus + 1;
            }
            g_inputScript.push_back(press);
        }
    }

    uint16_t scriptedButtons(uint64_t tick, uint8_t &sticks, uint8_t &stickPercent)
    {
        uint16_t buttons = 0u;
        sticks = 0u;
        stickPercent = 100u;
        for (ScriptedPress &press : g_inputScript)
        {
            if (press.done || tick < press.tick)
                continue;
            if (press.frames == 0u)
                press.startTick = tick;
            if (press.frames >= 2u && tick >= press.startTick + press.hold)
            {
                press.done = true;
                continue;
            }
            ++press.frames;
            buttons |= press.buttons;
            sticks |= press.sticks;
            if (press.sticks != 0u)
                stickPercent = press.stickPercent;
        }
        return buttons;
    }

    // Where the player and the view are, for the testing aids below. Every frame the routine
    // at 0x42E390 (it updates the sound's listener) asks for the current camera (0x20CB30)
    // and copies the fourth row of its transform (0x1AFD40, called from 0x42E3E8) to
    // 0x731ED0 - which is therefore the camera's position, some 18 units behind the player,
    // not the player's. That call is where the camera object ($v0) and its transform ($a1)
    // are picked up: the transform's first row is where the view looks, which is also
    // "forward" for the left stick; the camera's target (a pointer in the camera object) is
    // the player, which keeps its position - x, y, z with z up, about 4 units above its
    // feet - in itself.
    constexpr uint32_t kGetPosition = 0x001AFD40u;
    constexpr uint32_t kListenerPositionCaller = 0x0042E3F0u; // the return address of that call
    constexpr uint32_t kCameraTarget = 0x74u;                 // in the camera object
    constexpr uint32_t kPlayerPositionField = 0x620u;         // in the player
    PS2Runtime::RecompiledFunction g_getPosition = nullptr;
    uint32_t g_camera = 0u, g_cameraTransform = 0u;

    void getPosition(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == kListenerPositionCaller)
        {
            g_camera = getRegU32(ctx, 2);
            g_cameraTransform = getRegU32(ctx, 5);
            view_turn::noteCameraObject(g_camera, g_cameraTransform); // (N9: the camera's turn for every picture)
            camera_mode::noteCameraTransform(g_cameraTransform); // (TODO 190: the first person's free look)
        }
        g_getPosition(rdram, ctx, runtime);
    }

    // The address of the player's position, or 0 while no level is running.
    uint32_t playerPositionAddress(const uint8_t *rdram)
    {
        uint32_t player = 0u;
        if (const uint8_t *p = g_camera ? getConstMemPtr(rdram, g_camera + kCameraTarget) : nullptr)
            std::memcpy(&player, p, sizeof(player));
        return (player >= 0x00100000u && player < PS2_RAM_SIZE - 0x1000u) ? player + kPlayerPositionField : 0u;
    }

    // Walking to a place (testing aid): TF_GOTO="10330:760,-1667;11200:682,-1789,3j" walks
    // the player, from each VBlank tick on, to that x,y of the level until it is within the
    // radius (4 units unless given) or has passed it closely; a "j" after the radius makes
    // it jump when it sets off for that place (a ledge). The left stick is pushed straight
    // at the place, as seen from the way the view looks, while the right stick turns the
    // view that way too. When pushing brings the place no nearer for two thirds of a
    // second, it jumps (twice out of three times) or steps aside with a jump, and tries
    // again. Positions and arrivals are logged ("[tf:goto]"); tools/level_ground.py finds
    // the places to string together (and tools/level_heightmap.py shows the ground).
    struct WalkTarget
    {
        uint64_t tick = 0u;
        float x = 0.0f, y = 0.0f, radius = 4.0f;
        bool jump = false;
        bool done = false;
        bool started = false;
        uint64_t afterLevel = 0u; // "+N:..." (see g_levelTicks)
    };
    std::vector<WalkTarget> g_walkTargets;

    void parseWalkTargets()
    {
        const char *env = std::getenv("TF_GOTO");
        if (!env)
            return;
        for (const char *p = env; *p;)
        {
            WalkTarget target;
            char *end = nullptr;
            const bool afterLevel = *p == '+';
            target.tick = std::strtoull(afterLevel ? p + 1 : p, &end, 10);
            if (afterLevel)
            {
                target.afterLevel = target.tick;
                target.tick = kWaitingForLevel;
                g_levelTicks = true;
            }
            if (end == p || *end != ':')
                break;
            target.x = std::strtof(end + 1, &end);
            if (*end != ',')
                break;
            target.y = std::strtof(end + 1, &end);
            if (*end == ',')
                target.radius = std::strtof(end + 1, &end);
            if (*end == 'j')
            {
                target.jump = true; // "760,-1667,2j": a ledge - jump when setting off for it
                ++end;
            }
            g_walkTargets.push_back(target);
            p = (*end == ';') ? end + 1 : end;
            if (*end != ';')
                break;
        }
    }

    // Placing the player (testing aid): TF_WARP="12730:760,-1667,50" writes that position
    // (x, y and the height of the ground plus 4) into the player at the first frame at or
    // after the tick.
    struct Warp
    {
        uint64_t tick = 0u;
        float position[3] = {};
        bool done = false;
    };
    std::vector<Warp> g_warps;

    void parseWarps()
    {
        const char *env = std::getenv("TF_WARP");
        if (!env)
            return;
        for (const char *p = env; *p;)
        {
            Warp warp;
            char *end = nullptr;
            warp.tick = std::strtoull(p, &end, 10);
            if (end == p || *end != ':')
                break;
            bool complete = true;
            for (int i = 0; i < 3; ++i)
            {
                warp.position[i] = std::strtof(end + 1, &end);
                if (i < 2 && *end != ',')
                    complete = false;
            }
            if (!complete)
                break;
            g_warps.push_back(warp);
            if (*end != ';')
                break;
            p = end + 1;
        }
    }

    void applyWarps(uint8_t *rdram, uint64_t tick)
    {
        for (Warp &warp : g_warps)
        {
            if (warp.done || tick < warp.tick)
                continue;
            const uint32_t address = playerPositionAddress(rdram);
            uint8_t *place = address ? getMemPtr(rdram, address) : nullptr;
            if (!place)
                continue; // the level is not running yet: try again next frame
            std::memcpy(place, warp.position, sizeof(warp.position));
            warp.done = true;
            std::fprintf(stderr, "[tf:warp] tick %llu: player placed at (%.1f, %.1f, %.1f) (%08x)\n",
                         static_cast<unsigned long long>(tick), warp.position[0], warp.position[1], warp.position[2], address);
        }
    }

    // TF_GOD=1 (testing aid): the player's energy is filled up again every frame, so a
    // scripted walk gets past enemies it does not fight. The energy is a small record in
    // the game's static data (0x34F2D0 returns its owner, 0x350A70 the record): the current
    // value, the fraction that is left, and the full value. (The game has a setting of its
    // own for this, "godModePlayer", but reads it only when the player is created.)
    constexpr uint32_t kPlayerEnergy = 0x00716FB4u;     // float: what is left
    constexpr uint32_t kPlayerEnergyLeft = 0x00716FB8u; // float: the same, as a fraction
    constexpr uint32_t kPlayerEnergyFull = 0x00716FBCu; // float
    bool g_godMode = false;

    void keepEnergyFull(uint8_t *rdram)
    {
        uint8_t *energy = getMemPtr(rdram, kPlayerEnergy);
        uint8_t *left = getMemPtr(rdram, kPlayerEnergyLeft);
        const uint8_t *full = getConstMemPtr(rdram, kPlayerEnergyFull);
        if (!energy || !left || !full)
            return;
        float most = 0.0f;
        std::memcpy(&most, full, sizeof(most));
        if (most <= 0.0f)
            return; // no level running
        const float all = 1.0f;
        std::memcpy(energy, &most, sizeof(most));
        std::memcpy(left, &all, sizeof(all));
    }

    // TF_POKE="12730:df18e0=f762.5,df18e4=f-1668,6cea04=1f" (debugging aid): writes those
    // words of EE memory at the first frame at or after the tick; "f" before the value
    // writes a float, otherwise the value is hex. For finding out which copy of a value the
    // game works from.
    struct Poke
    {
        uint64_t tick = 0u;
        uint32_t address = 0u;
        uint32_t value = 0u;
        bool done = false;
    };
    std::vector<Poke> g_pokes;

    void parsePokes()
    {
        const char *env = std::getenv("TF_POKE");
        if (!env)
            return;
        uint64_t tick = 0u;
        for (const char *p = env; *p;)
        {
            char *end = nullptr;
            const uint64_t first = std::strtoull(p, &end, 16);
            if (end == p)
                break;
            if (*end == ':')
            {
                tick = std::strtoull(p, nullptr, 10);
                p = end + 1;
                continue;
            }
            if (*end != '=')
                break;
            Poke poke;
            poke.tick = tick;
            poke.address = static_cast<uint32_t>(first);
            if (end[1] == 'f')
            {
                const float value = std::strtof(end + 2, &end);
                std::memcpy(&poke.value, &value, sizeof(value));
            }
            else
            {
                poke.value = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            }
            g_pokes.push_back(poke);
            p = (*end == ',' || *end == ';') ? end + 1 : end;
            if (*end != ',' && *end != ';')
                break;
        }
    }

    void applyPokes(uint8_t *rdram, uint64_t tick)
    {
        for (Poke &poke : g_pokes)
        {
            if (poke.done || tick < poke.tick)
                continue;
            poke.done = true;
            if (uint8_t *p = getMemPtr(rdram, poke.address))
            {
                std::memcpy(p, &poke.value, sizeof(poke.value));
                std::fprintf(stderr, "[tf:poke] tick %llu: %08x = %08x\n", static_cast<unsigned long long>(tick), poke.address,
                             poke.value);
            }
        }
    }

    struct WalkSticks
    {
        bool active = false;
        uint8_t lx = 0x80u, ly = 0x80u, rx = 0x80u;
        uint16_t buttons = 0u;
    };

    WalkSticks walkToTarget(const uint8_t *rdram, uint64_t tick)
    {
        constexpr float kPi = 3.14159265f;
        const float turnPerFrame = kYawPerSecond / framesPerSecond(); // the view's top turning speed
        static int sidestepFrames = 0;
        static bool sidestepRight = true;
        static int jumpFrames = 0;     // jumping, onto what is in the way
        static uint32_t stuckCount = 0;
        static int pushFrames = 0;     // frames in a row of plain walking
        static float trail[20] = {};   // how far it was to go over the last 20 frames of walking
        static uint32_t trailAt = 0;
        static float fromX = 0.0f, fromY = 0.0f; // where the walk to this place began

        WalkSticks out;
        WalkTarget *target = nullptr;
        for (WalkTarget &candidate : g_walkTargets)
        {
            if (!candidate.done && tick >= candidate.tick)
            {
                target = &candidate;
                break;
            }
            if (!candidate.done)
                break; // in order: a later one waits for this one's tick
        }
        float rows[16] = {}; // the camera's transform: forward, left, up, position
        float position[3] = {};
        const uint8_t *transform = g_cameraTransform ? getConstMemPtr(rdram, g_cameraTransform) : nullptr;
        const uint32_t positionAddress = playerPositionAddress(rdram);
        const uint8_t *place = positionAddress ? getConstMemPtr(rdram, positionAddress) : nullptr;
        if (!target || !transform || !place)
        {
            sidestepFrames = jumpFrames = pushFrames = 0;
            return out;
        }
        std::memcpy(rows, transform, sizeof(rows));
        std::memcpy(position, place, sizeof(position));
        const float toX = target->x - position[0], toY = target->y - position[1];
        const float distance = std::hypot(toX, toY);
        if (!target->started)
        {
            target->started = true;
            fromX = position[0];
            fromY = position[1];
            std::fprintf(stderr, "[tf:goto] tick %llu: from (%.1f, %.1f, %.1f) to (%.1f, %.1f), %.1f away%s\n",
                         static_cast<unsigned long long>(tick), position[0], position[1], position[2], target->x, target->y,
                         distance, target->jump ? ", with a jump" : "");
            sidestepFrames = pushFrames = 0;
            jumpFrames = target->jump ? 24 : 0;
        }
        // There: inside the radius, or past the place and no further than three radii off
        // to the side (a walk seldom runs exactly over a point).
        const float legX = target->x - fromX, legY = target->y - fromY;
        const bool passed = toX * legX + toY * legY < 0.0f && distance <= 3.0f * target->radius;
        if (distance <= target->radius || passed)
        {
            target->done = true;
            std::fprintf(stderr, "[tf:goto] tick %llu: %s (%.1f, %.1f), at (%.1f, %.1f, %.1f)\n",
                         static_cast<unsigned long long>(tick), passed ? "passed" : "reached", target->x, target->y,
                         position[0], position[1], position[2]);
            sidestepFrames = jumpFrames = pushFrames = 0;
            return out;
        }

        // The stick that moves straight at the place: forward is the first row of the
        // camera's transform, left its second.
        const float forwardLength = std::hypot(rows[0], rows[1]);
        if (forwardLength < 0.5f || distance < 1e-3f)
            return out;
        const float aheadX = rows[0] / forwardLength, aheadY = rows[1] / forwardLength;
        const float dirX = toX / distance, dirY = toY / distance;
        float stickUp = dirX * aheadX + dirY * aheadY;
        float stickRight = -(dirX * -aheadY + dirY * aheadX);
        if (sidestepFrames > 0)
        {
            // Around whatever is in the way: across the line to the place.
            const float side = sidestepRight ? 1.0f : -1.0f;
            const float up = stickUp, right = stickRight;
            stickUp = 0.3f * up - side * right; // a quarter turn of the stick, with a little of the way on
            stickRight = 0.3f * right + side * up;
        }
        const float reach = std::max(std::fabs(stickUp), std::fabs(stickRight));
        const float push = (distance < 8.0f && sidestepFrames == 0 && jumpFrames == 0) ? 0.6f : 1.0f; // slower on the last stretch
        const auto axis = [reach, push](float value)
        { return static_cast<uint8_t>(std::clamp(128L + std::lround(value / reach * push * 127.0f), 0L, 255L)); };
        out.active = true;
        out.lx = axis(stickRight);
        out.ly = axis(-stickUp);

        // The view turns towards the place as well, so that it is walked to forwards.
        float error = std::atan2(toY, toX) - std::atan2(aheadY, aheadX);
        while (error > kPi)
            error -= 2.0f * kPi;
        while (error < -kPi)
            error += 2.0f * kPi;
        const float wanted = std::clamp(-error / 3.0f / turnPerFrame, -1.0f, 1.0f); // stick to the right turns clockwise
        const long steps = std::lround(std::cbrt(wanted) * 127.0f);
        if (std::fabs(error) > 0.05f && std::labs(steps) >= 25) // below that the game ignores the stick
            out.rx = static_cast<uint8_t>(std::clamp(128L + steps, 0L, 255L));

        if (sidestepFrames > 0)
        {
            if (--sidestepFrames > 20)
                out.buttons = 1u << 10; // L1: with a jump at the start
            pushFrames = 0;
            return out;
        }
        if (jumpFrames > 0)
        {
            if (--jumpFrames > 17)
                out.buttons = 1u << 10; // L1
            pushFrames = 0;
            return out;
        }
        // Stuck: pushing for a while brought the place no nearer.
        float &oldest = trail[trailAt % 20u];
        const bool stuck = pushFrames >= 20 && oldest - distance < 0.7f;
        oldest = distance;
        ++trailAt;
        ++pushFrames;
        if (stuck)
        {
            pushFrames = 0;
            if (++stuckCount % 3u != 0u)
            {
                std::fprintf(stderr, "[tf:goto] tick %llu: stuck at (%.1f, %.1f, %.1f), jumping\n",
                             static_cast<unsigned long long>(tick), position[0], position[1], position[2]);
                jumpFrames = 24;
            }
            else
            {
                std::fprintf(stderr, "[tf:goto] tick %llu: stuck at (%.1f, %.1f, %.1f), stepping %s\n",
                             static_cast<unsigned long long>(tick), position[0], position[1], position[2],
                             sidestepRight ? "right" : "left");
                sidestepFrames = 26;
                sidestepRight = !sidestepRight;
            }
            return out;
        }
        static const bool everyFrame = std::getenv("TF_GOTO_LOG") != nullptr; // TF_GOTO_LOG=1: a line per frame
        static uint32_t frames = 0;
        if (++frames % 60u == 0u || everyFrame)
            std::fprintf(stderr, "[tf:goto] tick %llu: at (%.1f, %.1f, %.1f), %.1f to go, facing %.0f of %.0f degrees, stick %d %d\n",
                         static_cast<unsigned long long>(tick), position[0], position[1], position[2], distance,
                         std::atan2(aheadY, aheadX) * 180.0f / kPi, std::atan2(toY, toX) * 180.0f / kPi, out.lx - 128,
                         128 - out.ly);
        return out;
    }

    // Called once per rendered frame: the FMV skip and the input script share the
    // runtime's pad override.
    void updatePadOverride(uint8_t *rdram, PS2Runtime *runtime)
    {
        const uint64_t tick = testTick(runtime);
        if (g_quitTick != 0u && tick >= g_quitTick)
        {
            playrec::finish("TF_QUIT_VSYNC"); // (a play recording's end written: playrec.h)
            writeCoverage(); // the process ends without running exit handlers
            writeCallMap();
            ext::beforeQuit(); // (the extensions' last words: the process ends without exit handlers)
            runtime->requestStop();
        }
        // Loading a save state moves the tick: the script restarts from there, without the
        // presses for earlier ticks (which would otherwise all fire now).
        static uint64_t seenStates = 0u;
        if (const uint64_t states = runtime->eeScheduler().loadedStateCount(); states != seenStates)
        {
            seenStates = states;
            for (ScriptedPress &press : g_inputScript)
            {
                press.done = press.tick < tick;
                press.frames = 0u;
            }
        }
        // "+N" ticks: from the first frame the player stands in a level - the taGame there and the player's position known
        // (see g_levelTicks).
        if (g_levelTicks && g_levelStartTick == 0u && playerPositionAddress(rdram) != 0u &&
            (rdram[0x006CF2B8u] | rdram[0x006CF2B9u] | rdram[0x006CF2BAu] | rdram[0x006CF2BBu]) != 0u) // (a level: the taGame, kGame)
        {
            g_levelStartTick = tick;
            for (ScriptedPress &press : g_inputScript)
                if (press.tick == kWaitingForLevel)
                    press.tick = tick + press.afterLevel;
            for (WalkTarget &target : g_walkTargets)
                if (target.tick == kWaitingForLevel)
                    target.tick = tick + target.afterLevel;
            std::fprintf(stderr, "[tf] the level's start at tick %llu: the \"+N\" ticks count from here\n",
                         static_cast<unsigned long long>(tick));
        }
        uint8_t sticks = 0u;
        uint8_t stickPercent = 100u;
        uint16_t pressed = static_cast<uint16_t>(fmvSkipButtons() | scriptedButtons(tick, sticks, stickPercent) |
                                                 mod_menu::padButtons(tick) | // (a cheat code the mod menu enters)
                                                 camera_mode::padButtons() | // (first person: into the aiming view)
                                                 camera_mode::reconButtons()); // (TODO 228: the right mouse button's recon mode)
        applyWarps(rdram, tick);
        // TF_RAM_SCAN=<tick> (debugging aid): at that tick, which 4 KB pages hold anything - the top 2 MB of the PS2's
        // 32 MB (the main thread's stack, the heap's first block's end) and the port's added data (N7: the top of the
        // 256 MB; with TF_RAM_MB=32 under the stack) - and the heap's bounds.
        if (static const uint64_t scanAt = std::getenv("TF_RAM_SCAN") ? std::strtoull(std::getenv("TF_RAM_SCAN"), nullptr, 10) : 0u;
            scanAt != 0u && tick >= scanAt)
        {
            static bool scanned = false;
            if (!scanned)
            {
                scanned = true;
                const PS2Runtime::GuestLayout &layout = PS2Runtime::guestLayout();
                const auto scan = [&](const char *what, uint32_t begin, uint32_t end)
                {
                    std::fprintf(stderr, "[tf:ram] %s %08x-%08x:\n", what, begin, end);
                    std::string used;
                    for (uint32_t page = begin; page < end; page += 0x1000u)
                    {
                        bool any = false;
                        for (uint32_t a = page; a < page + 0x1000u && !any; a += 4u)
                            any = rdram[a] | rdram[a + 1] | rdram[a + 2] | rdram[a + 3];
                        used += any ? '#' : '.';
                        if (used.size() == 64u || page + 0x1000u >= end)
                        {
                            std::fprintf(stderr, "[tf:ram] %08x %s\n", page + 0x1000u - static_cast<uint32_t>(used.size()) * 0x1000u,
                                         used.c_str());
                            used.clear();
                        }
                    }
                };
                scan("the top of the PS2's 32 MB", 0x01E00000u, 0x02000000u);
                if (layout.addedDataBegin >= 0x02000000u)
                    scan("the port's added data", layout.addedDataBegin & ~0xFFFu, layout.addedDataEnd);
                std::fprintf(stderr, "[tf:ram] heap base %08x end %08x limit %08x\n", runtime->guestHeapBase(), runtime->guestHeapEnd(),
                             runtime->guestHeapLimit());
            }
        }
        // TF_POS_LOG=<ticks> (debugging aid): the player's position every that many VBlank ticks.
        if (static const int posEvery = std::getenv("TF_POS_LOG") ? std::atoi(std::getenv("TF_POS_LOG")) : 0; posEvery > 0)
        {
            static uint64_t lastPos = 0u;
            if (tick >= lastPos + static_cast<uint64_t>(posEvery))
            {
                lastPos = tick;
                if (const uint32_t at = playerPositionAddress(rdram))
                {
                    float p[3] = {0.0f, 0.0f, 0.0f};
                    if (const uint8_t *m = getConstMemPtr(rdram, at))
                        std::memcpy(p, m, sizeof(p));
                    std::fprintf(stderr, "[tf:pos] tick %llu: %.2f %.2f %.2f\n", static_cast<unsigned long long>(tick), p[0], p[1], p[2]);
                }
            }
        }
        tfWatchdogFrame();
        tfTestWindow(runtime->eeScheduler().currentVSyncTick()); // (TF_TEST_WINDOW, tests: profiler.cpp)
        applyPokes(rdram, tick);
        game_config::frame(rdram); // the Cheats and Debug tabs (game_config.h)
        mod_menu::frame(rdram);    // the mod menu's Levels tab (mod_menu.h)
        {
            uint32_t game = 0u; // (the taGame: there while a level runs)
            std::memcpy(&game, rdram + 0x006CF2B8u, sizeof(game)); // (kGame, below)
            camera_mode::frame(rdram, game != 0u);
            dualsense::frame(rdram, game != 0u, playerPositionAddress(rdram)); // the pad's light bar (dualsense.h)
            speedrun_timer::frame(rdram, g_liveFmvStreams != 0, runtime->eeScheduler().currentVSyncTick());
            discord_presence::frame(rdram, game != 0u, g_executorVtable == 0x006B7360u, g_liveFmvStreams != 0 && g_liveFmvIsFilm);
            photo_mode::frame(rdram, game != 0u); // (before tuning: the game's speed it sets)
            fast_load::frame(rdram, *runtime, g_executorVtable == 0x006B7360u); // the sound muted while a level loads (fast_load.h)
            {
                // (TODO 55) The HQ entered or left: the native renderer lets the old place's textures go (nr_install.h).
                static int wasHq = -1;
                const int hq = g_executorVtable == 0x006B7360u ? 1 : 0;
                if (wasHq >= 0 && hq != wasHq)
                {
                    nr::placeChanged();
                    nr::n5::dropSummaries(); // (N5 step 1: the packets of the place before)
                }
                wasHq = hq;
            }
            hq_rate::frame(rdram, *runtime, g_executorVtable == 0x006B7360u, g_levelRate); // the HQ at the PC rate (hq_rate.h)
            guest_code::frame(rdram, *runtime);    // the added code's data and self-test (guest_code.h)
            if (!ext::all().empty()) // the extensions' frame work (port_ext.h: the HQ selection, the jet bike, the mod host)
            {
                ext::FrameFacts facts;
                facts.tick = tick;
                facts.inLevel = game != 0u;
                facts.inHq = g_executorVtable == 0x006B7360u;
                facts.levelRate = g_levelRate;
                facts.playerPosition = playerPositionAddress(rdram);
                ext::frame(rdram, *runtime, facts);
            }
            tuning::frame(rdram, game != 0u);
        }
        ultrawide::frame(rdram);   // the picture's shape beyond 16:9 (ultrawide.h)
        if (g_godMode)
            keepEnergyFull(rdram);
        WalkSticks walk = g_walkTargets.empty() ? WalkSticks{} : walkToTarget(rdram, tick);
        const bool photo = photo_mode::active(); // (photo mode: the pad at rest, photo_mode.h)
        if (photo)
        {
            pressed = 0u;
            sticks = 0u;
            walk = WalkSticks{};
        }
        pressed |= walk.buttons;
        const bool overriding = photo || pressed != 0u || sticks != 0u || walk.active || ((g_skipFmv || g_skipIntroMovie) && g_liveFmvStreams != 0);
        const auto axis = [sticks, stickPercent](unsigned low, unsigned high)
        {
            const int reach = 127 * stickPercent / 100;
            return static_cast<uint8_t>((sticks & (1u << low)) ? 0x80 - reach - (stickPercent == 100u ? 1 : 0)
                                                               : ((sticks & (1u << high)) ? 0x80 + reach : 0x80));
        };
        // The script's sticks come first; the walk fills in the ones it leaves alone.
        const bool leftScripted = (sticks & 0x0Fu) != 0u, rightScripted = (sticks & 0xF0u) != 0u;
        if (overriding)
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~pressed),
                                           walk.active && !leftScripted ? walk.lx : axis(3, 1),
                                           walk.active && !leftScripted ? walk.ly : axis(0, 2),
                                           walk.active && !rightScripted ? walk.rx : axis(7, 5), axis(4, 6));
        else if (g_padOverridden)
            ps2_stubs::clearPadOverrideState();
        g_padOverridden = overriding;
    }

    // Spawn trace (debugging aid): TF_SPAWN_TRACE=1 logs the creature mask a level loads with
    // (taCreatureSystem::StripEnemiesFromDatabase 0x2B0FA0: 0 all, 1 player and bosses, 2 player
    // only, from the spawnCreatures option or the game mode) and counts the AI creatures made
    // (taCreatureAI's constructor 0x2AD8B0: the player, Decepticons, bosses, turrets).
    constexpr uint32_t kStripEnemies = 0x002B0FA0u, kCreatureAICtor = 0x002AD8B0u;
    PS2Runtime::RecompiledFunction g_stripEnemies = nullptr, g_creatureAICtor = nullptr;
    uint32_t g_aiCreatures = 0u;

    void stripEnemies(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_aiCreatures = 0u;
        std::fprintf(stderr, "[spawn] level loads with creature mask %u\n", getRegU32(ctx, 5));
        g_stripEnemies(rdram, ctx, runtime);
    }

    void creatureAICtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_aiCreatures;
        if (g_aiCreatures <= 3u || g_aiCreatures % 10u == 0u)
            std::fprintf(stderr, "[spawn] AI creature #%u (vblank %llu)\n", g_aiCreatures,
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
        g_creatureAICtor(rdram, ctx, runtime);
    }

    // sub_00255F90 is the game's fatal error screen: it draws the message in the buffer at
    // 0x631058 and then spins forever. Log the message, since the screen stays black here.
    constexpr uint32_t kErrorScreen = 0x00255F90u;
    // (the fatal path - func_00104570 - copies its message to 0x6D0FA0 first; the screen's own
    // buffer is often still empty when it is called)
    constexpr uint32_t kErrorMessage = 0x00631058u, kFatalMessage = 0x006D0FA0u;
    PS2Runtime::RecompiledFunction g_errorScreen = nullptr;

    void errorScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto text = [rdram](uint32_t at)
        {
            std::string m;
            for (uint32_t i = 0; i < 2048u; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, at + i);
                if (!c || *c == 0u)
                    break;
                m.push_back(*c >= 0x20u || *c == '\n' ? static_cast<char>(*c) : '?');
            }
            return m;
        };
        std::string message = text(kErrorMessage);
        if (message.empty())
            message = text(kFatalMessage);
        // (out of memory - tdgInternal_HandleOutOfMemoryError 0x106270 - writes its memory report here)
        if (const std::string report = text(0x006D2000u); !report.empty())
            message += "\n[memory report]\n" + report;
        std::fprintf(stderr, "[tf] game error screen (vsync %llu, ra=%08x): %s\n",
                     static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()),
                     getRegU32(ctx, 31), message.c_str());
#if defined(_WIN32)
        tfLogCallStack("error screen");
#endif
        g_errorScreen(rdram, ctx, runtime);
    }

#if defined(_WIN32)
    // TF_GUARD_FRAME=<function>,<frame size>,<ra offset> (hex): while that function runs,
    // logs every write to its saved return address that leaves anything but the real one
    // there, with the host call stack (finds what smashes a frame).
    uint32_t g_guardFrameFunction = 0u, g_guardFrameSize = 0u, g_guardFrameRa = 0u;
    PS2Runtime::RecompiledFunction g_guardedFunction = nullptr;

    void guardedFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t slot = 0u, returnAddress = 0u, entrySp = 0u, calls = 0u;
        const bool entry = ctx->pc == g_guardFrameFunction;
        if (entry)
        {
            entrySp = getRegU32(ctx, 29);
            slot = entrySp - g_guardFrameSize + g_guardFrameRa;
            returnAddress = getRegU32(ctx, 31);
            ++calls;
        }
        // (Otherwise the function resumes after yielding to the scheduler: same slot.)
        tfGuardGuestWrites(slot, 8u, returnAddress, true);
        g_guardedFunction(rdram, ctx, runtime);
        tfStopGuestWriteGuard();
        uint32_t saved = 0u;
        if (const uint8_t *p = getConstMemPtr(rdram, slot))
            std::memcpy(&saved, p, sizeof(saved));
        if (ctx->pc != returnAddress || getRegU32(ctx, 29) != entrySp)
            std::fprintf(stderr, "[tf] guarded frame: call %u (%s) left pc=%08x ra=%08x sp=%08x; entered with ra=%08x sp=%08x, "
                                 "slot %08x holds %08x\n",
                         calls, entry ? "entry" : "resume", ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 29), returnAddress,
                         entrySp, slot, saved);
    }
#endif

    // Mouse look. The game turns the view with the right stick, and how it answers the stick
    // was measured in Amazon Basin (docs/NOTES.md, "Controls"): an axis is ignored below a
    // fifth of its travel, and beyond that a deflection d (of length at most 1) turns the
    // view each second by
    //     yaw   = 3 pi / 4 rad * |d|^2 * dx
    //     pitch =     pi / 4 rad * |d|^2 * dy
    // (a frame turns its share of that: the turning is timed).
    // The mouse goes through the inverse of that: every frame the stick is pushed exactly as
    // far as turns the view by the angle the mouse travelled. What does not fit into a frame
    // - less than the dead zone lets through, or more than the game's top speed - is kept
    // and turned in the frames that follow.
    // The game tilts the view like a flight stick: the stick pulled down looks up. A mouse
    // looks the way it moves, so its vertical travel pushes the stick the other way
    // (PS2X_MOUSE_INVERT_Y=1 gives the flight-stick way back).
    constexpr int kStickDeadZone = 25; // the smallest deflection of an axis the game answers, of 127 steps
    // Angle per pixel of mouse travel at sensitivity 1: 1200 pixels per second turn the view
    // at the game's top speed.
    constexpr float kRadiansPerPixel = 2.35619f / 1200.0f;
    // Travel faster than the game turns is kept for this long at top speed, so a quick flick
    // is finished a moment later instead of being cut short.
    constexpr float kMaxOwedSeconds = 5.0f / 30.0f;
    float g_owedYaw = 0.0f, g_owedPitch = 0.0f; // still to turn, radians, towards stick right / down
    const uint8_t *g_mouseLookRdram = nullptr;  // (TODO 176: main memory, for mouseLook's cutscene check)

    // Mouse look, straight to the camera's angles (2026-10-05). The third-person camera turns
    // in 0x46D410 (the camera control in a1: its pitch at +0x118, its yaw at +0x11C): the
    // stick's speed, reached through a rate that eases toward it a share a frame, times the
    // frame time. The stick's dead zone, that easing and its top speed made the mouse feel
    // sluggish and uneven; now the mouse's travel is added to the angles as the function is
    // entered - an angle per pixel at any frame rate, nothing in between - and the game's
    // own turning (a pad's right stick) goes on as before. The pitch is held within the
    // game's limit: the camera data's +0x15C times gp-0x75A4, the data entry at 0x717430 +
    // id x 0x240 + 0x60 (0x34F2B0, 0x4A57D0; the id at +0x18). The yaw is kept in [-pi, pi)
    // as 0x1AD450 keeps it. Mouse up looks up (the pitch goes down), mouse right turns right
    // (the yaw down). Where another camera turns (first person, gliding, ...), the mouse goes
    // through the stick as before (mouseLook below).
    constexpr uint32_t kCameraTurn = 0x0046D410u;
    constexpr uint32_t kCameraData = 0x00717430u;      // 0x34F2B0's
    constexpr uint32_t kPitchLimitScale = 0x006CBB4Cu; // gp-0x75A4
    constexpr uint32_t kTurnsPerRadian = 0x006CB144u;  // gp-0x7FAC (0x1AD450)
    constexpr uint32_t kRadiansPerTurn = 0x006CB148u;  // gp-0x7FA8
    PS2Runtime::RecompiledFunction g_cameraTurn = nullptr;
    uint64_t g_frameKicks = 0u;      // frames kicked so far (frameKick)
    std::atomic<uint64_t> g_listsKicked{0u}; // display lists kicked (tf::port::listsKicked)
    uint64_t g_cameraTurnKick = ~0ull; // ... when the camera last turned by the mouse

    // (TF_TURN_TEST=<tick>:<pixels a second x>[:<y>],..., a measuring aid for the camera's turn for every picture - N9: a
    // mouse moving at that speed in real time from each game-time tick on (view_turn.cpp); 0 stops it)
    struct TestTurn
    {
        uint64_t tick;
        float x, y;
    };
    std::vector<TestTurn> g_testTurns;
    size_t g_testTurnNext = 0;
    void parseTestTurns()
    {
        const char *v = std::getenv("TF_TURN_TEST");
        for (const char *p = v; p && *p;)
        {
            char *end = nullptr;
            TestTurn t{std::strtoull(p, &end, 10), 0.0f, 0.0f};
            if (end == p || *end != ':')
                break;
            t.x = std::strtof(end + 1, &end);
            if (*end == ':')
                t.y = std::strtof(end + 1, &end);
            g_testTurns.push_back(t);
            p = *end == ',' ? end + 1 : end;
            if (!*end || *end != ',')
                break;
        }
    }

    // (TODO 176) A cutscene owns the view: taCutSceneSystem's "a script plays" byte (gp-0x355C = 0x6CFB94, one of the
    // flags its Update 0x514FB0 reads first) - 1 from the moment a Data-Con's discovery, a level's opening or another .CS
    // script starts until it ends. The game takes no pad turn then (its right stick does nothing); the mouse's travel was
    // written straight into the third-person camera's angles, which the cutscene places the Autobot by: it is taken and
    // dropped instead (no turn piled up for afterwards). TF_CUTSCENE_MOUSE=1: as before; TF_CUTSCENE_MOUSE_LOG=1: the
    // byte's changes logged.
    bool cutsceneOwnsView(const uint8_t *rdram)
    {
        static const bool off = std::getenv("TF_CUTSCENE_MOUSE") && *std::getenv("TF_CUTSCENE_MOUSE") == '1';
        if (off)
            return false;
        const uint8_t *flag = getConstMemPtr(rdram, 0x006CFB94u);
        const bool on = flag && *flag != 0u;
        static bool was = false;
        static const bool log = std::getenv("TF_CUTSCENE_MOUSE_LOG") != nullptr;
        if (on != was && log)
            std::fprintf(stderr, "[turn] cutscene %s: the mouse %s\n", on ? "playing" : "over", on ? "left out" : "turns again");
        was = on;
        return on;
    }

    void dropMouseTravel()
    {
        float x = 0.0f, y = 0.0f;
        view_turn::takeMouseTravel(x, y);
        ps2_host_input::setMouseStick(0.0f, 0.0f);
        g_owedYaw = g_owedPitch = 0.0f;
    }

    void cameraTurn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (cutsceneOwnsView(rdram))
        {
            dropMouseTravel();
            g_cameraTurnKick = g_frameKicks;
            g_cameraTurn(rdram, ctx, runtime);
            return;
        }
        const uint32_t camera = getRegU32(ctx, 5);
        if (g_testTurnNext < g_testTurns.size() && testTick(runtime) >= g_testTurns[g_testTurnNext].tick)
        {
            const TestTurn &t = g_testTurns[g_testTurnNext++];
            view_turn::setTestMouse(t.x, t.y);
            std::fprintf(stderr, "[turn] test mouse %.0f, %.0f pixels a second from tick %llu\n", t.x, t.y, static_cast<unsigned long long>(t.tick));
        }
        float pixelsX = 0.0f, pixelsY = 0.0f;
        float mouseYaw = 0.0f, mousePitch = 0.0f; // (the mouse's part of the turn: N9's turn for every picture)
        uint8_t *angles = camera != 0u ? getMemPtr(rdram, camera + 0x118u) : nullptr;
        const uint8_t *idField = camera != 0u ? getConstMemPtr(rdram, camera + 0x18u) : nullptr;
        if (angles && idField && view_turn::takeMouseTravel(pixelsX, pixelsY))
        {
            if (g_mouseInvertY) // (TODO 178: the camera's invert as the stick gets it - the game's own turn never sees the mouse)
                pixelsY = -pixelsY;
            // (TODO 189) The aiming view's zoom (R3 / Circle into taTransformerStateLook, R3 again steps the zoom): the game's
            // stick turn is the rate times the state's +0x20 (0x46D74C: 1, then 0.57 and 0.33 at the two zoom steps - measured
            // 135, 77 and 45 deg/s), so zoomed in the view turns as much across the picture as before. The mouse went to the
            // angles at full speed: at the zoom steps 1.75 and 3 times as far across the picture a pixel - jumpy aiming. Its
            // travel is scaled the same. TF_AIM_MOUSE_ZOOM=0: as before.
            {
                static const bool off = std::getenv("TF_AIM_MOUSE_ZOOM") && *std::getenv("TF_AIM_MOUSE_ZOOM") == '0';
                float zoom = 1.0f;
                const uint32_t state = getRegU32(ctx, 4);
                if (const uint8_t *z = off || state < 0x00100000u ? nullptr : getConstMemPtr(rdram, state + 0x20u))
                    std::memcpy(&zoom, z, sizeof(zoom));
                if (zoom > 0.05f && zoom < 1.0f)
                {
                    pixelsX *= zoom;
                    pixelsY *= zoom;
                }
            }
            g_cameraTurnKick = g_frameKicks;
            ps2_host_input::setMouseStick(0.0f, 0.0f);
            g_owedYaw = g_owedPitch = 0.0f;
            if ((pixelsX != 0.0f || pixelsY != 0.0f) && !photo_mode::active()) // (photo mode: the free camera's)
            {
                const auto readFloat = [rdram](uint32_t address)
                {
                    float value = 0.0f;
                    if (const uint8_t *p = getConstMemPtr(rdram, address))
                        std::memcpy(&value, p, sizeof(value));
                    return value;
                };
                uint16_t id = 0u;
                std::memcpy(&id, idField, sizeof(id));
                const float limit = readFloat(kPitchLimitScale) * readFloat(kCameraData + id * 0x240u + 0x60u + 0x15Cu);
                float pitch = 0.0f, yaw = 0.0f;
                std::memcpy(&pitch, angles, sizeof(pitch));
                std::memcpy(&yaw, angles + 4, sizeof(yaw));
                const float pitch0 = pitch, yaw0 = yaw;
                pitch = std::clamp(pitch + pixelsY * kRadiansPerPixel, -std::fabs(limit), std::fabs(limit));
                const float turns = readFloat(kTurnsPerRadian) * (yaw - pixelsX * kRadiansPerPixel) + 100.5f;
                yaw = readFloat(kRadiansPerTurn) * (turns - static_cast<float>(static_cast<int32_t>(turns)) - 0.5f);
                std::memcpy(angles, &pitch, sizeof(pitch));
                std::memcpy(angles + 4, &yaw, sizeof(yaw));
                mousePitch = pitch - pitch0;
                mouseYaw = yaw - yaw0;
                if (mouseYaw > 3.14159265f)
                    mouseYaw -= 6.28318531f;
                else if (mouseYaw < -3.14159265f)
                    mouseYaw += 6.28318531f;
            }
        }
        g_cameraTurn(rdram, ctx, runtime);
        // (TF_TURN_LOG=1, a measuring aid: the third-person camera's pitch and yaw after the game's turn, each frame, with
        // the game-time tick - the turn's degrees a second at any rate)
        if (static const bool turnLog = std::getenv("TF_TURN_LOG") && *std::getenv("TF_TURN_LOG") == '1'; turnLog && angles)
        {
            float a[2] = {0.0f, 0.0f};
            std::memcpy(a, angles, sizeof(a));
            // (and the view as the game shows it: the 3D sound listener the game keeps with the camera - [0x6CE8C4] +0x24/+0x28,
            // its first microphone, the forward vector at +0x30)
            float view[3] = {0.0f, 0.0f, 0.0f};
            // (the camera's transform rotation, w x y z, as the listener update last found it)
            float cq[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (g_cameraTransform >= 0x00100000u && g_cameraTransform < PS2_RAM_SIZE - 0x50u)
                std::memcpy(cq, rdram + g_cameraTransform + 0x40u, sizeof(cq));
            {
                uint32_t device = 0u, count = 0u, list = 0u, mic = 0u;
                std::memcpy(&device, rdram + 0x006CE8C4u, 4);
                if (device >= 0x00100000u && device < PS2_RAM_SIZE - 0x40u)
                {
                    std::memcpy(&count, rdram + device + 0x24u, 4);
                    std::memcpy(&list, rdram + device + 0x28u, 4);
                }
                if (count && list >= 0x00100000u && list < PS2_RAM_SIZE - 4u)
                    std::memcpy(&mic, rdram + list, 4);
                if (mic >= 0x00100000u && mic < PS2_RAM_SIZE - 0x40u)
                    std::memcpy(view, rdram + mic + 0x30u, sizeof(view));
            }
            std::fprintf(stderr, "[turn log] tick %llu pitch %.5f yaw %.5f view %.4f %.4f %.4f cam %.4f %.4f %.4f %.4f\n", static_cast<unsigned long long>(testTick(runtime)), a[0], a[1],
                         view[0], view[1], view[2], cq[0], cq[1], cq[2], cq[3]);
        }
        if (angles && idField)
            view_turn::noteTurned(rdram, camera, mouseYaw, mousePitch, 1.0f / framesPerSecond());
    }

    void mouseLook()
    {
        // (The third-person camera took this frame's travel itself: cameraTurn above.)
        const bool direct = g_cameraTurnKick == g_frameKicks;
        ++g_frameKicks;
        if (direct)
            return;
        if (g_mouseLookRdram && cutsceneOwnsView(g_mouseLookRdram))
        {
            dropMouseTravel(); // (TODO 176: a cutscene's view)
            return;
        }
        const float yawPerFrame = kYawPerSecond / framesPerSecond();
        const float pitchPerFrame = kPitchPerSecond / framesPerSecond();
        float pixelsX = 0.0f, pixelsY = 0.0f;
        if (!view_turn::takeMouseTravel(pixelsX, pixelsY))
        {
            if (g_owedYaw != 0.0f || g_owedPitch != 0.0f)
                ps2_host_input::setMouseStick(0.0f, 0.0f);
            g_owedYaw = g_owedPitch = 0.0f;
            return;
        }
        // (TODO 178: the mouse up = the stick up - the game inverts it by the option, as the stick)
        g_owedYaw = std::clamp(g_owedYaw + pixelsX * kRadiansPerPixel, -kMaxOwedSeconds * kYawPerSecond,
                               kMaxOwedSeconds * kYawPerSecond);
        g_owedPitch = std::clamp(g_owedPitch + pixelsY * kRadiansPerPixel, -kMaxOwedSeconds * kPitchPerSecond,
                                 kMaxOwedSeconds * kPitchPerSecond);

        // The deflection that turns the owed angle in one frame, or as much of it as the
        // game allows, in the steps a pad sends: 127 to the right and down, 128 to the left
        // and up, and the game scales either side to 1.
        const auto toSteps = [](float deflection)
        { return std::lround(deflection * (deflection < 0.0f ? 128.0f : 127.0f)); };
        const auto fromSteps = [](long steps)
        { return static_cast<float>(steps) / (steps < 0 ? 128.0f : 127.0f); };
        const float ax = g_owedYaw / yawPerFrame, ay = g_owedPitch / pitchPerFrame;
        const float length = std::hypot(ax, ay);
        long stepsX = 0, stepsY = 0;
        if (length > 0.0f)
        {
            const float reach = std::cbrt(std::min(1.0f, length));
            stepsX = toSteps(ax / length * reach);
            stepsY = toSteps(ay / length * reach);
        }
        if (std::labs(stepsX) < kStickDeadZone)
            stepsX = 0;
        if (std::labs(stepsY) < kStickDeadZone)
            stepsY = 0;
        ps2_host_input::setMouseStick(static_cast<float>(stepsX) / 127.0f, static_cast<float>(stepsY) / 127.0f);

        // What the game turns with that.
        float dx = fromSteps(stepsX), dy = fromSteps(stepsY);
        float reach = std::hypot(dx, dy);
        if (reach > 1.0f)
        {
            dx /= reach;
            dy /= reach;
            reach = 1.0f;
        }
        g_owedYaw -= yawPerFrame * reach * reach * dx;
        g_owedPitch -= pitchPerFrame * reach * reach * dy;
    }

    // TF_PEEK=187caac:f,187caa8:f,6312a0 (debugging aid): logs those words of EE memory once
    // per rendered frame whenever one of them changed, with the VBlank tick. ":f" prints a
    // float, otherwise hex. Unlike TF_WATCH it is tied to game time, so a scripted run gives
    // the same log every time (for measuring how a value follows an input).
    struct PeekedWord
    {
        uint32_t address = 0u;
        bool asFloat = false;
        uint32_t last = 0u;
    };
    std::vector<PeekedWord> g_peeked;

    void parsePeekList()
    {
        const char *list = std::getenv("TF_PEEK");
        if (!list)
            return;
        for (const char *p = list; *p;)
        {
            char *end = nullptr;
            PeekedWord word;
            word.address = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p)
                break;
            if (end[0] == ':' && end[1] == 'f')
            {
                word.asFloat = true;
                end += 2;
            }
            g_peeked.push_back(word);
            p = (*end == ',') ? end + 1 : end;
        }
    }

    void reportPeekedWords(const uint8_t *rdram, uint64_t tick)
    {
        static bool first = true;
        bool changed = first;
        for (PeekedWord &word : g_peeked)
        {
            uint32_t value = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, word.address))
                std::memcpy(&value, p, sizeof(value));
            changed = changed || value != word.last;
            word.last = value;
        }
        first = false;
        if (!changed)
            return;
        std::string line;
        char text[64];
        std::snprintf(text, sizeof(text), "[tf:peek] tick %llu", static_cast<unsigned long long>(tick));
        line += text;
        for (const PeekedWord &word : g_peeked)
        {
            float f = 0.0f;
            std::memcpy(&f, &word.last, sizeof(f));
            if (word.asFloat)
                std::snprintf(text, sizeof(text), " %08x=%.6g", word.address, f);
            else
                std::snprintf(text, sizeof(text), " %08x=%08x", word.address, word.last);
            line += text;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    // The frame rate. The game runs 30 frames a second and draws each as two fields for two
    // moments ("betwixtMode", 0x6CD8DC: the second with the moving scene nodes blended halfway,
    // docs/NOTES.md "Frame rates"). The engine can also run a frame per field: its governor
    // (0x25B670, every frame) aims at the TV's rate (0x32E8B0: 59.94), halved when the "half
    // rate" byte (0x6CF654) is set - which 0x32E8A0 does when the game's modes are made, so the
    // game ships at 30. Without betwixtMode the main loop (0x295310) waits for the VBlanks a
    // frame should last, worked out from the target rate when the loop starts, and at a field a
    // frame the game draws field A alone.
    //
    // A level reads the frame period (0x32E910) when it begins and keeps it: its time step
    // (taGame+0x5C, every system's dt), the game clock's step (0x41E820/0x41E840), the gravity a
    // physics step (0x2C6230: 29.4 x the period), the in-level movies' rate and a few more
    // (docs/FRAME_RATE.md). So a level runs at one rate from its beginning to its end: the PC
    // port's rate R (TF_FRAME_RATE=<frames a second>: 60 by default, 120, 144, ...; 30 or
    // TF_60FPS=0 for the console's) is switched on when a taGame (a level: vtable 0x6B4C40)
    // begins (0x294950, which makes the game clock first thing) and off when it ends
    // (0x2942E0). The intro and the menus (taUIIntro, taUIPreGame) run as on the console. (Run
    // at 60 from boot, the menus' 3D scenes stayed black and the level after them crashed
    // loading.) In a level at R:
    // - the rates are the console's scaled to it (decomp/timing/rates.cpp): the TV's and the
    //   target 59.94 x R / 60, the period one over that; the half rate is off;
    // - a VBlank comes every 1/R s (EeScheduler::setVBlankPeriod) - but at the console's rate
    //   while a movie plays, whose pictures are timed in the console's VBlanks (MPEG.cpp);
    // - the EE runs 30/R of the console's cycles a frame (EeScheduler::setCycleScale: the
    //   busiest frames need about 30 ms of the console's EE);
    // - the governor keeps one field a frame and the full detail, and does not hold the game at
    //   30 while 0x6CD324 is set (0x202FE0: while a level loads), since the level's timers count
    //   its frames (decomp/timing/governor.cpp, rates.cpp);
    // - what the game does by the frame where it means time is made for frames of 1/R s: the
    //   blends, the model's turn, the cars, the jumps and the sliding contacts below, and the 3D
    //   sound's frame rate.
    // The level of a save state made at a PC rate plays at the rate set now (what it kept from
    // its begin rewritten: convertLevel); one made at 30 keeps 30 unless TF_60FPS=force.
    constexpr uint32_t kHalfRate = 0x006CF654u;
    constexpr uint32_t kBetwixtMode = 0x006CD8DCu;
    constexpr uint32_t kDropThreshold = 0x006CD988u; // frames without a margin before 60 drops to 30
    constexpr uint32_t kGovernorTarget = 0x006CE980u;   // targetRates[0]: the governor's target rate (the main loop sets it)
    constexpr uint32_t kWantTwoFields = 0x006CD984u;    // the governor's choice: frames of two fields (the console's 30) or one
    constexpr uint32_t kFramesOfTwoFields = 0x006CF010u; // the governor's "30" flag
    constexpr uint32_t kCountFromFlipA = 0x006CF014u;   // a frame's VBlank wait counted from the last flip (one field)
    constexpr uint32_t kGameBegin = 0x00294950u;
    constexpr uint32_t kGameEnd = 0x002942E0u;
    constexpr uint32_t kGame = 0x006CF2B8u;             // gp-0x3E38: the taGame (the level)
    constexpr uint32_t kGamePeriod = 0x5Cu;             // its frame period, from its begin
    constexpr uint32_t kGameClock = 0x006CF8A4u;        // gp-0x384C: there while a level is
    constexpr uint32_t kGravityStep = 0x006EC698u;      // z of the physics' gravity a step (0x2C6230)
    constexpr uint32_t kGravityPerSecond = 0x006CBA1Cu; // gp-0x76D4: times the period for it
    constexpr uint32_t kMovieRate = 0x006CF5F0u;        // gp-0x3B00: the in-level movies' rate (0x319AAC)
    constexpr uint32_t kSoundFrameRate = 0x006CD4F8u;   // gp-0x5BF8: twice the 3D sound's frames a second
    constexpr uint32_t kConsoleFrameRate = 0x0063D4A8u; // the rate table's 59.94 (what boot puts there)
    constexpr uint32_t kCarPeriod = 0x34u, kCarRate = 0x38u; // a car's period for its wheels, 1 over it
    bool g_force60 = false;         // TF_60FPS=force
    uint8_t g_consoleHalfRate = 1u; // what the game set (0x32E8A0)
    uint8_t g_consoleBetwixt = 1u;  // what the game's settings had, while at the PC rate
    uint32_t g_consoleDropThreshold = 3u;
    float g_consoleSoundFrameRate = 59.94f;
    float g_unpinnedCarPeriod = 0.0f; // the cars' period in a state's level made without the pin (pinCarPeriods)
    uint64_t g_statesSeen = 0u;
    PS2Runtime::RecompiledFunction g_gameBegin = nullptr, g_gameEnd = nullptr;

    void resetMotionFixes(); // (the per-controller records of the fixes, below)

    // In-between pictures (TF_IN_BETWEEN, on by default; see "The frame rate"): a level at the
    // PC rate R draws each frame as the console does at 30 - two fields, the second with the
    // moving parts of the scene halfway between the frame before and this one (the motion
    // interpolators, 0x20D3A0, by the factor 0x6CD6F0) - and every VBlank shows a field's
    // picture of its own: the half rate byte and betwixtMode stay on, as the game sets them,
    // the VBlanks come at 2R, and a frame lasts two of them. So R frames of game a second and 2R
    // pictures: 60 and 120 by default. For the level's whole run when the setting allows its
    // VBlanks (R up to 240: 480 VBlanks); not while a movie plays, whose pictures are timed in
    // the console's VBlanks - one field a frame then, as without them.
    // (N9) The native renderer's picture shown (nr_present.cpp sets the window's provider at start-up): it draws a
    // frame once, at field A's pose (nr_d3d.cpp), so the game's in-between field is never a picture of its own -
    // with in-between pictures the window showed each native picture at one VBlank, two or three, and the counter
    // counted twice the pictures there were (the user's "160 pictures", 80 of them new, 2026-10-30). A frame is
    // then one picture: the VBlanks at the frame rate, the views drawing one field. TF_NATIVE_IN_BETWEEN=1: the
    // in-between pictures as before (for an A/B).
    bool nativeOnePicture()
    {
        static const bool keep = [] {
            const char *v = std::getenv("TF_NATIVE_IN_BETWEEN");
            return v && *v && *v != '0';
        }();
        return !keep && ps2_host_display::pictureProvider() != nullptr;
    }

    bool inBetweenLevel()
    {
        if (g_levelRate == 0)
            return false;
        if (nativeOnePicture())
            return false;
        if (g_pictureLock != 0)
            return g_pictureLock == 120; // (locked: the level at 60)
        return g_inBetween && g_frameRate <= 240;
    }

    // Held off in this level by adaptFrameRate: the host fell behind with them.
    bool g_inBetweenHeld = false;
    // The field offset of late frames (tf::port::lateFramesTakeNextVBlank): 0 or 1, back to 0 when
    // a rate is set (switchFrameRate).
    int g_fieldOffset = 0;
    // A game frame rate above 60 (TF_FRAME_RATE, the panel's "Game frame rate"): a picture a frame
    // while the host keeps up; when it falls behind, the level goes to half the rate with the
    // in-between pictures - as many pictures, steadily, instead of fewer (120 -> 60 frames and 120
    // pictures rather than 75 and 75: the user's "40 fps", 2026-10-23). Held while above half.
    // TF_IN_BETWEEN=0: never; =1: in-between pictures at the fast rate too (twice its VBlanks).
    bool g_fastGameFallback = false;

    // Shown in the level (but while a movie plays): its views draw two passes.
    bool inBetweenShown()
    {
        return inBetweenLevel() && (g_pictureLock == 120 || !g_inBetweenHeld);
    }

    bool inBetweenNow()
    {
        return inBetweenShown() && g_liveFmvStreams == 0;
    }

    // The VBlanks a second in the level: R, or 2R with in-between pictures (0: no level at the
    // PC rate).
    int vblankRateNow()
    {
        return inBetweenNow() ? 2 * g_levelRate : g_levelRate;
    }

    // The VBlanks at the level's rate, or at the console's (above).
    void applyVBlankPeriod(PS2Runtime *runtime)
    {
        std::chrono::nanoseconds period{0}; // (the console's)
        if (g_levelRate != 0 && g_liveFmvStreams == 0)
            period = EeScheduler::kConsoleVBlankPeriod * 60 / vblankRateNow();
        runtime->eeScheduler().setVBlankPeriod(period);
    }

    // The rate levels run at from now on: R, or 0 for the console's. `force`: the host's side
    // as well when it stays (a state loaded). `keepMotion`: the records of the fixes below stay
    // (a level that goes on at another rate: adaptFrameRate).
    void switchFrameRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int rate, bool force = false,
                         bool keepMotion = false)
    {
        const int from = g_levelRate;
        if (rate == from && !force)
            return;
        g_levelRate = rate;
        g_fieldOffset = 0; // (late frames: a rate set begins on the game's own field)
        ps2_host_display::setFieldOffset(0);
        if (!keepMotion)
        {
            resetMotionFixes();
            g_inBetweenHeld = false; // (a new level or a state: tried again, adaptFrameRate)
        }
        if (g_fastGameFallback)
            g_inBetweenHeld = rate * 2 > g_frameRate; // (on at half the rate only: g_fastGameFallback)
        if (from == 0 && rate != 0)
        {
            g_consoleBetwixt = rdram[kBetwixtMode];
            std::memcpy(&g_consoleDropThreshold, rdram + kDropThreshold, sizeof(g_consoleDropThreshold));
            std::memcpy(&g_consoleSoundFrameRate, rdram + kSoundFrameRate, sizeof(g_consoleSoundFrameRate));
        }
        const bool inBetween = inBetweenNow();
        if (rate != 0)
        {
            // Frames of two fields with in-between pictures (as the game sets them), else of one.
            rdram[kHalfRate] = inBetween ? 1u : 0u;
            rdram[kBetwixtMode] = inBetween ? 1u : 0u;
            // The 3D sound takes a frame's movement times this, times 0.5 (0x2801D0): set at
            // boot to 59.94 for frames of two fields.
            float soundRate = 0.0f;
            {
                decomp::GuestScope scope(rdram, ctx, runtime);
                soundRate = 2.0f * tf::timing::LevelFrameRate(rate).v;
            }
            std::memcpy(rdram + kSoundFrameRate, &soundRate, sizeof(soundRate));
        }
        else if (from != 0)
        {
            rdram[kHalfRate] = g_consoleHalfRate;
            rdram[kBetwixtMode] = g_consoleBetwixt;
            std::memcpy(rdram + kDropThreshold, &g_consoleDropThreshold, sizeof(g_consoleDropThreshold));
            std::memcpy(rdram + kSoundFrameRate, &g_consoleSoundFrameRate, sizeof(g_consoleSoundFrameRate));
            g_unpinnedCarPeriod = 0.0f;
        }
        static const bool ownCycleScale = std::getenv("PS2X_EE_CYCLE_SCALE") == nullptr;
        if (ownCycleScale)
            runtime->eeScheduler().setCycleScale(rate != 0 ? 30.0 / rate : 0.0); // (0: as without it)
        applyVBlankPeriod(runtime);
        // (TODO 161 a: with the fixed logic rate a game frame late by up to TF_CATCH_UP_MS - 100 by default - is caught up, the
        // frames after it run back to back and their pictures are left out, not the time lost: the test PC's long Deep
        // Amazon run gave up the schedule 87 times in 13 minutes at the runtime's 2 VBlanks, 17 ms at 120. 0: as before.)
        {
            static const int catchUpMs = std::getenv("TF_CATCH_UP_MS") && *std::getenv("TF_CATCH_UP_MS") ? std::atoi(std::getenv("TF_CATCH_UP_MS")) : 100;
            runtime->eeScheduler().setMaxPacingLag(rate != 0 && catchUpMs > 0 ? std::max(2, rate * catchUpMs / 1000) : 0);
        }
        // At a field a frame the frame draws field A (the even rows) alone, and the frame's last
        // passes blend the odd rows with the frame before - for a TV, which shows the fields by
        // turns. Field A is the clean picture: every VBlank shows it, and the GS draws it to
        // every row of the display buffer and leaves the odd rows' passes out, for the glow
        // and line passes to read. At 30, and with in-between pictures, the fields are two
        // moments, one per VBlank.
        ps2_host_display::setShownField(rate != 0 && !inBetween ? 0 : -1);
        ps2_host_display::setFieldsAsMoments(inBetween);
        setInBetweenDrop(inBetween);
        ps2_host_display::setTvFiltersLeftOut(rate != 0); // (no trail, no anti-flicker blur at 60 or 120)
        if (inBetween)
            std::fprintf(stderr, "[tf] %d frames a second, %d pictures\n", rate, 2 * rate);
        else
            std::fprintf(stderr, "[tf] %d frames a second\n", rate != 0 ? rate : 30);
    }

    // (The half-rate setter 0x32E8A0, the rates and the governor's hold at 30, 0x202FE0, are
    // decompiled with these rules: decomp/timing/rates.cpp.)

    void startAdapting(); // (adaptFrameRate, below)

    // (Before the originals run: they may stop at a checkpoint and finish outside the hook.)
    void gameBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        switchFrameRate(rdram, ctx, runtime, g_frameRate);
        startAdapting();
        g_gameBegin(rdram, ctx, runtime);
    }

    void gameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        switchFrameRate(rdram, ctx, runtime, 0);
        g_gameEnd(rdram, ctx, runtime);
    }

    // (Since 2026-10-28, TODO 56: the HQ runs at the setting's rate like a level - hq_rate.cpp makes
    // the steps it took by the frame for it; TF_HQ_RATE=30 is the arrangement described here.)
    // The HQ with in-between pictures: its game frames at the console's 30, each drawn twice
    // (the second field posed halfway to the next frame, as levels have it) - 60 pictures at the
    // HQ's own speed. Its game rate is never raised: unlike a level, the HQ has no clock - its
    // ring, its robots and its timers move by the frame, so at 60 game frames it all ran twice
    // as fast (the user's "everything plays fast forward", 2026-10-23). TF_HQ_RATE=0: the
    // console's single picture a frame, as before; above 30 only as a test (fast forward).
    // The executors (TF_EXEC_TRACE): taUIIntro (0x6C2D10) from boot to the title, then the HQ -
    // taUIPreGame (0x6B7360) - until a level, taGame (0x6B4C40). The HQ's rate is set when the
    // main loop starts it (executorLoop) and taken back at its end (its slot 2, 0x334650), before
    // the level begins and sets its own. The title stays at the console's (a first try, everything
    // faster from boot, left the menus' 3D scenes black and crashed the next level's load).
    // Not the title and its menus (taUIIntro): a 2D screen drawn as one full picture a frame
    // (the GS's frame mode), with no in-between field to show - tried, 2026-10-23: 30 pictures.
    constexpr uint32_t kPreGameEnd = 0x00334650u, kPreGameVtable = 0x006B7360u;
    PS2Runtime::RecompiledFunction g_preGameEnd = nullptr;
    int g_hqRate = 0;
    // In the HQ (its rate set by executorLoop): the settings panel and F4 leave its rate alone,
    // and their choice takes effect at the next level.
    bool g_inHq = false;

    void preGameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[tf] the HQ ends (vblank %llu)\n",
                     static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
        g_hqMoments.store(false); // (the levels' own setting from here)
        g_executorVtable = 0u;    // (the HQ is over: the next level's load is muted again, fast_load)
        if (g_inHq)
            switchFrameRate(rdram, ctx, runtime, 0);
        g_inHq = false;
        g_preGameEnd(rdram, ctx, runtime);
    }

    // TF_EXEC_TRACE=1 (debugging aid): the executor the main loop (0x295310, every executor's slot
    // 3) runs, logged when it changes - taGame 0x6B4C40, taUIIntro 0x6C2D10, taUIPreGame 0x6B7360.
    constexpr uint32_t kExecutorLoop = 0x00295310u;
    PS2Runtime::RecompiledFunction g_executorLoop = nullptr;

    void executorLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t last = ~0u;
        uint32_t vptr = 0u;
        if (const uint8_t *p = getConstMemPtr(rdram, getRegU32(ctx, 4)))
            std::memcpy(&vptr, p, sizeof(vptr));
        g_executorVtable = vptr;
        // (TODO 157: an HQ left for the title - the pause menu's quit to the main menu - ends without its end (0x334650,
        // preGameEnd): g_inHq stayed set, and the next HQ - a new game's - was never given its rate: it ran at the
        // console's 30 (the user's playtest of 2026-11-04 15:35: the HQ at 240, the quit, a new game, its HQ at 30 and
        // no "the HQ at"; "the HQ ends" only when that second HQ ended). The title's executor ends the HQ here.)
        if (g_inHq && vptr == 0x006C2D10u)
        {
            std::fprintf(stderr, "[tf] the HQ left for the title without its end: its rate taken back (vblank %llu)\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
            g_hqMoments.store(false);
            switchFrameRate(rdram, ctx, runtime, 0);
            g_inHq = false;
        }
        // (The hook is in at every rate for the line above; the rest only where it was before: with
        // the HQ's own rate, or traced.)
        static const bool trace = std::getenv("TF_EXEC_TRACE") != nullptr;
        if (vptr != last && !(g_hqRate || trace))
            last = vptr;
        if (vptr != last)
        {
            // The title and its menus (taUIIntro): one full picture a frame in the GS's field mode -
            // the display reads the even rows one VBlank, the odd rows the next, both from the same
            // frame. Shown a field at a time over the whole height (the presenter's way at the
            // console's rate, for frames whose fields are two moments) the letters and the whole
            // picture shook by half a line every VBlank (the user, 2026-10-26, the difficulty screen
            // on the other PC). Shown whole, as a level's frames are; back to the rate's rule after.
            // TF_MENU_FIELDS=1: a field a VBlank, as before.
            constexpr uint32_t kUIIntroVtable = 0x006C2D10u;
            static const bool menuFields = std::getenv("TF_MENU_FIELDS") && *std::getenv("TF_MENU_FIELDS") == '1';
            if (!menuFields && vptr == kUIIntroVtable)
                ps2_host_display::setShownField(0);
            else if (!menuFields && last == kUIIntroVtable)
                ps2_host_display::setShownField(g_levelRate != 0 && !inBetweenNow() ? 0 : -1);
            last = vptr;
            if (trace)
                std::fprintf(stderr, "[tf] executor %08x (vtable %08x) at vblank %llu\n", getRegU32(ctx, 4), vptr,
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
        }
        // An HQ starting, whenever one does - also the HQ the game comes back to through a warp gate
        // after a level begun from such an HQ (that level ran in no executor of its own, so the
        // vtable seen last is still the HQ's: levelAfterHq). Gated on the vtable changing, as it
        // was, that second HQ ran at the level's rate (test PC, 2026-10-28).
        {
            if (vptr == kPreGameVtable && g_hqRate && !g_inHq)
            {
                // Off unless asked for (TF_HQ_MOMENTS=1): in the HQ they tore Optimus's grille while
                // he sits (the user, 2026-10-23; about one picture in eight, none without them).
                static const bool hqMoments = []()
                {
                    const char *env = std::getenv("TF_HQ_MOMENTS");
                    return env && *env && *env != '0';
                }();
                const int hqRate = g_hqRate < 0 ? g_frameRate : g_hqRate;
                std::fprintf(stderr, "[tf] the HQ at %d frames a second%s\n", hqRate,
                             hqMoments && hqRate <= 30 ? ", pictures at the screen's refresh" : "");
                g_hqMoments.store(hqMoments && hqRate <= 30); // (before the switch: the renderer's moments)
                switchFrameRate(rdram, ctx, runtime, hqRate);
                g_inHq = true;
                if (hqRate > 30)
                    startAdapting(); // (as a level: down the rate ladder when the host cannot keep it)
            }
        }
        g_executorLoop(rdram, ctx, runtime);
    }

    // A share of the way taken each frame at 30 (x += (target - x) x share), for a frame at the
    // level's rate: as far in a 30th of a second as at 30, 1 - (1 - share)^(30/R).
    float shareAtLevelRate(float atThirty)
    {
        if (g_levelRate == 0 || !(atThirty > 0.0f && atThirty < 1.0f))
            return atThirty;
        if (g_levelRate == 60)
            return 1.0f - std::sqrt(1.0f - atThirty);
        return 1.0f - std::pow(1.0f - atThirty, 30.0f / static_cast<float>(g_levelRate));
    }

    // The words of .sdata padding (0x6CD2A0..0x6CD2FC, no code of the game's uses them) that
    // loads of per-frame constants are patched to read (config/overrides.toml), kept every frame
    // for the level's rate: at 30 the game's own constant, bit for bit (copied from where the
    // game keeps it); at R a share of the way a frame made 1 - (1 - s)^(30/R) (shareAtLevelRate),
    // a factor a frame f^(30/R), an amount a frame 30/R of it. As far, as much, as often in a
    // 30th of a second as at 30. The first five came with 60 (the walk's and the truck's blends,
    // the air steering); the rest are the frame-counting code an analysis agent found and
    // checked site by site (docs/FRAME_RATE.md, P2): the player's other states and motion
    // shaders, the cameras, the physics smoothing, enemies, bosses, the drop ship, the sidekick,
    // a weapon, the car state, a shader's drift, snow, a beam, the tree fades, sound fades and
    // the rumble baseline.
    enum class PerFrame
    {
        Share,  // x += (target - x) x s
        Factor, // x *= f
        Amount, // x += c (or a limit of c a frame)
    };
    struct FrameWord
    {
        uint32_t address; // the word the patched loads read
        uint32_t source;  // the game's constant (its value at 30), or 0: `atThirty`
        PerFrame kind;
        float atThirty = 0.0f;
    };
    constexpr FrameWord kFrameWords[] = {
        {0x006CD2A0u, 0u, PerFrame::Share, 0.2f},     // the walk's blends, the glide camera, the sidekick's fall
        {0x006CD2A4u, 0u, PerFrame::Share, 0.3f},     // the walk's, the fall's animation weight, the cameras
        {0x006CD2A8u, 0u, PerFrame::Amount, 0.4f},    // the stick's steering in the air
        {0x006CD2ACu, 0u, PerFrame::Share, 0.03f},    // the truck's two angles
        {0x006CD2B0u, 0u, PerFrame::Share, 0.07f},
        {0x006CD2B4u, 0x006CBC64u, PerFrame::Share},  // 0.1: the player's states and motion shaders, cameras, tree fades, rumble
        {0x006CD2B8u, 0x006CBD88u, PerFrame::Share},  // 0.05: the jet pack camera
        {0x006CD2BCu, 0x006CBE18u, PerFrame::Share},  // 0.06: the camera's boom
        {0x006CD2C8u, 0x006CBA4Cu, PerFrame::Factor}, // 0.9: the vortex, Unicron
        {0x006CD2CCu, 0x006CCE1Cu, PerFrame::Factor}, // 0.995: the drop ship's spins
        {0x006CD2D0u, 0x006CE194u, PerFrame::Factor}, // 0.97: snow
        {0x006CD2D4u, 0x006CCCA4u, PerFrame::Amount}, // 0.1: the heavy grunt's aim, a searchlight's fade, the sidekick's push
        {0x006CD2D8u, 0x006CE064u, PerFrame::Amount}, // 0.01: the car state's +0x11C
        {0x006CD2DCu, 0x006CCDACu, PerFrame::Amount}, // 5 degrees: Megatron's bone
        {0x006CD2E0u, 0x006CDA44u, PerFrame::Amount}, // the drift shader's speed: a variable (0x360600 sets it per area)
        {0x006CD2E4u, 0x006CE198u, PerFrame::Amount}, // 0.2: snow's spin
        {0x006CD2E8u, 0x006CB2A4u, PerFrame::Amount}, // 0.45: the ribbon beam's spiral
        {0x006CD2ECu, 0x006CB468u, PerFrame::Amount}, // 0.007: the ambience's master fade
        {0x006CD2F0u, 0x006CBF04u, PerFrame::Amount}, // 0.0003: an ambience fade
        {0x006CD2F4u, 0x006CBE10u, PerFrame::Amount}, // 0.0005: area sounds' fades
        {0x006CD2F8u, 0x006CBD5Cu, PerFrame::Amount}, // 0.005: the wind's fade
    };
    // The physics smoothing of every body (0x1B3AE0) keeps s = (s + c p) d with c = 0.1 and
    // d = 1/1.1, a share of 1 - d a frame (and the orientation's normalise(q + c q') the same):
    // at R d^(30/R) and c = 1/d - 1, written together.
    constexpr uint32_t kSmoothingAdd = 0x006CD2C0u, kSmoothingAddSource = 0x006CB3F0u;
    constexpr uint32_t kSmoothingFactor = 0x006CD2C4u, kSmoothingFactorSource = 0x006CBADCu;
    // Starscream's fire cooldowns (0x48D7D0) reload by 0.1 in a frame that takes no time off
    // them: a cycle lasts 0.1 s and a frame on average. At R the reload is 0.1 + (1/30 s - a
    // frame) - the remainder carries over, so on average a cycle lasts as at 30.
    constexpr uint32_t kReload = 0x006CD2FCu, kReloadSource = 0x006CCDF8u;
    // The frame time of a level at the level's rate and at the console's 30 (setFrameWords, every
    // frame; 0 at 30): the Mini-Cons' cooldowns count down as the console would (abilityUpdate).
    float g_levelFramePeriod = 0.0f, g_consoleFramePeriod = 0.0f;

    void setEcoScale(uint8_t *rdram); // (the draw distance, below)
    // (The forest's give-way, below: each frame's interval and work as the frame-rate ladder sees them, and whether
    // the forest would still give way - the ladder steps a rate down only once it would not.)
    void forestFrameSample(double intervalMs, double workMs, bool counts);
    bool forestCanGiveWay();

    void setFrameWords(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setEcoScale(rdram);
        const auto read = [rdram](uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        };
        const auto write = [rdram](uint32_t address, float value) { std::memcpy(rdram + address, &value, sizeof(value)); };
        const float k = g_levelRate != 0 ? 30.0f / static_cast<float>(g_levelRate) : 1.0f;
        for (const FrameWord &w : kFrameWords)
        {
            const float atThirty = w.source != 0u ? read(w.source) : w.atThirty;
            float value = atThirty;
            if (g_levelRate != 0)
            {
                switch (w.kind)
                {
                case PerFrame::Share:
                    value = shareAtLevelRate(atThirty);
                    break;
                case PerFrame::Factor:
                    value = std::pow(atThirty, k);
                    break;
                case PerFrame::Amount:
                    value = atThirty * k;
                    break;
                }
            }
            write(w.address, value);
        }
        if (g_levelRate == 0)
        {
            write(kSmoothingAdd, read(kSmoothingAddSource));
            write(kSmoothingFactor, read(kSmoothingFactorSource));
            write(kReload, read(kReloadSource));
            g_levelFramePeriod = g_consoleFramePeriod = 0.0f;
            return;
        }
        const float factor = std::pow(read(kSmoothingFactorSource), k);
        write(kSmoothingFactor, factor);
        write(kSmoothingAdd, 1.0f / factor - 1.0f);
        float consolePeriod = 0.0f, period = 0.0f;
        {
            decomp::GuestScope scope(rdram, ctx, runtime);
            consolePeriod = tf::timing::ConsoleFramePeriod().v;
            period = tf::timing::LevelFramePeriod(g_levelRate).v;
        }
        write(kReload, read(kReloadSource) + (consolePeriod - period));
        g_levelFramePeriod = period;
        g_consoleFramePeriod = consolePeriod;
    }

    // The controller turns its model toward the way it goes by a share every frame (+0x1F0,
    // 0x47D6C0 in a0; 0 does not turn, 1 or more turns at once), set by some twenty places
    // in the states. At the PC rate the share is made for the frame (shareAtLevelRate) -
    // remembered, so the next frame starts from the state's own value again, unless a state
    // set another.
    constexpr uint32_t kControllerMove = 0x0047D6C0u;
    constexpr uint32_t kControllerTurnShare = 0x1F0u;
    PS2Runtime::RecompiledFunction g_controllerMove = nullptr;
    struct TurnShare
    {
        float set = 0.0f;  // what a state set
        float made = 0.0f; // what it was made for the frame
    };
    std::unordered_map<uint32_t, TurnShare> g_turnShares; // by controller (cleared with each level)

    void controllerMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t controller = getRegU32(ctx, 4);
        if (uint8_t *field = g_levelRate != 0 ? getMemPtr(rdram, controller + kControllerTurnShare) : nullptr)
        {
            float share = 0.0f;
            std::memcpy(&share, field, sizeof(share));
            TurnShare &t = g_turnShares[controller];
            if (share == t.made)
                share = t.set; // (unchanged since: the state's own value)
            const float made = shareAtLevelRate(share);
            t = {share, made};
            std::memcpy(field, &made, sizeof(made));
        }
        g_controllerMove(rdram, ctx, runtime);
    }

    // The cars (the truck, and every other vehicle: reCar). The manager steps them once a frame
    // (0x3231D0, a0 the manager, f12 the frame time: wheels, springs, tyres, then each body,
    // 0x1C59F0) and then copies each body's place to its vehicle (0x3230D0, through 0x319D10).
    // The step takes the frame time, but its stiff springs and tyre forces were tuned at 1/30 s:
    // at 1/60 the truck pulled away 17% slower in its first second and gathered speed 8% more
    // slowly. At the PC rate the cars step every 30th of a second as at 30 - every R/30 frames,
    // by those frames' time (with the slow motion's scale in it) - and a car's wheels and tyres
    // keep the period of 30 its reset gives them (0x31F7D8: rates.cpp). In the frames between,
    // each body moves on by its velocity for the time since the step (place and turn), shown
    // and seen by the game, and is put back before the next step. At a rate that is not a
    // multiple of 30 a step falls inside a frame: it takes the part of the frame before it, and
    // the bodies move on by the rest.
    constexpr uint32_t kCarsStep = 0x003231D0u, kCarsPlace = 0x003230D0u;
    constexpr uint32_t kCarsCount = 0x0Cu, kCarsArray = 0x14u, kCarStride = 0xB0u;
    constexpr uint32_t kVehicleCar = 0x3Cu, kCarBody = 0x04u, kVehiclePlace = 0x10u, kVehicleLookAhead = 0x90u;
    constexpr uint32_t kBodyPlace = 0x40u, kBodyTurn = 0x50u, kBodyVelocity = 0x60u, kBodyMassCentre = 0x90u,
                       kBodySpin = 0xA0u;
    PS2Runtime::RecompiledFunction g_carsStep = nullptr, g_carsPlace = nullptr;
    int g_carsPhase = 0;         // 30 a frame since the last step: the cars step at R
    bool g_carsMovedOn = false;  // this frame the bodies move on from the last step
    float g_carsHeldTime = 0.0f; // by this much: the time since it
    struct MovedBody
    {
        uint32_t body = 0u;
        float stepped[12] = {}; // place, turn, mass centre after the last step
        float shown[12] = {};   // and moved on
    };
    std::vector<MovedBody> g_movedBodies;

    // The bodies as the last step left them, unless something moved them since.
    void putBodiesBack(uint8_t *rdram)
    {
        for (const MovedBody &m : g_movedBodies)
        {
            uint8_t *b = getMemPtr(rdram, m.body);
            if (!b)
                continue;
            float now[12];
            std::memcpy(now, b + kBodyPlace, 8 * sizeof(float));
            std::memcpy(now + 8, b + kBodyMassCentre, 4 * sizeof(float));
            if (std::memcmp(now, m.shown, sizeof(now)) != 0)
                continue;
            std::memcpy(b + kBodyPlace, m.stepped, 8 * sizeof(float));
            std::memcpy(b + kBodyMassCentre, m.stepped + 8, 4 * sizeof(float));
        }
        g_movedBodies.clear();
    }

    // A state's level made before the cars kept the period of 30: the cars reset at its rate
    // (their period that rate's) take the period of 30, as a reset now gives them. Once, at the
    // first step after the state was loaded.
    void pinCarPeriods(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t manager)
    {
        if (g_unpinnedCarPeriod == 0.0f)
            return;
        const float unpinned = g_unpinnedCarPeriod;
        g_unpinnedCarPeriod = 0.0f;
        const uint8_t *m = getConstMemPtr(rdram, manager);
        if (!m)
            return;
        decomp::GuestScope scope(rdram, ctx, runtime);
        const decomp::f32 period = tf::timing::ConsoleFramePeriod();
        const decomp::f32 rate = decomp::f32(1.0f) / period; // (as the reset works it out)
        uint32_t count = 0u, vehicle = 0u, pinned = 0u;
        std::memcpy(&count, m + kCarsCount, sizeof(count));
        std::memcpy(&vehicle, m + kCarsArray, sizeof(vehicle));
        for (uint32_t i = 0; i < count && i < 64u; ++i, vehicle += kCarStride)
        {
            uint32_t car = 0u;
            const uint8_t *v = getConstMemPtr(rdram, vehicle);
            uint8_t *c = v ? (std::memcpy(&car, v + kVehicleCar, sizeof(car)), getMemPtr(rdram, car)) : nullptr;
            float carPeriod = 0.0f;
            if (!c || (std::memcpy(&carPeriod, c + kCarPeriod, sizeof(carPeriod)), carPeriod != unpinned))
                continue;
            std::memcpy(c + kCarPeriod, &period.v, sizeof(float));
            std::memcpy(c + kCarRate, &rate.v, sizeof(float));
            ++pinned;
        }
        if (pinned != 0u)
            std::fprintf(stderr, "[tf] %u cars given the period of 30\n", pinned);
    }

    // (TF_CAR_LOG=1, a test aid - TODO 154: each car's body at every call of the cars' step, stepped or not - its
    // place, turn (w x y z), velocity and spin - as the frame's other code left it)
    std::atomic<uint32_t> g_carLogBody{0u}; // (the first car's body, for TF_CALL_WATCH=car)
    void logCars(uint8_t *rdram, PS2Runtime *runtime, uint32_t manager, bool steps)
    {
        static const bool on = std::getenv("TF_CAR_LOG") && *std::getenv("TF_CAR_LOG") == '1';
        if (!on)
            return;
        const uint8_t *m = getConstMemPtr(rdram, manager);
        if (!m)
            return;
        uint32_t count = 0u, vehicle = 0u;
        std::memcpy(&count, m + kCarsCount, sizeof(count));
        std::memcpy(&vehicle, m + kCarsArray, sizeof(vehicle));
        for (uint32_t i = 0; i < count && i < 8u; ++i, vehicle += kCarStride)
        {
            uint32_t car = 0u, body = 0u;
            const uint8_t *v = getConstMemPtr(rdram, vehicle);
            const uint8_t *c = v ? (std::memcpy(&car, v + kVehicleCar, sizeof(car)), getConstMemPtr(rdram, car)) : nullptr;
            const uint8_t *b = c ? (std::memcpy(&body, c + kCarBody, sizeof(body)), getConstMemPtr(rdram, body)) : nullptr;
            if (!b)
                continue;
            if (i == 0u)
                g_carLogBody.store(body);
            float f[16];
            std::memcpy(f, b + kBodyPlace, 3 * sizeof(float));
            std::memcpy(f + 3, b + kBodyTurn, 4 * sizeof(float));
            std::memcpy(f + 7, b + kBodyVelocity, 3 * sizeof(float));
            std::memcpy(f + 10, b + kBodySpin, 3 * sizeof(float));
            std::fprintf(stderr, "[tf:car] %llu %u %d %.4f %.4f %.4f  %.5f %.5f %.5f %.5f  %.4f %.4f %.4f  %.4f %.4f %.4f  body %08x\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), i, steps ? 1 : 0, f[0], f[1],
                         f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], body);
        }
    }


    void carsStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        logCars(rdram, runtime, getRegU32(ctx, 4), g_levelRate == 0 || g_carsPhase + 30 >= g_levelRate);
        if (g_levelRate == 0)
        {
            g_carsMovedOn = false;
            g_carsPhase = 0;
            g_carsHeldTime = 0.0f;
            putBodiesBack(rdram);
            g_carsStep(rdram, ctx, runtime);
            return;
        }
        pinCarPeriods(rdram, ctx, runtime, getRegU32(ctx, 4));
        g_carsPhase += 30;
        if (g_carsPhase < g_levelRate)
        {
            g_carsMovedOn = true;
            g_carsHeldTime += ctx->f[12];
            ctx->pc = getRegU32(ctx, 31); // (no step this frame)
            return;
        }
        g_carsPhase -= g_levelRate; // (30ths of this frame after the step)
        const float frame = ctx->f[12];
        const float before = g_carsPhase == 0 ? frame : frame * (static_cast<float>(30 - g_carsPhase) / 30.0f);
        putBodiesBack(rdram);
        ctx->f[12] = before + g_carsHeldTime; // (at 60, two frames: a step of 1/30 s)
        g_carsHeldTime = frame - before;
        g_carsMovedOn = g_carsPhase != 0;
        g_carsStep(rdram, ctx, runtime);
    }

    // A body moved on by its velocity and spin for `dt` (the step's own way: the turn is a
    // quaternion w,x,y,z, turned by the spin's rotation in front, 0x1AF690 / 0x1AE990).
    void moveBodyOn(uint8_t *b, float dt)
    {
        float place[4], turn[4], velocity[4], centre[4], spin[4];
        std::memcpy(place, b + kBodyPlace, sizeof(place));
        std::memcpy(turn, b + kBodyTurn, sizeof(turn));
        std::memcpy(velocity, b + kBodyVelocity, sizeof(velocity));
        std::memcpy(centre, b + kBodyMassCentre, sizeof(centre));
        std::memcpy(spin, b + kBodySpin, sizeof(spin));
        for (int i = 0; i < 3; ++i)
        {
            place[i] += velocity[i] * dt;
            centre[i] += velocity[i] * dt;
        }
        const float rate = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]);
        if (rate > 1e-6f)
        {
            const float half = 0.5f * rate * dt, s = std::sin(half) / rate;
            const float dw = std::cos(half), dx = spin[0] * s, dy = spin[1] * s, dz = spin[2] * s;
            const float w = turn[0], x = turn[1], y = turn[2], z = turn[3];
            float q[4] = {dw * w - dx * x - dy * y - dz * z, dw * x + dx * w + dy * z - dz * y,
                          dw * y - dx * z + dy * w + dz * x, dw * z + dx * y - dy * x + dz * w};
            const float n = 1.0f / std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (float &c : q)
                c *= n;
            std::memcpy(turn, q, sizeof(q));
        }
        std::memcpy(b + kBodyPlace, place, sizeof(place));
        std::memcpy(b + kBodyTurn, turn, sizeof(turn));
        std::memcpy(b + kBodyMassCentre, centre, sizeof(centre));
    }

    void carsPlace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_levelRate == 0 || !g_carsMovedOn)
        {
            g_carsPlace(rdram, ctx, runtime);
            return;
        }
        // Between steps: each body moved on from where the step left it, and its vehicle set to
        // the place as the copy does.
        putBodiesBack(rdram);
        const uint32_t manager = getRegU32(ctx, 4);
        const uint8_t *m = getConstMemPtr(rdram, manager);
        const float period = g_carsHeldTime; // (the time since the step)
        if (m && m[8] != 0u && period > 0.0f)
        {
            uint32_t count = 0u, vehicle = 0u;
            std::memcpy(&count, m + kCarsCount, sizeof(count));
            std::memcpy(&vehicle, m + kCarsArray, sizeof(vehicle));
            for (uint32_t i = 0; i < count && i < 64u; ++i, vehicle += kCarStride)
            {
                uint32_t car = 0u, body = 0u;
                const uint8_t *v = getConstMemPtr(rdram, vehicle);
                const uint8_t *c = v ? (std::memcpy(&car, v + kVehicleCar, sizeof(car)), getConstMemPtr(rdram, car)) : nullptr;
                uint8_t *b = c ? (std::memcpy(&body, c + kCarBody, sizeof(body)), getMemPtr(rdram, body)) : nullptr;
                if (!b)
                    continue;
                MovedBody moved;
                moved.body = body;
                std::memcpy(moved.stepped, b + kBodyPlace, 8 * sizeof(float));
                std::memcpy(moved.stepped + 8, b + kBodyMassCentre, 4 * sizeof(float));
                moveBodyOn(b, period);
                std::memcpy(moved.shown, b + kBodyPlace, 8 * sizeof(float));
                std::memcpy(moved.shown + 8, b + kBodyMassCentre, 4 * sizeof(float));
                g_movedBodies.push_back(moved);
                // (0x319D10: the vehicle's place; its look-ahead point moves with it)
                uint8_t *vw = getMemPtr(rdram, vehicle);
                float place[4], before[4], ahead[4];
                std::memcpy(before, vw + kVehiclePlace, sizeof(before));
                std::memcpy(place, b + kBodyPlace, sizeof(place));
                std::memcpy(ahead, vw + kVehicleLookAhead, sizeof(ahead));
                for (int k = 0; k < 3; ++k)
                    ahead[k] += place[k] - before[k];
                std::memcpy(vw + kVehiclePlace, place, sizeof(place));
                std::memcpy(vw + kVehicleLookAhead, ahead, sizeof(ahead));
            }
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    // TODO 154 (the car pitching up, its back wheels in the air): the world's physics step (0x2C60D0, from taGame's
    // update right after the cars' step: 0x1B5E50 - every rigid body's contacts with the world, a car's body among them)
    // runs every frame, the cars' step only every 30th of a second; in the frames between, a car's body is the one moved
    // on for the picture (carsPlace, kept until the next frame's carsPlace) - and the physics step took that for where the
    // car is. Driving into a wall at 240 the moved-on body was 0.2-0.5 units into it when the physics step met it, and
    // pushed it out up the wall's face: +5.5 upward and a pitching spin, the car thrown 1.8 up (at 30 the cars' own step
    // stops it before the wall: +1.5, 0.6 up). So before the physics step the bodies are put back where the last step
    // left them (carsPlace moves them on again after it): the physics step works from the car's stepped place every
    // frame, as at 30. Measured on a scripted drive into Amazon's ledge: the 30's response at 240 (the wall's pitch
    // -0.51 deg on average against 30's -0.54; -1.55 before; the highest point 11.3 against 11.2; 12.6 before).
    // TF_CAR_WORLD=0: as before (the physics step on the moved-on bodies).
    constexpr uint32_t kPhysicsStep = 0x002C60D0u;
    PS2Runtime::RecompiledFunction g_physicsStep = nullptr;

    void physicsStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool asBefore = std::getenv("TF_CAR_WORLD") && *std::getenv("TF_CAR_WORLD") == '0';
        if (g_levelRate != 0 && !asBefore)
            putBodiesBack(rdram); // (nothing moved on: a no-op)
        g_physicsStep(rdram, ctx, runtime);
    }

    // VBlanks in a level at the PC rate that last as long as `atSixty` at 60 (the hooks below
    // tell frames apart by the VBlank count).
    uint64_t vblanksLike(uint64_t atSixty)
    {
        return atSixty * static_cast<uint64_t>(std::max(vblankRateNow(), 60)) / 60u;
    }

    // In the air (0x470F70, every frame of it: the controller in a1, the frame time in f12)
    // the vertical velocity takes gravity (0x6CF9B4) x the frame time and then moves the
    // controller: a jump rises v t - g t^2/2 - g dt t/2, the last part from the step. At 60
    // the step is half, and a jump went 4% higher (5.80 instead of 5.60 in place). The first
    // frame in the air at the PC rate the velocity takes another g (1/30 - dt) / 2 = g dt
    // (R/30 - 1) / 2: then every jump and fall follows the arc it has at 30.
    constexpr uint32_t kAirborneUpdate = 0x00470F70u;
    constexpr uint32_t kGravity = 0x006CF9B4u;
    constexpr uint32_t kControllerVerticalVelocity = 0x28u;
    PS2Runtime::RecompiledFunction g_airborneUpdate = nullptr;
    std::array<std::pair<uint32_t, uint64_t>, 16> g_airborneSeen{}; // controller, last tick in the air

    void airborneUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t controller = getRegU32(ctx, 5);
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        auto *slot = &g_airborneSeen[0];
        bool known = false;
        for (auto &seen : g_airborneSeen)
        {
            if (seen.first == controller)
            {
                slot = &seen;
                known = true;
                break;
            }
            if (seen.second < slot->second)
                slot = &seen;
        }
        const bool takeOff = !known || tick > slot->second + vblanksLike(4u); // (not in the air a moment ago)
        *slot = {controller, tick};
        if (g_levelRate != 0 && takeOff)
        {
            float gravity = 0.0f, vertical = 0.0f;
            std::memcpy(&gravity, rdram + kGravity, sizeof(gravity));
            if (uint8_t *v = getMemPtr(rdram, controller + kControllerVerticalVelocity))
            {
                const float catchUp = 0.5f * (static_cast<float>(g_levelRate) / 30.0f - 1.0f); // (0.5 at 60)
                std::memcpy(&vertical, v, sizeof(vertical));
                vertical += gravity * ctx->f[12] * catchUp;
                std::memcpy(v, &vertical, sizeof(vertical));
            }
        }
        g_airborneUpdate(rdram, ctx, runtime);
    }

    // A wall or a rock the walk runs into: the physics' contact callback (0x28BAD0, returning
    // to 0x28C154) sets the controller's velocity to the part along the surface (0x47C720);
    // the frame moves with that, and the next frame's walk (0x47B910) starts from its length
    // (0x47C500 sets the speed, +0x44, from the velocity, +0x20). Contacts come in a pattern
    // of frames (every other frame against a rock: into it, then along it), each one taking
    // its share of the speed, while the walk adds acceleration x the frame time: at 60 twice
    // as many contacts a second took the speed down to half (the player slid along a rock at
    // 2 units a second instead of 4-6). At the PC rate the speed is cut as often a second as
    // at 30: a contact adds 30/R to a credit, and only one that brings it to 1 cuts the speed
    // (at 60 every other one); the others only turn the frame's movement along the surface,
    // and the next frame starts from the speed before it.
    constexpr uint32_t kSetWorldVelocity = 0x0047C720u;
    constexpr uint32_t kContactVelocityReturn = 0x0028C154u;
    constexpr uint32_t kSpeedFromVelocity = 0x0047C500u;
    constexpr uint32_t kControllerVelocity = 0x20u, kControllerSpeed = 0x44u;
    PS2Runtime::RecompiledFunction g_setWorldVelocity = nullptr, g_speedFromVelocity = nullptr;
    struct ContactSpeed
    {
        uint32_t controller = 0u;
        uint64_t lastSeen = 0u; // (tick)
        float credit = 0.0f;    // 30/R a contact; at 1 the contact cuts the speed
        bool pending = false;   // this one kept it: the next frame starts from `speed`
        float speed = 0.0f;
    };
    std::array<ContactSpeed, 16> g_contactSpeeds{};

    ContactSpeed &contactSpeedOf(uint32_t controller)
    {
        ContactSpeed *slot = &g_contactSpeeds[0];
        for (ContactSpeed &c : g_contactSpeeds)
        {
            if (c.controller == controller)
                return c;
            if (c.lastSeen < slot->lastSeen)
                slot = &c;
        }
        *slot = {};
        slot->controller = controller;
        slot->credit = 1.0f - 30.0f / static_cast<float>(g_levelRate); // (the first contact cuts)
        return *slot;
    }

    // TODO 183 (a human-size Autobot's ragdoll bouncing ever higher and off the map at 240): the ragdoll state's update
    // (0x477700, from 0x46D1BC) gives the controller each frame the velocity its root moved with since the last frame -
    // (root now - root then, +0x180) / dt - through 0x47C720 (return 0x4777D0). That velocity goes back into the next
    // frame's ragdoll, so the root's frame-to-frame wobble, divided by a dt of 1/240 instead of 1/30, came back eight
    // times as large: measured (Amazon, a fall from 70), the human ragdoll settled within 3 units at 30, at 240 it
    // bounced up to 35 units and travelled 120 for six seconds (the full-size robot hides it: its wobble is small
    // against its size); with the call left out it settled as at 30. At the PC rate the velocity is given as at 30:
    // once every 30th of a second, the mean of the frames' velocities (= the move over that 30th / its time); the
    // frames between keep it. TF_RAGDOLL_RATE=0: as before.
    constexpr uint32_t kRagdollVelocityReturn = 0x004777D0u;
    struct RagdollVelocity
    {
        uint32_t controller = 0u;
        uint64_t lastSeen = 0u; // (tick)
        float credit = 0.0f;    // 30/R a frame; at 1 the velocity is given
        float sum[3] = {};
        int frames = 0;
    };
    std::array<RagdollVelocity, 8> g_ragdollVelocities{};

    // true: let the call through (with the mean put in its vector); false: leave it out this frame
    bool ragdollVelocityAt30(uint8_t *rdram, uint32_t controller, uint32_t vector, uint64_t tick)
    {
        static const bool off = std::getenv("TF_RAGDOLL_RATE") && *std::getenv("TF_RAGDOLL_RATE") == '0';
        float *v = reinterpret_cast<float *>(getMemPtr(rdram, vector));
        if (off || g_levelRate <= 30 || !v)
            return true;
        RagdollVelocity *slot = &g_ragdollVelocities[0];
        bool known = false;
        for (RagdollVelocity &r : g_ragdollVelocities)
        {
            if (r.controller == controller)
            {
                slot = &r;
                known = true;
                break;
            }
            if (r.lastSeen < slot->lastSeen)
                slot = &r;
        }
        if (!known || tick > slot->lastSeen + vblanksLike(4u)) // (a new ragdoll: its first frame given at once)
        {
            *slot = {};
            slot->controller = controller;
            slot->credit = 1.0f - 30.0f / static_cast<float>(g_levelRate);
        }
        slot->lastSeen = tick;
        for (int i = 0; i < 3; ++i)
            slot->sum[i] += v[i];
        ++slot->frames;
        slot->credit += 30.0f / static_cast<float>(g_levelRate);
        if (slot->credit < 0.999f)
            return false;
        slot->credit -= 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            v[i] = slot->sum[i] / static_cast<float>(slot->frames);
            slot->sum[i] = 0.0f;
        }
        slot->frames = 0;
        return true;
    }

    void setWorldVelocity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), controller = getRegU32(ctx, 4);
        if (ra == kRagdollVelocityReturn &&
            !ragdollVelocityAt30(rdram, controller, getRegU32(ctx, 5), runtime->eeScheduler().currentVSyncTick()))
        {
            ctx->pc = ra; // (the frame keeps the velocity given last)
            return;
        }
        const uint8_t *speed = g_levelRate != 0 && ra == kContactVelocityReturn ? getMemPtr(rdram, controller + kControllerSpeed) : nullptr;
        float before = 0.0f;
        if (speed)
            std::memcpy(&before, speed, sizeof(before));
        g_setWorldVelocity(rdram, ctx, runtime);
        if (!speed || ctx->pc != ra)
            return; // (not finished here: it stopped at a checkpoint)
        float after = 0.0f;
        std::memcpy(&after, speed, sizeof(after));
        if (after >= before)
            return;
        // (Tried against 11 runs into the rock at 30: keeping halfway, or half the cut as a
        // factor, slid on too far; every other contact at 60 came closest.)
        ContactSpeed &c = contactSpeedOf(controller);
        c.lastSeen = runtime->eeScheduler().currentVSyncTick();
        c.credit += 30.0f / static_cast<float>(g_levelRate);
        if (c.credit >= 0.999f)
        {
            c.credit -= 1.0f; // (this one cuts it)
        }
        else
        {
            c.pending = true;
            c.speed = before;
        }
    }

    void speedFromVelocity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t controller = getRegU32(ctx, 4);
        for (ContactSpeed &c : g_contactSpeeds)
        {
            if (c.controller != controller || !c.pending)
                continue;
            c.pending = false;
            float *v = reinterpret_cast<float *>(getMemPtr(rdram, controller + kControllerVelocity));
            const float length = v ? std::hypot(v[0], v[1]) : 0.0f;
            if (g_levelRate != 0 && length > 1e-4f &&
                runtime->eeScheduler().currentVSyncTick() <= c.lastSeen + vblanksLike(2u)) // (the frame after the contact)
            {
                v[0] *= c.speed / length;
                v[1] *= c.speed / length;
            }
            break;
        }
        g_speedFromVelocity(rdram, ctx, runtime);
    }

    // The Mini-Cons' cooldowns. An ability's update (0x3EF1F0, a0 the ability, f12 the frame
    // time; 0x28E260 calls it for each Mini-Con the player carries) takes the frame time off its
    // timer (+0x44) and, when the timer is below 0 and the button asks (0x3EE890), fires and sets
    // the timer to the cooldown (+0x48, less a bonus: 0x3EF28C) - the part of the frame after the
    // timer ran out is lost. A cycle lasts a whole number of frames: at 30 the Impact Gun's
    // 0.17 s takes six (0.2 s), at 60 eleven (0.183 s, 9% more shots); a cooldown of 0.15 s ten
    // 60ths or nine as the floats round. At the PC rate a timer set again after it ran out lasts
    // the console's frames: as many as the console's countdown from that value takes (k, by its
    // own float steps), k x R/30 frames at R - set at the update after (nothing reads it between
    // but the HUD's bar) to k frames of 30 less half a frame of R. At a rate that is not a
    // multiple of 30 the part of a frame the last cycle ran over carries over instead of the
    // half, so that on average a cycle lasts as at 30. At 30 the game's own.
    // TF_CONSOLE_COOLDOWNS=0 leaves them to the game; TF_COOLDOWN_LOG=1 logs each one retimed.
    constexpr uint32_t kAbilityUpdate = 0x003EF1F0u;
    constexpr uint32_t kAbilityTimer = 0x44u;
    PS2Runtime::RecompiledFunction g_abilityUpdate = nullptr;
    struct CooldownSeen
    {
        float timer = 0.0f; // at the last update's start
        float frame = 0.0f; // and that update's frame time
    };
    std::unordered_map<uint32_t, CooldownSeen> g_cooldowns; // by ability (cleared with each level)

    // A cooldown `set` after the timer ran out (`ranOut`, below 0) by this frame's time `frame`:
    // the value that counts down to below 0 at the level's rate as late as the console's does.
    float consoleCooldown(float set, float ranOut, float frame)
    {
        // The console's frame time: the level's at 30, or (in slow motion) this one's x R/30.
        const float consoleFrame = frame == g_levelFramePeriod && g_consoleFramePeriod > 0.0f
                                       ? g_consoleFramePeriod
                                       : static_cast<float>(static_cast<double>(frame) * g_levelRate / 30.0);
        if (!(consoleFrame > 0.0f) || !(set < 1000.0f * consoleFrame))
            return set;
        int frames = 0;
        decomp::f32 timer(set);
        do
        {
            timer = timer - decomp::f32(consoleFrame);
            ++frames;
        } while (!(timer < decomp::f32(0.0f)));
        const double cycle = frames * static_cast<double>(consoleFrame);
        const double over = g_levelRate % 30 != 0 && ranOut >= -frame ? ranOut : -0.5 * frame;
        return static_cast<float>(cycle + over);
    }

    void abilityUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool retime = []() { const char *env = std::getenv("TF_CONSOLE_COOLDOWNS"); return !env || *env != '0'; }();
        static const bool log = []() { const char *env = std::getenv("TF_COOLDOWN_LOG"); return env && *env == '1'; }();
        const uint32_t ability = getRegU32(ctx, 4);
        uint8_t *field = retime && g_levelRate != 0 && ctx->pc == kAbilityUpdate ? getMemPtr(rdram, ability + kAbilityTimer) : nullptr;
        if (field)
        {
            const float frame = ctx->f[12];
            float timer = 0.0f;
            std::memcpy(&timer, field, sizeof(timer));
            const auto seen = g_cooldowns.find(ability);
            if (seen != g_cooldowns.end() && frame > 0.0f)
            {
                // (what the last update left, unless it set the timer again)
                const decomp::f32 left = decomp::f32(seen->second.timer) - decomp::f32(seen->second.frame);
                if (left < decomp::f32(0.0f) && timer > 0.0f && decomp::f32(timer) != left)
                {
                    const float made = consoleCooldown(timer, left.v, frame);
                    if (log)
                        std::fprintf(stderr, "[tf] cooldown %08x: %.4f s set (ran out by %.4f) -> %.4f s, %.2f frames\n", ability, timer,
                                     -left.v, made, made / frame);
                    timer = made;
                    std::memcpy(field, &timer, sizeof(timer));
                }
            }
            g_cooldowns[ability] = {timer, frame};
        }
        g_abilityUpdate(rdram, ctx, runtime);
    }

    // The homing missiles (the player's Mini-Con, and those of TidalWave, the heavy grunt and
    // Cyclonus: taWeaponLogic2_HomingMissile, its update 0x3D2510) steer by angles a frame. The
    // weapon data's TurnDegree (1.5 degrees for the player's) is the most a missile turns toward
    // its target in a frame (0x2A46A0, the limit in f12), ramped up from 0 over the launch; in a
    // loop it spins by LoTurnDegree or HiTurnDegree (1 and 3) a frame, made once as a quaternion
    // (0x1AF810, the angle in f12) and applied each frame of the loop's 0.7 s. Its speed, speed-up
    // and range go by the frame time. At 60 a missile turned twice as many degrees a second: it
    // homed in twice as tightly, hitting where the console's overshot and looped (the user: "the
    // homing missile especially moves at twice the speed", the enemies died sooner). At the PC
    // rate the angles the update asks for are 30/R of theirs (from its calls only: by the return
    // address), as far a 30th of a second as at 30. TF_CONSOLE_STEERING=0 leaves them.
    constexpr uint32_t kTurnToward = 0x002A46A0u, kAxisAngle = 0x001AF810u;
    constexpr uint32_t kMissileTurns[] = {0x003D274Cu, 0x003D2D48u, 0x003D341Cu,  // launch, homing, recovering
                                          0x003D2ADCu, 0x003D2C70u, 0x003D302Cu, 0x003D31B4u}; // (behind a flag nothing sets)
    constexpr uint32_t kMissileSpins[] = {0x003D2A50u, 0x003D2BE4u, 0x003D2FA0u, 0x003D3128u};
    PS2Runtime::RecompiledFunction g_turnToward = nullptr, g_axisAngle = nullptr;

    template <size_t N>
    void scaleMissileAngle(R5900Context *ctx, const uint32_t (&returns)[N])
    {
        static const bool scale = []() { const char *env = std::getenv("TF_CONSOLE_STEERING"); return !env || *env != '0'; }();
        if (!scale || g_levelRate == 0)
            return;
        const uint32_t ra = getRegU32(ctx, 31);
        for (const uint32_t r : returns)
            if (ra == r)
            {
                ctx->f[12] *= 30.0f / static_cast<float>(g_levelRate); // (before the original: it may stop at a checkpoint)
                return;
            }
    }

    void turnToward(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scaleMissileAngle(ctx, kMissileTurns);
        g_turnToward(rdram, ctx, runtime);
    }

    void axisAngle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scaleMissileAngle(ctx, kMissileSpins);
        g_axisAngle(rdram, ctx, runtime);
    }

    void resetMotionFixes()
    {
        g_contactSpeeds = {};
        g_airborneSeen = {};
        g_turnShares.clear();
        g_cooldowns.clear();
        g_movedBodies.clear();
        g_carsPhase = g_levelRate != 0 ? g_levelRate - 30 : 0; // (the first frame steps)
        g_carsMovedOn = false;
        g_carsHeldTime = 0.0f;
    }

    // The views (0x20CE50 makes them, vtable 0x6B2CA0) keep what betwixtMode was when they
    // were made: +0x20C of the object at +4 is 0 with it (two passes, one per field) and -1
    // without (field A, the other pass with a scissor of one pixel). A level from a state made
    // at 30 switched to the PC rate (TF_60FPS=force) gets those of a field a frame, as a level
    // begun at it has.
    void setViewFields(uint8_t *rdram, int32_t field)
    {
        constexpr uint32_t kViewVtable = 0x006B2CA0u;
        constexpr uint32_t kRamBytes = PS2_RAM_SIZE;
        uint32_t views = 0u;
        for (uint32_t at = 0u; at + 8u <= kRamBytes; at += 4u)
        {
            uint32_t word = 0u;
            std::memcpy(&word, rdram + at, sizeof(word));
            if (word != kViewVtable)
                continue;
            uint32_t inner = 0u;
            std::memcpy(&inner, rdram + at + 4u, sizeof(inner));
            inner &= 0x1FFFFFFFu;
            if ((inner & 3u) != 0u || inner < 0x00100000u || inner + 0x210u > kRamBytes)
                continue;
            int32_t current = 0;
            std::memcpy(&current, rdram + inner + 0x20Cu, sizeof(current));
            if (current != 0 && current != -1)
                continue; // (not a view)
            std::memcpy(rdram + inner + 0x20Cu, &field, sizeof(field));
            ++views;
        }
        std::fprintf(stderr, "[tf] %u views made to draw %s a frame\n", views, field < 0 ? "one field" : "two fields");
    }

    // The rate the level in memory began at: one over its frame period (taGame+0x5C), the
    // console's 59.94 counted as 60. 0 when no level runs.
    int levelRateInMemory(const uint8_t *rdram)
    {
        uint32_t game = 0u, clock = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        std::memcpy(&clock, rdram + kGameClock, sizeof(clock));
        const uint8_t *p = game != 0u && clock != 0u ? getConstMemPtr(rdram, game + kGamePeriod) : nullptr;
        float period = 0.0f;
        if (p)
            std::memcpy(&period, p, sizeof(period));
        if (!(period > 0.0005f && period < 0.1f))
            return 0;
        return static_cast<int>(std::lround(60.0 / 59.94 / period));
    }

    // The level of a save state made at another rate set up for `rate`: what it kept from its
    // begin - its frame period (every system's time step), the clock's step (from now on), the
    // physics' gravity a step and the in-level movies' rate - as a level begun at `rate` has
    // it.
    void convertLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int madeAt, int rate)
    {
        using decomp::f32;
        decomp::GuestScope scope(rdram, ctx, runtime);
        const decomp::Guest &g = decomp::guest();
        const uint32_t game = g.read<uint32_t>(kGame);
        const uint32_t clock = g.read<uint32_t>(kGameClock);
        if (game == 0u || clock == 0u)
            return;
        // (rate 0: the console's 30 - its half rate byte set again first)
        const f32 period = rate != 0 ? tf::timing::LevelFramePeriod(rate) : tf::timing::ConsoleFramePeriod();
        g.write<f32>(game + kGamePeriod, period);
        // The clock in steps of the new period from the next frame on (0x41E820 sets the step).
        // Its time lags a frame - the frame after a step is set shows the time of the one
        // before again - so it is set to go on from the time the next frame would have had.
        tf::timing::GameClock gameClock;
        gameClock.address = clock;
        const f32 next = f32(gameClock.time()) + f32(gameClock.step()) * f32(gameClock.scale());
        tf::timing::GameClock_SetStep(gameClock, period);
        gameClock.timeAtStep() = next;
        g.write<f32>(kGravityStep, g.read<f32>(kGravityPerSecond) * period);
        g.write<f32>(kMovieRate, rate != 0 ? tf::timing::LevelFrameRate(rate) : tf::timing::TargetFrameRate());
    }

    // The rate adapts to the host (TF_ADAPTIVE_RATE=0 holds it). Above 60 a level runs at the
    // highest of R, R/2, R/3, ... (40 or more) and 60 that the host keeps up with in real time:
    // a scene too heavy for R plays at a lower rate instead of slower than real time - at R/2 on
    // a screen of R Hz every picture is shown for two refreshes. From each frame: the host time
    // since the last one and how much of it the EE thread waited for the host's clock
    // (EeScheduler::idleHostTime), the rest being the frame's work. Down when half a second of
    // frames took more than 1.05 periods (the median) - to the first rate whose period holds
    // the frames' work with 10% to spare - and up a step when two seconds of frames' work would
    // fit in 85% of the higher rate's period (the median) and the slowest in 105% (the 95th
    // percentile: a frame a little late is caught up by the ones after). The game's speed never
    // follows the frame rate: a frame is 1/R s of game time at every R, and the VBlanks keep the
    // frames to R a second of the host's clock (R itself at most 480). Not in turbo mode or
    // while a movie plays; frames of over 100 ms (loading) do not count. After a level begins
    // nothing for 1 s, after a change for 2 s, and no going up for 2 s after coming down -
    // twice as long each time it comes down within 5 s of going up, at most 64 s. The level
    // goes on at the new rate: what it keeps from its begin is set as for a state made at
    // another rate (convertLevel), the records of the fixes are kept, and the cars' count to
    // their next step is counted in the new rate's frames.
    //
    // With in-between pictures (TF_IN_BETWEEN) the host draws two fields a frame, which costs
    // the GS and the GPU and not the EE: frames that fall behind with them at the lowest rate
    // drop them - one field a frame, the VBlanks at the frame rate, the views drawing one
    // field's pass - and they are tried again after the same holds as a rate. Taken back
    // within 5 s of being tried twice in a row (the host cannot draw them there: at 4K the GPU
    // renderer takes 25 ms for a frame's two), they stay off for the rest of the level.
    bool g_adaptiveRate = true;
    std::vector<int> g_rates; // the rates a level may run at, highest first
    size_t g_rateIndex = 0;   // the one it runs at
    struct RateSample
    {
        float interval = 0.0f; // ms
        float work = 0.0f;     // ms
        double leftOut = 0.0;  // (TODO 165) the drawing's frames left out behind, so far (nr::framesLeftOutBehind)
    };
    std::vector<RateSample> g_rateSamples; // at the current rate
    // (TODO 165) A state just loaded: its first seconds are slow (its textures and meshes made again) and judged as the PC's
    // they took this PC's 240 to 89 at once on the user's jet bike state - nothing judged until then.
    std::chrono::steady_clock::time_point g_rateSettleUntil{};
    // (TODO 168) The ladder's first seconds after a load or a level's start, after the settle: the textures, meshes and
    // the forest still coming in. A fall then is not held against the rate (no bar, no doubling hold-off: back up once the
    // frames show room, judged on 2 s rather than 6), so the rate returns to the cap in seconds - a state loaded on a busy
    // PC fell 240 -> 60 at once, held 60 for 13 s and 80 for 7, and was not back at 240 after 45 s. Lasting misses after
    // it are judged as before (the bars and hold-offs against flapping).
    std::chrono::steady_clock::time_point g_rateRecoverUntil{};
    std::deque<std::chrono::steady_clock::time_point> g_rateFalls; // (TODO 165: the falls of the last 2 minutes)
    std::chrono::steady_clock::time_point g_lastKickTime{};
    std::chrono::nanoseconds g_lastKickIdle{0};
    std::chrono::steady_clock::time_point g_rateHeldUntil{}, g_raiseHeldUntil{}, g_rateRaisedAt{};
    double g_raiseHoldSeconds = 2.0;
    std::chrono::steady_clock::time_point g_inBetweenSince{}; // when the in-between pictures came on
    std::chrono::steady_clock::time_point g_adaptingSince{};  // when the level began (or a state was loaded)
    int g_inBetweenQuickFalls = 0;                            // taken back within 5 s, in a row
    // Per rate (by index): the frames' interval the last time it fell behind there, and when.
    std::vector<double> g_failedPeriodMs;
    std::vector<std::chrono::steady_clock::time_point> g_failedAt;
    // (N9) Per rate: how often the level fell behind there, and until when it is not tried again - a minute after
    // the first time, twice as long each time after, at most ten minutes. Before, a rate it fell from was barred
    // 30 s, and the user's level stepped 60 -> 72 -> 80 -> 60 about every half minute for the whole level
    // (runs\playtest.err, 2026-10-30): each change a change of pace mid-play.
    std::vector<int> g_failCount;
    std::vector<std::chrono::steady_clock::time_point> g_barredUntil;
    // (TODO 112) TF_RATE_LOG=1: every 10 s at a lower rung, why it does not go up; and each step down's numbers.
    const bool g_rateLog = []()
    {
        const char *e = std::getenv("TF_RATE_LOG");
        return e && *e && *e != '0';
    }();

    // The in-between pictures on or off from the next frame on (adaptFrameRate).
    void setInBetween(uint8_t *rdram, PS2Runtime *runtime, bool on)
    {
        g_inBetweenHeld = !on;
        setViewFields(rdram, inBetweenShown() ? 0 : -1);
        applyVBlankPeriod(runtime);
        const bool inBetween = inBetweenNow();
        rdram[kHalfRate] = inBetween ? 1u : 0u;
        rdram[kBetwixtMode] = inBetween ? 1u : 0u;
        ps2_host_display::setShownField(inBetween ? -1 : 0);
        ps2_host_display::setFieldsAsMoments(inBetween);
        setInBetweenDrop(inBetween);
        if (inBetween && tf::port::momentPictures())
            std::fprintf(stderr, "[tf] %d frames a second, pictures at the screen's refresh\n", g_levelRate);
        else
            std::fprintf(stderr, "[tf] %d frames a second, %d pictures\n", g_levelRate, inBetween ? 2 * g_levelRate : g_levelRate);
    }

    // ---- Frame pacing (TODO 35, 2026-11-04: as PC games do) -------------------------------------------------
    // The settings' "Frame pacing" (TF_FRAME_PACING): auto (the default), even (only rates that divide the screen's
    // refresh: every picture up the same number of refreshes - 240 Hz: 240, 120, 80, 60 ...; 144: 144, 72, 48;
    // 60: 60, 30 - the camera's turn filling the refreshes between frames), any (any rate: for a screen whose
    // refresh follows the pictures - FreeSync, G-Sync - topped a little under its refresh so it stays in its range).
    // Auto = any where the screen's refresh measures variable (the vblank watcher: its refreshes no longer keep the
    // mode's clock - what the screen does, not what the driver says it could), steady for 3 s; else even. Taken
    // when a level (or the HQ) begins. TF_RATE_STEPS=fine / divisors (tests) goes over it.
    std::atomic<int> g_pacingChoice{-1}; // (the panel's choice: 0 auto, 1 even, 2 any; -1 none - the environment's)
    // ---- The frame-rate cap (TODO 160, 2026-11-04, the user: "behave like every other PC game") --------------------
    // The settings' "Frame rate cap" (TF_FPS_CAP: 30, 60, 90, 120, 144, 165, 240, 0 = unlimited; 240 by default) is the
    // top of the levels' rate. The game's logic moves a fixed step a frame (a level reads FramePeriod when it begins:
    // its clock's step, the gravity a frame), so a PC that cannot draw the cap would play it in slow motion: the rate
    // follows what the PC manages instead - any whole rate from the cap down to 30, down on a miss that lasts, up again
    // to the highest rate the frames' measured work fits as soon as it fits (convertLevel at each change, as the
    // ladder's steps did). The FPS counter shows the pictures really drawn. TF_RATE_LADDER=1: the ladder as before
    // (even steps / fine steps, frame pacing Auto), for an A/B.
    std::atomic<int> g_wantCap{-1};     // (the panel's cap, taken on the game thread: takeSettings)
    int g_ceilingRate = 0;              // (follow: the rate the last miss came at, and until when nothing at or above it)
    std::chrono::steady_clock::time_point g_ceilingUntil{};
    int g_ceilingFails = 0;             // (follow: misses in a row - the wall's 10 s doubles with each, to 120 s; forgotten 30 s on)
    std::chrono::steady_clock::time_point g_lastFallAt{};
    // (TODO 272) The last step down: when, from which rate (its place among the rates), and whether it was held against it.
    std::chrono::steady_clock::time_point g_lastDownAt{};
    size_t g_lastDownIndex = 0;
    int g_lastDownFrom = 0;
    bool g_lastDownCounted = false;
    std::chrono::steady_clock::time_point g_rateExplainUntil{}; // (after the window came back: why a lower rate is held, said)
    double g_switchMsMax = 0.0, g_switchMsSum = 0.0; // (the changes' own cost, said with the rates' times)
    int g_switches = 0;

    bool followRates()
    {
        static const bool ladder = std::getenv("TF_RATE_LADDER") && *std::getenv("TF_RATE_LADDER") == '1';
        return !ladder;
    }
    std::atomic<bool> g_vrrSteady{false};
    // (TODO 35: the time the level spent at each rate - `[tf] frame rates: ...` every 30 s and when the ladder starts again)
    std::map<int, double> g_rateTimeMs;
    std::map<int, uint64_t> g_rateFrames; // (TODO 161: the frames run at each, so the real rate shows - a PC behind its fixed rate)
    std::chrono::steady_clock::time_point g_rateTimeSaid{};
    uint64_t g_detailFrames = 0, g_detailLow = 0; // (TODO 160: the governor's detail scale, once a frame in a level)
    float g_detailMin = 1.0f;

    void sayRateTimes(const char *when)
    {
        double all = 0.0;
        for (const auto &[rate, ms] : g_rateTimeMs)
            all += ms;
        if (all < 1000.0)
            return;
        std::string text;
        for (auto it = g_rateTimeMs.rbegin(); it != g_rateTimeMs.rend(); ++it)
        {
            char part[96];
            const uint64_t frames = g_rateFrames.count(it->first) ? g_rateFrames[it->first] : 0u;
            std::snprintf(part, sizeof(part), "%s%d %.0f%% (%.0f s, %.1f a second)", text.empty() ? "" : ", ", it->first, 100.0 * it->second / all,
                          it->second / 1000.0, it->second > 0.0 ? 1000.0 * static_cast<double>(frames) / it->second : 0.0);
            text += part;
        }
        std::fprintf(stderr, "[tf] frame rates %s: %s; %d changes, each %.2f ms on average, %.2f at most; the detail under 1 in %llu of %llu frames (at least %.3f)\n",
                     when, text.c_str(), g_switches, g_switches ? g_switchMsSum / g_switches : 0.0, g_switchMsMax,
                     static_cast<unsigned long long>(g_detailLow), static_cast<unsigned long long>(g_detailFrames), g_detailMin);
    }

    int pacingChoice()
    {
        const int set = g_pacingChoice.load(std::memory_order_relaxed);
        if (set >= 0)
            return set;
        const char *v = std::getenv("TF_FRAME_PACING");
        if (!v || !*v)
            return 0;
        if (std::strcmp(v, "even") == 0)
            return 1;
        if (std::strcmp(v, "any") == 0)
            return 2;
        return 0;
    }

    // Once a frame (charDetailFrame): the screen's variable refresh, steady - a reading held 3 s (a load's few pictures
    // or a pause read as variable for a moment: one line of 155 in a playtest of the user's fixed-refresh screen).
    void vrrSample()
    {
        static bool raw0 = false, first = true;
        static auto since = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        // (TF_VRR_ASSUME=1, a test aid: the screen taken as variable - the any-rate ladder and its cap where none is)
        static const bool assume = std::getenv("TF_VRR_ASSUME") && *std::getenv("TF_VRR_ASSUME") == '1';
        const bool raw = assume || ps2_host_display::screenVariable();
        if (first || raw != raw0)
        {
            first = false;
            raw0 = raw;
            since = now;
            return;
        }
        if (now - since >= std::chrono::seconds(3) && g_vrrSteady.load(std::memory_order_relaxed) != raw)
        {
            g_vrrSteady.store(raw, std::memory_order_relaxed);
            std::fprintf(stderr, "[tf] the screen's refresh is %s (steady 3 s): frame pacing Auto takes %s from the next level\n",
                         raw ? "variable (FreeSync / G-Sync)" : "fixed", raw ? "any rate" : "even steps");
        }
    }

    void startAdapting()
    {
        const auto now = std::chrono::steady_clock::now();
        sayRateTimes("until now");
        g_rateTimeMs.clear();
        g_rateFrames.clear();
        g_detailFrames = g_detailLow = 0;
        g_detailMin = 1.0f;
        g_switches = 0;
        g_switchMsSum = g_switchMsMax = 0.0;
        g_ceilingRate = 0;
        g_ceilingFails = 0;
        g_rateTimeSaid = now;
        g_rates.clear();
        // On a screen with a clock of its own the rates divide its refresh (each picture shown for
        // the same number of refreshes). With variable refresh (FreeSync, G-Sync: the screen
        // refreshes when a picture comes) any rate is even, so the ladder goes down by tenths: a
        // host that manages 78 frames a second runs at 75, not at 60 (240/120/80/60 - measured
        // 2026-10-22, the GS thread the limit). TF_RATE_STEPS=fine / divisors forces either.
        // (tests: fine / divisors, over the setting - not over the frame rate cap, the player's own choice: a settings.ini
        // that still has the old TF_RATE_STEPS=fine beside TF_FPS_CAP follows the cap, TODO 160)
        const char *steps = std::getenv("TF_FPS_CAP") && *std::getenv("TF_FPS_CAP") ? nullptr : std::getenv("TF_RATE_STEPS");
        const int pacing = pacingChoice();
        const bool vrr = g_vrrSteady.load(std::memory_order_relaxed);
        const bool fine = steps ? std::strcmp(steps, "fine") == 0 : pacing == 2 || (pacing == 0 && vrr);
        bool capped = false; // (any rate: its top under the setting's - the refresh's, a variable one's margin)
        std::chrono::steady_clock::time_point screenVBlank{};
        std::chrono::steady_clock::duration screenPeriod{};
        const double screenHz = ps2_host_display::screenTiming(screenVBlank, screenPeriod) && screenPeriod.count() > 0
                                    ? 1.0 / std::chrono::duration<double>(screenPeriod).count()
                                    : 0.0;
        // (TODO 161 a: the logic at its fixed rate, one rate - no steps; the renderer leaves out pictures it cannot draw)
        const bool fixed = g_logicRate.load() > 0 && g_fastGameFallback && nativeOnePicture();
        const bool follow = !fixed && followRates() && g_fastGameFallback && nativeOnePicture() && !steps;
        if (fixed)
        {
            g_rates.push_back(g_frameRate);
            capped = true; // (no other rate added below)
        }
        else if (follow)
        {
            // (TODO 160: every whole rate from the cap down to 30 - the PC's own, as drawn)
            // (never above what the screen shows: the presents wait for its refresh on a fixed one, so a rate over it
            // is pictures thrown away - the test PC's 144 Hz screen at 160-218 with the cap at 240, its graphics card
            // the limit, fell and climbed every 10 s; with variable refresh a little under its top, as the ladder's
            // "any rate" - 240 Hz: 235. VSync off (the settings', PS2X_PRESENT_SYNC=0 - presents at once, allowed to tear):
            // the cap itself, Unlimited as many as the PC manages - TODO 266.)
            int top = g_frameRate;
            if (screenHz >= 50.0 && ps2_host_present::sync() != 0)
            {
                const double margin = vrr ? std::max(3.0, 0.02 * screenHz) : 0.0;
                top = std::max(30, std::min(top, static_cast<int>(std::floor(screenHz - margin + 0.5))));
            }
            capped = top < g_frameRate; // (the setting's rate not added back below)
            for (int r = top; r >= 30; --r)
                g_rates.push_back(r);
        }
        else if (g_fastGameFallback && nativeOnePicture() && !fine && screenHz >= 50.0)
        {
            // (N9: with the native renderer a frame is one picture - nativeOnePicture. On a screen with a clock of its
            // own the rungs divide its refresh - 240 Hz: 240, 120, 80, 60; 144: 144, 72, 48 - so that every picture
            // is up for the same number of refreshes; never above the refresh, which would show no more pictures.)
            // (down to 30 - the console's own - so a 60 Hz screen has 60 and 30, 144 Hz 144, 72, 48, 36)
            for (int k = 1; k <= 8; ++k)
            {
                const int rate = static_cast<int>(std::lround(screenHz / k));
                if (rate < 30)
                    break;
                if (rate <= g_frameRate)
                    g_rates.push_back(rate);
            }
            if (g_rates.empty())
                g_rates.push_back(g_frameRate);
        }
        else if (g_fastGameFallback && nativeOnePicture())
        {
            // (N9: with the native renderer a frame is one picture - nativeOnePicture - so the rungs are the
            // pictures themselves, each a frame: 240, 200, 180, ... 60, and below for a slow computer. With variable
            // refresh the screen shows each when it comes: any rate is even.)
            // (TODO 35: never above the screen's refresh - pictures it would not show - and with variable refresh a
            // little under it, 2% and at least 3 frames (Blur Busters' advice, 2026: about 3 fps under, more at 240 Hz and up):
            // the screen stays in its variable range rather than wait at its top - 240 Hz: 235, 144: 141, 60: 57)
            int top = g_frameRate;
            const tf::vrr::Range range = tf::vrr::screenRange(screenHz);
            if (screenHz >= 50.0)
            {
                const double margin = vrr ? std::max(3.0, 0.02 * screenHz) : 0.0;
                top = std::min(top, static_cast<int>(std::floor(screenHz - margin + 0.5)));
                capped = top < g_frameRate;
            }
            g_rates.push_back(top);
            for (int pictures : {240, 200, 180, 160, 144, 120, 100, 90, 80, 72, 60, 50, 45, 40})
                if (pictures <= top)
                    g_rates.push_back(pictures);
            // (TODO 35: where the driver says how far the screen's refresh follows - AMD's FreeSync range - a rate below
            // its floor without frame doubling (the range under 2:1, e.g. 210..240 Hz) is not followed: the screen
            // falls back to its own refresh, so the rungs under the floor divide the refresh, as even steps do)
            if (range.known && range.minHz > 0.0 && range.maxHz < 2.0 * range.minHz && screenHz >= 50.0)
            {
                std::vector<int> kept;
                for (int r : g_rates)
                    if (r >= range.minHz - 0.5)
                        kept.push_back(r);
                for (int k = 2; k <= 8; ++k)
                {
                    const int r = static_cast<int>(std::lround(screenHz / k));
                    if (r < 30)
                        break;
                    if (r < range.minHz - 0.5 && r <= top)
                        kept.push_back(r);
                }
                if (!kept.empty())
                    g_rates = kept;
            }
        }
        else if (g_fastGameFallback)
        {
            // (halved down to 60, each with the in-between pictures - 240: 240, 120 (240
            // pictures), 60 (120 pictures), and at 60 the in-between pictures can go as at 120. The
            // ladder used to stop at half the rate: a host that drew only 65 pictures held 120
            // frames at 65 a second - the game and its sound at 54% speed, the audio running dry
            // every half second, for good; the user's playtest at 240, 2026-10-28, TODO 62.)
            // (2026-10-29, TODO 62: rungs in between - the user's "144, 160, 180, 200, not straight to
            // half". Below the setting's own rate every rung has the in-between pictures (half its
            // pictures' rate in game frames: 200 pictures = 100 frames x 2), so the PC settles on the
            // most pictures it holds, the game's work and the pictures both going down a rung at a
            // time; the last, 30 frames x 2, is the console's own pace.)
            g_rates.push_back(g_frameRate);
            for (int pictures : {240, 200, 180, 160, 144, 120, 100, 90, 80, 72, 60})
                if (pictures <= g_frameRate)
                    g_rates.push_back(pictures / 2);
        }
        else if (fine)
        {
            // (by tenths down to 75, then small steps - 72, 68, 65, 62 - where a heavy stretch sits:
            // a host managing 64 there holds 62, not 60)
            double r = g_frameRate;
            for (; std::lround(r) > 75; r *= 0.9)
                g_rates.push_back(static_cast<int>(std::lround(r)));
            for (int step : {75, 72, 68, 65, 62})
                if (step < g_frameRate)
                    g_rates.push_back(step);
        }
        else
            for (int k = 1; static_cast<int>(std::lround(static_cast<double>(g_frameRate) / k)) >= 40; ++k)
                g_rates.push_back(static_cast<int>(std::lround(static_cast<double>(g_frameRate) / k)));
        if (g_frameRate > 60 && !g_fastGameFallback)
            g_rates.push_back(60);
        // (the setting's own rate always: at 60 with variable refresh the ladder above is empty -
        // nothing above 75, no step below 60 - and the governor read past it: a crash at a level's
        // start, 2026-10-23. Not when the native ladder divides the screen's refresh: above it.)
        if (g_rates.empty() || !(g_fastGameFallback && nativeOnePicture() && ((!fine && screenHz >= 50.0) || capped)))
            g_rates.push_back(g_frameRate);
        std::sort(g_rates.begin(), g_rates.end(), std::greater<int>());
        g_rates.erase(std::unique(g_rates.begin(), g_rates.end()), g_rates.end());
        g_rateIndex = 0;
        g_failedPeriodMs.assign(g_rates.size(), 0.0);
        g_failedAt.assign(g_rates.size(), {});
        g_failCount.assign(g_rates.size(), 0);
        g_barredUntil.assign(g_rates.size(), {});
        std::string ladder;
        for (int r : g_rates)
            ladder += (ladder.empty() ? "" : "/") + std::to_string(r);
        // (N9: says which it is - variable refresh, or fine steps asked for by TF_RATE_STEPS on a screen without it)
        if (fixed)
            std::fprintf(stderr, "[tf] frame rate: the game's logic at %d frames a second, fixed (TF_LOGIC_RATE); pictures up to the cap %d%s, "
                                 "those the renderer cannot draw in time left out (screen %.0f Hz, variable refresh %s)\n",
                         g_frameRate, g_rateSetting >= kUnlimitedRate ? 0 : g_rateSetting, g_rateSetting >= kUnlimitedRate ? " (unlimited)" : "", screenHz,
                         vrr ? "on" : "off");
        else if (follow)
            std::fprintf(stderr, "[tf] frame rate: the cap %d%s, any rate from %d down to 30 as the PC keeps it (TF_FPS_CAP; screen %.0f Hz, variable refresh %s)\n",
                         g_frameRate, g_frameRate >= kUnlimitedRate ? " (unlimited)" : "", g_rates.empty() ? g_frameRate : g_rates.front(), screenHz, vrr ? "on" : "off");
        else
        std::fprintf(stderr, "[tf] adaptive rates %s (%s; frame pacing %s%s)\n", ladder.c_str(),
                     !fine ? "the screen's refresh divided" : vrr ? "any rate, variable refresh" : "any rate",
                     steps ? "TF_RATE_STEPS" : pacing == 1 ? "even steps" : pacing == 2 ? "any rate" : "auto",
                     steps || pacing != 0 ? "" : vrr ? ": variable refresh" : ": a fixed refresh");
        g_rateSamples.clear();
        g_rateHeldUntil = now + std::chrono::seconds(1);
        g_raiseHeldUntil = now + std::chrono::seconds(2);
        g_rateRaisedAt = {};
        g_raiseHoldSeconds = 2.0;
        g_inBetweenSince = now;
        g_adaptingSince = now;
        g_inBetweenQuickFalls = 0;
        // (TODO 168: the falls of before forgotten - a new level, a state loaded; and its first seconds forgiving.
        // TF_RATE_RECOVER_S=<seconds> after the settle, 15 by default; 0: as before)
        static const int recoverSeconds = []()
        {
            const char *e = std::getenv("TF_RATE_RECOVER_S");
            return e && *e ? std::max(0, std::atoi(e)) : 15;
        }();
        if (recoverSeconds > 0)
        {
            g_rateFalls.clear();
            g_lastFallAt = {};
            g_rateRecoverUntil = std::max(now + std::chrono::seconds(5), g_rateSettleUntil) + std::chrono::seconds(recoverSeconds);
        }
    }

    double percentile(std::vector<float> values, double share)
    {
        if (values.empty())
            return 0.0;
        const size_t at = std::min(values.size() - 1, static_cast<size_t>(share * static_cast<double>(values.size())));
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
        return values[at];
    }

    // (N12 b) A play recording's replay: the ladder's step as adaptFrameRate made it when the recording was made
    // (playrec.h) - kind 0 the screen's refresh rate at a level's start, 1 the in-between pictures, 2 a step to `value`.
    void replayRateStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int kind, int value)
    {
        if (kind == 1)
        {
            setInBetween(rdram, runtime, value != 0);
            return;
        }
        const int from = g_levelRate, to = value;
        if (from <= 0 || to <= 0)
            return;
        for (size_t i = 0; i < g_rates.size(); ++i)
            if (g_rates[i] == to)
                g_rateIndex = i;
        std::fprintf(stderr, "[playrec] the recorded step: %d -> %d frames a second\n", from, to);
        g_carsPhase = g_carsPhase * to / from;
        switchFrameRate(rdram, ctx, runtime, to, false, true);
        if (!g_inHq)
            convertLevel(rdram, ctx, runtime, from, to);
        if (g_fastGameFallback)
            setInBetween(rdram, runtime, to * 2 <= g_frameRate);
    }

    void adaptFrameRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // (N12 b: replaying a play recording, the ladder judges nothing - the recorded steps come at their frames)
        if (playrec::armed() &&
            playrec::rateCall([&](int kind, int value) { replayRateStep(rdram, ctx, runtime, kind, value); }))
            return;
        // (TODO 150, a checking aid: TF_RATE_STEP_TEST=<tick>:<rate>,... - a step to that rate at that tick, recorded as the
        // ladder's - a play recording with steps at known frames)
        {
            static std::vector<std::pair<uint64_t, int>> steps = []()
            {
                std::vector<std::pair<uint64_t, int>> v;
                if (const char *e = std::getenv("TF_RATE_STEP_TEST"))
                    for (const char *p = e; *p;)
                    {
                        char *end = nullptr;
                        const uint64_t t = std::strtoull(p, &end, 10);
                        if (!end || *end != ':')
                            break;
                        const int r = std::atoi(end + 1);
                        v.emplace_back(t, r);
                        p = std::strchr(end, ',');
                        if (!p)
                            break;
                        ++p;
                    }
                return v;
            }();
            if (!steps.empty() && g_levelRate > 0 && !g_inHq && testTick(runtime) >= steps.front().first)
            {
                const int to = steps.front().second;
                steps.erase(steps.begin());
                if (to != g_levelRate && to >= 30)
                {
                    std::fprintf(stderr, "[tf] TF_RATE_STEP_TEST: a step to %d frames a second (vblank %llu)\n", to,
                                 static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
                    playrec::noteRate(2, to);
                    replayRateStep(rdram, ctx, runtime, 2, to);
                    g_rateSamples.clear();
                    return;
                }
            }
        }
        // With moment pictures (TF_MOMENTS) the in-between pictures are what the moments are made
        // of, and the GPU renderer takes moments off while the GPU is behind (its cap,
        // ps2_host_display::momentCap): they go only once the cap is down to a moment a frame and
        // the frames are still late over 2 s, a try again is judged once the moments have settled
        // (2 s: the renderer's first second of moments is not judged), and never for the rest of the
        // level. (A freeze, a try again's first moments, used to take them off for good: 60 pictures
        // where the GPU held 120 and more.)
        const bool moments = tf::port::momentPictures();
        EeScheduler &ee = runtime->eeScheduler();
        const auto now = std::chrono::steady_clock::now();
        const auto idle = ee.idleHostTime();
        const double intervalMs = std::chrono::duration<double, std::milli>(now - g_lastKickTime).count();
        const double waitedMs = std::chrono::duration<double, std::milli>(idle - g_lastKickIdle).count();
        g_lastKickTime = now;
        g_lastKickIdle = idle;
        {
            // (The forest's own record of the frames: at every rate and whatever the ladder holds - not in a load's
            // turbo, a movie, a level's loading or while the window is left.)
            const uint8_t *loading = getConstMemPtr(rdram, 0x006CD324u);
            forestFrameSample(intervalMs, std::max(0.0, intervalMs - waitedMs),
                              !ee.turboActive() && g_liveFmvStreams == 0 && !tfWindowAway() && !(loading && *loading != 0u && !g_inHq));
        }
        // (After a movie, as after a level begins, nothing for 2 s: the first frames bring the
        // scene's textures in again - the texture pack's are made on the GPU as they are first
        // drawn - and were taken for a host that cannot keep up, the in-between pictures dropped.)
        static bool movieBefore = false;
        if (movieBefore && g_liveFmvStreams == 0)
            g_rateHeldUntil = std::max(g_rateHeldUntil, now + std::chrono::seconds(2));
        movieBefore = g_liveFmvStreams != 0;
        // (TODO 114: nor for 2 s after turbo mode ends - PS2X_TURBO_VSYNCS in test runs, the fast load's turbo while a
        // level loads: frames were judged from the first real-time one on, the turbo's backlog still in them. In a
        // scripted run the HQ at 240 fell 240 -> 40 at VBlank 7834, 34 VBlanks after PS2X_TURBO_VSYNCS=7800.)
        static bool turboBefore = false;
        const bool turbo = ee.turboActive();
        if (turboBefore && !turbo)
        {
            g_rateHeldUntil = std::max(g_rateHeldUntil, now + std::chrono::seconds(2));
            g_rateSamples.clear();
        }
        turboBefore = turbo;
        // (TODO 117: nor while the player has left the window - minimised, or another window has the focus - and for
        // 2 s after they come back: those frames are timed by what the system gives a window in the background, not by
        // what this PC manages, and a rate stepped down or barred for them stayed down once the player was back.)
        // (TODO 272: a player's rate stayed where it was after alt-tabbing back - 30, or the 107 it had when he left - "until
        // I tab out and back again". Leaving the window and coming back are hitches of their own: the task switcher over
        // the game, the screen changing how it shows the window. A step down in the 3 s before the window was left was
        // made by leaving, yet it was held against the rate like any miss: nothing up for 15 s, doubling to 2 minutes
        // with each fall. And the first seconds back were judged like any others. Now a change of window is taken like
        // a load: a fall in the 3 s before leaving is forgiven on the way back (its bar and its hold-offs gone), and for
        // 12 s after coming back a fall is not held against the rate and the way up is judged on 2 s, not 6. For 30 s
        // after coming back the log says every 10 s why a lower rate is held, so a stuck moment explains itself.
        // TF_RATE_FOCUS=0: as before.)
        static const bool focusGrace = !(std::getenv("TF_RATE_FOCUS") && *std::getenv("TF_RATE_FOCUS") == '0');
        static bool awayBefore = false;
        static std::chrono::steady_clock::time_point leftAt{};
        static int rateWhenLeft = 0;
        if (tfWindowAway())
        {
            if (!awayBefore)
            {
                leftAt = now;
                rateWhenLeft = g_levelRate;
                std::fprintf(stderr, "[tf] the window was left: the frame-rate steps wait until it is back\n");
            }
            awayBefore = true;
            g_rateSamples.clear();
            return;
        }
        if (awayBefore)
        {
            awayBefore = false;
            g_rateHeldUntil = std::max(g_rateHeldUntil, now + std::chrono::seconds(2));
            g_rateSamples.clear();
            bool forgiven = false;
            if (focusGrace)
            {
                if (g_lastDownAt != std::chrono::steady_clock::time_point{} && g_lastDownAt + std::chrono::seconds(3) >= leftAt &&
                    g_lastDownAt <= leftAt + std::chrono::seconds(1))
                {
                    forgiven = true;
                    if (g_lastDownCounted && g_lastDownIndex < g_failCount.size() && g_failCount[g_lastDownIndex] > 0)
                        --g_failCount[g_lastDownIndex];
                    if (g_lastDownIndex < g_barredUntil.size())
                        g_barredUntil[g_lastDownIndex] = now;
                    if (g_ceilingFails > 0)
                        --g_ceilingFails;
                    g_ceilingUntil = now;
                    if (!g_rateFalls.empty())
                        g_rateFalls.pop_back();
                    g_raiseHoldSeconds = 2.0;
                    g_raiseHeldUntil = now + std::chrono::seconds(2);
                    g_lastDownAt = {};
                }
                g_rateRecoverUntil = std::max(g_rateRecoverUntil, now + std::chrono::seconds(14)); // (the 2 s held, then 12 s)
                g_rateExplainUntil = now + std::chrono::seconds(32);
            }
            std::fprintf(stderr, "[tf] the window is back at %d frames a second (%d when it was left%s): the frame-rate steps judge again in 2 s\n",
                         g_levelRate, rateWhenLeft, forgiven ? "; the fall while leaving is not held against the rate" : "");
            // (The fall undone at once: back at the rate the level had before leaving made it fall. If this PC does not
            // hold it, it falls again as from any miss - that one is its own and counts.)
            if (forgiven && g_adaptiveRate && g_pictureLock == 0 && g_levelRate != 0 && g_liveFmvStreams == 0 && !ee.turboActive() &&
                g_lastDownIndex < g_rates.size() && g_rates[g_lastDownIndex] == g_lastDownFrom && g_lastDownFrom > g_levelRate)
            {
                const int from = g_levelRate, to = g_lastDownFrom;
                std::fprintf(stderr, "[tf] back to the %d frames a second the level had before the window was left\n", to);
                g_rateIndex = g_lastDownIndex;
                playrec::noteRate(2, to);
                g_carsPhase = g_carsPhase * to / from;
                switchFrameRate(rdram, ctx, runtime, to, false, true);
                if (!g_inHq)
                    convertLevel(rdram, ctx, runtime, from, to);
                if (g_fastGameFallback)
                    setInBetween(rdram, runtime, to * 2 <= g_frameRate);
                g_rateRaisedAt = now;
                return;
            }
        }
        // (N9: the native ladder on a screen with a clock of its own begins at its refresh, below the setting - a level
        // begun at the setting's rate goes to it at once)
        if (g_adaptiveRate && g_pictureLock == 0 && g_levelRate != 0 && !g_rates.empty() && g_rateIndex == 0 &&
            g_rates[0] < g_levelRate && g_liveFmvStreams == 0 && !ee.turboActive() &&
            !(getConstMemPtr(rdram, 0x006CD324u) && *getConstMemPtr(rdram, 0x006CD324u) != 0u && !g_inHq)) // (not while it loads)
        {
            const int from = g_levelRate, to = g_rates[0];
            std::fprintf(stderr, "[tf] the screen refreshes %d times a second (no variable refresh): %d frames a second\n", to, to);
            playrec::noteRate(0, to); // (N12 b: a play recording keeps the ladder's steps)
            g_carsPhase = g_carsPhase * to / from;
            switchFrameRate(rdram, ctx, runtime, to, false, true);
            if (!g_inHq)
                convertLevel(rdram, ctx, runtime, from, to);
            if (g_fastGameFallback)
                setInBetween(rdram, runtime, to * 2 <= g_frameRate);
            g_rateSamples.clear();
            return;
        }
        // (TODO 161 a: the fixed logic rate - no steps; the level's time and frames counted, said every 30 s: a PC whose
        // processor cannot keep the rate shows it there, `120 100% (300 s, 117.2 a second)`)
        if (g_logicRate.load() > 0 && g_levelRate != 0 && !g_inHq && g_liveFmvStreams == 0 && !ee.turboActive() && intervalMs > 0.0 &&
            intervalMs <= 100.0 && !(getConstMemPtr(rdram, 0x006CD324u) && *getConstMemPtr(rdram, 0x006CD324u) != 0u))
        {
            g_rateTimeMs[g_levelRate] += intervalMs;
            ++g_rateFrames[g_levelRate];
            if (now - g_rateTimeSaid >= std::chrono::seconds(30))
            {
                g_rateTimeSaid = now;
                sayRateTimes("so far this level");
            }
        }
        // (no rates made: not in a level - the HQ at its own rate, TF_HQ_RATE)
        if (!g_adaptiveRate || g_pictureLock != 0 || g_levelRate == 0 || g_rates.empty() || g_rateIndex >= g_rates.size() ||
            (g_rates.size() < 2u && !inBetweenLevel()) || g_rates[g_rateIndex] != g_levelRate || g_liveFmvStreams != 0 ||
            ee.turboActive())
        {
            g_rateSamples.clear();
            return;
        }
        // (Not while the level loads - 0x6CD324 set, as the governor of the game's own uses it - nor
        // for 3 s after: the loading screen's frames are slow but under 100 ms, and were taken for a
        // host that cannot keep up - 120 down to 40 in a level's first seconds, then held there by
        // the failed rate's 30 s. The user's playtest log, 2026-10-23.)
        static bool loadingBefore = false;
        if (const uint8_t *loading = getConstMemPtr(rdram, 0x006CD324u); loading && *loading != 0u && !g_inHq) // (the HQ holds it set throughout)
        {
            loadingBefore = true;
            g_rateSamples.clear();
            return;
        }
        if (loadingBefore)
        {
            loadingBefore = false;
            g_rateSamples.clear();
            g_rateHeldUntil = std::max(g_rateHeldUntil, now + std::chrono::seconds(3));
            return;
        }
        if (now < g_rateSettleUntil)
        {
            g_rateSamples.clear();
            return;
        }
        if (now < g_rateHeldUntil || intervalMs <= 0.0 || intervalMs > 100.0)
            return;
        g_rateSamples.push_back({static_cast<float>(intervalMs), static_cast<float>(std::max(0.0, intervalMs - waitedMs)),
                                 static_cast<double>(nr::framesLeftOutBehind())});
        g_rateTimeMs[g_levelRate] += intervalMs;
        ++g_rateFrames[g_levelRate];
        if (followRates() && g_ceilingFails > 0 && g_lastFallAt != std::chrono::steady_clock::time_point{} &&
            now - g_lastFallAt > std::chrono::seconds(30))
        {
            std::fprintf(stderr, "[tf] 30 s without a miss at %d frames a second: %d misses forgotten\n", g_levelRate, g_ceilingFails);
            g_ceilingFails = 0;
            g_lastFallAt = {};
        }
        // (TODO 158: a rate held 30 s after the ladder climbed back to it has its falls forgotten - its next fall bars it
        // 10 s again, not the minutes its doubling had reached: the user's levels ended at 80 after five minutes of dips)
        static const bool forgive = !(std::getenv("TF_RATE_FORGIVE") && *std::getenv("TF_RATE_FORGIVE") == '0');
        if (forgive && g_rateRaisedAt != std::chrono::steady_clock::time_point{} && now - g_rateRaisedAt > std::chrono::seconds(30) &&
            g_rateIndex < g_failCount.size() && g_failCount[g_rateIndex] > 0)
        {
            std::fprintf(stderr, "[tf] %d frames a second held 30 s since the climb back: its %d falls forgotten\n", g_levelRate, g_failCount[g_rateIndex]);
            g_failCount[g_rateIndex] = 0;
        }
        if (now - g_rateTimeSaid >= std::chrono::seconds(30))
        {
            g_rateTimeSaid = now;
            sayRateTimes("so far this level");
        }
        const auto periodMs = [](int rate)
        { return std::chrono::duration<double, std::milli>(EeScheduler::kConsoleVBlankPeriod).count() * 60.0 / rate; };
        // The last `ms` of the host's time (at least 8 frames), or nothing while there is less.
        const auto lastOf = [](double ms, float RateSample::*field)
        {
            std::vector<float> values;
            double span = 0.0;
            for (size_t i = g_rateSamples.size(); i-- > 0 && (span < ms || values.size() < 8u);)
            {
                span += g_rateSamples[i].interval;
                values.push_back(g_rateSamples[i].*field);
            }
            if (span < ms)
                values.clear();
            return values;
        };
        size_t index = g_rateIndex;
        int inBetweenChange = 0; // (+1 on, -1 off)
        const std::vector<float> recent = lastOf(500.0, &RateSample::interval);
        // (TODO 35: down on a miss that lasts, not a moment's dip - a texture's first draws, a load's tail: the half
        // second's median late and that of the last TF_RATE_SUSTAIN_MS too (1.5 s; 500 = as before), or the half second
        // far behind - 1.5 times the period, a host that cannot hold the rate at all. Before, a half second's dip took
        // the user's 240 to 120 with 2 ms a frame to spare, and the bar kept it there a minute.)
        static const double sustainMs = []()
        {
            const char *e = std::getenv("TF_RATE_SUSTAIN_MS");
            // (TODO 160: 1.5 s following too - measured: half a second took this PC's 240 to 211-216 on every dip, 7% of a
            // 13-minute Deep Amazon run at 240 and 54 changes; 1.5 s held 240 for 99%. A miss of over 1.5 periods still
            // goes at half a second, so the slow motion of a frame that cannot be kept stays short.)
            return e && *e ? std::max(500.0, std::atof(e)) : 1500.0;
        }();
        bool behind = !recent.empty() && percentile(recent, 0.5) > 1.05 * periodMs(g_levelRate);
        if (behind && sustainMs > 500.0 && percentile(recent, 0.5) <= 1.5 * periodMs(g_levelRate))
        {
            const std::vector<float> longer = lastOf(sustainMs, &RateSample::interval);
            behind = !longer.empty() && percentile(longer, 0.5) > 1.05 * periodMs(g_levelRate);
        }
        // (TODO 165) The drawing behind: the game's frames keep their time (the drawing leaves pictures out) but a lasting share
        // of them left out - over 10% of the last half second's frames and of the last 1.5 s's - is a PC that cannot draw the
        // rate; it goes down to what was drawn. (A short hitch leaves a few out and nothing changes.)
        const auto leftOutShare = [&](double ms)
        {
            const std::vector<double> marks = [&]()
            {
                std::vector<double> v;
                double span = 0.0;
                for (size_t i = g_rateSamples.size(); i-- > 0 && (span < ms || v.size() < 8u);)
                {
                    span += g_rateSamples[i].interval;
                    v.push_back(g_rateSamples[i].leftOut);
                }
                if (span < ms)
                    v.clear();
                return v;
            }();
            return marks.size() < 2u ? 0.0 : (marks.front() - marks.back()) / static_cast<double>(marks.size());
        };
        double drawnShare = 1.0;
        if (!behind && g_fastGameFallback && nativeOnePicture())
        {
            const double shortShare = leftOutShare(500.0), longShare = leftOutShare(sustainMs);
            if (shortShare > 0.10 && longShare > 0.10)
            {
                behind = true;
                drawnShare = 1.0 - longShare;
            }
        }
        if (behind)
        {
            if (g_rateIndex + 1 < g_rates.size())
            {
                // Straight down to the rate whose period holds the frames' work with 10% to spare
                // (a rate far above what the host manages, 480, does not take a step every 2 s).
                // The work is the EE's; a host held back by the GS or the GPU shows it only in the
                // frames' intervals - what it managed, with 5% to spare.
                double needed = std::max(1.1 * percentile(lastOf(500.0, &RateSample::work), 0.5), 1.05 * percentile(recent, 0.5));
                if (drawnShare < 1.0) // (TODO 165: the drawing's rate, with 5% to spare)
                    needed = std::max(needed, 1.05 * periodMs(g_levelRate) / std::max(0.05, drawnShare));
                index = g_rateIndex + 1;
                while (index + 1 < g_rates.size() && periodMs(g_rates[index]) < needed)
                    ++index;
            }
            else if (inBetweenNow())
            {
                // (at the lowest rate: the in-between pictures go - with moments, see above - when
                // it falls behind for 2 s, not half a second: a busy fight in Deep Amazon keeps 60
                // frames on the whole with a few slow ones, and the half second's dips took the
                // pictures from 120 to 60 there for the rest of the level, 2026-10-23)
                const std::vector<float> longer = lastOf(2000.0, &RateSample::interval);
                if (!longer.empty() && percentile(longer, 0.5) > 1.05 * periodMs(g_levelRate) &&
                    (!moments || ps2_host_display::momentCap() == 1))
                    inBetweenChange = -1;
            }
        }
        else if (now >= g_raiseHeldUntil)
        {
            if (g_inBetweenHeld && inBetweenLevel() && (g_inBetweenQuickFalls < 2 || moments) &&
                !(g_fastGameFallback && g_levelRate * 2 > g_frameRate))
            {
                // (Their cost is the GS's and the GPU's, which the EE's work does not show: tried.)
                inBetweenChange = 1;
            }
            else if (g_rateIndex > 0)
            {
                // (N9: up only after 6 s steady at this rate - 95% of the frames on time - with the frames' work
                // well inside the higher rate's period: the median under 80% of it, the 98th percentile under 95%.
                // Before: 2 s, 85% and the 95th percentile under 105% - it went up where it could not stay.)
                // (TODO 168: on the last 2 s in a load's or a level's first seconds)
                const double judgeMs = now < g_rateRecoverUntil ? 2000.0 : 6000.0;
                const std::vector<float> work = lastOf(judgeMs, &RateSample::work);
                const std::vector<float> intervals = lastOf(judgeMs, &RateSample::interval);
                const double higher = periodMs(g_rates[g_rateIndex - 1]);
                // (TODO 112: "on time" judged by the frames' AVERAGE interval, not the 95th percentile. In a level
                // the game waits for a VBlank inside some frames (EE work 'at most' ~ a whole period at every rate):
                // such a frame starts late and the next one at once - 42 ms then 8 at 40 a second, the pictures still
                // 25 ms apart - one frame in 6-10. The percentile took those pairs for frames that fall behind, and the
                // user's Antarctica sat at 40 for minutes with half of every frame idle (measured: p95 38-43 ms at a
                // 25 ms period, the work p50 8-12 ms). The average shows whether the frames keep the rate. TF_RATE_RECOVER=0:
                // the 95th percentile under 3% over the period, as before.)
                static const bool recover = []()
                {
                    const char *e = std::getenv("TF_RATE_RECOVER");
                    return !(e && *e == '0');
                }();
                const double period = periodMs(g_levelRate);
                const double onTime = 1.03 * period;
                if (!work.empty())
                {
                    double mean = 0.0;
                    for (float v : intervals)
                        mean += v;
                    mean /= static_cast<double>(std::max<size_t>(1u, intervals.size()));
                    const double p95 = recover ? mean : percentile(intervals, 0.95), w50 = percentile(work, 0.5), w98 = percentile(work, 0.98);
                    // (TODO 165: and the drawing kept every frame - under 1% left out in the last 6 s)
                    if (p95 < onTime && w50 < 0.80 * higher && w98 < 0.95 * higher && leftOutShare(judgeMs) < 0.01)
                    {
                        index = g_rateIndex - 1;
                        // (TODO 160: following, straight to the highest rate the work fits as well - under the last miss's
                        // rate while its bar runs, so it does not climb back into the same wall a rate at a time)
                        // (and while that bar runs, no climb unless this rate is under 80% of the wall: 210 waits the 10 s
                        // and goes to 240 in one change, not 239 now and 240 after - each change a little hitch)
                        const bool nearWall = g_ceilingRate > 0 && now < g_ceilingUntil && g_levelRate >= 0.8 * g_ceilingRate;
                        if (followRates() && g_rates.size() > 40u && nearWall)
                            index = g_rateIndex;
                        else if (followRates() && g_rates.size() > 40u)
                        {
                            index = g_rateIndex; // (none fits: held - before, the next rung up, one frame a second)
                            for (size_t i = 0; i < g_rateIndex; ++i)
                            {
                                const double p = periodMs(g_rates[i]);
                                const bool walled = g_ceilingRate > 0 && now < g_ceilingUntil && g_rates[i] >= 0.97 * g_ceilingRate;
                                // (at most 12% up a step: the work is the EE's, and a PC held back by the graphics card or the
                                // screen shows little of it - the test PC's 4 ms of work at 130 had frames of 8; it climbed to
                                // 172 and fell back. The cap itself when it is that near.)
                                // (only near a miss still counted - from 85% of its rate up: far below it, or with the misses
                                // forgotten, straight to what the work fits - an outside stall took this PC to 30 and the 12%
                                // steps took two minutes back to 240)
                                const bool nearMiss = g_ceilingFails > 0 && g_rates[i] >= 0.85 * g_ceilingRate;
                                const bool tooFar = nearMiss && g_rates[i] > std::max(static_cast<double>(g_levelRate) * 1.12, g_levelRate + 3.0) &&
                                                    g_rates[i] != g_rates[0];
                                const bool capNear = g_rates[i] == g_rates[0] && g_rates[0] <= g_levelRate * 1.12;
                                // (and at least 8% up, the cap excepted: each change is a conversion and a little hitch, and the
                                // test PC's 138 -> 139 -> 141 bought nothing)
                                const bool tooNear = g_rates[i] < std::max(static_cast<double>(g_levelRate) * 1.08, g_levelRate + 6.0) &&
                                                     g_rates[i] != g_rates[0];
                                if (tooNear)
                                    break;
                                if (!walled && (!tooFar || capNear) && w50 < 0.80 * p && w98 < 0.95 * p)
                                {
                                    index = i;
                                    break;
                                }
                            }
                        }
                    }
                    else if (g_rateLog || now < g_rateExplainUntil)
                    {
                        static auto lastSaid = std::chrono::steady_clock::time_point{};
                        if (now - lastSaid >= std::chrono::seconds(10))
                        {
                            lastSaid = now;
                            std::fprintf(stderr, "[tf] rate %d held (not up to %d): intervals %s %.2f ms (up when < %.2f), work p50 %.2f (< %.2f), "
                                                 "p98 %.2f (< %.2f)\n", g_levelRate, g_rates[g_rateIndex - 1], recover ? "mean" : "p95", p95, onTime, w50, 0.80 * higher,
                                         w98, 0.95 * higher);
                        }
                    }
                }
            }
        }
        if (g_rateSamples.size() > 4096u)
            g_rateSamples.erase(g_rateSamples.begin(), g_rateSamples.begin() + 2048);
        if (inBetweenChange != 0)
        {
            const bool on = inBetweenChange > 0;
            if (on)
            {
                g_inBetweenSince = now;
                std::fprintf(stderr, "[tf] in-between pictures tried again:\n");
            }
            else
            {
                // (Falls in a level's first seconds - loading, the textures coming in - are not held
                // against them.)
                const bool quick = now - g_inBetweenSince < std::chrono::seconds(5) && now - g_adaptingSince >= std::chrono::seconds(5);
                g_inBetweenQuickFalls = quick ? g_inBetweenQuickFalls + 1 : 0;
                g_raiseHoldSeconds = quick ? std::min(g_raiseHoldSeconds * 2.0, 64.0) : 2.0;
                g_raiseHeldUntil = now + std::chrono::milliseconds(static_cast<int64_t>(g_raiseHoldSeconds * 1000.0));
                std::fprintf(stderr, "[tf] the host falls behind with in-between pictures at %d frames a second%s:\n", g_levelRate,
                             g_inBetweenQuickFalls >= 2 && !moments ? " (off for the rest of the level)" : "");
            }
            playrec::noteRate(1, on ? 1 : 0); // (N12 b)
            setInBetween(rdram, runtime, on);
            g_rateSamples.clear();
            // (Tried again: half a second shows whether they fit, rather than two of a slower game.)
            g_rateHeldUntil = now + (on && !moments ? std::chrono::milliseconds(500) : std::chrono::milliseconds(2000));
            return;
        }
        if (index == g_rateIndex)
            return;
        if (index < g_rateIndex && g_failCount[index] > 0 && now < g_barredUntil[index])
            return; // (it fell behind there not long ago, the EE's work showing nothing of why: g_barredUntil)
        // (TODO 124: behind, the forest's far trees go flat first - its reach comes in by 40% a second, to half the
        // setting's at most - and the rate steps down only once the forest has given what it can. The samples are
        // kept: the step is judged again next frame on the frames since.)
        if (index > g_rateIndex && forestCanGiveWay())
            return;

        const int from = g_levelRate, to = g_rates[index];
        int barredSeconds = 0;
        if (index > g_rateIndex)
        {
            g_failedPeriodMs[g_rateIndex] = percentile(recent, 0.5);
            g_failedAt[g_rateIndex] = now;
            g_lastDownAt = now, g_lastDownIndex = g_rateIndex, g_lastDownFrom = g_levelRate; // (TODO 272)
            g_lastDownCounted = now - g_adaptingSince >= std::chrono::seconds(5) && now >= g_rateRecoverUntil;
            // (Not after the level's first seconds - loading, the textures coming in: those are not held against it.)
            // (TODO 168: nor in the first seconds after a load's settle: g_rateRecoverUntil)
            if (now - g_adaptingSince >= std::chrono::seconds(5) && now >= g_rateRecoverUntil)
            {
                const int n = ++g_failCount[g_rateIndex];
                // (TODO 35: 10 s the first time, doubling to 5 minutes - TF_RATE_BAR_S; was 60 s doubling to 10 minutes,
                // which held a rate the host keeps but for a dip a minute away)
                static const int bar = []()
                {
                    const char *e = std::getenv("TF_RATE_BAR_S");
                    return e && *e ? std::max(1, std::atoi(e)) : 10;
                }();
                static const int barMax = []()
                {
                    const char *e = std::getenv("TF_RATE_BAR_MAX_S");
                    return e && *e ? std::max(1, std::atoi(e)) : 120;
                }();
                barredSeconds = std::min(barMax, bar << std::min(n - 1, 6));
                g_barredUntil[g_rateIndex] = now + std::chrono::seconds(barredSeconds);
                g_ceilingRate = from;
                g_ceilingUntil = g_barredUntil[g_rateIndex];
                if (followRates())
                {
                    // (TODO 160: every whole rate has its own count, so a count per rate never grew - the test PC changed
                    // rate every 6 s, 97 times in 10 minutes. One count of misses in a row instead.)
                    ++g_ceilingFails;
                    barredSeconds = std::min(barMax, bar << std::min(g_ceilingFails - 1, 6));
                    g_ceilingUntil = now + std::chrono::seconds(barredSeconds);
                    g_lastFallAt = now;
                }
            }
        }
        if (index > g_rateIndex)
        {
            // Down: no going up for a while, longer when it went up only just now (not while recovering: TODO 168).
            g_raiseHoldSeconds = now - g_rateRaisedAt < std::chrono::seconds(5) && now >= g_rateRecoverUntil
                                     ? std::min(g_raiseHoldSeconds * 2.0, 64.0)
                                     : 2.0;
            g_raiseHeldUntil = now + std::chrono::milliseconds(static_cast<int64_t>(g_raiseHoldSeconds * 1000.0));
        }
        else
        {
            g_rateRaisedAt = now;
        }
        {
            // (TODO 158: what the step was taken on - the last 2 s's frame intervals and the EE's work against the period)
            const std::vector<float> iv = lastOf(2000.0, &RateSample::interval), wk = lastOf(2000.0, &RateSample::work);
            if (!iv.empty() && !wk.empty())
                std::fprintf(stderr, "[tf] rate step %d -> %d: the last 2 s's frames %.2f ms (median), %.2f (95%%), the EE's work %.2f / %.2f (median / 98%%) against %.2f ms a frame at %d, %.2f at %d\n",
                             from, to, percentile(iv, 0.5), percentile(iv, 0.95), percentile(wk, 0.5), percentile(wk, 0.98), periodMs(from), from,
                             periodMs(to), to);
        }
        if (barredSeconds)
            std::fprintf(stderr, "[tf] the host falls behind at %d frames a second (not tried again for %d s):\n", from, barredSeconds);
        else
            std::fprintf(stderr, "[tf] the host %s at %d frames a second:\n", index > g_rateIndex ? "falls behind" : "has room", from);
        if (g_rateLog && index > g_rateIndex)
        {
            const std::vector<float> work = lastOf(500.0, &RateSample::work);
            std::fprintf(stderr, "[tf]   (vblank %llu%s; last 0.5 s: %zu frames, intervals p50 %.2f max %.2f ms, work p50 %.2f max %.2f ms; period %.2f)\n",
                         static_cast<unsigned long long>(ee.currentVSyncTick()), ee.turboActive() ? ", turbo" : "", recent.size(),
                         percentile(recent, 0.5), percentile(recent, 1.0), percentile(work, 0.5), percentile(work, 1.0),
                         periodMs(from));
        }
        g_rateIndex = index;
        playrec::noteRate(2, to); // (N12 b)
        const auto switchStart = std::chrono::steady_clock::now();
        g_carsPhase = g_carsPhase * to / from; // (the time since the cars' last step, in frames of `to`)
        switchFrameRate(rdram, ctx, runtime, to, false, true);
        if (!g_inHq) // (the HQ is no level: the game object there may be one's left over)
            convertLevel(rdram, ctx, runtime, from, to);
        if (g_fastGameFallback)
            setInBetween(rdram, runtime, to * 2 <= g_frameRate); // (it logs the pictures a second)
        {
            // (TODO 160: what a change costs the frame it happens in - the rate switched, the level converted)
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - switchStart).count();
            ++g_switches;
            g_switchMsSum += ms;
            g_switchMsMax = std::max(g_switchMsMax, ms);
            std::fprintf(stderr, "[tf] rate %d -> %d in %.2f ms (the switch and the level's conversion)\n", from, to, ms);
        }
        g_rateSamples.clear();
        g_rateHeldUntil = now + std::chrono::seconds(2);
        // (N9: no going up within 10 s of a change; TODO 160, following: 6 s - a change at most every 2 s down, 6 s up)
        // (following, after a step down 15 s: the test PC, held back by its graphics card, fell every 30 s and climbed
        // straight back into the same load - 85 changes in 11 minutes; TF_RATE_CLIMB_HOLD_S)
        static const int climbHold = []()
        {
            const char *e = std::getenv("TF_RATE_CLIMB_HOLD_S");
            return e && *e ? std::max(1, std::atoi(e)) : 15;
        }();
        // (TODO 165: and the falls of the last 2 minutes doubling it - 15, 30, 60, at most 120 s - so a PC whose load comes and
        // goes (another program's work beside the game) settles at what it holds instead of climbing into the load again: this
        // PC with a video encoder on ten cores beside it changed its rate 40 times in 13 minutes)
        std::deque<std::chrono::steady_clock::time_point> &falls = g_rateFalls;
        int hold = followRates() ? 6 : 10;
        const bool recovering = now < g_rateRecoverUntil; // (TODO 168: 2 s either way, and the fall not counted)
        if (recovering)
        {
            hold = 2;
            if (to < from)
                std::fprintf(stderr, "[tf]   (a load's or a level's first seconds: not held against %d - back up when the frames show room)\n",
                             from);
        }
        else if (followRates() && to < from)
        {
            falls.push_back(now);
            while (!falls.empty() && now - falls.front() > std::chrono::minutes(2))
                falls.pop_front();
            hold = std::min(120, climbHold << std::min<int>(3, static_cast<int>(falls.size()) - 1));
        }
        g_raiseHeldUntil = std::max(g_raiseHeldUntil, now + std::chrono::seconds(hold));
    }

    // A state just loaded (EeScheduler::setStateLoadedHook, before the game runs on): its level
    // plays at the rate set now when it was made at a PC rate (or at 30, forced), and the frame
    // words are set for it - a state from before them holds zeros there. At the console's 30 (no
    // level hooks) a level made at a PC rate is set back to the console's: the bytes the PC rate
    // changes as this game sets them, the level converted to 1/29.97 s a frame (convertLevel),
    // the views two passes. Without it such a state ran at half speed at 30 (its frame period a
    // 60th of a second, two fields a frame). Its cars keep the period of their last reset until
    // the next one (at the PC rate it is the console's already: rates.cpp).
    // (TODO 282) Whether memory holds a running HQ: its executor (the taUIPreGame, by its vtable) and no level.
    bool hqInMemory(const uint8_t *rdram)
    {
        constexpr uint32_t kPreGameObject = 0x006CF688u; // (the pointer to the taUIPreGame: hq_rate.cpp)
        uint32_t game = 0u, preGame = 0u, vtable = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        std::memcpy(&preGame, rdram + kPreGameObject, sizeof(preGame));
        if (game != 0u || preGame == 0u)
            return false;
        if (const uint8_t *p = getConstMemPtr(rdram, preGame))
            std::memcpy(&vtable, p, sizeof(vtable));
        return vtable == kPreGameVtable;
    }

    void adoptLoadedState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        nr::resetFrameTexturePolicy(); // also clears interframe microcode receipts/prefix and advances their epoch
        g_statesSeen = runtime->eeScheduler().loadedStateCount();
        tuning::thawLoadedState(rdram); // (TODO 269: before the level's rate is read from its period below)
        // (TODO 150) The port's carry-overs from frame to frame, which the state does not hold, as in a game just started
        // with it: the cars' 30ths and held time and the bodies moved on for the frames between their steps (a body the
        // state holds moved on was put back by the session that made it, and not by one started with it), the contacts'
        // speed credits. Kept from before the load, a play recording's start and its replay ran the game apart.
        g_carsPhase = 0;
        g_carsHeldTime = 0.0f;
        g_carsMovedOn = false;
        g_movedBodies.clear();
        g_contactSpeeds = {};
        g_rateSettleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5); // (TODO 165)
        g_inHq = false; // (a state in the HQ goes on at the console's rate until the next HQ)
        g_hqMoments.store(false);
        if (const int madeAt = g_frameRate <= 30 ? levelRateInMemory(rdram) : 0; madeAt > 30)
        {
            const uint32_t dropThreshold = 3u;
            rdram[kHalfRate] = 1u;
            rdram[kBetwixtMode] = 1u;
            std::memcpy(rdram + kDropThreshold, &dropThreshold, sizeof(dropThreshold));
            std::memcpy(rdram + kSoundFrameRate, rdram + kConsoleFrameRate, sizeof(float));
            convertLevel(rdram, ctx, runtime, madeAt, 0);
            setViewFields(rdram, 0);
            // (TODO 221) The governor's target too, and its choice: the main loop sets targetRates[0] when it starts, so the
            // state kept the PC rate's (124 a second: a frame's period 8 ms) - every frame of the 30's work (~9 ms) looked
            // late, the governor stayed at one field a frame (wantTwoFields 0, countFromFlipA 1, as the PC rate leaves
            // them) and the level ran 60 frames a second with the console's 1/30 s a frame: the whole game twice as fast
            // (the jet bikes' invitation timer, measured 0.5 s apart for its 1 s). Now the console's target (59.94 / 2) and
            // two fields a frame, as a level begun at 30 has them.
            {
                float console = 0.0f;
                std::memcpy(&console, rdram + kConsoleFrameRate, sizeof(console));
                const float target = console > 1.0f ? console * 0.5f : 29.97f;
                std::memcpy(rdram + kGovernorTarget, &target, sizeof(target));
                rdram[kWantTwoFields] = 1u;
                rdram[kFramesOfTwoFields] = 1u;
                rdram[kCountFromFlipA] = 0u;
            }
            std::fprintf(stderr, "[tf] the level, made at %d frames a second, set up for the console's 30\n", madeAt);
        }
        if (g_frameRate > 30)
        {
            const int madeAt = levelRateInMemory(rdram);
            const int rate = madeAt > 30 || (madeAt == 30 && g_force60) ? g_frameRate : 0;
            g_unpinnedCarPeriod = 0.0f;
            if (madeAt > 30)
            {
                // The state holds the PC port's values: back at 30, those this game sets.
                g_consoleHalfRate = 1u;
                g_consoleBetwixt = 1u;
                g_consoleDropThreshold = 3u;
                std::memcpy(&g_consoleSoundFrameRate, rdram + kConsoleFrameRate, sizeof(g_consoleSoundFrameRate));
            }
            g_levelRate = madeAt > 30 ? madeAt : 0; // (what memory is set up for)
            switchFrameRate(rdram, ctx, runtime, rate, true);
            startAdapting();
            if (rate != 0 && madeAt != rate)
            {
                convertLevel(rdram, ctx, runtime, madeAt, rate);
                std::fprintf(stderr, "[tf] the level, made at %d frames a second, set up for %d\n", madeAt, rate);
            }
            // The views as a level begun at the rate has them: two passes with in-between
            // pictures, one field's without. (A state made at 30 has two; one made at a PC rate
            // has either.)
            const int32_t viewFields = inBetweenLevel() ? 0 : -1;
            if (rate != 0 && (madeAt > 30 || viewFields != 0))
                setViewFields(rdram, viewFields);
            if (rate != 0 && madeAt > 30)
            {
                // (Its cars reset before the pin have that rate's period: pinCarPeriods.)
                decomp::GuestScope scope(rdram, ctx, runtime);
                g_unpinnedCarPeriod = tf::timing::LevelFramePeriod(madeAt).v;
            }
            // (TODO 282) A state loaded in the HQ: the HQ at its rate again, as an HQ just begun has it. The HQ has no
            // level in memory to read a rate from, so it was left at the console's 30 "until the next HQ" - and every
            // start that resumes from a state (an update, a setting's "Restart to apply") put a player who was in the
            // HQ at 30 frames a second, 15 on its Drop Zones screen, on a 60 Hz screen and on any other. The executor
            // is known again too (the level-load mute, the HQ's per-frame steps and its field of view go by it).
            g_inSceneStreams.clear(); // (the players of the memory before the load)
            g_sceneStreamPace.clear();
            g_adoptSceneStream = false;
            if (g_hqRate != 0 && hqInMemory(rdram))
            {
                const int hqRate = g_hqRate < 0 ? g_frameRate : g_hqRate;
                g_adoptSceneStream = true;
                g_executorVtable = kPreGameVtable;
                std::fprintf(stderr, "[tf] the HQ at %d frames a second (a state loaded in it)\n", hqRate);
                switchFrameRate(rdram, ctx, runtime, hqRate);
                g_inHq = true;
                if (hqRate > 30)
                    startAdapting();
            }
        }
        setFrameWords(rdram, ctx, runtime);
    }

    // Every frame (the governor and the game's set-up code write these).
    // F4: the pictures a second locked at 60 or 120, the other of what is shown now - in a level
    // at once (at 60 frames, converted as by adaptFrameRate when it ran at another rate), else
    // from the next one on.
    void switchPictureLock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (followRates())
        {
            // (TODO 217) The frame rate cap decides the rates: F4's 60 / 120 lock would bring the hidden 60 back.
            std::fprintf(stderr, "[tf] F4: the frame rate cap decides the rates (Graphics, Frame rate limit): no lock\n");
            return;
        }
        const bool showing120 = g_levelRate != 0 ? inBetweenNow() : (g_pictureLock != 0 ? g_pictureLock == 120 : g_inBetween);
        // (from the screen's refresh: 120, the nearest)
        g_pictureLock = showing120 && !g_moments.load() ? 60 : 120;
        g_moments.store(false);
        g_momentsShown.store(false);
        g_frameRate = 60;
        std::fprintf(stderr, "[tf] F4: %d pictures a second, locked\n", g_pictureLock);
        g_lockShown.store(g_pictureLock);
        ps2_settings::noteChanged("TF_FPS_LOCK", std::to_string(g_pictureLock));
        if (g_levelRate == 0 || g_inHq || g_frameRate <= 30) // (the HQ keeps its own rate: g_inHq)
            return;
        if (g_levelRate != 60)
        {
            const int from = g_levelRate;
            g_carsPhase = g_carsPhase * 60 / from; // (in frames of 60)
            switchFrameRate(rdram, ctx, runtime, 60, false, true);
            convertLevel(rdram, ctx, runtime, from, 60);
            startAdapting();
        }
        setInBetween(rdram, runtime, g_pictureLock == 120);
    }

    // The settings panel's frame rate - 60 or 120 pictures a second - on the game thread: in a level
    // at once (its rate converted as F4 converts it, adapting started over, the in-between pictures
    // redone), else from the next level on. (Not at the console's 30: its hooks are not in.)
    void takeSettings(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int lock = g_wantLock.exchange(-1);
        const int moments = g_wantMoments.exchange(-1);
        const int cap = g_wantCap.exchange(-1);
        if (lock < 0 && moments < 0 && cap < 0)
            return;
        if (cap >= 0)
            g_rateSetting = cap == 0 ? kUnlimitedRate : std::clamp(cap, 30, kUnlimitedRate); // (TODO 160)
        if (moments >= 0)
        {
            g_moments.store(moments == 1);
            g_momentsShown.store(moments == 1);
        }
        if (lock >= 0)
            g_pictureLock = lock;
        g_lockShown.store(g_pictureLock);
        // (TODO 282: by the hooks, not by the rate now - a limit set to 30 on the way through the list left the rate
        // at 30 for the rest of the session, whatever was chosen after it: no "settings:" line, the next HQ at 30)
        if (!g_levelHooks)
            return;
        // (TODO 161 a: the cap caps pictures; TODO 217: with the cap deciding the rates, a picture lock no longer means 60)
        g_frameRate = g_pictureLock != 0 && !followRates() ? 60 : g_logicRate.load() > 0 ? g_logicRate.load() : g_rateSetting;
        std::fprintf(stderr, "[tf] settings: %d frames a second, %s\n", g_frameRate,
                     g_moments.load() ? "pictures at the screen's refresh"
                                      : (g_pictureLock != 0 ? (g_pictureLock == 120 ? "120 pictures" : "60 pictures") : "unlocked"));
        if (g_levelRate == 0 || g_inHq) // (the HQ keeps its own rate: g_inHq)
            return;
        if (g_levelRate != g_frameRate)
        {
            const int from = g_levelRate, to = g_frameRate;
            g_carsPhase = g_carsPhase * to / from; // (the time since the cars' last step, in frames of `to`)
            switchFrameRate(rdram, ctx, runtime, to, false, true);
            convertLevel(rdram, ctx, runtime, from, to);
        }
        startAdapting();
        setInBetween(rdram, runtime, g_pictureLock != 0 ? g_pictureLock == 120 : g_inBetween);
    }

    // A level begun from an HQ the game came back to through a warp gate: that HQ ends without
    // its executor's end (0x334650) and the level begins without the level-begin hook (gameBegin),
    // so the level went on at the HQ's rate - 30 frames, 60 pictures - whatever the setting (the
    // test PC's Amazon -> gate -> HQ -> Amazon, executors traced, 2026-10-28: one HQ executor,
    // made at the warp home, then the level again with no other executor). Found once it runs (the
    // game object there and no longer held at 30, the HQ's rate still set): set up for the setting's
    // rate as a state made at another rate is (convertLevel), the views to match, adapting again.
    void levelAfterHq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_inHq || g_frameRate <= 30)
            return;
        uint32_t game = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        uint32_t held = 0u;
        std::memcpy(&held, rdram + 0x006CD324u, sizeof(held)); // (held at 30: from boot until a level is set up)
        if (game == 0u || held != 0u) // (no level, or still loading / the HQ itself)
            return;
        const int from = g_levelRate, to = g_frameRate;
        std::fprintf(stderr, "[tf] a level begun from the HQ at %d frames a second: set up for %d\n", from, to);
        g_inHq = false;
        g_hqMoments.store(false);
        g_executorVtable = 0u; // (fast_load: not the HQ)
        if (from != 0)
            g_carsPhase = g_carsPhase * to / from;
        switchFrameRate(rdram, ctx, runtime, to);
        convertLevel(rdram, ctx, runtime, from, to);
        setViewFields(rdram, inBetweenShown() ? 0 : -1);
        startAdapting();
    }

    void applyFrameRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (runtime->eeScheduler().loadedStateCount() != g_statesSeen)
            adoptLoadedState(rdram, ctx, runtime); // (in case the hook was not there)
        levelAfterHq(rdram, ctx, runtime);
        takeSettings(rdram, ctx, runtime);
        setFrameWords(rdram, ctx, runtime);
        if (g_frameRate > 30 && playrec::keyPresses(kKeyF4, ps2_host_input::takeKeyPresses(kKeyF4)) > 0) // (N12 b)
            switchPictureLock(rdram, ctx, runtime);
        // (Testing aid: TF_F4_TICKS=<tick>[,<tick>...] presses F4 at those ticks - testTick.)
        static std::vector<uint64_t> f4Ticks = []()
        {
            std::vector<uint64_t> ticks;
            if (const char *env = std::getenv("TF_F4_TICKS"))
                for (const char *p = env; *p;)
                {
                    char *end = nullptr;
                    const uint64_t value = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    ticks.push_back(value);
                    p = *end == ',' ? end + 1 : end;
                }
            std::reverse(ticks.begin(), ticks.end());
            return ticks;
        }();
        if (!f4Ticks.empty() && g_frameRate > 30 && testTick(runtime) >= f4Ticks.back())
        {
            f4Ticks.pop_back();
            switchPictureLock(rdram, ctx, runtime);
        }
        if (g_frameRate <= 30 || g_levelRate == 0)
            return;
        // Frames of two fields with in-between pictures, of one without (and while a movie
        // plays) - and the fields shown to match.
        const bool inBetween = inBetweenNow();
        rdram[kHalfRate] = inBetween ? 1u : 0u;
        rdram[kBetwixtMode] = inBetween ? 1u : 0u;
        ps2_host_display::setShownField(inBetween ? -1 : 0);
        ps2_host_display::setFieldsAsMoments(inBetween);
        setInBetweenDrop(inBetween);
        ps2_host_display::setTvFiltersLeftOut(true);
        // (Their fields and the full detail at the PC rate: the decompiled governor's rule,
        // decomp/timing/governor.cpp.)
        rdram[0x006CD96Cu] = 1u; // the governor on
        // Its target (0x6CE980, set from 0x32E8B0 when a mode starts): the TV's rate, one over
        // the VBlank's length (0x6CEF68), scaled to the level's.
        float period = 0.0f, target = 59.94f;
        std::memcpy(&period, rdram + 0x006CEF68u, sizeof(period));
        if (period > 0.01f && period < 0.03f)
            target = 1.0f / period;
        if (g_levelRate != 60)
            target *= static_cast<float>(g_levelRate) / 60.0f;
        std::memcpy(rdram + 0x006CE980u, &target, sizeof(target));
        adaptFrameRate(rdram, ctx, runtime);
    }

    // Draw distance (TF_DRAW_DISTANCE=<factor>, 2 by default, 1 for the console's): how far
    // the scene's detail reaches, times the factor. A level sets six distances
    // (DetailLevelViewDistance_0..5; in Amazon Basin none, 2000, 1200, 735, 400 and 150)
    // through 0x213B70 into the table at 0x61A960. The scene's cull (0x2132D0) leaves out a
    // mesh surface or a mesh instance whose detail level's distance is behind it, the LOD
    // switcher (0x26B260) draws the children of the most detailed level whose distance holds,
    // and the incidental objects (0x33FEE0, the ruins' pillars) are drawn within the distance
    // of their level and within MaxIndicentalDrawDistance (750) over a factor of the field of
    // view (0x2B2130, called from there alone). At the console's distances the temple of
    // Amazon Basin is a plain block until it is about 735 away, and its stairs and the
    // buildings on top come in with a jump. All of it only decides what is drawn; the cost
    // is EE and VU1 time (2x: about 3% of the heaviest stretch, 4x: 6%).
    // The game keeps the distances it set in a store (*0x6CF860: an array of a "no limit"
    // byte and the distance, eight bytes each; the zoom divides and multiplies them there and
    // sets them again): the setter gets them times the factor, and after a state is loaded the
    // table is made from the store again, so a state made with any factor loads right.
    // The trees of the eco system (the forest; 2026-10-05, the user saw distant trees come in)
    // have distances of their own, all over one scale the level sets (0x61DFE8, 2.57 in Amazon
    // Basin; the record from 0x61DFD0: instanceNear0/1, instanceFar1/0, sprite0/1, scale,
    // zoomFactorPower). 0x42FEE0 walks a quadtree of patches (a frustum test only) and at a
    // leaf 0x430190 draws each kind of tree whose reach - instanceFar0 x scale x the kind's
    // size and a constant or two - holds the leaf's nearest point; 0x429510 takes an
    // instance's distance over the same reach, turns it into a sprite between sprite0 and
    // sprite1 and fades it out between instanceFar1 and instanceFar0. The two loads of the
    // scale read 0x6CD298 instead (config/overrides.toml; a word of .sdata padding no code
    // uses), set every frame and after a state is loaded (setFrameWords) to the scale times the
    // factor: every distance of the forest times the factor, its fades and sprites the same
    // over it (at 1 the game's own scale, bit for bit).
    constexpr uint32_t kDetailDistances = 0x0061A960u;
    constexpr uint32_t kDetailDistanceCount = 6u;
    constexpr uint32_t kDetailDistanceStore = 0x006CF860u;
    constexpr uint32_t kSetDetailDistance = 0x00213B70u;
    constexpr uint32_t kIncidentalViewFactor = 0x002B2130u;
    constexpr float kNoLimit = 1e30f; // (a level without a limit gets 3.4e37)
    float g_drawDistance = 2.0f;
    uint64_t g_drawDistanceStates = 0u;
    PS2Runtime::RecompiledFunction g_setDetailDistance = nullptr;
    PS2Runtime::RecompiledFunction g_incidentalViewFactor = nullptr;

    // N8 item 7 (TODO 137, the terrain): the ground's detail texture - a close-up texture splatted over the terrain
    // that fades out by its material's AlphaFadeEnd (150 units unless set) - reaches as far as the static scene's
    // scale (+0x3C, set from the detail-level store's camera scale by taDisplayList_StaticScene::Clear, 0x361AC0)
    // times that. TF_TERRAIN_DETAIL=<f>: the scale times f after each Clear (+0x40, the grass cover's eighth root, is
    // made before and stays the game's).
    constexpr uint32_t kStaticSceneClear = 0x00361AC0u;
    PS2Runtime::RecompiledFunction g_staticSceneClear = nullptr;
    std::atomic<float> g_terrainDetail{1.0f};
    void staticSceneClear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t list = getRegU32(ctx, 4);
        g_staticSceneClear(rdram, ctx, runtime);
        const float f = g_terrainDetail.load(std::memory_order_relaxed);
        if (f == 1.0f)
            return;
        if (uint8_t *p = list >= 0x00100000u && list < PS2_RAM_SIZE - 0x44u ? getMemPtr(rdram, list + 0x3Cu) : nullptr)
        {
            float scale = 0.0f;
            std::memcpy(&scale, p, 4);
            scale *= f;
            std::memcpy(p, &scale, 4);
        }
    }

    // TF_TERRAIN_DETAIL: the factor (1-8); "" / "auto" from the Draw distance preset (TODO 137: the preset's factor, at
    // most 4 - 1 = the console's).
    float terrainDetailOf(const std::string &value, float preset)
    {
        if (value.empty() || value == "auto")
            return preset <= 1.0f ? 1.0f : std::min(preset, 4.0f);
        return std::clamp(static_cast<float>(std::atof(value.c_str())), 1.0f, 8.0f);
    }
    constexpr uint32_t kEcoScale = 0x0061DFE8u, kEcoScaleWord = 0x006CD298u;

    // The forest's 3D trees (TODO 124/99, 2026-11-02). Every tree the forest draws is an instance in a map
    // (0x733EE0, its count at 0x733EE4): 0x430190 makes one for each tree of a leaf within the kind's reach
    // and in view (CreateInstance 0x4293C0, the candidates of a frame in a list of 400 at 0x733F00), and
    // DeleteAllInstances (0x429260) drops each frame the ones no tree asked for; 0x429510 draws an instance
    // whole near, as its flat sprite between 0.6 and 0.8 of the reach and fading out to 1.0. A tree without
    // an instance is not drawn at all. CreateInstance refused a new one at 400 (0x190): at the console's reach
    // a level never has that many, at draw distance 4 (16 times the area) it was full most of the time in
    // the dense parts, and the trees that came into view found no slot - first come, first served, near or
    // far - until slots came free and a whole group came at once (the user's "trees pop / jump").
    // The limit is now a word of .sdata padding (0x6CD29C, config/overrides.toml), TF_FOREST_CAP trees (800,
    // at most 800). Two fixed arrays held the game to 400 as well, and both are dealt with:
    //   - the map's nodes come from a pool the game makes at start (0x695960: 800 nodes of 0x120 bytes,
    //     0x731E60 its used-bytes, 0x731E64 its count, 0x731E68 the nodes; taken by 0x42B860, which has no
    //     answer to a full pool - it writes through a null node). 800 is already twice the old limit: the
    //     cap is held to the pool's count, read from the game.
    //   - the drawing (0x429510) writes a record per instance it draws into a static array of 400 (0x728220,
    //     0x60 bytes) and a pointer to each into a list of 400 (0x731820) with no bound: the 401st record ran
    //     over the list and the list over the pool's globals - the hang (watchdog in the map's walks, pc
    //     0x1092CC / 0x42AF24 / 0x1B2E3C) seen with any cap over 400. Its six address loads now read the
    //     arrays' addresses from the list's first two words (config/overrides.toml), which setEcoScale keeps
    //     pointing at two blocks of the added data (guest_code::addState) of 800 entries each, every frame
    //     and after a state loads. TF_FOREST=old draws through the same blocks with the cap at 400.
    // And the forest's reach - the draw
    // distance's factor on the eco scale (setEcoScale) - is held to what fits: it shrinks while the map
    // would overflow or while the frames fall behind their period (the PC cannot keep the rate), and grows
    // back slowly to the setting when there is room. A smaller reach is the game's own way of fewer trees:
    // the farthest go flat (sprite) and fade out first, nothing pops; the nearest are always the 3D ones.
    // TF_FOREST=old: the cap 400 and the reach the setting's, as before.
    constexpr uint32_t kEcoCreateInstance = 0x004293C0u, kEcoDeleteAll = 0x00429260u;
    constexpr uint32_t kEcoInstanceCount = 0x00733EE4u; // (the map's size)
    constexpr uint32_t kEcoCapWord = 0x006CD29Cu;       // (the patched compare in CreateInstance reads it)
    constexpr uint32_t kEcoNodePoolCount = 0x00731E64u; // (the node pool's size, 800: 0x695960)
    constexpr uint32_t kEcoDrawRecordsWord = 0x00731820u, kEcoDrawListWord = 0x00731824u; // (the patched loads in 0x429510 read them)
    constexpr uint32_t kEcoDrawRecordSize = 0x60u;
    // (N7 step 2) The forest's slots: 4,096 with the 256 MB RAM - the node pool grown to it (ecoGrowPool), the drawing's
    // arrays and the cap with it - room for every tree at draw distance 4 (at 800, 236 were refused in Amazon's dense
    // parts and the reach was held in); with TF_RAM_MB=32 the 800 of before (the 32 MB arena has no room to spare).
    uint32_t forestSlots() { return PS2Runtime::guestLayout().ramMb > 32u ? 4096u : 800u; }
    uint32_t g_forestRecords = 0u, g_forestList = 0u; // (the added data's blocks: forestSlots() records, forestSlots() pointers)
    PS2Runtime::RecompiledFunction g_ecoCreateInstance = nullptr, g_ecoDeleteAll = nullptr;
    bool g_forestOld = false;
    uint32_t g_forestCap = 0u; // (installForest: forestSlots(), or TF_FOREST_CAP)
    float g_forestFit = 1.0f;               // (the reach's share of the setting's, 1/draw distance .. 1)
    uint32_t g_forestPeak = 0u, g_forestRefused = 0u, g_forestCreated = 0u, g_forestRefusedTotal = 0u;
    uint32_t g_forestMadeFrame = 0u, g_forestMadeFrameMax = 0u; // (made in one frame: a group coming at once)
    bool g_forestBehind = false;
    struct ForestFrame
    {
        float interval, work; // ms
    };
    std::deque<ForestFrame> g_forestFrames; // (the last frames, from adaptFrameRate: forestFrameSample)
    std::chrono::steady_clock::time_point g_forestSeen{}; // (the forest's last frame: ecoDeleteAll)
    std::chrono::steady_clock::time_point g_forestLogAt{};
    // (TODO 148) The forest's own clock: the game's time, a frame of the level's rate per frame of the forest - not the
    // PC's. Timed by the PC's clock, how fast the reach came in and went back out, and when its two seconds of room were
    // up, followed how the PC happened to run: the same route gave one of two pictures at the same tick (Amazon's far
    // trees, ~22,000 pixels, both workers). On the game's time the reach follows the trees alone; only the frame-time
    // give-way (below: whether the PC keeps the rate) still answers to the PC - TF_FOREST_FPS=0 turns it off, for runs
    // that compare pictures. TF_FOREST_CLOCK=pc: the PC's clock, as before.
    bool g_forestPcClock = false;
    double g_forestClock = 0.0, g_forestRoomSince = 0.0; // (seconds of the game's time)
    std::chrono::steady_clock::time_point g_forestLast{};
    bool g_forestLog = false;
    int g_forestFps = 1000; // (TF_FOREST_FPS: the highest frame rate the forest gives way at - every rate; 0 = never)
    // (N7 step 2, user decision 2026-11-05) The frame-time give-way only when the player chose it: "Forest on slower PCs"
    // (TF_FOREST_ADAPTIVE, F1 > Graphics) - Full (the default: the forest always to the Draw distance's reach) or
    // Adaptive. Measured before (N7_PLAN.md, step 2): judged at every rate it held the reach at half the setting on
    // 99% of every forest level on all three machines, also where the trees cost nothing (level 5: 58 trees, 238 fps
    // either way - the frames were "late" by the presentation's own jitter at a held 240).
    std::atomic<bool> g_forestAdaptive{false};
    // Adaptive, recalibrated: the reach comes in only while the frames are late (their median over a second 3% over the
    // period) and the forest's own work is a part of it - the time each frame spends in its two functions (0x430190,
    // the trees of each leaf; 0x429510, their drawing, the native renderer's capture of them included), its median over
    // a second at least a quarter of the frames' lateness and 10% of the period. (An A/B of the whole frame - its work
    // before and after the reach came in - could not tell: with the play it swings by 20% from one half second to the
    // next, and the test PC still took level 5's 58 free trees for the cause in 7 of 18 trials.)
    double g_forestUs = 0.0;                 // (this frame's time in the forest's functions, microseconds)
    std::deque<float> g_forestCostMs;        // (the last frames', ms)
    PS2Runtime::RecompiledFunction g_ecoLeafTrees = nullptr, g_ecoDraw = nullptr;
    int g_forestDepth = 0;                   // (inside the forest's drawing, 0x429510: TF_FOREST_SPLIT's mesh-surface time)
    double g_leafTreesUs = 0.0, g_forestDrawUs = 0.0, g_forestMeshUs = 0.0; // (TF_FOREST_SPLIT: the sums since the last log)
    PS2Runtime::RecompiledFunction g_meshSurfaceRender = nullptr;
    void meshSurfaceTimed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_forestDepth <= 0)
        {
            g_meshSurfaceRender(rdram, ctx, runtime);
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        g_meshSurfaceRender(rdram, ctx, runtime);
        g_forestMeshUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    // (TODO 194, a measuring aid: TF_FOREST_DIAG=1 with TF_FOREST_LOG=1) What the forest shows at which distance, a
    // line every 2 s beside the log's: the trees of the leaves in view (the quadtree's patches, 0x430190 - every tree
    // of them is drawn by the quadtree as a flat sprite unless it has an instance: DeleteAllInstances shrinks the
    // sprite of an instance kept a frame to a hundredth) and how many of them lie beyond their kind's 3D reach (stay
    // flat), the farthest leaf, the reach; the instances made and dropped (each a swap between the flat sprite and the
    // 3D model) and how far out; the secondary layouts (a tree's leaves and fronds, 0x4332A0 - the drawing gives them
    // to the instances nearest first while the placements queued stay under the game's 800, 0x6CDFC4) and the calls of
    // the detail-level choice (0x26B260 / 0x213B30) inside the forest's drawing (a tree mesh with detail levels).
    bool g_forestDiag = false;
    struct ForestDiagFrame
    {
        double treesInView = 0.0, treesFlat = 0.0, leafFar = 0.0, reach = 0.0;
        uint32_t made = 0u, madeFar = 0u, dropped = 0u, droppedFar = 0u, secCalls = 0u, secUsed = 0u, secCap = 0u, drawn = 0u;
        float madeDistMax = 0.0f, droppedDistMin = 1e30f, secDistMax = 0.0f, instDistMax = 0.0f;
        uint32_t lodCalls = 0u, levelCalls = 0u;
    };
    ForestDiagFrame g_fdFrame;                     // (this frame's, being counted)
    struct ForestDiagWindow
    {
        uint32_t frames = 0u;
        double treesInView = 0.0, treesFlat = 0.0, leafFar = 0.0, reach = 0.0;
        uint64_t made = 0u, madeFar = 0u, dropped = 0u, droppedFar = 0u, secCalls = 0u, drawn = 0u, secCapFrames = 0u;
        float madeDistMax = 0.0f, droppedDistMin = 1e30f, secDistMax = 0.0f, instDistMax = 0.0f;
        uint32_t secUsedMax = 0u, secCap = 0u;
        uint64_t lodCalls = 0u, levelCalls = 0u;
    };
    ForestDiagWindow g_fdWindow;
    void forestDiagLeaf(uint8_t *rdram, R5900Context *ctx);
    PS2Runtime::RecompiledFunction g_fdSecondary = nullptr, g_fdLod = nullptr, g_fdLevel = nullptr, g_fdAfterDraw = nullptr;
    // (inside the drawing: from 0x429510's entry to ecoQuadtree::Render's next call, 0x241600 - the drawing may stop at
    // a checkpoint and go on later, outside ecoTimed's count)
    bool g_fdInDraw = false;
    void forestDiagAfterDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_fdInDraw = false;
        g_fdAfterDraw(rdram, ctx, runtime);
    }
    void forestDiagSecondary(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void forestDiagLod(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_fdInDraw && ctx->pc == 0x0026B260u)
            ++g_fdFrame.lodCalls;
        g_fdLod(rdram, ctx, runtime);
    }
    void forestDiagLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_fdInDraw && ctx->pc == 0x00213B30u)
            ++g_fdFrame.levelCalls;
        g_fdLevel(rdram, ctx, runtime);
    }
    template <int Which>
    void ecoTimed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto start = std::chrono::steady_clock::now();
        if (Which == 0 && g_forestDiag && ctx->pc == 0x00430190u) // (not a resumed call)
            forestDiagLeaf(rdram, ctx);
        if (Which == 1 && g_forestDiag && ctx->pc == 0x00429510u)
            g_fdInDraw = true;
        g_forestDepth += Which;
        (Which == 0 ? g_ecoLeafTrees : g_ecoDraw)(rdram, ctx, runtime);
        g_forestDepth -= Which;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        g_forestUs += us;
        (Which == 0 ? g_leafTreesUs : g_forestDrawUs) += us;
    }
    // (TODO 94 probe, TF_FOREST_SPLIT=1) The secondary layouts (0x4332A0, ecoSecondaryObjectLayout::Render) and their
    // draws (0x432340, RenderInstanceList, inside them) per frame - what a native layout function could save.
    double g_layoutUs = 0.0, g_layoutDrawUs = 0.0, g_layoutUsSum = 0.0, g_layoutDrawUsSum = 0.0;
    uint64_t g_layoutCalls = 0u, g_layoutFrames = 0u;
    int g_layoutDepth = 0;
    PS2Runtime::RecompiledFunction g_layoutRender = nullptr, g_layoutDraw = nullptr;
    void layoutTimed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_layoutCalls;
        ++g_layoutDepth;
        const auto start = std::chrono::steady_clock::now();
        g_layoutRender(rdram, ctx, runtime);
        g_layoutUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        --g_layoutDepth;
    }
    void layoutDrawTimed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto start = std::chrono::steady_clock::now();
        g_layoutDraw(rdram, ctx, runtime);
        if (g_layoutDepth > 0)
            g_layoutDrawUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    double forestCostMedian(size_t frames)
    {
        if (g_forestCostMs.empty())
            return 0.0;
        std::vector<float> v(g_forestCostMs.end() - static_cast<std::ptrdiff_t>(std::min(frames, g_forestCostMs.size())), g_forestCostMs.end());
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    }

    // N8 (2026-11-02): the draw distance split - Scenery (g_drawDistance: the level's detail distances, the incidental
    // objects), Forest (its own factor on the eco scale) and the far trees' look - with "Draw distance"
    // (TF_DRAW_DISTANCE) the preset that sets each one left on Auto: 1 = the console's everywhere (exactly as the
    // game), otherwise the scenery at 4 (it costs almost nothing: test PC, Deep Amazon at 240, scenery 4 with the
    // forest at 1 168.9 fps against 169.9 at 1), the forest at the preset's factor (its trees are the whole cost:
    // 169.9 / 144.8 / 120.8 / 96.3 fps at 1 to 4) and its far trees 3D to the edge. TF_SCENERY_DISTANCE /
    // TF_FOREST_DISTANCE / TF_FOREST_SPRITES set one apart (unset or "auto": the preset's).
    // The far trees' look (TF_FOREST_SPRITES=<k>, 0..1): where they turn into flat sprites - the level's shares
    // (sprite0 / sprite1, 0.6 / 0.8 of a tree's reach in Amazon) moved k of the way to the edge of the reach
    // (instanceFar0): 1 = a 3D model to the edge, no sprites. Cheaper as well as the same look (Deep Amazon at 240:
    // 145.7 -> 156.2 fps at draw distance 2, 96.3 -> 105.9 at 4; route shots alike), and no swap from flat to 3D
    // (TODO 124). The record (0x61DFD0, ECOALPHADISTANCES: near0 near1 far1 far0 sprite0 sprite1 scale zoom) is the
    // level's; its sprite shares are written each frame from the level's own, kept in the added data (a state keeps
    // them) and taken again whenever the record holds other values than the ones written (a level loaded).
    std::atomic<float> g_forestDistance{2.0f};
    std::atomic<float> g_forestSprites{1.0f};
    uint32_t g_forestSpriteState = 0u; // (added data: the level's sprite0, sprite1, then the two written)
    constexpr uint32_t kEcoFar0 = 0x0061DFDCu, kEcoSprite0 = 0x0061DFE0u, kEcoSprite1 = 0x0061DFE4u;

    // (TODO 194, 2026-11-08) "Trees: full 3D at any distance" (TF_FOREST_FULL_DETAIL=1, F1 > Graphics; off = as before,
    // the default until the user confirms). Measured walking Amazon at draw distance 4 (TF_FOREST_DIAG): the quadtree
    // draws every tree of a patch in view as a flat picture (0x42FEE0's sprites) and only the ones within their kind's
    // reach as the 3D model (an instance: its picture shrunk to a hundredth after a frame) - the reach 747 units, where a
    // hilltop sees forest to 3,400 (on the highland track 8,000-10,500 of the trees in view flat, 88%); walking makes and
    // drops 2-7 instances a frame at the reach's edge (picture <-> model: the "jumping"). And of the 3D trees only the
    // nearest got their leaves and fronds (the secondary layouts, 0x4332A0): the drawing gives them out nearest first
    // while the placements queued in a frame stay under 800 (0x6CDFC4, read only by 0x429510 and 0x4332A0) - full in
    // every frame, the leaves ending 300-470 units out, the rest bare models. Not the cause: the tree meshes have one
    // model each (no detail level is asked for in the drawing) and the slots (4,096: 0 refused, at most 1,432 in use).
    // On: the reach at kForestFullFactor times the console's - past any view's far edge, every tree in view its model
    // (no pictures; the reach's fades never reached, the leaves at full strength) - and the leaves for every 3D tree.
    // Both need the larger DMA buffers below (on a state made with the game's buffers it stays off). The slots and the
    // reach's fit to them stay: a view with more trees than 4,096 brings the reach in, the farthest flat first.
    std::atomic<bool> g_forestFull{false};
    constexpr float kForestFullFactor = 64.0f;
    // (The leaves' budget on: 32,768 placements - every tree's leaves in every view measured (a hilltop: 3,000 trees,
    // 27,000 placements, 4.8 of the 8 MB DMA buffer) - and held down by the guard below when a frame fills its buffer.)
    constexpr uint32_t kEcoSecondaryBudget = 0x006CDFC4u, kEcoSecondaryBudgetGame = 800u, kEcoSecondaryBudgetFull = 32768u;
    uint32_t g_forestFullBudget = kEcoSecondaryBudgetFull; // (TF_FOREST_FULL_LEAVES=<n>: a test aid)
    uint32_t g_leafGuard = kEcoSecondaryBudgetFull;        // (the guard's: at most the budget)
    uint32_t g_budgetWritten = 0u;                         // (the budget we wrote last; off puts the game's back over it)

    // (TODO 194) The render device's two DMA buffers - the frame's whole display list for the VU1/GS, built in one while
    // the other is drawn (InitialiseDMABuffers, 0x25FF50, once from StartupDevice at boot: both from the game's DMA pool,
    // half of its free memory each - 1.5 MB; 0x6CEF28 / 0x6CEF2C their addresses, 0x6CEF30 their size in words and
    // 0x70F1B0 the size kept, which StealDMABuffer, 0x25FD90, puts back). Nothing checks the end: a frame that draws more
    // writes on into the heap behind (measured with every tree 3D: 1,500 trees and 5,400 leaf placements filled both
    // to the last word and the frame's packets overwrote the objects after the second, 0x24E4E00 - a call to 0 in a
    // display list's drawing, 0x2B7980 / 0x2BA3A0, and the game stopped). The game's own limits - 400 trees, 800 leaf
    // placements - kept it inside. With the 256 MB RAM both now come from the runtime's heap at kDmaBufferMb MB each
    // (TF_DMA_BUFFER_MB=<n>, 2-16; 0 = the game's), the game's pool left unused; TF_RAM_MB=32: the game's. A state made
    // before keeps the buffers it has (its words), and the full forest then keeps the game's leaf budget.
    constexpr uint32_t kInitDmaBuffers = 0x0025FF50u, kDmaBuffer0 = 0x006CEF28u, kDmaBuffer1 = 0x006CEF2Cu, kDmaWords = 0x006CEF30u,
                       kDmaWordsKept = 0x0070F1B0u;
    constexpr uint32_t kDmaFill = 0xBEEFF00Du; // (the game's fill: a word never written)
    uint32_t g_dmaBufferMb = 8u;
    PS2Runtime::RecompiledFunction g_initDmaBuffers = nullptr;
    void initDmaBuffers(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t words = g_dmaBufferMb * (1024u * 1024u / 4u), bytes = words * 4u;
        const uint32_t at = g_dmaBufferMb ? runtime->guestMalloc(2u * bytes, 64u) : 0u;
        if (at == 0u)
        {
            if (g_dmaBufferMb)
                std::fprintf(stderr, "[tf] the DMA buffers: no room for 2 x %u MB - the game's\n", g_dmaBufferMb);
            g_initDmaBuffers(rdram, ctx, runtime);
            return;
        }
        const uint32_t second = at + bytes;
        std::memcpy(rdram + kDmaWordsKept, &words, 4);
        std::memcpy(rdram + kDmaWords, &words, 4);
        std::memcpy(rdram + kDmaBuffer0, &at, 4);
        std::memcpy(rdram + kDmaBuffer1, &second, 4);
        for (uint32_t i = 0u; i < 2u * words; ++i)
            std::memcpy(rdram + ((at + 4u * i) & PS2_RAM_MASK), &kDmaFill, 4);
        std::fprintf(stderr, "[tf] the DMA buffers: 2 x %u MB from the runtime's heap (%08x, %08x; the game's 1.5 MB)\n", g_dmaBufferMb, at,
                     second);
        ctx->pc = getRegU32(ctx, 31);
    }
    // Whether the DMA buffers in use are ours (a state made before keeps the game's).
    bool dmaBuffersGrown(const uint8_t *rdram)
    {
        uint32_t kept = 0u;
        std::memcpy(&kept, rdram + kDmaWordsKept, 4);
        return g_dmaBufferMb != 0u && kept >= g_dmaBufferMb * (1024u * 1024u / 4u);
    }
    // (TF_FOREST_DIAG) How full the frame's DMA buffer got: the display list's kick (0x25D2B0, once a frame) finds the
    // frame's write cursor (the scratchpad's 0x70000D30) past the buffer's start (0x6CEF38) - the most of a window, in
    // 1/1000ths of the buffer (0x6CEF30 words).
    constexpr uint32_t kDmaKick = 0x0025D2B0u, kDmaCursor = 0x70000D30u, kDmaBase = 0x006CEF38u;
    // The guard (with our buffers, always): a frame that filled more than 70% of its buffer cuts the leaves' budget by a
    // quarter (never under the game's 800), one under 55% gives a 32nd back - the far trees' leaves go first, the
    // buffer never runs over (the most measured is under 60%).
    PS2Runtime::RecompiledFunction g_dmaKick = nullptr;
    uint32_t g_dmaFillMax = 0u, g_dmaBytesMax = 0u;
    void dmaKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kDmaKick)
            if (const uint8_t *c = getConstMemPtr(rdram, kDmaCursor))
            {
                uint32_t cursor = 0u, base = 0u, words = 0u;
                std::memcpy(&cursor, c, 4);
                std::memcpy(&base, rdram + kDmaBase, 4);
                std::memcpy(&words, rdram + kDmaWords, 4);
                const uint32_t used = (cursor & 0x0FFFFFFFu) - (base & 0x0FFFFFFFu);
                if (words != 0u && used < 0x10000000u)
                {
                    const double fill = used / (4.0 * words);
                    g_dmaBytesMax = std::max(g_dmaBytesMax, used);
                    g_dmaFillMax = std::max(g_dmaFillMax, static_cast<uint32_t>(1000.0 * fill));
                    if (fill > 0.70)
                        g_leafGuard = std::max(kEcoSecondaryBudgetGame, g_leafGuard - g_leafGuard / 4u);
                    else if (fill < 0.55)
                        g_leafGuard = std::min(kEcoSecondaryBudgetFull, g_leafGuard + g_leafGuard / 32u + 1u);
                }
            }
        g_dmaKick(rdram, ctx, runtime);
    }

    // (Every frame, setEcoScale: the option in force - on, and the DMA buffers ours: on a state made with the game's
    // buffers the forest stays as Forest distance says, as more trees would overrun them.)
    bool g_forestFullNow = false;
    float forestFactor()
    {
        const float f = g_forestDistance.load(std::memory_order_relaxed);
        return g_forestFullNow ? std::max(f, kForestFullFactor) : f;
    }

    // N8 item 4, TF_ENEMY_DETAIL: "0" / "off" the game's (the console's), N (1-3) the enemies drawn N detail steps finer,
    // "" / "full" / 4 (TODO 151, the default) whole at any distance - kEnemyFull.
    constexpr int kEnemyFull = 16; // (more steps than any description has: the finest always)
    int enemyDetailOf(const std::string &value)
    {
        if (value.empty() || value == "full")
            return kEnemyFull;
        if (value == "off")
            return 0;
        const int n = std::clamp(std::atoi(value.c_str()), 0, 4);
        return n >= 4 ? kEnemyFull : n;
    }

    // N8 item 5, TF_SHARP_TEXTURES: "" / "auto" matched to the screen's height (-1), "0" the PS2's mip levels, a number
    // that many levels finer (test runs).
    float textureSharpnessOf(const std::string &value)
    {
        if (value.empty() || value == "auto")
            return -1.0f;
        char *end = nullptr;
        const float v = std::strtof(value.c_str(), &end);
        return end != value.c_str() ? std::clamp(v, 0.0f, 4.0f) : -1.0f;
    }

    // A distance setting's value: a number, or -1 for Auto (unset, "auto").
    float distanceChoice(const std::string &value, float lo, float hi)
    {
        if (value.empty() || value == "auto")
            return -1.0f;
        return std::clamp(static_cast<float>(std::atof(value.c_str())), lo, hi);
    }
    // The preset's values for the settings left on Auto.
    float presetScenery(float preset) { return preset == 1.0f ? 1.0f : std::max(preset, 4.0f); }
    float presetSprites(float preset) { return preset == 1.0f ? 0.0f : 1.0f; }
    // (TODO 137: the grass's reach the preset's factor - 1 = the console's; measured cheap: 4x about +0.2 ms of GPU time)
    float presetGrass(float preset) { return preset <= 1.0f ? 1.0f : std::min(preset, 16.0f); }
    // The four from the preset and the settings' values ("" or "auto": the preset's).
    struct DrawDistances
    {
        float scenery, forest, sprites, grass;
    };
    DrawDistances resolveDistances(float preset, const std::string &scenery, const std::string &forest, const std::string &sprites,
                                   const std::string &grass = "")
    {
        DrawDistances d{};
        d.scenery = distanceChoice(scenery, 0.25f, 16.0f);
        d.forest = distanceChoice(forest, 1.0f, 16.0f);
        d.sprites = distanceChoice(sprites, 0.0f, 1.0f);
        d.grass = distanceChoice(grass, 1.0f, 16.0f);
        if (d.scenery < 0.0f)
            d.scenery = presetScenery(preset);
        if (d.forest < 0.0f)
            d.forest = std::max(1.0f, preset);
        if (d.sprites < 0.0f)
            d.sprites = presetSprites(preset);
        if (d.grass < 0.0f)
            d.grass = presetGrass(preset);
        return d;
    }

    void setEcoSprites(uint8_t *rdram)
    {
        g_forestFullNow = g_forestFull.load(std::memory_order_relaxed) && !g_forestOld && dmaBuffersGrown(rdram);
        if (g_forestSpriteState == 0u)
            return;
        float rec[2], st[4], far0 = 1.0f;
        std::memcpy(rec, rdram + kEcoSprite0, sizeof(rec));
        std::memcpy(st, rdram + g_forestSpriteState, sizeof(st));
        std::memcpy(&far0, rdram + kEcoFar0, sizeof(far0));
        if (rec[0] != st[2] || rec[1] != st[3]) // (the level's own: loaded since, or never written)
        {
            st[0] = rec[0];
            st[1] = rec[1];
        }
        const float k = g_forestFullNow ? 1.0f : g_forestSprites.load(std::memory_order_relaxed);
        if (k <= 0.0f || g_forestOld || !(st[0] > 0.0f && st[0] <= far0 && st[1] <= far0))
        {
            if (rec[0] != st[0] || rec[1] != st[1]) // (the setting taken off: the level's own back)
                std::memcpy(rdram + kEcoSprite0, st, 2 * sizeof(float));
            st[2] = st[0];
            st[3] = st[1];
        }
        else
        {
            st[2] = st[0] + k * (far0 - st[0]);
            st[3] = st[1] + k * (far0 - st[1]);
            std::memcpy(rdram + kEcoSprite0, st + 2, 2 * sizeof(float));
        }
        std::memcpy(rdram + g_forestSpriteState, st, sizeof(st));
    }

    void setEcoScale(uint8_t *rdram)
    {
        setEcoSprites(rdram);
        float scale = 0.0f;
        std::memcpy(&scale, rdram + kEcoScale, sizeof(scale));
        const float factor = g_forestOld ? forestFactor() : std::max(1.0f, forestFactor() * g_forestFit);
        if (factor != 1.0f)
            scale *= factor;
        std::memcpy(rdram + kEcoScaleWord, &scale, sizeof(scale));
        // (TODO 194: the leaves' budget - every frame and after a state loads; off, the game's 800 back if ours is there)
        uint32_t budget = 0u;
        std::memcpy(&budget, rdram + kEcoSecondaryBudget, sizeof(budget));
        const uint32_t wanted = g_forestFullNow                                        ? std::min(g_forestFullBudget, g_leafGuard)
                                : budget == g_budgetWritten && budget != 0u ? kEcoSecondaryBudgetGame
                                                                            : budget;
        if (wanted != budget)
            std::memcpy(rdram + kEcoSecondaryBudget, &wanted, sizeof(wanted));
        g_budgetWritten = g_forestFullNow ? wanted : 0u;
        // (The drawing's arrays: every frame and after a state loads - a state made before this build holds a
        // pointer of the game's list there.)
        if (g_forestRecords != 0u && g_forestList != 0u)
        {
            std::memcpy(rdram + kEcoDrawRecordsWord, &g_forestRecords, sizeof(uint32_t));
            std::memcpy(rdram + kEcoDrawListWord, &g_forestList, sizeof(uint32_t));
        }
    }

    uint32_t ecoInstanceCount(const uint8_t *rdram)
    {
        uint32_t n = 0u;
        std::memcpy(&n, rdram + kEcoInstanceCount, sizeof(n));
        return n;
    }

    // The cap in force: the setting's, never more than the node pool or the drawing's arrays hold (nor 400 without
    // the arrays: the game's own).
    uint32_t ecoCap(const uint8_t *rdram)
    {
        if (g_forestOld || g_forestRecords == 0u)
            return 400u;
        uint32_t pool = 0u;
        std::memcpy(&pool, rdram + kEcoNodePoolCount, sizeof(pool));
        return std::min({g_forestCap, forestSlots(), pool != 0u ? pool : 400u});
    }

    // An instance's node (the map's, in the game's pool): its key (the tree: 8 bytes at +0x10), its position (+0x70),
    // its age in frames (+0x110) and its flags (+0x114; bit 2: a tree asked for it this frame - DeleteAllInstances
    // drops at once every instance without it, and clears it on the rest).
    constexpr uint32_t kEcoNodeKey = 0x10u, kEcoNodePosition = 0x70u, kEcoNodeFlags = 0x114u, kEcoNodeAsked = 0x04u;
    constexpr uint32_t kEcoPoolUsed = 0x00731E60u, kEcoPoolNodes = 0x00731E68u, kEcoNodeSize = 0x120u;
    constexpr uint32_t kEcoCameraPosition = 0x00731ED0u;
    // (A candidate of 0x430190's list - CreateInstance's a1: its position at +0, its tree's key at +0x18.)
    constexpr uint32_t kEcoCandidateKey = 0x18u;

    struct ForestRefusal
    {
        uint64_t key;
        float distance;
    };
    std::vector<ForestRefusal> g_forestRefusals;                                 // (this frame's, from CreateInstance)
    std::unordered_map<uint64_t, double> g_forestEvicted; // (keys given up, a second's worth: the forest's clock)
    uint32_t g_forestEvictions = 0u, g_forestComebacks = 0u, g_forestTurns = 0u;
    int g_forestLastStep = 0; // (the reach's last move: -1 in, +1 out - a turn is a change between them)

    float ecoDistance(const uint8_t *rdram, uint32_t position)
    {
        float p[3] = {}, c[3] = {};
        std::memcpy(p, rdram + (position & PS2_RAM_MASK), sizeof(p));
        std::memcpy(c, rdram + kEcoCameraPosition, sizeof(c));
        const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    uint64_t ecoKey(const uint8_t *rdram, uint32_t at)
    {
        uint64_t key = 0u;
        std::memcpy(&key, rdram + (at & PS2_RAM_MASK), sizeof(key));
        return key;
    }

    // Every live node of the pool (the map's nodes: the pool holds nothing else).
    template <typename F>
    void ecoForEachNode(uint8_t *rdram, F &&f)
    {
        uint32_t used = 0u, nodes = 0u, count = 0u;
        std::memcpy(&used, rdram + kEcoPoolUsed, sizeof(used));
        std::memcpy(&count, rdram + kEcoNodePoolCount, sizeof(count));
        std::memcpy(&nodes, rdram + kEcoPoolNodes, sizeof(nodes));
        if (used == 0u || nodes == 0u || count == 0u || count > 65536u)
            return;
        for (uint32_t i = 0u; i < count; ++i)
            if (rdram[(used + i) & PS2_RAM_MASK] != 0u)
                f((nodes + i * kEcoNodeSize) & PS2_RAM_MASK);
    }

    float ecoLoadFloat(const uint8_t *rdram, uint32_t at)
    {
        float v = 0.0f;
        std::memcpy(&v, rdram + (at & PS2_RAM_MASK), sizeof(v));
        return v;
    }
    uint32_t ecoLoadWord(const uint8_t *rdram, uint32_t at)
    {
        uint32_t v = 0u;
        std::memcpy(&v, rdram + (at & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    // (TF_FOREST_DIAG) A leaf of the quadtree (0x430190's a0, the owner of its sprite groups a2, its nearest point's
    // distance $f13): per kind of tree its sprites (+0x30: six bytes a kind, the count at +2) and the kind's reach as
    // 0x430190 works it out - its size (+0x12C, kinds 0x520 bytes at *0x6CF8E0) x +0x148 x instanceFar0 x the eco scale
    // word x the detail scale over the zoom's factor (0x6CF8E8).
    void forestDiagLeaf(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t node = getRegU32(ctx, 4), owner = getRegU32(ctx, 6);
        const float distance = ctx->f[13];
        if (node < 0x00100000u || node >= PS2_RAM_SIZE - 0x40u || owner < 0x00100000u || owner >= PS2_RAM_SIZE - 0x40u)
            return;
        const uint32_t sprites = ecoLoadWord(rdram, node + 0x30u), items = ecoLoadWord(rdram, owner + 0x28u);
        const uint32_t kinds = ecoLoadWord(rdram, 0x006CF8E0u);
        if (sprites == 0u || items == 0u || kinds == 0u)
            return;
        const float zoom = ecoLoadFloat(rdram, 0x006CF8E8u);
        const float common = ecoLoadFloat(rdram, kEcoFar0) * ecoLoadFloat(rdram, kEcoScaleWord) * ecoLoadFloat(rdram, 0x006CD974u);
        g_fdFrame.leafFar = std::max(g_fdFrame.leafFar, static_cast<double>(distance));
        for (uint32_t k = 0u; k < 10u; ++k)
        {
            uint16_t count = 0u;
            std::memcpy(&count, rdram + ((sprites + 6u * k + 2u) & PS2_RAM_MASK), sizeof(count));
            if (count == 0u)
                continue;
            const uint32_t kind = kinds + 0x520u * k;
            const float reach = zoom > 0.0f ? ecoLoadFloat(rdram, kind + 0x12Cu) * (ecoLoadFloat(rdram, kind + 0x148u) * common) / zoom : 0.0f;
            g_fdFrame.reach = std::max(g_fdFrame.reach, static_cast<double>(reach));
            g_fdFrame.treesInView += count;
            if (!(distance <= reach))
                g_fdFrame.treesFlat += count;
        }
    }

    // (TF_FOREST_DIAG) A tree's secondary layout queued by the drawing (0x4332A0: the layout in a0, the tree's position
    // at a1).
    void forestDiagSecondary(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x004332A0u) // (its one caller is the drawing, 0x429510)
        {
            ++g_fdFrame.secCalls;
            g_fdFrame.secDistMax = std::max(g_fdFrame.secDistMax, ecoDistance(rdram, getRegU32(ctx, 5)));
        }
        g_fdSecondary(rdram, ctx, runtime);
        // (the placements queued so far: the queue is begun and ended by the drawing, so read here, not at DeleteAll)
        const uint32_t owner = ecoLoadWord(rdram, 0x006CF904u);
        if (owner >= 0x00100000u && owner < PS2_RAM_SIZE - 0x90u)
            g_fdFrame.secUsed = std::max(g_fdFrame.secUsed, ecoLoadWord(rdram, owner + 0x8Cu));
    }

    // (TF_FOREST_DIAG) Once a frame, at DeleteAllInstances: the frame's counts into the window. The drawing's (the
    // instances drawn, 0x6CF8C8; the secondaries queued, *0x6CF904 + 0x8C, and their budget 0x6CDFC4) are the frame
    // before's - the drawing comes after.
    void forestDiagFrame(uint8_t *rdram)
    {
        ForestDiagFrame &f = g_fdFrame;
        const float reach = static_cast<float>(f.reach);
        ecoForEachNode(rdram, [&](uint32_t node) {
            const float d = ecoDistance(rdram, node + kEcoNodePosition);
            if (rdram[node + kEcoNodeFlags] & kEcoNodeAsked)
                f.instDistMax = std::max(f.instDistMax, d);
            else
            {
                ++f.dropped;
                f.droppedDistMin = std::min(f.droppedDistMin, d);
                if (reach > 0.0f && d > 0.5f * reach)
                    ++f.droppedFar;
            }
        });
        f.secCap = ecoLoadWord(rdram, 0x006CDFC4u);
        f.drawn = ecoLoadWord(rdram, 0x006CF8C8u);
        ForestDiagWindow &w = g_fdWindow;
        ++w.frames;
        w.treesInView += f.treesInView;
        w.treesFlat += f.treesFlat;
        w.leafFar = std::max(w.leafFar, f.leafFar);
        w.reach = std::max(w.reach, f.reach);
        w.made += f.made;
        w.madeFar += f.madeFar;
        w.dropped += f.dropped;
        w.droppedFar += f.droppedFar;
        w.secCalls += f.secCalls;
        w.drawn += f.drawn;
        if (f.secCap != 0u && f.secUsed >= f.secCap)
            ++w.secCapFrames;
        w.secUsedMax = std::max(w.secUsedMax, f.secUsed);
        w.secCap = f.secCap;
        w.madeDistMax = std::max(w.madeDistMax, f.madeDistMax);
        w.droppedDistMin = std::min(w.droppedDistMin, f.droppedDistMin);
        w.secDistMax = std::max(w.secDistMax, f.secDistMax);
        w.instDistMax = std::max(w.instDistMax, f.instDistMax);
        w.lodCalls += f.lodCalls;
        w.levelCalls += f.levelCalls;
        f = ForestDiagFrame{};
    }

    // (N7 step 2) The map's node pool grown to forestSlots(): the game makes it at start (0x695960: count 800 at
    // 0x731E64, a used byte a node at *0x731E60 - 0 free -, the nodes at *0x731E68, both from its heap), and takes
    // (0x42B860) and gives back (0x42B500, 0x42B270, 0x42B5A0) by those three words alone. Before the first tree
    // (every node free), new blocks from the runtime's heap take their place - for good: nothing frees them (the old
    // two stay unused, 233 KB). A state holds the three words and the blocks, so a load needs nothing.
    void ecoGrowPool(uint8_t *rdram, PS2Runtime *runtime)
    {
        static bool checked = false;
        if (checked || g_forestOld)
            return;
        checked = true;
        uint32_t used = 0u, count = 0u, nodes = 0u;
        std::memcpy(&used, rdram + kEcoPoolUsed, sizeof(used));
        std::memcpy(&count, rdram + kEcoNodePoolCount, sizeof(count));
        std::memcpy(&nodes, rdram + kEcoPoolNodes, sizeof(nodes));
        const uint32_t want = forestSlots();
        if (count >= want || used == 0u || nodes == 0u || count == 0u || used + count > PS2_RAM_SIZE)
            return; // (grown already - a state of this build - or not the pool we know)
        for (uint32_t i = 0u; i < count; ++i)
            if (rdram[used + i] != 0u)
            {
                std::fprintf(stderr, "[forest] the node pool not grown: node %u in use already\n", i);
                return;
            }
        const uint32_t newUsed = runtime->guestMalloc(want, 16u);
        const uint32_t newNodes = newUsed ? runtime->guestMalloc(want * kEcoNodeSize, 16u) : 0u;
        if (newUsed == 0u || newNodes == 0u)
        {
            if (newUsed)
                runtime->guestFree(newUsed);
            std::fprintf(stderr, "[forest] the node pool not grown: no room for %u nodes\n", want);
            return;
        }
        std::memset(rdram + newUsed, 0, want);
        std::memset(rdram + newNodes, 0, static_cast<size_t>(want) * kEcoNodeSize);
        std::memcpy(rdram + kEcoPoolUsed, &newUsed, sizeof(newUsed));
        std::memcpy(rdram + kEcoNodePoolCount, &want, sizeof(want));
        std::memcpy(rdram + kEcoPoolNodes, &newNodes, sizeof(newNodes));
        std::fprintf(stderr, "[forest] the node pool grown from %u to %u nodes (%08x, %08x)\n", count, want, newUsed, newNodes);
    }

    void ecoCreateInstance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ecoGrowPool(rdram, runtime);
        const uint32_t cap = ecoCap(rdram);
        std::memcpy(rdram + kEcoCapWord, &cap, sizeof(cap));
        const uint32_t candidate = getRegU32(ctx, 5);
        if (ecoInstanceCount(rdram) >= cap)
        {
            // (the game's own refusal - its compare is ==: a count over the word, after a lower cap, would pass)
            ++g_forestRefused;
            if (!g_forestOld)
                g_forestRefusals.push_back({ecoKey(rdram, candidate + kEcoCandidateKey), ecoDistance(rdram, candidate)});
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        ++g_forestCreated;
        ++g_forestMadeFrame;
        if (g_forestDiag)
        {
            const float d = ecoDistance(rdram, candidate);
            ++g_fdFrame.made;
            g_fdFrame.madeDistMax = std::max(g_fdFrame.madeDistMax, d);
            if (g_fdFrame.reach > 0.0 && d > 0.5 * g_fdFrame.reach)
                ++g_fdFrame.madeFar;
        }
        if (!g_forestEvicted.empty())
            if (const auto e = g_forestEvicted.find(ecoKey(rdram, candidate + kEcoCandidateKey)); e != g_forestEvicted.end())
            {
                ++g_forestComebacks; // (given up, and asked for again within the second: the flip-flop to avoid)
                g_forestEvicted.erase(e);
            }
        g_ecoCreateInstance(rdram, ctx, runtime);
    }

    // The ladder's kick (adaptFrameRate) gives each frame: its interval and its work; a frame that does not count
    // (turbo, a movie, loading, the window left) clears the record.
    void forestFrameSample(double intervalMs, double workMs, bool counts)
    {
        if (!counts || intervalMs <= 0.0 || intervalMs > 100.0)
        {
            g_forestFrames.clear();
            return;
        }
        g_forestFrames.push_back({static_cast<float>(intervalMs), static_cast<float>(workMs)});
        while (g_forestFrames.size() > 1024u)
            g_forestFrames.pop_front();
    }

    // The median of the last `ms` of the frames' `field`, or 0 while there is less than that.
    double forestMedian(double ms, float ForestFrame::*field)
    {
        std::vector<float> values;
        double span = 0.0;
        for (size_t i = g_forestFrames.size(); i-- > 0 && span < ms;)
        {
            span += g_forestFrames[i].interval;
            values.push_back(g_forestFrames[i].*field);
        }
        if (span < ms || values.size() < 8u)
            return 0.0;
        std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
        return values[values.size() / 2];
    }

    // Whether the frame time is judged for the forest now: a forest drawn (its frame within the last half second), a
    // reach beyond the console's, at a PC rate up to TF_FOREST_FPS.
    bool forestJudges()
    {
        return !g_forestOld && g_forestAdaptive.load(std::memory_order_relaxed) && forestFactor() > 1.0f && g_forestFps > 0 &&
               g_levelRate > 30 && g_levelRate <= g_forestFps &&
               std::chrono::steady_clock::now() - g_forestSeen < std::chrono::milliseconds(500);
    }

    // The trees come back only at the player's rate - the ladder at its top, or the rate above barred for now: the
    // rate the player chose comes back before the far trees do.
    bool forestRateAtTop(std::chrono::steady_clock::time_point now)
    {
        if (!g_adaptiveRate || g_rates.empty() || g_rateIndex == 0 || g_rateIndex >= g_rates.size())
            return true;
        return g_failCount[g_rateIndex - 1] > 0 && now < g_barredUntil[g_rateIndex - 1];
    }

    // Whether the forest would still give way: it is judging, behind or not, and its reach is above its floor (half
    // the setting's) - the ladder waits for it before stepping down.
    bool forestCanGiveWay()
    {
        if (!forestJudges())
            return false;
        const float floor = std::max(1.0f / forestFactor(), 0.5f);
        return g_forestFit > floor + 0.01f;
    }

    // Once a frame, after the trees of the frame have asked for their instances and before the forest drops the
    // ones no tree asked for:
    //   1. Nearest first. The trees refused for want of a slot that next frame's free slots (the cap less the
    //      instances asked for this frame) cannot take, nearest first, each take the place of the farthest instance
    //      asked for if it is nearer the camera - by more than 15%, the margin that keeps two trees at nearly one
    //      distance from trading places: that instance's "asked" flag is cleared, the game drops it now, and the
    //      nearer tree finds the slot next frame.
    //   2. The reach fitted to the count. The forest's reach (the draw distance's factor on the eco scale) is
    //      what keeps the trees asked for under the cap: over 90% of it, the reach comes in towards the share
    //      that would hold 90% (the trees asked for go as the reach squared), at most 3% of itself a frame, so the
    //      trees given up are the farthest, the ones fading out (the game fades them over the last fifth of the
    //      reach); under 75% for two seconds, it goes back out by 5% a second, never past the setting.
    //   3. The frame time, at every rate (TF_FOREST_FPS: up to that rate; 0 = never): while the frames fall
    //      behind the level's period (their median over a quarter second 2% over it) the reach comes in by 40%
    //      a second, down to half the setting's at most - and the frame-rate ladder steps the rate down only
    //      once the reach is there (forestCanGiveWay). It goes back out (5% a second) only with headroom - a
    //      second of frames whose work is under 80% of the period - and with the ladder back at the player's
    //      rate: the rate comes back first, then the trees.
    // The gap between 75% and 90% and the two seconds keep the reach still at a steady count (no back and
    // forth); a tree given up is counted again (a "comeback") if asked for again within a second.
    void ecoDeleteAll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto now = std::chrono::steady_clock::now();
        double dt = 1.0 / framesPerSecond(); // (a frame of the game's time - TODO 148)
        if (g_forestPcClock)
        {
            const double interval = g_forestLast.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - g_forestLast).count() : 0.0;
            dt = std::min(0.1, interval / 1000.0);
        }
        g_forestLast = now;
        g_forestSeen = now;
        g_forestClock += dt;
        const double clock = g_forestClock;
        const uint32_t count = ecoInstanceCount(rdram);
        g_forestPeak = std::max(g_forestPeak, count);
        g_forestCostMs.push_back(static_cast<float>(g_forestUs / 1000.0)); // (the frame since the last: one frame's forest)
        g_forestUs = 0.0;
        g_layoutUsSum += g_layoutUs;
        g_layoutDrawUsSum += g_layoutDrawUs;
        g_layoutUs = g_layoutDrawUs = 0.0;
        ++g_layoutFrames;
        while (g_forestCostMs.size() > 1024u)
            g_forestCostMs.pop_front();
        g_forestRefusedTotal += g_forestRefused;
        g_forestMadeFrameMax = std::max(g_forestMadeFrameMax, g_forestMadeFrame);
        g_forestMadeFrame = 0u;
        for (auto e = g_forestEvicted.begin(); e != g_forestEvicted.end();)
            e = clock - e->second > 1.0 ? g_forestEvicted.erase(e) : std::next(e);
        if (g_forestDiag)
            forestDiagFrame(rdram);
        // (The instances a tree asked for this frame: the rest go at the end of it, their slots free for next frame.)
        uint32_t askedCount = count;
        if (!g_forestOld)
        {
            askedCount = 0u;
            ecoForEachNode(rdram, [&](uint32_t node) { askedCount += (rdram[node + kEcoNodeFlags] & kEcoNodeAsked) ? 1u : 0u; });
        }
        const uint32_t cap = ecoCap(rdram);
        // (Only as many as next frame's free slots - the cap less the instances asked for - cannot take: a new view
        // (a turn, a cut) asks for its trees while the old view's still hold their slots this one frame.)
        const size_t freeNext = cap > askedCount ? cap - askedCount : 0u;
        if (!g_forestOld && g_forestRefusals.size() > freeNext)
        {
            struct Asked
            {
                uint32_t node;
                float distance;
            };
            std::vector<Asked> asked;
            asked.reserve(askedCount);
            ecoForEachNode(rdram, [&](uint32_t node) {
                if (rdram[node + kEcoNodeFlags] & kEcoNodeAsked)
                    asked.push_back({node, ecoDistance(rdram, node + kEcoNodePosition)});
            });
            std::sort(asked.begin(), asked.end(), [](const Asked &a, const Asked &b) { return a.distance > b.distance; });
            std::sort(g_forestRefusals.begin(), g_forestRefusals.end(),
                      [](const ForestRefusal &a, const ForestRefusal &b) { return a.distance < b.distance; });
            const size_t excess = g_forestRefusals.size() - freeNext;
            size_t far = 0u;
            for (const ForestRefusal &r : g_forestRefusals)
            {
                if (far >= excess || far >= asked.size() || asked[far].distance <= r.distance * 1.15f)
                    break;
                rdram[asked[far].node + kEcoNodeFlags] &= static_cast<uint8_t>(~kEcoNodeAsked);
                g_forestEvicted[ecoKey(rdram, asked[far].node + kEcoNodeKey)] = clock;
                ++g_forestEvictions;
                ++far;
            }
        }
        if (!g_forestOld && forestFactor() > 1.0f && dt > 0.0)
        {
            // (behind: the last quarter second's frames, by their median, more than 2% over the level's period -
            // only at or below TF_FOREST_FPS (60): above it the frame-rate ladder steps down first. Judged at every
            // rate, the forest lost each time: the ladder climbs again as soon as the frames have room, they are late
            // at the higher rate, the forest shrinks again - a 240 setting a PC cannot hold took it down to the
            // console's reach in 20 s (test PC and other PC, 2026-11-02).)
            g_forestBehind = false;
            bool headroom = false;
            if (forestJudges())
            {
                const double periodMs = std::chrono::duration<double, std::milli>(EeScheduler::kConsoleVBlankPeriod).count() * 60.0 / g_levelRate;
                // (N7 step 2: over a second, 3% - over a quarter second at 2% the presentation's own jitter at a held 240
                // made frames "late" that the trees did not cause)
                const double frameMs = forestMedian(1000.0, &ForestFrame::interval);
                const double forestMs = forestCostMedian(static_cast<size_t>(std::max(30, g_levelRate)));
                // (the forest's share: a quarter of the lateness and a tenth of the period - at 5% the test PC's level 5,
                // 58 trees at 0.12-0.23 ms a frame, pulled in for good)
                const double bar = std::max(0.25 * (frameMs - periodMs), 0.10 * periodMs);
                g_forestBehind = frameMs > 1.03 * periodMs && forestMs >= bar;
                // (Room to grow: a second of frames whose work (the EE's, the host's clock's waits taken out) is under
                // 80% of the period - the ladder's own bar for a step up - or the forest's share well under its bar: the
                // frames are late for something else, which a smaller forest does not mend.)
                const double work = forestMedian(1000.0, &ForestFrame::work);
                headroom = (work > 0.0 && work < 0.80 * periodMs) || forestMs < 0.5 * bar;
            }
            else
                headroom = true; // (not judged: the cap alone)
            const double wanted = static_cast<double>(askedCount) + static_cast<double>(g_forestRefusals.size()); // (the trees that asked)
            const double high = 0.90 * cap, low = 0.75 * cap;
            const float least = 1.0f / forestFactor(), before = g_forestFit;
            float fit = g_forestFit;
            if (wanted > high)
                fit *= static_cast<float>(std::max(std::sqrt(high / wanted), std::pow(0.97, dt * 60.0)));
            if (g_forestBehind)
            {
                // (Never under half the setting's reach for the frame time: what holds a PC back may not be the trees -
                // a load of its own, the rest of the scene - and the reach would go on falling to the console's for
                // nothing (test PC at a 120 test rate it could not hold: 1.0 to 0.25 in 10 s, 2026-11-02).)
                const float frameFloor = std::min(g_forestFit, std::max(least, 0.5f));
                fit = std::min(fit, std::max(frameFloor, g_forestFit * static_cast<float>(std::exp(-0.5 * dt))));
            }
            if (wanted > high || g_forestBehind)
                g_forestRoomSince = clock;
            else if (wanted < low && headroom && forestRateAtTop(now))
            {
                if (clock - g_forestRoomSince > 2.0)
                    fit = std::min(1.0f, fit * static_cast<float>(std::exp(0.05 * dt)));
            }
            else
                g_forestRoomSince = clock;
            g_forestFit = std::clamp(fit, least, 1.0f);
            const int step = g_forestFit < before ? -1 : g_forestFit > before ? 1 : 0;
            if (step != 0)
            {
                if (g_forestLastStep != 0 && step != g_forestLastStep)
                    ++g_forestTurns;
                g_forestLastStep = step;
            }
        }
        if (playrec::armed())
            g_forestFit = playrec::forestFit(g_forestFit); // (N12 b: the reach a play recording had - the PC's clock moves it)
        if (g_forestLog && now >= g_forestLogAt)
        {
            g_forestLogAt = now + std::chrono::seconds(2);
            std::fprintf(stderr,
                         "[forest] tick %llu: %u 3D trees (peak %u, cap %u), %u made (at most %u in a frame) / %u refused / %u given up (%u back within 1 s) in 2 s, reach x%.2f of the setting (%u turns), the forest %.2f ms a frame%s\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), count, g_forestPeak, ecoCap(rdram), g_forestCreated,
                         g_forestMadeFrameMax, g_forestRefusedTotal, g_forestEvictions, g_forestComebacks, g_forestOld ? 1.0f : g_forestFit, g_forestTurns,
                         forestCostMedian(static_cast<size_t>(std::max(30, g_levelRate))), g_forestBehind ? ", frames late" : "");
            if (g_layoutRender && g_layoutFrames)
                std::fprintf(stderr, "[forest] a frame: the leaves' trees %.3f ms, the drawing %.3f ms - of it the mesh surfaces (their capture included) %.3f ms "
                                     "and the secondary layouts %.3f ms (%.0f calls; of it their draws %.3f ms)\n",
                             g_leafTreesUs / 1000.0 / g_layoutFrames, g_forestDrawUs / 1000.0 / g_layoutFrames, g_forestMeshUs / 1000.0 / g_layoutFrames,
                             g_layoutUsSum / 1000.0 / g_layoutFrames, double(g_layoutCalls) / g_layoutFrames, g_layoutDrawUsSum / 1000.0 / g_layoutFrames);
            if (g_forestDiag && g_fdWindow.frames)
            {
                const ForestDiagWindow &w = g_fdWindow;
                const double n = w.frames;
                std::fprintf(stderr,
                             "[forest] diag: %u frames; a frame: %.0f trees in the leaves in view, %.0f of them flat (beyond their reach %.0f), "
                             "the farthest leaf %.0f; %.1f drawn 3D (the farthest %.0f); made %.2f (%.2f past half the reach, the farthest %.0f), "
                             "dropped %.2f (%.2f past half the reach, the nearest %.0f); secondaries %.1f (the farthest %.0f), queued at most %u "
                             "of %u (full in %llu frames); detail-level calls in the drawing %llu / %llu; the frame's DMA buffer at most %.1f%% full (%.2f MB)\n",
                             w.frames, w.treesInView / n, w.treesFlat / n, w.reach, w.leafFar, w.drawn / n, w.instDistMax, w.made / n,
                             w.madeFar / n, w.madeDistMax, w.dropped / n, w.droppedFar / n, w.droppedDistMin < 1e29f ? w.droppedDistMin : 0.0f,
                             w.secCalls / n, w.secDistMax, w.secUsedMax, w.secCap, static_cast<unsigned long long>(w.secCapFrames),
                             static_cast<unsigned long long>(w.lodCalls), static_cast<unsigned long long>(w.levelCalls),
                             g_dmaFillMax / 10.0, g_dmaBytesMax / 1048576.0);
                g_fdWindow = ForestDiagWindow{};
                g_dmaFillMax = g_dmaBytesMax = 0u;
            }
            g_leafTreesUs = g_forestDrawUs = g_forestMeshUs = 0.0;
            g_layoutUsSum = g_layoutDrawUsSum = 0.0;
            g_layoutCalls = g_layoutFrames = 0u;
            g_forestCreated = g_forestRefusedTotal = g_forestMadeFrameMax = g_forestEvictions = g_forestComebacks = g_forestTurns = 0u;
        }
        g_forestRefused = 0u;
        g_forestRefusals.clear();
        g_ecoDeleteAll(rdram, ctx, runtime);
    }

    void installForest(PS2Runtime &runtime)
    {
        const char *mode = std::getenv("TF_FOREST");
        g_forestOld = mode && std::string(mode) == "old";
        if (const char *cap = std::getenv("TF_FOREST_CAP"); cap && *cap)
            g_forestCap = static_cast<uint32_t>(std::clamp(std::atol(cap), 16L, static_cast<long>(forestSlots())));
        else
            g_forestCap = forestSlots();
        g_forestLog = std::getenv("TF_FOREST_LOG") && *std::getenv("TF_FOREST_LOG") == '1';
        g_forestPcClock = std::getenv("TF_FOREST_CLOCK") && std::string(std::getenv("TF_FOREST_CLOCK")) == "pc";
        if (const char *a = std::getenv("TF_FOREST_ADAPTIVE"); a && *a == '1')
            g_forestAdaptive.store(true);
        if (const char *f = std::getenv("TF_FOREST_FPS"); f && *f)
            g_forestFps = std::clamp(std::atoi(f), 0, 1000);
        if (const char *full = std::getenv("TF_FOREST_FULL_DETAIL"); full && *full == '1')
            g_forestFull.store(true);
        if (const char *leaves = std::getenv("TF_FOREST_FULL_LEAVES"); leaves && *leaves)
            g_forestFullBudget = static_cast<uint32_t>(std::clamp(std::atol(leaves), 1L, 0x7FFFFFFFL));
        if (const char *mb = std::getenv("TF_DMA_BUFFER_MB"); mb && *mb)
            g_dmaBufferMb = std::atoi(mb) <= 0 ? 0u : static_cast<uint32_t>(std::clamp(std::atoi(mb), 2, 16));
        if (PS2Runtime::guestLayout().ramMb <= 32u)
            g_dmaBufferMb = 0u;
        if (g_dmaBufferMb && (g_initDmaBuffers = runtime.lookupFunction(kInitDmaBuffers)) != nullptr)
            runtime.registerFunction(kInitDmaBuffers, initDmaBuffers);
        else
            g_dmaBufferMb = 0u;
        if (g_dmaBufferMb && (g_dmaKick = runtime.lookupFunction(kDmaKick)) != nullptr)
            runtime.registerFunction(kDmaKick, dmaKick);
        // (Always, and the same size whatever the settings: the added data's addresses stay put.)
        g_forestSpriteState = guest_code::addState("forest: the level's tree sprite shares (N8)", 16u);
        g_forestRecords = guest_code::addState("forest: the drawn trees' records (0x429510)", forestSlots() * kEcoDrawRecordSize);
        g_forestList = guest_code::addState("forest: the drawn trees' list (0x429510)", forestSlots() * 4u);
        // (Always: CreateInstance's patched compare reads the word, which only these set.)
        g_ecoCreateInstance = runtime.lookupFunction(kEcoCreateInstance);
        g_ecoDeleteAll = runtime.lookupFunction(kEcoDeleteAll);
        if (g_ecoCreateInstance)
            runtime.registerFunction(kEcoCreateInstance, ecoCreateInstance);
        if (g_ecoDeleteAll)
            runtime.registerFunction(kEcoDeleteAll, ecoDeleteAll);
        if ((g_ecoLeafTrees = runtime.lookupFunction(0x00430190u)) != nullptr)
            runtime.registerFunction(0x00430190u, ecoTimed<0>);
        if ((g_ecoDraw = runtime.lookupFunction(0x00429510u)) != nullptr)
            runtime.registerFunction(0x00429510u, ecoTimed<1>);
        if (const char *diag = std::getenv("TF_FOREST_DIAG"); diag && *diag == '1')
        {
            g_forestDiag = true;
            if ((g_fdSecondary = runtime.lookupFunction(0x004332A0u)) != nullptr)
                runtime.registerFunction(0x004332A0u, forestDiagSecondary);
            if ((g_fdLod = runtime.lookupFunction(0x0026B260u)) != nullptr)
                runtime.registerFunction(0x0026B260u, forestDiagLod);
            if ((g_fdLevel = runtime.lookupFunction(0x00213B30u)) != nullptr)
                runtime.registerFunction(0x00213B30u, forestDiagLevel);
            if ((g_fdAfterDraw = runtime.lookupFunction(0x00241600u)) != nullptr)
                runtime.registerFunction(0x00241600u, forestDiagAfterDraw);
        }
        if (const char *split = std::getenv("TF_FOREST_SPLIT"); split && *split == '1')
        {
            g_layoutRender = runtime.lookupFunction(0x004332A0u);
            g_layoutDraw = runtime.lookupFunction(0x00432340u);
            if (g_layoutRender && g_layoutDraw)
            {
                runtime.registerFunction(0x004332A0u, layoutTimed);
                runtime.registerFunction(0x00432340u, layoutDrawTimed);
            }
            if ((g_meshSurfaceRender = runtime.lookupFunction(0x001FFC70u)) != nullptr) // (vlMeshSurface::vfunc_03, the mesh's render)
                runtime.registerFunction(0x001FFC70u, meshSurfaceTimed);
        }
        std::fprintf(stderr, "[forest] %s%s\n", g_forestOld ? "3D trees at most 400 (the game's)"
                                                 : g_forestAdaptive.load() ? "3D trees: the reach fitted to the cap, adaptive to the frame time"
                                                                          : "3D trees: the reach fitted to the cap, full whatever the frame time",
                     g_forestFull.load() && !g_forestOld ? "; every tree full 3D at any distance (TF_FOREST_FULL_DETAIL)" : "");
    }

    // (TODO 99, a check: TF_DD_LOG=1 - every call of the setter, and every frame whose table differs from the frame before's)
    const bool g_ddLog = std::getenv("TF_DD_LOG") && *std::getenv("TF_DD_LOG") == '1';

    void setDetailDistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const float given = ctx->f[12];
        if (ctx->f[12] < kNoLimit)
            ctx->f[12] *= g_drawDistance;
        if (g_ddLog)
            std::fprintf(stderr, "[tf:dd] tick %llu set level %u: %g -> %g (from %08x)\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), getRegU32(ctx, 4), given, ctx->f[12],
                         getRegU32(ctx, 31));
        g_setDetailDistance(rdram, ctx, runtime);
    }

    void incidentalViewFactor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_incidentalViewFactor(rdram, ctx, runtime);
        ctx->f[0] /= g_drawDistance;
    }

    // Every frame: after a state is loaded, the table from the store.
    void applyDrawDistance(uint8_t *rdram, PS2Runtime *runtime)
    {
        if (g_ddLog)
        {
            static float last[kDetailDistanceCount] = {};
            float now[kDetailDistanceCount];
            std::memcpy(now, rdram + kDetailDistances, sizeof(now));
            float scale = 0.0f;
            std::memcpy(&scale, rdram + kEcoScaleWord, sizeof(scale));
            static float lastScale = 0.0f;
            if (std::memcmp(now, last, sizeof(now)) != 0 || scale != lastScale)
                std::fprintf(stderr, "[tf:dd] tick %llu table %g %g %g %g %g %g, eco scale %g\n",
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), now[0], now[1], now[2], now[3],
                             now[4], now[5], scale);
            std::memcpy(last, now, sizeof(now));
            lastScale = scale;
        }
        if (const float factor = g_wantDrawDistance.exchange(0.0f); factor > 0.0f)
        {
            // (the settings panel's: the table again from the store, below)
            g_drawDistance = factor;
            g_drawDistanceStates = ~0ull;
            std::fprintf(stderr, "[tf] draw distance x%g\n", g_drawDistance);
        }
        const uint64_t states = runtime->eeScheduler().loadedStateCount();
        if (states == g_drawDistanceStates)
            return;
        g_drawDistanceStates = states;
        uint32_t store = 0u, array = 0u;
        std::memcpy(&store, rdram + kDetailDistanceStore, sizeof(store));
        const uint8_t *storeWords = store ? getConstMemPtr(rdram, store) : nullptr;
        if (!storeWords)
            return; // (no level)
        std::memcpy(&array, storeWords, sizeof(array));
        for (uint32_t level = 0; level < kDetailDistanceCount; ++level)
        {
            const uint8_t *element = array ? getConstMemPtr(rdram, array + 8u * level) : nullptr;
            if (!element || element[0] != 0u)
                continue;
            float distance = 0.0f;
            std::memcpy(&distance, element + 4, sizeof(distance));
            if (distance < kNoLimit)
                distance *= g_drawDistance;
            std::memcpy(rdram + kDetailDistances + 4u * level, &distance, sizeof(distance));
        }
    }

    // A scene node's destructor (vlSceneNode::~vlSceneNode, 0x2137D0) deletes its children one
    // by one (`while (child) delete child`). Going back to the HQ after a level (the user, after
    // Antarctica's end, 2026-10-23; also the warp gate route from Amazon) a child was there whose
    // memory was all zeros - left in the list after something else freed and reused it - and its
    // destructor was called through a null vtable: the game stopped behind the warp screen for
    // good. A child with no vtable ends the list there (it and any after it are left - a node
    // the game was done with); logged with its parent, to find who leaves it.
    constexpr uint32_t kSceneNodeDtor = 0x002137D0u;
    PS2Runtime::RecompiledFunction g_sceneNodeDtor = nullptr;

    void sceneNodeDtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t node = getRegU32(ctx, 4);
        const auto word = [rdram](uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = address ? getConstMemPtr(rdram, address) : nullptr)
                std::memcpy(&v, p, sizeof(v));
            return v;
        };
        // (only entered afresh: after a yield it goes on at another pc, a0 no longer the node)
        if (ctx->pc == kSceneNodeDtor && node >= 0x00100000u && node < PS2_RAM_SIZE - 0x50u)
        {
            uint32_t link = node + 0x0Cu; // (the first child; then each child's sibling, +0x08)
            for (int guard = 0; guard < 100000; ++guard)
            {
                const uint32_t c = word(link);
                if (c == 0u)
                    break;
                const uint32_t vtable = word(c);
                if (c < 0x00100000u || c >= PS2_RAM_SIZE - 0x50u || vtable < 0x00100000u || vtable >= 0x00800000u)
                {
                    static int reports = 0;
                    if (reports++ < 8)
                    {
                        std::string name;
                        const uint32_t text = word(node + 0x40u);
                        for (uint32_t i = 0; text && i < 48; ++i)
                        {
                            const uint32_t ch = word((text + i) & ~3u) >> (8 * ((text + i) & 3u)) & 0xFFu;
                            if (ch < 32u || ch > 126u)
                                break;
                            name += static_cast<char>(ch);
                        }
                        std::fprintf(stderr, "[tf] a scene node's child at %08x has no vtable (%08x): left out; the parent %08x (vtable %08x, '%s')\n",
                                     c, vtable, node, word(node), name.c_str());
                    }
                    if (uint8_t *p = getMemPtr(rdram, link))
                        std::memset(p, 0, 4);
                    break;
                }
                link = c + 0x08u;
            }
        }
        g_sceneNodeDtor(rdram, ctx, runtime);
    }

    // TF_FREE_WATCH=<hex address> (testing aid): the heap block that holds that address - who
    // allocates it and who frees it, with the guest call stack. The libc allocator is native in the
    // port (stubs that return at once), so the block is read after the call.
    struct HeapStub
    {
        uint32_t address;
        int sizeReg;  // the size's register (0: none - a free)
        int blockReg; // the freed block's register (0: an allocation)
        PS2Runtime::RecompiledFunction original;
    };
    HeapStub g_heapStubs[] = {
        {0x0010F668u, 4, 0, nullptr}, // malloc(size)
        {0x0010F960u, 5, 0, nullptr}, // _malloc_r(reent, size)
        {0x0010C588u, 6, 0, nullptr}, // _calloc_r(reent, n, size) (the size: n*size, read below)
        {0x001120E8u, 6, 5, nullptr}, // _realloc_r(reent, old, size)
        {0x0010F6C0u, 0, 4, nullptr}, // free(block)
        {0x0010E398u, 0, 5, nullptr}, // _free_r(reent, block)
        // the game's own heap (its pools, then malloc): alloc/new/new[] reach 0x106410 and free/delete
        // 0x106370 through the pointers at gp-0x5DE0/-0x5DDC (a jump: the wrappers at 0x106160... see
        // no result)
        {0x00106410u, 4, 0, nullptr}, // alloc(size)
        {0x00106370u, 0, 4, nullptr}, // free(block)
    };
    uint32_t g_freeWatch = 0u;
    uint32_t g_watchBlock = 0u;
    uint32_t g_heapLogRa = 0xFFFFFFFFu; // TF_HEAP_LOG_RA=<hex>: every allocation returning there
    uint32_t g_freeSize = 0u;           // TF_FREE_SIZE=<hex>: every free of a block of that size (its header's)
    // TF_HEAP_CHECK=1: the game heap's live blocks - a free of a block not allocated (a double free),
    // an allocation handed out over a live block
    bool g_heapCheck = false;
    int g_heapCheckReports = 0;
    std::mutex g_liveMutex;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_live; // block -> size, ra
    std::unordered_set<uint32_t> g_pools;                     // the pools (blocks of 512 KB or more)
    std::unordered_map<uint32_t, std::pair<unsigned long long, uint32_t>> g_freedAt; // block -> vblank, ra

    template <int I>
    void heapStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const HeapStub &stub = g_heapStubs[I];
        const uint32_t block = stub.blockReg ? getRegU32(ctx, stub.blockReg) : 0u;
        uint32_t size = stub.sizeReg ? getRegU32(ctx, stub.sizeReg) : 0u;
        if (stub.address == 0x0010C588u)
            size *= getRegU32(ctx, 5);
        const unsigned long long tick = runtime->eeScheduler().currentVSyncTick();
        if (block != 0u && block == g_watchBlock)
        {
            std::fprintf(stderr, "[tf:heap] the block %08x (holds %08x) freed by %08x: ra %08x (vblank %llu)\n", block,
                         g_freeWatch, stub.address, getRegU32(ctx, 31), tick);
            tfLogCallStack("heap free");
            g_watchBlock = 0u;
        }
        const uint32_t ra = getRegU32(ctx, 31);
        if (g_freeSize && stub.address == 0x00106370u && block != 0u)
        {
            uint32_t raw = 0u, first = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, block - 4u))
                std::memcpy(&raw, p, 4);
            if (const uint8_t *p = getConstMemPtr(rdram, block))
                std::memcpy(&first, p, 4);
            if (raw == g_freeSize)
                std::fprintf(stderr, "[tf:freesize] %08x (size %x, first word %08x) freed: ra %08x (vblank %llu)\n", block, raw, first, ra, tick);
        }
        if (g_heapCheck && stub.address == 0x00106370u && block != 0u)
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            if (g_live.erase(block) == 0u && g_pools.erase(block) == 0u && g_heapCheckReports < 40)
            {
                ++g_heapCheckReports;
                const auto freed = g_freedAt.find(block);
                std::fprintf(stderr, "[tf:heapcheck] free of %08x, not allocated%s (ra %08x, vblank %llu; freed before at vblank %llu by ra %08x)\n",
                             block, freed != g_freedAt.end() ? " - a DOUBLE FREE" : "", ra, tick,
                             freed != g_freedAt.end() ? freed->second.first : 0ull, freed != g_freedAt.end() ? freed->second.second : 0u);
                tfLogCallStack("heapcheck free");
            }
            g_freedAt[block] = {tick, ra};
        }
        stub.original(rdram, ctx, runtime);
        if (g_heapCheck && stub.address == 0x00106410u && ctx->pc == ra && getRegU32(ctx, 2) != 0u)
        {
            const uint32_t got = getRegU32(ctx, 2);
            std::lock_guard<std::mutex> lock(g_liveMutex);
            auto next = g_live.lower_bound(got);
            bool overlap = next != g_live.end() && next->first < got + size;
            if (next != g_live.begin())
            {
                auto before = std::prev(next);
                overlap = overlap || before->first + before->second.first > got;
            }
            if (overlap && g_heapCheckReports < 40)
            {
                ++g_heapCheckReports;
                std::fprintf(stderr, "[tf:heapcheck] %08x (size %x) handed out over a live block (ra %08x, vblank %llu)\n", got, size, ra, tick);
                tfLogCallStack("heapcheck alloc");
            }
            if (size < 0x80000u) // (not the pools themselves: the blocks inside them are tracked)
                g_live[got] = {size, ra};
            else
                g_pools.insert(got);
            g_freedAt.erase(got);
        }
        if (g_heapCheck && stub.sizeReg && ctx->pc != ra)
        {
            static std::atomic<int> stopped{0};
            if (stopped.fetch_add(1) < 20)
                std::fprintf(stderr, "[tf:heapcheck] %08x(size %x) from %08x stopped part way at pc %08x (vblank %llu)\n", stub.address,
                             size, ra, ctx->pc, tick);
        }
        if (stub.sizeReg && ra == g_heapLogRa)
            std::fprintf(stderr, "[tf:heap] %08x(size %x) from %08x -> %08x%s (vblank %llu)\n", stub.address, size, ra,
                         getRegU32(ctx, 2), ctx->pc == ra ? "" : " (stopped part way)", tick);
        if (stub.sizeReg && ctx->pc == ra) // (not when the call stopped part way: its result is not in yet)
        {
            const uint32_t got = getRegU32(ctx, 2);
            if (got != 0u && got <= g_freeWatch && g_freeWatch < got + size)
            {
                g_watchBlock = got;
                std::fprintf(stderr, "[tf:heap] the block %08x (size %x, holds %08x) allocated by %08x: ra %08x (vblank %llu)\n",
                             got, size, g_freeWatch, stub.address, getRegU32(ctx, 31), tick);
                tfLogCallStack("heap alloc");
            }
        }
    }

    // TF_WRITE_WATCH=<hex address>: the native libc stubs (memset, memmove, strcpy...) whose
    // destination covers that address - a bulk write that starts below it, which the page guard
    // (TF_WATCH_ARM) does not see as a write to the range.
    struct WriteStub
    {
        uint32_t address;
        const char *name;
        PS2Runtime::RecompiledFunction original;
    };
    WriteStub g_writeStubs[] = {
        {0x00110340u, "memset", nullptr}, {0x00110238u, "memmove", nullptr}, {0x001135F0u, "strcpy", nullptr},
        {0x00113AD0u, "strncpy", nullptr}, {0x001131E8u, "strcat", nullptr}, {0x00118048u, "vsprintf", nullptr},
    };
    uint32_t g_writeWatch = 0u;

    uint32_t guestStrlen(uint8_t *rdram, uint32_t at)
    {
        uint32_t n = 0u;
        while (n < 0x10000u)
        {
            const uint8_t *p = getConstMemPtr(rdram, at + n);
            if (!p || *p == 0u)
                break;
            ++n;
        }
        return n;
    }

    template <int I>
    void writeStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const WriteStub &stub = g_writeStubs[I];
        const uint32_t dst = getRegU32(ctx, 4);
        uint32_t len = 0u;
        switch (I)
        {
        case 0: case 1: case 3: len = getRegU32(ctx, 6); break;     // memset/memmove/strncpy (dst, x, n)
        case 2: len = guestStrlen(rdram, getRegU32(ctx, 5)) + 1u; break; // strcpy
        case 4: len = guestStrlen(rdram, dst) + guestStrlen(rdram, getRegU32(ctx, 5)) + 1u; break; // strcat
        default: len = 0x400u; break;                                 // vsprintf (unknown before)
        }
        const uint32_t ra = getRegU32(ctx, 31);
        stub.original(rdram, ctx, runtime);
        if (I == 5)
            len = guestStrlen(rdram, dst) + 1u;
        if (dst <= g_writeWatch && g_writeWatch < dst + len)
        {
            std::fprintf(stderr, "[tf:write] %s(%08x, %x bytes) covers %08x: ra %08x (vblank %llu)\n", stub.name, dst, len,
                         g_writeWatch, ra, static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
            tfLogCallStack("covering write");
        }
    }

    void installWriteWatch(PS2Runtime &runtime)
    {
        const char *watch = std::getenv("TF_WRITE_WATCH");
        if (!watch || !*watch)
            return;
        g_writeWatch = static_cast<uint32_t>(std::strtoul(watch, nullptr, 16));
        PS2Runtime::RecompiledFunction wrappers[] = {writeStub<0>, writeStub<1>, writeStub<2>, writeStub<3>, writeStub<4>, writeStub<5>};
        for (int i = 0; i < 6; ++i)
            if ((g_writeStubs[i].original = runtime.lookupFunction(g_writeStubs[i].address)) != nullptr)
                runtime.registerFunction(g_writeStubs[i].address, wrappers[i]);
    }

    // TF_POOL_FREE_WATCH=<hex>[,<hex>...]: the pool free itself (TDG_MemoryManager func_00104770,
    // (heap, block)) - every way a block is freed goes through it.
    PS2Runtime::RecompiledFunction g_poolFree = nullptr;
    std::vector<uint32_t> g_poolFreeWatch;

    void poolFree(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x00104770u)
        {
            const uint32_t block = getRegU32(ctx, 5);
            uint32_t size = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, block - 4u))
                std::memcpy(&size, p, 4);
            for (uint32_t w : g_poolFreeWatch)
                if (block <= w && w < block + size)
                {
                    std::fprintf(stderr, "[tf:poolfree] %08x (size %x, holds %08x) freed: ra %08x (vblank %llu)\n", block, size, w,
                                 getRegU32(ctx, 31), static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()));
                    tfLogCallStack("pool free");
                }
        }
        g_poolFree(rdram, ctx, runtime);
    }

    // TF_CALL_WATCH=<hex function>[,<from vblank>]: every call of a guest function - its a0 and the
    // thread's last guest dispatches (who got there).
    PS2Runtime::RecompiledFunction g_callWatched = nullptr;
    uint32_t g_callWatch = 0u;
    unsigned long long g_callWatchFrom = 0ull;

    void callWatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const unsigned long long tick = runtime->eeScheduler().currentVSyncTick();
        if (ctx->pc == g_callWatch && tick >= g_callWatchFrom)
        {
            std::string trace = ps2xRecentDispatches();
            if (trace.size() > 400)
                trace = "..." + trace.substr(trace.size() - 400);
            std::fprintf(stderr, "[tf:call] %08x(a0 %08x, a1 %08x) ra %08x (vblank %llu) %s\n", g_callWatch, getRegU32(ctx, 4),
                         getRegU32(ctx, 5), getRegU32(ctx, 31), tick, trace.c_str());
        }
        g_callWatched(rdram, ctx, runtime);
    }

    // (N7) The game's memory probe (func_001071D0, called once by the memory manager's start-up, 0x107A50, before it
    // makes its main arena: tdgMemoryManager_Startup 0x105DC0 mallocs the result less 128 KB). It mallocs 128 MB, then
    // a MB less until one fits (N), frees it, then from (N+1)*1024 KB a KB less until one fits (K), frees it, and
    // returns K KB - so the game's arena is "the largest block the system malloc has", up to 129 MB (the start, the
    // 0x80 at 0x1071E4). With 256 MB of main RAM its largest block is ~223 MB: this is the same search from 256 MB,
    // through the runtime's malloc (the game's malloc wrapper, 0x106410, has no arena yet to look in at this point and
    // passes every size under 512 MB to the system malloc). Its byte at gp-0x4C0C (the out-of-memory message's mute,
    // set during the search) is left at 0 as the original leaves it. Only at 256 MB (TF_RAM_MB=32: the original).
    void ramProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto fits = [&](uint32_t bytes)
        {
            const uint32_t at = runtime->guestMalloc(bytes);
            if (at != 0u)
                runtime->guestFree(at);
            return at != 0u;
        };
        uint32_t mb = 256u;
        while (mb > 0u && !fits(mb << 20))
            --mb;
        uint32_t kb = (mb + 1u) * 1024u;
        while (kb > 0u && !fits(kb << 10))
            --kb;
        if (uint8_t *mute = getMemPtr(rdram, getRegU32(ctx, 28) - 0x4C0Cu))
            *mute = 0u;
        std::fprintf(stderr, "[tf] main RAM %u MB: the game's memory manager sized to %u KB (N7)\n", PS2Runtime::guestLayout().ramMb,
                     kb);
        setReturnU32(ctx, kb << 10);
        ctx->pc = getRegU32(ctx, 31);
    }

    void installRamProbe(PS2Runtime &runtime)
    {
        if (PS2Runtime::guestLayout().ramMb > 32u)
            runtime.registerFunction(0x001071D0u, ramProbe);
    }

    void installHeapWatch(PS2Runtime &runtime)
    {
        installRamProbe(runtime);
        installWriteWatch(runtime);
        if (const char *w = std::getenv("TF_CALL_WATCH"); w && *w)
        {
            char *end = nullptr;
            g_callWatch = static_cast<uint32_t>(std::strtoul(w, &end, 16));
            if (end && *end == ',')
                g_callWatchFrom = std::strtoull(end + 1, nullptr, 10);
            if ((g_callWatched = runtime.lookupFunction(g_callWatch)) != nullptr)
                runtime.registerFunction(g_callWatch, callWatch);
        }
        if (const char *w = std::getenv("TF_POOL_FREE_WATCH"); w && *w && (g_poolFree = runtime.lookupFunction(0x00104770u)) != nullptr)
        {
            for (const char *p = w; *p;)
            {
                char *end = nullptr;
                g_poolFreeWatch.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 16)));
                p = (end && *end == ',') ? end + 1 : "";
            }
            runtime.registerFunction(0x00104770u, poolFree);
        }
        const char *watch = std::getenv("TF_FREE_WATCH");
        const char *check = std::getenv("TF_HEAP_CHECK");
        g_heapCheck = check && *check == '1';
        if (const char *fs = std::getenv("TF_FREE_SIZE"); fs && *fs)
            g_freeSize = static_cast<uint32_t>(std::strtoul(fs, nullptr, 16));
        if ((!watch || !*watch) && !g_heapCheck && !g_freeSize)
            return;
        g_freeWatch = watch && *watch ? static_cast<uint32_t>(std::strtoul(watch, nullptr, 16)) : 0xFFFFFFF0u;
        if (const char *ra = std::getenv("TF_HEAP_LOG_RA"); ra && *ra)
            g_heapLogRa = static_cast<uint32_t>(std::strtoul(ra, nullptr, 16));
        PS2Runtime::RecompiledFunction wrappers[] = {heapStub<0>, heapStub<1>, heapStub<2>, heapStub<3>,
                                                     heapStub<4>, heapStub<5>, heapStub<6>, heapStub<7>};
        static_assert(sizeof(wrappers) / sizeof(wrappers[0]) == sizeof(g_heapStubs) / sizeof(g_heapStubs[0]));
        for (int i = 0; i < 8; ++i)
            if ((g_heapStubs[i].original = runtime.lookupFunction(g_heapStubs[i].address)) != nullptr)
                runtime.registerFunction(g_heapStubs[i].address, wrappers[i]);
        std::fprintf(stderr, "[tf:heap] watching the block that holds %08x\n", g_freeWatch);
    }

    // (TODO 99, a check: TF_LOD_LOG=1 - the LOD switcher's (0x26B260) pick per node: it asks the table getter (0x213B30) for
    // levels 5, 4, ... with the node's depth in $f20 and the node in $s4; a node whose level changes is logged with the
    // depth before and now)
    constexpr uint32_t kDetailDistanceOf = 0x00213B30u, kLodSwitcherAsk = 0x0026B3CCu;
    PS2Runtime::RecompiledFunction g_detailDistanceOf = nullptr;
    struct LodSeen
    {
        uint64_t tick = 0;
        int level = -1;
        float depth = 0.0f;
    };
    std::unordered_map<uint32_t, LodSeen> g_lodSeen;
    void detailDistanceOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == kLodSwitcherAsk && getRegU32(ctx, 4) == 5u)
        {
            const uint32_t node = getRegU32(ctx, 20);
            const float depth = ctx->f[20];
            float table[kDetailDistanceCount];
            std::memcpy(table, rdram + kDetailDistances, sizeof(table));
            int level = -1;
            for (int k = 5; k >= 0; --k)
                if (depth < table[k])
                {
                    level = k;
                    break;
                }
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            LodSeen &seen = g_lodSeen[node];
            if (seen.level >= 0 && seen.level != level)
            {
                std::fprintf(stderr, "[tf:lod] tick %llu node %08x: level %d -> %d, depth %g -> %g (last seen tick %llu)\n",
                             static_cast<unsigned long long>(tick), node, seen.level, level, seen.depth, depth,
                             static_cast<unsigned long long>(seen.tick));
            }
            seen.tick = tick;
            seen.level = level;
            seen.depth = depth;
        }
        // (the incidental objects' (0x33FEE0) pick: their squared distance from the camera object (*(gp-0x3DC0)) in $f20,
        // the object in $s2, the level kept at +0x168)
        if (getRegU32(ctx, 31) == 0x0033FFD8u && getRegU32(ctx, 4) == 5u)
        {
            const uint32_t object = getRegU32(ctx, 18);
            const float d2 = ctx->f[20];
            float table[kDetailDistanceCount];
            std::memcpy(table, rdram + kDetailDistances, sizeof(table));
            int level = -1;
            for (int k = 5; k >= 0; --k)
                if (d2 < table[k] * table[k])
                {
                    level = k;
                    break;
                }
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            LodSeen &seen = g_lodSeen[object | 0x80000000u];
            if (seen.level >= 0 && seen.level != level)
            {
                uint32_t camera = 0u;
                if (const uint8_t *p = getConstMemPtr(rdram, getRegU32(ctx, 28) - 0x3DC0u))
                    std::memcpy(&camera, p, 4);
                float eye[3] = {}, at[3] = {};
                if (const uint8_t *p = camera ? getConstMemPtr(rdram, camera + 0x40u) : nullptr)
                    std::memcpy(eye, p, 12);
                if (const uint8_t *p = getConstMemPtr(rdram, object + 0x40u))
                    std::memcpy(at, p, 12);
                std::fprintf(stderr, "[tf:lod] tick %llu incidental %08x: level %d -> %d, distance %g -> %g (last seen tick %llu); camera %08x at %g %g %g, object at %g %g %g\n",
                             static_cast<unsigned long long>(tick), object, seen.level, level, std::sqrt(seen.depth), std::sqrt(d2),
                             static_cast<unsigned long long>(seen.tick), camera, eye[0], eye[1], eye[2], at[0], at[1], at[2]);
            }
            seen.tick = tick;
            seen.level = level;
            seen.depth = d2;
        }
        g_detailDistanceOf(rdram, ctx, runtime);
    }

    // (TODO 99, a check, with TF_LOD_LOG=1: the scene's cull (0x2132D0) per node - its depth as the cull works it out (the
    // matrix stack's top, times the node's own matrix, the bounding sphere's centre at +0x30 and radius at +0x3C) and
    // whether its detail level's distance (+0x50, the fade's factors +0x54 / +0x58) holds; a node whose verdict changes, and
    // the cull's other inputs when they change: the render context's mask/match (+4, +8) and scale (+0x2C), gp-0x5BB8)
    constexpr uint32_t kSceneCull = 0x002132D0u;
    PS2Runtime::RecompiledFunction g_sceneCull = nullptr;
    struct CullSeen
    {
        uint64_t tick = 0;
        float depth = 0.0f;
        int verdict = -1;
    };
    std::unordered_map<uint64_t, CullSeen> g_cullSeen;
    uint64_t g_cullFrame = 0; // (frames, counted where the frame's list is kicked)
    bool g_cullFresh = true;
    uint32_t g_cullView = 0; // (the views a frame culls for, in order: a change of the context's scale starts the next)
    // (TODO 99, a check: TF_SET_TRACE=1 with TF_LOD_LOG=1 - the cull's nodes whose data (three pointer levels deep) names one
    // of the ground's geometry sets of the flash frames, traced every view of every frame: their depth and verdict, their
    // flags, the cull callback's object (node+0x18) and what its two methods (vtable +0x14, +0x18) are given and give back)
    const bool g_setTrace = std::getenv("TF_SET_TRACE") && *std::getenv("TF_SET_TRACE") == '1';
    constexpr uint32_t kTraceSets[] = {0x013e7a50u, 0x013e8480u, 0x013e87f0u, 0x013e9130u, 0x0140b760u};
    std::unordered_map<uint32_t, int> g_traceNode; // node -> 1 + the set's index, 0 none
    int g_traceBudget = 30000;
    int traceSetOf(uint8_t *rdram, uint32_t node, uint32_t &path)
    {
        auto word = [rdram](uint32_t a, uint32_t &w) {
            const uint8_t *p = (a & 3u) == 0u && a >= 0x00100000u && a < PS2_RAM_SIZE ? getConstMemPtr(rdram, a) : nullptr;
            if (p)
                std::memcpy(&w, p, 4);
            return p != nullptr;
        };
        auto isSet = [](uint32_t w) {
            for (int k = 0; k < 5; ++k)
                if (w == kTraceSets[k])
                    return k + 1;
            return 0;
        };
        for (uint32_t i = 0; i < 0x100u; i += 4u)
        {
            uint32_t a = 0u;
            if (!word(node + i, a))
                continue;
            if (const int k = isSet(a))
                return path = i, k;
            for (uint32_t j = 0; j < 0x80u; j += 4u)
            {
                uint32_t b = 0u;
                if (!word(a + j, b))
                    continue;
                if (const int k = isSet(b))
                    return path = i << 16 | j << 8, k;
                for (uint32_t m = 0; m < 0x40u; m += 4u)
                {
                    uint32_t c = 0u;
                    if (word(b + m, c))
                        if (const int k = isSet(c))
                            return path = i << 16 | j << 8 | m, k;
                }
            }
        }
        return 0;
    }
    struct TraceCb
    {
        uint32_t address = 0u;
        PS2Runtime::RecompiledFunction original = nullptr;
    };
    TraceCb g_traceCb[8];
    int g_traceCbCount = 0;
    template <int N>
    void traceCb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t node = getRegU32(ctx, 20), object = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        g_traceCb[N].original(rdram, ctx, runtime);
        const auto it = g_traceNode.find(node);
        if (it != g_traceNode.end() && it->second > 0 && g_traceBudget-- > 0)
        {
            uint32_t w[8] = {};
            if (const uint8_t *p = getConstMemPtr(rdram, object))
                std::memcpy(w, p, sizeof(w));
            std::fprintf(stderr, "[tf:set] tick %llu frame %llu view %u node %08x cb %08x (from %08x) a1 %u -> %u; object %08x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), static_cast<unsigned long long>(g_cullFrame), g_cullView, node, g_traceCb[N].address, ra,
                         a1, getRegU32(ctx, 2), object, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    PS2Runtime::RecompiledFunction g_traceCbFns[8] = {traceCb<0>, traceCb<1>, traceCb<2>, traceCb<3>, traceCb<4>, traceCb<5>, traceCb<6>, traceCb<7>};
    void traceHook(PS2Runtime *runtime, uint32_t address)
    {
        if (!address)
            return;
        for (int i = 0; i < g_traceCbCount; ++i)
            if (g_traceCb[i].address == address)
                return;
        if (g_traceCbCount >= 8)
            return;
        PS2Runtime::RecompiledFunction original = runtime->lookupFunction(address);
        if (!original)
            return;
        g_traceCb[g_traceCbCount] = TraceCb{address, original};
        runtime->registerFunction(address, g_traceCbFns[g_traceCbCount]);
        ++g_traceCbCount;
        std::fprintf(stderr, "[tf:set] cull callback %08x hooked\n", address);
    }

    void sceneCull(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t node = getRegU32(ctx, 4), gp = getRegU32(ctx, 28);
        auto rd = [rdram](uint32_t a, void *out, size_t n) {
            const uint8_t *p = a ? getConstMemPtr(rdram, a) : nullptr;
            if (p)
                std::memcpy(out, p, n);
            else
                std::memset(out, 0, n);
            return p != nullptr;
        };
        uint32_t rc = 0u;
        rd(gp - 0x464Cu, &rc, 4);
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        {
            static uint32_t lastMask = 0u, lastMatch = 0u;
            static float lastScale = 0.0f, lastK = 0.0f;
            uint32_t mask = 0u, match = 0u;
            float scale = 0.0f, k = 0.0f;
            rd(rc + 4u, &mask, 4);
            rd(rc + 8u, &match, 4);
            rd(rc + 0x2Cu, &scale, 4);
            rd(gp - 0x5BB8u, &k, 4);
            if (g_cullFresh)
                g_cullFresh = false;
            else if (scale != lastScale)
                ++g_cullView;
            static std::vector<float> knownScales;
            if (mask != lastMask || match != lastMatch || k != lastK ||
                std::find(knownScales.begin(), knownScales.end(), scale) == knownScales.end())
            {
                if (knownScales.size() < 64u)
                    knownScales.push_back(scale);
                std::fprintf(stderr, "[tf:cull] tick %llu context %08x view %u: mask %08x match %08x scale %g k %g\n",
                             static_cast<unsigned long long>(tick), rc, g_cullView, mask, match, scale, k);
            }
            lastMask = mask, lastMatch = match, lastScale = scale, lastK = k;
        }
        uint32_t flags = 0u, mask = 0u, match = 0u, local = 0u, top = 0u, level = 0u;
        rd(node + 0x10u, &flags, 4);
        rd(rc + 4u, &mask, 4);
        rd(rc + 8u, &match, 4);
        rd(node + 0x14u, &local, 4);
        rd(rc + 0x3Cu, &top, 4);
        rd(node + 0x50u, &level, 4);
        float P[16], L[16], sphere[4], fade[2] = {};
        uint8_t built = 1u;
        if ((flags & mask) == match && rd(top - 0x40u, P, 64) && rd(node + 0x30u, sphere, 16) && level < kDetailDistanceCount &&
            (!local || (rd(local, L, 64) && rd(local + 0xCu, &built, 1) && built)))
        {
            rd(node + 0x54u, fade, 8);
            float zc[4] = {P[2], P[6], P[10], P[14]}; // (the z column: rows 0-3)
            if (local)
                for (int i = 0; i < 4; ++i)
                    zc[i] = L[4 * i] * P[2] + L[4 * i + 1] * P[6] + L[4 * i + 2] * P[10] + (i == 3 ? P[14] : 0.0f);
            float depth = zc[0] * sphere[0] + zc[1] * sphere[1] + zc[2] * sphere[2] + zc[3] - sphere[3];
            if (depth < 0.0f)
                depth = 0.0f;
            float table[kDetailDistanceCount];
            std::memcpy(table, rdram + kDetailDistances, sizeof(table));
            const float t = table[level];
            const int verdict = depth > t ? 0 : depth < t * fade[0] ? 1 : depth <= t * fade[1] ? 2 : 0;
            CullSeen &seen = g_cullSeen[(static_cast<uint64_t>(g_cullView) << 32) | node];
            static int budget = 4000;
            const bool logIt = seen.tick != g_cullFrame && budget > 0;
            if (logIt && seen.verdict >= 0 && seen.verdict != verdict && seen.tick + 2u >= g_cullFrame && budget--)
                std::fprintf(stderr, "[tf:cull] tick %llu view %u node %08x: %d -> %d, depth %g -> %g, level %u (%g x %g, %g), local %08x\n",
                             static_cast<unsigned long long>(tick), g_cullView, node, seen.verdict, verdict, seen.depth, depth, level, t, fade[0],
                             fade[1], local);
            else if (logIt && seen.verdict >= 0 && seen.tick + 2u >= g_cullFrame && std::fabs(depth - seen.depth) > 200.0f + 0.3f * seen.depth && budget--)
                std::fprintf(stderr, "[tf:cull] tick %llu view %u node %08x: depth jumped %g -> %g, level %u, local %08x\n",
                             static_cast<unsigned long long>(tick), g_cullView, node, seen.depth, depth, level, local);
            seen.tick = g_cullFrame;
            seen.depth = depth;
            seen.verdict = verdict;
        }
        if (g_setTrace)
        {
            auto it = g_traceNode.find(node);
            uint32_t path = 0u;
            if (it == g_traceNode.end())
            {
                const int k = traceSetOf(rdram, node, path);
                it = g_traceNode.emplace(node, k).first;
                if (k)
                    std::fprintf(stderr, "[tf:set] node %08x names set %08x (offsets %06x)\n", node, kTraceSets[k - 1], path);
            }
            if (it->second > 0)
            {
                uint32_t object = 0u, vt = 0u, f14 = 0u, f18 = 0u, kind = 0u, nflags = 0u, nmask = 0u, nmatch = 0u;
                rd(node + 0x18u, &object, 4);
                rd(object, &vt, 4);
                rd(vt + 0x14u, &f14, 4);
                rd(vt + 0x18u, &f18, 4);
                rd(node + 0x4Cu, &kind, 4);
                rd(node + 0x10u, &nflags, 4);
                rd(rc + 4u, &nmask, 4);
                rd(rc + 8u, &nmatch, 4);
                if (object && vt)
                {
                    traceHook(runtime, f14);
                    traceHook(runtime, f18);
                }
                // (and the draw it is handed to when kept: the render context's drawer of its kind, vtable +0x10)
                uint32_t drawer = 0u, dvt = 0u, dfn = 0u;
                if (kind < 64u && rd(rc + 0x40u + 4u * kind, &drawer, 4) && drawer && rd(drawer, &dvt, 4) && dvt && rd(dvt + 0x10u, &dfn, 4))
                    traceHook(runtime, dfn);
                const auto seen = g_cullSeen.find((static_cast<uint64_t>(g_cullView) << 32) | node);
                if (g_traceBudget-- > 0)
                    std::fprintf(stderr, "[tf:set] tick %llu frame %llu view %u node %08x (set %d): flags %08x (mask %08x match %08x), depth %g verdict %d, kind %u, object %08x (%08x %08x)\n",
                                 static_cast<unsigned long long>(tick), static_cast<unsigned long long>(g_cullFrame), g_cullView, node, it->second, nflags,
                                 nmask, nmatch, seen != g_cullSeen.end() && seen->second.tick == g_cullFrame ? seen->second.depth : -1.0f,
                                 seen != g_cullSeen.end() && seen->second.tick == g_cullFrame ? seen->second.verdict : -9, kind, object, f14, f18);
            }
        }
        g_sceneCull(rdram, ctx, runtime);
    }

    // N8 item 4 / TODO 137 (the user's choice B, 2026-11-03): an enemy's detail level (taLODAnimationActor, vtable
    // 0x6BB0E0: its level +0xD8 over the description +0xF0 - +0xC the steps' count, +0x10 the steps, 12 bytes: flags, a
    // vertex count, MeshDetailLevel) puts the step's MeshDetailLevel at +0xA4 each frame. Two readers: the animation's
    // streams (anAnimationControllerStream::vfunc_09: a stream entry runs while its threshold is at or under it) and the
    // drawing (anAnimationActor::Cull, 0x162E20: copied to gp-0x5D8C for the mesh surfaces' own cull, and a node whose
    // flag 0x2000 is set is left out - not posed, not drawn - while +0xA4 is at or under its threshold, +0x54 -> +0x5C).
    // A test aid to map which parts each level leaves out and whether they draw right with the animation of that level:
    // TF_CHAR_DETAIL="<vblank>:<mode>,..." - "game" (as the game), "draw<N>" (the drawing alone, +0xA4 for Cull only
    // N steps finer; the animation reads the game's), "anim" (+0xA4 the finest step's for everything: the reference, the
    // animation at full detail too). Off without it; never a setting (the animation must stay as the game has it).
    constexpr uint32_t kAnimationActorCull = 0x00162E20u, kLodAnimationActorVtable = 0x006BB0E0u;
    PS2Runtime::RecompiledFunction g_animationActorCull = nullptr;
    struct CharDetailStep
    {
        uint64_t at;
        int mode; // 0 the game's, N > 0 the drawing N steps finer, -1 the finest step's for everything
    };
    std::vector<CharDetailStep> g_charDetailSteps;
    int g_charDetailMode = 0;
    // The Enemy detail setting (TF_ENEMY_DETAIL, off by default): the drawing alone that many steps finer, as the test
    // aid's draw<N> (the animation and MeshDetailLevel the game's - gameplay unchanged). TF_CHAR_DETAIL's steps win.
    std::atomic<int> g_enemyDetail{0};

    // (TF_CHAR_DETAIL_ONLY=<n>[,<n>...]: the finer drawing only for the types whose first step has that many vertices -
    // a bisect of which type's parts feed the game)
    std::vector<uint32_t> g_charDetailOnly;
    bool charDetailTypeWanted(const uint8_t *rdram, uint32_t actor)
    {
        if (g_charDetailOnly.empty())
            return true;
        uint32_t description = 0u, steps = 0u, vertices = 0u;
        std::memcpy(&description, rdram + ((actor + 0xF0u) & PS2_RAM_MASK), 4);
        if (description < 0x00100000u || description >= PS2_RAM_SIZE - 0x20u)
            return false;
        std::memcpy(&steps, rdram + description + 0x10u, 4);
        if (steps < 0x00100000u || steps >= PS2_RAM_SIZE - 12u)
            return false;
        std::memcpy(&vertices, rdram + steps + 4u, 4);
        return std::find(g_charDetailOnly.begin(), g_charDetailOnly.end(), vertices) != g_charDetailOnly.end();
    }

    float lodStepValue(const uint8_t *rdram, uint32_t actor, int level)
    {
        uint32_t description = 0u, steps = 0u;
        int32_t count = 0;
        std::memcpy(&description, rdram + ((actor + 0xF0u) & PS2_RAM_MASK), 4);
        if (description < 0x00100000u || description >= PS2_RAM_SIZE - 0x20u)
            return -1.0f;
        std::memcpy(&count, rdram + description + 0xCu, 4);
        std::memcpy(&steps, rdram + description + 0x10u, 4);
        if (count <= 0 || count > 16 || steps < 0x00100000u || steps >= PS2_RAM_SIZE - 12u * 16u)
            return -1.0f;
        level = std::clamp(level, 0, count - 1);
        float value = 0.0f;
        std::memcpy(&value, rdram + steps + 12u * static_cast<uint32_t>(level) + 8u, 4);
        return value;
    }

    uint32_t gameSeed(const uint8_t *rdram)
    {
        uint32_t at = 0u, seed = 0u;
        std::memcpy(&at, rdram + 0x006CF320u, 4);
        if (at >= 0x00100000u && at < PS2_RAM_SIZE - 4u)
            std::memcpy(&at, rdram + at, 4);
        if (at >= 0x00100000u && at < PS2_RAM_SIZE - 4u)
            std::memcpy(&seed, rdram + at, 4);
        return seed;
    }

    void animationActorCullInner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void lodCensusVerdict(const uint8_t *rdram, uint32_t actor);
    uint64_t g_lodCensusDrawn[2] = {}, g_lodCensusCulled[2] = {}; // ([0] the LOD actors, [1] the others)

    // (TF_RNG_LOG=1 also logs each actor Cull that moves the game's random seed: the drawing drawing on the game's
    // random numbers - the Antarctica split's suspect)
    // (TODO 151) Enemy detail Full: an animation actor - an enemy, a character, a pickup - whole at any distance. Its own
    // node's cull (0x2132D0, the first thing its Cull does) leaves the whole actor out past its detail level's distance
    // (the table at 0x61A960, by the node's +0x50) and when it is small on the screen (its radius +0x3C under
    // k (gp-0x5BB8) x the view's scale x its depth: tuned for the console's 640 x 448) - a far enemy's own sub-actors
    // (switcher parts drawn as actors of their own) dropped while the rest was drawn. For the actor's Cull alone the
    // distance has no limit and the size test is off; then both are the game's again. Drawing only: the cull decides
    // what is drawn (its verdict is the node's visibility flags, which the drawing reads).
    struct ActorCullOpen
    {
        uint32_t tableAt = 0u, table = 0u, k = 0u;
        bool on = false;
    };
    ActorCullOpen openActorCull(uint8_t *rdram, R5900Context *ctx)
    {
        ActorCullOpen o;
        static const bool off = std::getenv("TF_ENEMY_WHOLE") && *std::getenv("TF_ENEMY_WHOLE") == '0'; // (debug: the finest step alone)
        const uint32_t actor = getRegU32(ctx, 4), gp = getRegU32(ctx, 28);
        if (off || !g_charDetailSteps.empty() || g_enemyDetail.load(std::memory_order_relaxed) < kEnemyFull || actor < 0x00100000u ||
            actor >= PS2_RAM_SIZE - 0x100u || gp < 0x00100000u || gp >= PS2_RAM_SIZE)
            return o;
        uint32_t level = 0u;
        std::memcpy(&level, rdram + actor + 0x50u, 4);
        if (level < kDetailDistanceCount)
        {
            o.tableAt = kDetailDistances + 4u * level;
            std::memcpy(&o.table, rdram + o.tableAt, 4);
            std::memcpy(rdram + o.tableAt, &kNoLimit, 4);
        }
        std::memcpy(&o.k, rdram + gp - 0x5BB8u, 4);
        const float none = 0.0f;
        std::memcpy(rdram + gp - 0x5BB8u, &none, 4);
        o.on = true;
        return o;
    }
    void closeActorCull(uint8_t *rdram, uint32_t gp, const ActorCullOpen &o)
    {
        if (!o.on)
            return;
        if (o.tableAt)
            std::memcpy(rdram + o.tableAt, &o.table, 4);
        std::memcpy(rdram + gp - 0x5BB8u, &o.k, 4);
    }

    // (TODO 137) The Mini-Con and Data-Con pickups (taPickupPanel, its slot 9: 0x0037C4C0, called by the pickup manager's
    // Cull for each pickup) are not animation actors: each frame the panel sets its model node's (+0xD0) matrix and
    // calls that node's Cull (vtable +0x10). With Enemy detail Full that Cull runs with the node cull's distance limit
    // and small-on-screen test lifted, as an animation actor's does (openActorCull, the model node's own +0x50) - the
    // model drawn at any distance, not only its glow. TF_LOD_CENSUS=1: each panel's model once - class, level, radius.
    constexpr uint32_t kPickupPanelDraw = 0x0037C4C0u;
    PS2Runtime::RecompiledFunction g_pickupPanelDraw = nullptr;
    ActorCullOpen openActorCull(uint8_t *rdram, R5900Context *ctx);
    void closeActorCull(uint8_t *rdram, uint32_t gp, const ActorCullOpen &o);
    void pickupPanelDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t panel = getRegU32(ctx, 4), gp = getRegU32(ctx, 28);
        uint32_t model = 0u;
        if (panel >= 0x00100000u && panel < PS2_RAM_SIZE - 0x100u)
            std::memcpy(&model, rdram + panel + 0xD0u, 4);
        if (model < 0x00100000u || model >= PS2_RAM_SIZE - 0x100u)
        {
            g_pickupPanelDraw(rdram, ctx, runtime);
            return;
        }
        static const bool census = std::getenv("TF_LOD_CENSUS") && *std::getenv("TF_LOD_CENSUS") == '1';
        if (census)
        {
            static std::unordered_set<uint32_t> seen;
            if (seen.insert(model).second && seen.size() < 200u)
            {
                uint32_t pv = 0u, mv = 0u, level = 0u;
                float radius = 0.0f;
                std::memcpy(&pv, rdram + panel, 4);
                std::memcpy(&mv, rdram + model, 4);
                std::memcpy(&level, rdram + model + 0x50u, 4);
                std::memcpy(&radius, rdram + model + 0x3Cu, 4);
                std::fprintf(stderr, "[tf:lodcensus] pickup %08x (vtable %06x): model %08x vtable %06x level %u radius %g\n", panel, pv, model, mv,
                             level, radius);
            }
        }
        // (the model node's own cull: its detail level and the size test - as for an animation actor)
        R5900Context *c = ctx;
        const uint32_t a0 = getRegU32(c, 4);
        SET_GPR_U32(c, 4, model);
        const ActorCullOpen open = openActorCull(rdram, c);
        SET_GPR_U32(c, 4, a0);
        g_pickupPanelDraw(rdram, ctx, runtime);
        closeActorCull(rdram, gp, open);
    }

    // (TF_LOD_CENSUS=1) The pickup manager's Cull (0x00378B70: its pickups from +0x140, the next at +8): every 20 s
    // each pickup's class and position - where to put the player for a far look (TF_PLACE).
    constexpr uint32_t kPickupManagerCull = 0x00378B70u;
    PS2Runtime::RecompiledFunction g_pickupManagerCull = nullptr;
    void pickupManagerCull(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t last = 0u;
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        const uint32_t manager = getRegU32(ctx, 4);
        if (tick >= last + 2400u && manager >= 0x00100000u && manager < PS2_RAM_SIZE - 0x200u)
        {
            last = tick;
            uint32_t p = 0u;
            std::memcpy(&p, rdram + manager + 0x140u, 4);
            std::string text;
            for (int i = 0; i < 64 && p >= 0x00100000u && p < PS2_RAM_SIZE - 0x200u; ++i)
            {
                uint32_t v = 0u;
                float pos[3] = {};
                std::memcpy(&v, rdram + p, 4);
                std::memcpy(pos, rdram + p + 0x40u, 12);
                char one[80];
                std::snprintf(one, sizeof(one), " [%08x %06x %.0f %.0f %.0f]", p, v, pos[0], pos[1], pos[2]);
                text += one;
                std::memcpy(&p, rdram + p + 8u, 4);
            }
            std::fprintf(stderr, "[tf:lodcensus] tick %llu pickups:%s\n", static_cast<unsigned long long>(tick), text.c_str());
        }
        g_pickupManagerCull(rdram, ctx, runtime);
    }

    void animationActorCull(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool rngLog = std::getenv("TF_RNG_LOG") && *std::getenv("TF_RNG_LOG") == '1';
        if (!rngLog)
        {
            const uint32_t gp = getRegU32(ctx, 28), actor = getRegU32(ctx, 4);
            const ActorCullOpen open = openActorCull(rdram, ctx);
            animationActorCullInner(rdram, ctx, runtime);
            closeActorCull(rdram, gp, open); // (gp as at the call: a Cull that stopped part-way comes back mid-way)
            lodCensusVerdict(rdram, actor);
            return;
        }
        const uint32_t actor = getRegU32(ctx, 4);
        const uint32_t before = gameSeed(rdram);
        animationActorCullInner(rdram, ctx, runtime);
        const uint32_t after = gameSeed(rdram);
        if (after != before)
        {
            static int budget = 4000;
            if (budget-- > 0)
                std::fprintf(stderr, "[tf:rngcull] %llu actor %08x seed %08x -> %08x\n",
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), actor, before, after);
        }
    }

    // (TF_LOD_CENSUS=1, a test aid: every 10 s, per actor class - vtable - the Culls by the LOD level the game picked
    // and its step count, and how many the Enemy detail setting still left at a coarser step than the finest)
    void lodCensus(const uint8_t *rdram, uint32_t actor, uint64_t tick)
    {
        static const bool on = std::getenv("TF_LOD_CENSUS") && *std::getenv("TF_LOD_CENSUS") == '1';
        if (!on || actor < 0x00100000u || actor >= PS2_RAM_SIZE - 0x100u)
            return;
        static std::map<std::tuple<uint32_t, int32_t, int32_t, int32_t>, uint64_t> counts; // (vtable, level, steps, drawn)
        static uint64_t last = 0u;
        uint32_t vptr = 0u, description = 0u;
        int32_t level = -1, steps = -1;
        std::memcpy(&vptr, rdram + actor, 4);
        if (vptr == kLodAnimationActorVtable)
        {
            std::memcpy(&level, rdram + actor + 0xD8u, 4);
            std::memcpy(&description, rdram + actor + 0xF0u, 4);
            if (description >= 0x00100000u && description < PS2_RAM_SIZE - 0x20u)
                std::memcpy(&steps, rdram + description + 0xCu, 4);
        }
        const int mode = g_charDetailSteps.empty() ? g_enemyDetail.load(std::memory_order_relaxed) : g_charDetailMode;
        int32_t drawn = vptr != kLodAnimationActorVtable ? -1 : mode == 0 ? level : std::max(0, level - mode);
        if (vptr != kLodAnimationActorVtable)
        {
            // (another class: the parts its detail +0xA4 leaves out - drawn = -100 - that count, -1 none)
            float detail = 0.0f;
            uint32_t list = 0u;
            int32_t count = 0, out = 0;
            std::memcpy(&detail, rdram + actor + 0xA4u, 4);
            std::memcpy(&list, rdram + actor + 0x98u, 4);
            std::memcpy(&count, rdram + actor + 0x94u, 4);
            for (int32_t i = 0; i < count && i < 256 && list >= 0x00100000u && list < PS2_RAM_SIZE - 0x70u * 256u; ++i)
            {
                uint16_t flags = 0u;
                uint32_t binding = 0u;
                float threshold = 0.0f;
                std::memcpy(&flags, rdram + list + 0x70u * i + 0x50u, 2);
                std::memcpy(&binding, rdram + list + 0x70u * i + 0x54u, 4);
                if (!(flags & 0x2000u) || binding < 0x00100000u || binding >= PS2_RAM_SIZE - 0x60u)
                    continue;
                std::memcpy(&threshold, rdram + binding + 0x5Cu, 4);
                if (detail <= threshold)
                    ++out;
            }
            level = static_cast<int32_t>(detail * 10.0f); // (the detail x10)
            drawn = out ? -100 - out : -1;
        }
        ++counts[{vptr, level, steps, drawn}];
        if (tick >= last + 1200u)
        {
            last = tick;
            std::string text;
            for (const auto &[k, n] : counts)
            {
                char one[96];
                std::snprintf(one, sizeof(one), " [%06x L%d/%d drawn %d: %llu]", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k),
                              static_cast<unsigned long long>(n));
                text += one;
            }
            std::fprintf(stderr, "[tf:lodcensus] tick %llu: LOD actors drawn %llu / left out %llu, others %llu / %llu;%s\n",
                         static_cast<unsigned long long>(tick), static_cast<unsigned long long>(g_lodCensusDrawn[0]),
                         static_cast<unsigned long long>(g_lodCensusCulled[0]), static_cast<unsigned long long>(g_lodCensusDrawn[1]),
                         static_cast<unsigned long long>(g_lodCensusCulled[1]), text.c_str());
            counts.clear();
            g_lodCensusDrawn[0] = g_lodCensusDrawn[1] = g_lodCensusCulled[0] = g_lodCensusCulled[1] = 0u;
        }
    }

    // (TF_LOD_CENSUS: after each Cull, the actor drawn or left out by its node's cull - +0xA2 set and +0x10 bits 0 and 1)
    void lodCensusVerdict(const uint8_t *rdram, uint32_t actor)
    {
        static const bool on = std::getenv("TF_LOD_CENSUS") && *std::getenv("TF_LOD_CENSUS") == '1';
        if (!on || actor < 0x00100000u || actor >= PS2_RAM_SIZE - 0x100u)
            return;
        uint32_t vptr = 0u, flags = 0u;
        std::memcpy(&vptr, rdram + actor, 4);
        std::memcpy(&flags, rdram + actor + 0x10u, 4);
        const int c = vptr == kLodAnimationActorVtable ? 0 : 1;
        if (rdram[actor + 0xA2u] && (flags & 3u) == 3u)
            ++g_lodCensusDrawn[c];
        else
            ++g_lodCensusCulled[c];
    }

    void animationActorCullInner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t actor = getRegU32(ctx, 4);
        lodCensus(rdram, actor, runtime->eeScheduler().currentVSyncTick());
        uint32_t vptr = 0u;
        const int mode = g_charDetailSteps.empty() ? g_enemyDetail.load(std::memory_order_relaxed) : g_charDetailMode;
        if (mode == 0 || actor < 0x00100000u || actor >= PS2_RAM_SIZE - 0x100u)
        {
            g_animationActorCull(rdram, ctx, runtime);
            return;
        }
        std::memcpy(&vptr, rdram + actor, 4);
        if (vptr != kLodAnimationActorVtable)
        {
            g_animationActorCull(rdram, ctx, runtime);
            return;
        }
        int32_t level = 0;
        std::memcpy(&level, rdram + actor + 0xD8u, 4);
        // (each description once: its steps - an enemy type known by its vertex counts; the test aid's)
        if (!g_charDetailSteps.empty())
        {
            static std::unordered_set<uint32_t> logged;
            uint32_t description = 0u;
            std::memcpy(&description, rdram + ((actor + 0xF0u) & PS2_RAM_MASK), 4);
            if (description >= 0x00100000u && description < PS2_RAM_SIZE - 0x20u && logged.insert(description).second)
            {
                int32_t count = 0;
                uint32_t steps = 0u;
                std::memcpy(&count, rdram + description + 0xCu, 4);
                std::memcpy(&steps, rdram + description + 0x10u, 4);
                std::string text;
                for (int32_t i = 0; i < count && i < 16 && steps >= 0x00100000u && steps < PS2_RAM_SIZE - 12u * 16u; ++i)
                {
                    uint32_t flags = 0u, vertices = 0u;
                    float detail = 0.0f;
                    std::memcpy(&flags, rdram + steps + 12u * i, 4);
                    std::memcpy(&vertices, rdram + steps + 12u * i + 4u, 4);
                    std::memcpy(&detail, rdram + steps + 12u * i + 8u, 4);
                    char one[64];
                    std::snprintf(one, sizeof(one), " [%x %u %g]", flags, vertices, detail);
                    text += one;
                }
                std::fprintf(stderr, "[tf:chardetail] description %08x (actor %08x, level %d): %d steps%s\n", description, actor, level, count,
                             text.c_str());
            }
        }
        if (!charDetailTypeWanted(rdram, actor))
        {
            g_animationActorCull(rdram, ctx, runtime);
            return;
        }
        float before = 0.0f;
        std::memcpy(&before, rdram + actor + 0xA4u, 4);
        const float value = lodStepValue(rdram, actor, mode > 0 ? level - mode : 0);
        if (value < 0.0f)
        {
            g_animationActorCull(rdram, ctx, runtime);
            return;
        }
        // (TF_CHAR_DETAIL_PARTS, a bisect of what in the finer drawing feeds the game: "parts" - only the parts Cull leaves
        // out at the game's level drawn, MeshDetailLevel the game's; "surfaces" - MeshDetailLevel finer, the parts left
        // out as at the game's level; "bones:<a>-<b>" - "parts" for those part indices only. The parts: the actor's binding
        // list +0x98 (+0x94 of them, 0x70 bytes each: +0x50 flags, 0x2000 = by detail; +0x52 depth; +0x54 -> +0x5C the
        // detail at or under which it is left out) - their threshold put past the level for the one Cull, then back.)
        static const std::string partsMode = []() { const char *e = std::getenv("TF_CHAR_DETAIL_PARTS"); return std::string(e ? e : ""); }();
        if (!partsMode.empty() && mode > 0)
        {
            int lo = 0, hi = 1 << 30;
            if (partsMode.rfind("bones:", 0) == 0)
                std::sscanf(partsMode.c_str() + 6, "%d-%d", &lo, &hi);
            const bool surfaces = partsMode == "surfaces";
            uint32_t list = 0u;
            int32_t count = 0;
            std::memcpy(&list, rdram + actor + 0x98u, 4);
            std::memcpy(&count, rdram + actor + 0x94u, 4);
            std::vector<std::pair<uint32_t, float>> changed;
            static std::unordered_set<uint32_t> dumped;
            const bool dump = dumped.insert(list).second;
            for (int32_t i = 0; i < count && i < 256 && list >= 0x00100000u && list < PS2_RAM_SIZE - 0x70u * 256u; ++i)
            {
                const uint32_t bone = list + 0x70u * static_cast<uint32_t>(i);
                uint16_t flags = 0u;
                int16_t depth = 0;
                uint32_t binding = 0u;
                std::memcpy(&flags, rdram + bone + 0x50u, 2);
                std::memcpy(&depth, rdram + bone + 0x52u, 2);
                std::memcpy(&binding, rdram + bone + 0x54u, 4);
                if (binding < 0x00100000u || binding >= PS2_RAM_SIZE - 0x60u)
                    continue;
                float threshold = 0.0f;
                std::memcpy(&threshold, rdram + binding + 0x5Cu, 4);
                if (dump)
                    std::fprintf(stderr, "[tf:chardetail] parts of %08x: %d flags %04x depth %d threshold %g (binding %08x)\n", list, i, flags,
                                 depth, threshold, binding);
                if (!(flags & 0x2000u) || !(before <= threshold) || !(value > threshold))
                    continue; // (not one the finer drawing adds)
                if (surfaces)
                {
                    changed.emplace_back(binding + 0x5Cu, threshold);
                    std::memcpy(rdram + binding + 0x5Cu, &value, 4); // (left out at the finer level too)
                }
                else if (i >= lo && i <= hi)
                {
                    changed.emplace_back(binding + 0x5Cu, threshold);
                    const float under = before - 1.0f;
                    std::memcpy(rdram + binding + 0x5Cu, &under, 4); // (drawn at the game's level)
                }
            }
            if (!surfaces)
                std::memcpy(rdram + actor + 0xA4u, &before, 4);
            else
                std::memcpy(rdram + actor + 0xA4u, &value, 4);
            g_animationActorCull(rdram, ctx, runtime);
            for (auto it = changed.rbegin(); it != changed.rend(); ++it)
                std::memcpy(rdram + it->first, &it->second, 4);
            std::memcpy(rdram + actor + 0xA4u, &before, 4);
            return;
        }
        std::memcpy(rdram + actor + 0xA4u, &value, 4);
        g_animationActorCull(rdram, ctx, runtime);
        if (mode > 0) // (the drawing's alone: the animation reads the game's value again)
            std::memcpy(rdram + actor + 0xA4u, &before, 4);
    }

    // N7 (TF_POOL_CENSUS=1, a test aid): how close the game comes to its fixed limits. Each pool's "take" function is
    // wrapped: its calls, and how many came back empty (0: the pool was full - the shot not fired, the effect without
    // particles...); the memory manager's out-of-memory handler and the generic fatal error are counted on entry. The
    // main arena (tdg's, *0x6CE4B4: +8 its free bytes, +0xC its size, +4 the root of its free-block tree whose
    // +0x10/+0x14/+0x18 are the left/right maxima and the node's own size) is sampled every VBlank: its lowest free
    // bytes and lowest largest free block. Printed every 10 s of wall time ([tf:census]; the counts since the start).
    struct PoolCensusSite
    {
        uint32_t address;
        const char *name;
        bool zeroIsFull; // (false: counted on entry)
    };
    constexpr PoolCensusSite kPoolCensusSites[] = {
        {0x0036A670u, "particle buffer take", true},   {0x003D4F50u, "projectile reserve", true},
        {0x0040F610u, "surface impact take", true},    {0x0027B7F0u, "sound emitter take", true},
        {0x002B4EB0u, "actor pool take", true},        {0x00525EF0u, "actor budget check", true},
        {0x00106270u, "OUT OF MEMORY handler", false}, {0x00106620u, "generic fatal error", false},
        {0x002F90F0u, "tree collision add", false},
        {0x005CD3D0u, "gib pool take", true},          {0x0035C320u, "decal add", false},
        {0x002B46C0u, "actor block allocate", false},  {0x005A8170u, "display list clear", false},
    };
    // (N7 step 2) The trees' collision (taEcoCollisionSystem): 0x2F90F0 adds a tree near the player to one array of
    // 150 instances (a0+4: its +0xC the count, +8 the entries) unless the tree stands in a cleared rectangle; at 150
    // it adds nothing (the tree has no collision). Counted as empty when it is called at 150; the highest count seen.
    uint32_t g_poolCensusTreeCollisionMax = 0u;
    // (N7 step 4) The safety limits - full is a fatal error or worse - and two more look pools:
    // - gibs: 5 pools of 64 (the manager *0x6CFDF4; 0x5CD3D0 takes a pool, 0 = none free: the gibs not shown);
    // - decals (taOrientedDecalSet: +0x10 the count, +0x24 the size; 0x35C320 adds one - at the size, it replaces one
    //   of the newest, or none): counted as full when it is called at the size, the largest size seen;
    // - the actor-pool block (a pool's actors are built in one block, from the first build's footprint, 400 KB:
    //   0x2B46C0 allocates in it, *0x6CF30C the block, *0x6CF310 the offset, *0x6CF314 its size; past it the build
    //   overruns the block - the game checks only before the allocation): the fullest a block got, in 1/1000ths,
    //   and the allocations that ended past it;
    // - the display lists' blocks (*0x6CFD88: +0 the free blocks, +4 how many were made; 75, or 125 on episode type 7;
    //   none free: a generic fatal error): the fewest free, sampled when a list gives its blocks back (0x5A8170) and
    //   every VBlank;
    // - the creature lists' items (taCreatureListUtil: vectors at 0x726700 and 0x726710 - +4 their counts - of 500
    //   each; none left: a generic fatal error): the fewest left;
    // - the actor pools (*0x6CFBD8: 64 slots at +0xC; the pool table *+0 - +4 its free nodes): the most slots in use,
    //   the fewest free nodes;
    // - the class registry (two tables of 100 entries at 0x6F8BE0 and 0x6F8BF0: +0 a byte per entry, +4 the size):
    //   the most entries in use.
    uint64_t g_censusDecalFull = 0u;
    uint32_t g_censusDecalSizeMax = 0u, g_censusDecalCountMax = 0u;
    uint32_t g_censusActorBlockFill = 0u; // (1/1000ths of the block, the most)
    uint64_t g_censusActorBlockOverruns = 0u;
    uint32_t g_censusDlFreeMin = ~0u, g_censusDlMade = 0u;
    uint32_t g_censusCreatureItemsMin = ~0u, g_censusCreatureEntriesMin = ~0u; uint32_t g_censusCreatureCapacity = 0u;
    uint32_t g_censusActorSlotsMax = 0u, g_censusPoolNodesFreeMin = ~0u;
    uint32_t g_censusActorMemoryMax = 0u, g_censusActorBudget = 0u; // (step 5: the actor budget's count at most, its budget)
    uint32_t g_censusClassesMax[2] = {0u, 0u}, g_censusClassesSize[2] = {0u, 0u};
    uint32_t censusWord(uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0u;
        if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
            std::memcpy(&v, rdram + a, 4);
        return v;
    }
    void censusDisplayListFree(uint8_t *rdram)
    {
        const uint32_t pool = censusWord(rdram, 0x006CFD88u);
        if (pool == 0u)
            return;
        const uint32_t made = censusWord(rdram, pool + 4u);
        uint32_t n = 0u;
        for (uint32_t b = censusWord(rdram, pool); b != 0u && n <= made + 1u; b = censusWord(rdram, b))
            ++n;
        if (made != g_censusDlMade)
        {
            g_censusDlMade = made; // (a new pool: the HQ's or a level's)
            g_censusDlFreeMin = n;
        }
        g_censusDlFreeMin = std::min(g_censusDlFreeMin, n);
    }
    void censusSafetyFrame(uint8_t *rdram)
    {
        censusDisplayListFree(rdram);
        if (censusWord(rdram, 0x00726700u + 8u) != 0u) // (the creature lists are made)
        {
            g_censusCreatureItemsMin = std::min(g_censusCreatureItemsMin, censusWord(rdram, 0x00726700u + 4u));
            g_censusCreatureEntriesMin = std::min(g_censusCreatureEntriesMin, censusWord(rdram, 0x00726710u + 4u));
            g_censusCreatureCapacity = censusWord(rdram, 0x00726700u);
        }
        if (const uint32_t pools = censusWord(rdram, 0x006CFBD8u); pools != 0u)
        {
            uint32_t used = 0u;
            for (uint32_t i = 0; i < 64u; ++i)
                used += censusWord(rdram, pools + 0xCu + i * 4u) != 0u ? 1u : 0u;
            g_censusActorSlotsMax = std::max(g_censusActorSlotsMax, used);
            g_censusActorMemoryMax = std::max(g_censusActorMemoryMax, censusWord(rdram, pools + 4u));
            g_censusActorBudget = censusWord(rdram, pools + 8u);
            if (const uint32_t table = censusWord(rdram, pools); table != 0u)
            {
                uint32_t n = 0u;
                for (uint32_t node = censusWord(rdram, table + 4u); node != 0u && n < 1000u; node = censusWord(rdram, node + 4u))
                    ++n;
                g_censusPoolNodesFreeMin = std::min(g_censusPoolNodesFreeMin, n);
            }
        }
        for (int t = 0; t < 2; ++t)
        {
            const uint32_t table = t == 0 ? 0x006F8BE0u : 0x006F8BF0u;
            const uint32_t flags = censusWord(rdram, table), size = censusWord(rdram, table + 4u);
            if (flags == 0u || size == 0u || size > 0x10000u || flags + size > PS2_RAM_SIZE)
                continue;
            uint32_t used = 0u;
            for (uint32_t i = 0; i < size; ++i)
                used += rdram[flags + i] != 0u ? 1u : 0u;
            g_censusClassesSize[t] = size;
            g_censusClassesMax[t] = std::max(g_censusClassesMax[t], used);
        }
    }
    // (N7 step 3) The one-shot player's emitters (0x27B7F0; 35 taSoundNodes of 0x40 bytes at *0x6CF25C: +0x20 the sound,
    // +0x24 its priority, +0x28 its state - 0 free, 1-4 starting/playing, 5-24 lingering after the end): with none
    // free it takes over the one with the worst (highest) priority value - the oldest of them - if the new sound's
    // priority is no worse (a steal: that sound cut short), else refuses. Counted here before the call.
    uint32_t g_censusEmitterPeak = 0u;
    uint32_t g_censusHoldersPeak = 0u; // (the looping-sound holders in use at most: 70 at *0x6CF2B0, 0x30 bytes, +0x14 bit 0)
    uint64_t g_censusEmitterFull = 0u, g_censusEmitterSteals = 0u, g_censusEmitterRefused = 0u, g_censusEmitterLingerBlocked = 0u;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> g_censusEmitterBySound; // (sound: its sounds cut by a steal, its asks refused)
    std::map<uint32_t, std::string> g_censusSoundNames; // (a sound id's name, read at its first count)
    uint32_t g_censusGp = 0u;
    std::string censusSoundName(uint8_t *rdram, uint32_t id)
    {
        if (auto it = g_censusSoundNames.find(id); it != g_censusSoundNames.end())
            return it->second;
        std::string name;
        uint32_t table = 0u;
        if (g_censusGp >= 0x3E8Cu)
            std::memcpy(&table, rdram + ((g_censusGp - 0x3E8Cu) & PS2_RAM_MASK), 4);
        if (table >= 0x00100000u && table + (id + 1u) * 16u < PS2_RAM_SIZE)
            for (uint32_t w = 0u; w < 4u && name.empty(); ++w)
            {
                uint32_t ptr = 0u;
                std::memcpy(&ptr, rdram + table + id * 16u + w * 4u, 4);
                for (uint32_t depth = 0u; depth < 2u && name.empty() && ptr >= 0x00100000u && ptr + 64u < PS2_RAM_SIZE; ++depth)
                {
                    std::string t;
                    for (uint32_t i = 0u; i < 48u; ++i)
                    {
                        const char c = static_cast<char>(rdram[ptr + i]);
                        if (c == 0)
                            break;
                        if (c < 0x20 || c > 0x7E)
                        {
                            t.clear();
                            break;
                        }
                        t += c;
                    }
                    if (t.size() >= 3u)
                        name = t;
                    else
                        std::memcpy(&ptr, rdram + ptr, 4); // (one more step: a pointer to the name)
                }
            }
        g_censusSoundNames[id] = name;
        return name;
    }
    void poolCensusEmitters(uint8_t *rdram, R5900Context *ctx)
    {
        uint32_t array = 0u;
        std::memcpy(&array, rdram + 0x006CF25Cu, 4);
        if (array < 0x00100000u || array + pool_growth::kSoundEmitters * 0x40u > PS2_RAM_SIZE)
            return;
        uint32_t busy = 0u, lingering = 0u, worst = 0u, worstAge = 0u, worstSound = 0u;
        bool first = true;
        for (uint32_t i = 0; i < pool_growth::kSoundEmitters; ++i)
        {
            const uint8_t *n = rdram + array + i * 0x40u;
            uint32_t sound = 0u, priority = 0u, state = 0u, age = 0u;
            std::memcpy(&sound, n + 0x20, 4);
            std::memcpy(&priority, n + 0x24, 4);
            std::memcpy(&state, n + 0x28, 4);
            std::memcpy(&age, n + 0x2C, 4);
            if (state != 0u)
            {
                ++busy;
                lingering += state >= 5u ? 1u : 0u;
            }
            if (first || priority > worst || (priority == worst && age < worstAge))
            {
                worst = priority;
                worstAge = age;
                worstSound = sound;
                first = false;
            }
        }
        g_censusEmitterPeak = std::max(g_censusEmitterPeak, busy);
        g_censusGp = getRegU32(ctx, 28);
        if (static bool named = false; !named && std::getenv("TF_SOUND_NAMES"))
        {
            named = true;
            std::string list;
            for (const char *at = std::getenv("TF_SOUND_NAMES"); *at;)
            {
                char *end = nullptr;
                const uint32_t id = static_cast<uint32_t>(std::strtoul(at, &end, 10));
                if (end == at)
                    break;
                list += " " + std::to_string(id) + "='" + censusSoundName(rdram, id) + "'";
                at = *end == ',' ? end + 1 : end;
            }
            std::fprintf(stderr, "[tf:census] sound names:%s\n", list.c_str());
        }
        if (busy < pool_growth::kSoundEmitters)
            return;
        ++g_censusEmitterFull;
        if (lingering > 0u)
            ++g_censusEmitterLingerBlocked;
        const uint32_t asked = getRegU32(ctx, 5), priority = getRegU32(ctx, 8);
        g_censusGp = getRegU32(ctx, 28);
        censusSoundName(rdram, asked);
        censusSoundName(rdram, worstSound);
        if (priority <= worst)
        {
            ++g_censusEmitterSteals;
            ++g_censusEmitterBySound[worstSound].first;
        }
        else
        {
            ++g_censusEmitterRefused;
            ++g_censusEmitterBySound[asked].second;
        }
    }
    constexpr size_t kPoolCensusCount = sizeof(kPoolCensusSites) / sizeof(kPoolCensusSites[0]);
    PS2Runtime::RecompiledFunction g_poolCensusOriginal[kPoolCensusCount] = {};
    uint64_t g_poolCensusCalls[kPoolCensusCount] = {}, g_poolCensusEmpty[kPoolCensusCount] = {};
    uint32_t g_poolCensusFreeLow = ~0u, g_poolCensusLargestLow = ~0u, g_poolCensusArenaSize = 0u, g_poolCensusFreeNow = 0u;
    const bool g_poolCensusOn = std::getenv("TF_POOL_CENSUS") && *std::getenv("TF_POOL_CENSUS") == '1';
    // The actor budget check (0x525EF0, the actor-pool manager at *(gp-0x3518); a1 the pool, +0x14 its footprint)
    // refuses when the manager's room (its +8 less +4: the 1 MB dynamic-actor budget left) is still short after it
    // took suspended actors back, or when the main arena's largest free block is under 1.5 x the footprint. Which
    // one, counted at each refusal - and the largest footprint asked for, the least room seen.
    uint64_t g_poolCensusByBudget = 0u, g_poolCensusByArena = 0u;
    uint32_t g_poolCensusFootprintMax = 0u;
    int32_t g_poolCensusRoomMin = 0x7FFFFFFF;
    uint32_t poolCensusLargestFree(uint8_t *rdram);
    void poolCensusBudgetRefused(uint8_t *rdram, uint32_t gp, uint32_t pool)
    {
        auto word = [rdram](uint32_t a) {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, 4);
            return v;
        };
        const uint32_t manager = word(gp - 0x3518u), footprint = word(pool + 0x14u);
        const int32_t room = static_cast<int32_t>(word(manager + 8u) - word(manager + 4u));
        g_poolCensusFootprintMax = std::max(g_poolCensusFootprintMax, footprint);
        g_poolCensusRoomMin = std::min(g_poolCensusRoomMin, room);
        if (room - static_cast<int32_t>(footprint) <= 0)
            ++g_poolCensusByBudget;
        else if (poolCensusLargestFree(rdram) <= footprint + footprint / 2u)
            ++g_poolCensusByArena;
    }
    // Per pool (= per creature type): its refusals and passes, the first and last refusal, and the first pass after
    // a refusal (a retry that got room later); at a pool's first refusal its first 16 words and any text they point
    // at, to name the type.
    struct PoolCensusPool
    {
        uint64_t refused = 0u, passed = 0u, passedAfter = 0u, firstRefused = 0u, lastRefused = 0u, firstPassAfter = 0u;
        uint32_t footprint = 0u;
    };
    std::map<uint32_t, PoolCensusPool> g_poolCensusPools;
    void poolCensusPerPool(uint8_t *rdram, PS2Runtime *runtime, uint32_t pool, bool passed)
    {
        auto word = [rdram](uint32_t a) {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, 4);
            return v;
        };
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        PoolCensusPool &p = g_poolCensusPools[pool];
        p.footprint = word(pool + 0x14u);
        if (passed)
        {
            ++p.passed;
            if (p.refused != 0u)
            {
                ++p.passedAfter;
                if (p.firstPassAfter == 0u)
                    p.firstPassAfter = tick;
            }
            return;
        }
        if (p.refused++ == 0u)
        {
            p.firstRefused = tick;
            std::string words, text;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t w = word(pool + 4u * i);
                char b[12];
                std::snprintf(b, sizeof(b), " %08x", w);
                words += b;
                if (w >= 0x00100000u && w < PS2_RAM_SIZE - 32u && text.size() < 120u)
                {
                    const char *s = reinterpret_cast<const char *>(rdram + w);
                    size_t n = 0;
                    while (n < 31u && s[n] >= 0x20 && s[n] < 0x7F)
                        ++n;
                    if (n >= 4u && s[n] == 0)
                        text += " [+" + std::to_string(4u * i) + "] \"" + std::string(s, n) + "\"";
                }
            }
            std::fprintf(stderr, "[tf:census] actor pool %08x first refused at tick %llu, footprint %u; words%s;%s\n", pool,
                         static_cast<unsigned long long>(tick), p.footprint, words.c_str(), text.empty() ? " no text" : text.c_str());
        }
        p.lastRefused = tick;
    }

    template <size_t I>
    void poolCensusHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), a1 = getRegU32(ctx, 5), gp = getRegU32(ctx, 28);
        ++g_poolCensusCalls[I];
        if (kPoolCensusSites[I].address == 0x0027B7F0u)
            poolCensusEmitters(rdram, ctx);
        if (kPoolCensusSites[I].address == 0x002F90F0u)
        {
            uint32_t list = 0u, count = 0u;
            if (const uint8_t *s = getConstMemPtr(rdram, getRegU32(ctx, 4) + 4u))
                std::memcpy(&list, s, 4);
            if (const uint8_t *c = list ? getConstMemPtr(rdram, list + 0xCu) : nullptr)
                std::memcpy(&count, c, 4);
            g_poolCensusTreeCollisionMax = std::max(g_poolCensusTreeCollisionMax, count);
            if (count >= 150u)
                ++g_poolCensusEmpty[I];
        }
        if (kPoolCensusSites[I].address == 0x0035C320u)
        {
            const uint32_t set = getRegU32(ctx, 4), count = censusWord(rdram, set + 0x10u), size = censusWord(rdram, set + 0x24u);
            g_censusDecalSizeMax = std::max(g_censusDecalSizeMax, size);
            g_censusDecalCountMax = std::max(g_censusDecalCountMax, std::min(count + 1u, size));
            if (count + 1u > size)
                ++g_censusDecalFull;
        }
        if (kPoolCensusSites[I].address == 0x005A8170u)
            censusDisplayListFree(rdram);
        g_poolCensusOriginal[I](rdram, ctx, runtime);
        if (kPoolCensusSites[I].address == 0x002B46C0u && ctx->pc == ra)
        {
            const uint32_t base = censusWord(rdram, 0x006CF30Cu), at = censusWord(rdram, 0x006CF310u), size = censusWord(rdram, 0x006CF314u);
            const uint32_t got = getRegU32(ctx, 2);
            if (size != 0u && got >= base && got < base + size + 16u) // (in the block, not the heap's)
            {
                g_censusActorBlockFill = std::max(g_censusActorBlockFill, static_cast<uint32_t>(uint64_t(at) * 1000u / (size + 16u)));
                if (at > size + 16u)
                    ++g_censusActorBlockOverruns;
            }
        }
        if (ctx->pc != ra)
            return;
        const bool empty = getRegU32(ctx, 2) == 0u;
        if (kPoolCensusSites[I].zeroIsFull && empty) // (returned, and empty-handed)
        {
            ++g_poolCensusEmpty[I];
            if (kPoolCensusSites[I].address == 0x00525EF0u)
                poolCensusBudgetRefused(rdram, gp, a1);
        }
        if (kPoolCensusSites[I].address == 0x00525EF0u)
            poolCensusPerPool(rdram, runtime, a1, !empty);
    }
    template <size_t... I>
    void installPoolCensusHooks(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        const PS2Runtime::RecompiledFunction hooks[] = {poolCensusHook<I>...};
        for (size_t i = 0; i < kPoolCensusCount; ++i)
            if ((g_poolCensusOriginal[i] = runtime.lookupFunction(kPoolCensusSites[i].address)) != nullptr)
                runtime.registerFunction(kPoolCensusSites[i].address, hooks[i]);
    }
    // The main arena's largest free block: the root of its free-block tree (arena +4), the most of its left/right
    // maxima and its own size (+0x10/+0x14/+0x18), as tdg's func_00104680 reads it.
    uint32_t poolCensusLargestFree(uint8_t *rdram)
    {
        auto word = [rdram](uint32_t a) {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, 4);
            return v;
        };
        const uint32_t arena = word(0x006CE4B4u), root = arena ? word(arena + 4u) : 0u;
        return root ? std::max({word(root + 0x10u), word(root + 0x14u), word(root + 0x18u)}) : 0u;
    }
    void poolCensusPrint(uint64_t tick, const char *why)
    {
        std::fprintf(stderr, "[tf:census] %s at tick %llu: main arena %u bytes, free now %u, lowest free %u, lowest largest block %u\n",
                     why, static_cast<unsigned long long>(tick), g_poolCensusArenaSize, g_poolCensusFreeNow, g_poolCensusFreeLow,
                     g_poolCensusLargestLow);
        std::fprintf(stderr, "[tf:census]   actor budget refusals: %llu by the 1 MB budget, %llu by the arena's largest block, "
                             "largest footprint %u, least room %d\n",
                     static_cast<unsigned long long>(g_poolCensusByBudget), static_cast<unsigned long long>(g_poolCensusByArena),
                     g_poolCensusFootprintMax, g_poolCensusRoomMin);
        for (const auto &[pool, p] : g_poolCensusPools)
            if (p.refused != 0u)
                std::fprintf(stderr, "[tf:census]   pool %08x (footprint %u): refused %llu (ticks %llu-%llu), passed %llu, of which "
                                     "after a refusal %llu (first at %llu)\n",
                             pool, p.footprint, static_cast<unsigned long long>(p.refused),
                             static_cast<unsigned long long>(p.firstRefused), static_cast<unsigned long long>(p.lastRefused),
                             static_cast<unsigned long long>(p.passed), static_cast<unsigned long long>(p.passedAfter),
                             static_cast<unsigned long long>(p.firstPassAfter));
        for (size_t i = 0; i < kPoolCensusCount; ++i)
            std::fprintf(stderr, "[tf:census]   %-22s %10llu calls, %8llu empty\n", kPoolCensusSites[i].name,
                         static_cast<unsigned long long>(g_poolCensusCalls[i]), static_cast<unsigned long long>(g_poolCensusEmpty[i]));
        std::fprintf(stderr, "[tf:census]   tree collision instances: at most %u of 150\n", g_poolCensusTreeCollisionMax);
        std::fprintf(stderr, "[tf:census]   (step 4) decal sets: at most %u of the largest %u, %llu adds at the size; actor-pool blocks: "
                             "the fullest %u.%u%%, %llu allocations past the block\n",
                     g_censusDecalCountMax, g_censusDecalSizeMax, static_cast<unsigned long long>(g_censusDecalFull),
                     g_censusActorBlockFill / 10u, g_censusActorBlockFill % 10u, static_cast<unsigned long long>(g_censusActorBlockOverruns));
        std::fprintf(stderr, "[tf:census]   (step 4) display-list blocks: %u made, the fewest free %d; creature list items: the fewest left "
                             "%d and %d of %u; actor pools: at most %u of 64 slots, the fewest free table nodes %d; class registry: "
                             "at most %u of %u and %u of %u\n",
                     g_censusDlMade, g_censusDlFreeMin == ~0u ? -1 : static_cast<int>(g_censusDlFreeMin),
                     g_censusCreatureItemsMin == ~0u ? -1 : static_cast<int>(g_censusCreatureItemsMin),
                     g_censusCreatureEntriesMin == ~0u ? -1 : static_cast<int>(g_censusCreatureEntriesMin), g_censusCreatureCapacity, g_censusActorSlotsMax,
                     g_censusPoolNodesFreeMin == ~0u ? -1 : static_cast<int>(g_censusPoolNodesFreeMin), g_censusClassesMax[0],
                     g_censusClassesSize[0], g_censusClassesMax[1], g_censusClassesSize[1]);
        std::fprintf(stderr, "[tf:census]   (step 5) the actors' memory: at most %u KB (the budget now %u KB)\n",
                     g_censusActorMemoryMax / 1024u, g_censusActorBudget / 1024u);
        std::fprintf(stderr, "[tf:census]   looping-sound holders (ambient sounds): at most %u of 70 in use\n", g_censusHoldersPeak);
        std::fprintf(stderr,
                     "[tf:census]   sound emitters: at most %u of %u busy; %llu asks found none free (%llu with some lingering): "
                     "%llu took over a playing sound, %llu refused\n",
                     g_censusEmitterPeak, pool_growth::kSoundEmitters, static_cast<unsigned long long>(g_censusEmitterFull),
                     static_cast<unsigned long long>(g_censusEmitterLingerBlocked), static_cast<unsigned long long>(g_censusEmitterSteals),
                     static_cast<unsigned long long>(g_censusEmitterRefused));
        if (!g_censusEmitterBySound.empty())
        {
            std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> v(g_censusEmitterBySound.begin(), g_censusEmitterBySound.end());
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.first + a.second.second > b.second.first + b.second.second; });
            std::string list;
            for (size_t i = 0; i < v.size() && i < 12u; ++i)
            {
                const auto nm = g_censusSoundNames.find(v[i].first);
                list += " " + std::to_string(v[i].first) + (nm != g_censusSoundNames.end() && !nm->second.empty() ? "'" + nm->second + "'" : "") +
                        ":" + std::to_string(v[i].second.first) + "/" + std::to_string(v[i].second.second);
            }
            std::fprintf(stderr, "[tf:census]   sound emitters by sound (id:cut/refused):%s\n", list.c_str());
        }
    }
    void poolCensusFrame(PS2Runtime *runtime)
    {
        uint8_t *rdram = runtime->memory().getRDRAM();
        auto word = [rdram](uint32_t a) {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a <= PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, 4);
            return v;
        };
        if (const uint32_t holders = word(0x006CF2B0u); holders != 0u)
        {
            uint32_t used = 0u;
            for (uint32_t i = 0; i < 70u; ++i)
                used += (word(holders + i * 0x30u + 0x14u) & 1u) ? 1u : 0u;
            g_censusHoldersPeak = std::max(g_censusHoldersPeak, used);
        }
        censusSafetyFrame(rdram);
        const uint32_t arena = word(0x006CE4B4u);
        if (arena != 0u)
        {
            g_poolCensusArenaSize = word(arena + 0xCu);
            g_poolCensusFreeNow = word(arena + 8u);
            g_poolCensusFreeLow = std::min(g_poolCensusFreeLow, g_poolCensusFreeNow);
            const uint32_t root = word(arena + 4u);
            if (root != 0u)
                g_poolCensusLargestLow = std::min(g_poolCensusLargestLow, std::max({word(root + 0x10u), word(root + 0x14u), word(root + 0x18u)}));
        }
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(10))
        {
            last = now;
            poolCensusPrint(runtime->eeScheduler().currentVSyncTick(), "every 10 s");
        }
    }

    // Once a frame: the mode of the moment (TF_CHAR_DETAIL's steps by VBlank).
    void charDetailFrame(PS2Runtime *runtime)
    {
        vrrSample(); // (TODO 35: frame pacing's Auto)
        if (g_levelRate != 0 || (g_frameRate <= 30 && g_executorVtable == 0x006B4C40u))
        {
            float detail = 1.0f;
            std::memcpy(&detail, runtime->memory().getRDRAM() + 0x006CD974u, sizeof(detail)); // (timing.h detailScale)
            ++g_detailFrames;
            if (detail < 1.0f)
            {
                ++g_detailLow;
                g_detailMin = std::min(g_detailMin, detail);
            }
            // (at the console's 30 - a 30 cap - no rate governor says it: every 60 s of a level here)
            if (g_levelRate == 0 && g_detailFrames % 1800u == 0u)
            {
                std::fprintf(stderr, "[tf] at 30 frames a second: the detail under 1 in %llu of %llu frames (at least %.3f)\n",
                             static_cast<unsigned long long>(g_detailLow), static_cast<unsigned long long>(g_detailFrames), g_detailMin);
                g_detailFrames = g_detailLow = 0;
                g_detailMin = 1.0f;
            }
        }
        pool_growth::frame(runtime->memory().getRDRAM()); // (N7 step 4: the creature lists)
        if (g_poolCensusOn)
            poolCensusFrame(runtime);
        // (TF_RNG_LOG=1, a test aid: the game's random seed, *(*0x6CF320), each VBlank it changes - where two runs part)
        static const bool rngLog = std::getenv("TF_RNG_LOG") && *std::getenv("TF_RNG_LOG") == '1';
        if (rngLog)
        {
            uint8_t *rdram = runtime->memory().getRDRAM();
            static uint32_t last = 0u;
            uint32_t at = 0u, seed = 0u;
            std::memcpy(&at, rdram + 0x006CF320u, 4);
            if (at >= 0x00100000u && at < PS2_RAM_SIZE - 4u)
                std::memcpy(&at, rdram + at, 4);
            if (at >= 0x00100000u && at < PS2_RAM_SIZE - 4u)
                std::memcpy(&seed, rdram + at, 4);
            if (seed != last)
                std::fprintf(stderr, "[tf:rng] %llu %08x\n", static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), seed);
            last = seed;
        }
        // (TF_RAM_DUMP=<vblank>[,<vblank>...], a test aid: EE RAM written to runs\\ram_<vblank>.bin at the first frame at or after each)
        {
            static std::vector<uint64_t> dumps = []() {
                std::vector<uint64_t> v;
                if (const char *e = std::getenv("TF_RAM_DUMP"))
                    for (const char *p = e; *p;)
                    {
                        char *end = nullptr;
                        const unsigned long long t = std::strtoull(p, &end, 10);
                        if (end == p)
                            break;
                        v.push_back(t);
                        p = *end ? end + 1 : end;
                    }
                return v;
            }();
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            for (uint64_t &t : dumps)
                if (t && tick >= t)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "runs/ram_%llu.bin", static_cast<unsigned long long>(t));
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(runtime->memory().getRDRAM(), 1, PS2_RAM_SIZE, f);
                        std::fclose(f);
                    }
                    std::fprintf(stderr, "[tf] RAM dumped at vblank %llu: %s\n", static_cast<unsigned long long>(tick), name);
                    t = 0;
                }
        }
        if (g_charDetailSteps.empty())
            return;
        const uint64_t now = runtime->eeScheduler().currentVSyncTick();
        int mode = 0;
        for (const CharDetailStep &st : g_charDetailSteps)
            if (now >= st.at)
                mode = st.mode;
        if (mode != g_charDetailMode)
        {
            g_charDetailMode = mode;
            std::fprintf(stderr, "[tf:chardetail] vblank %llu: %s\n", static_cast<unsigned long long>(now),
                         mode == 0 ? "the game's" : mode < 0 ? "the finest step for everything" : "the drawing finer");
        }
    }

    void installCharDetail(PS2Runtime &runtime)
    {
        const char *env = std::getenv("TF_CHAR_DETAIL");
        std::string spec = env ? env : "";
        size_t at = 0u;
        while (at < spec.size())
        {
            size_t end = spec.find_first_of(";,", at); // (',' as well: the test tools split their lists on ';')
            if (end == std::string::npos)
                end = spec.size();
            const std::string item = spec.substr(at, end - at);
            at = end + 1u;
            const size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            CharDetailStep st{std::strtoull(item.c_str(), nullptr, 10), 0};
            const std::string mode = item.substr(colon + 1u);
            if (mode == "anim")
                st.mode = -1;
            else if (mode.rfind("draw", 0) == 0)
                st.mode = std::max(1, std::atoi(mode.c_str() + 4));
            g_charDetailSteps.push_back(st);
        }
        if ((g_animationActorCull = runtime.lookupFunction(kAnimationActorCull)) != nullptr)
            runtime.registerFunction(kAnimationActorCull, animationActorCull);
        if ((g_pickupPanelDraw = runtime.lookupFunction(kPickupPanelDraw)) != nullptr)
            runtime.registerFunction(kPickupPanelDraw, pickupPanelDraw);
        if (std::getenv("TF_LOD_CENSUS") && *std::getenv("TF_LOD_CENSUS") == '1')
            if ((g_pickupManagerCull = runtime.lookupFunction(kPickupManagerCull)) != nullptr)
                runtime.registerFunction(kPickupManagerCull, pickupManagerCull);
        if (const char *only = std::getenv("TF_CHAR_DETAIL_ONLY"))
            for (const char *p = only; *p;)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 10);
                if (end == p)
                    break;
                g_charDetailOnly.push_back(static_cast<uint32_t>(v));
                p = *end ? end + 1 : end;
            }
        std::fprintf(stderr, "[tf:chardetail] %zu steps, %zu types only\n", g_charDetailSteps.size(), g_charDetailOnly.size());
    }

    void installDrawDistance(PS2Runtime &runtime)
    {
        installCharDetail(runtime);
        installForest(runtime);
        if (g_poolCensusOn)
            installPoolCensusHooks(runtime, std::make_index_sequence<kPoolCensusCount>{});
        if (std::getenv("TF_LOD_LOG") && *std::getenv("TF_LOD_LOG") == '1')
            if ((g_detailDistanceOf = runtime.lookupFunction(kDetailDistanceOf)) != nullptr)
                runtime.registerFunction(kDetailDistanceOf, detailDistanceOf);
        if (std::getenv("TF_LOD_LOG") && *std::getenv("TF_LOD_LOG") == '1')
            if ((g_sceneCull = runtime.lookupFunction(kSceneCull)) != nullptr)
                runtime.registerFunction(kSceneCull, sceneCull);
        float preset = 2.0f;
        if (const char *env = std::getenv("TF_DRAW_DISTANCE"); env && *env)
            preset = std::clamp(static_cast<float>(std::atof(env)), 0.25f, 16.0f);
        const auto envOf = [](const char *key)
        {
            const char *v = std::getenv(key);
            return std::string(v ? v : "");
        };
        const DrawDistances d = resolveDistances(preset, envOf("TF_SCENERY_DISTANCE"), envOf("TF_FOREST_DISTANCE"), envOf("TF_FOREST_SPRITES"),
                                                 envOf("TF_GRASS_DISTANCE"));
        nr::setGrassReach(d.grass);
        nr::setTextureSharpness(textureSharpnessOf(envOf("TF_SHARP_TEXTURES")));
        g_enemyDetail.store(enemyDetailOf(envOf("TF_ENEMY_DETAIL")));
        g_terrainDetail.store(terrainDetailOf(envOf("TF_TERRAIN_DETAIL"), preset));
        if ((g_staticSceneClear = runtime.lookupFunction(kStaticSceneClear)) != nullptr)
            runtime.registerFunction(kStaticSceneClear, staticSceneClear);
        g_drawPresetShown.store(preset);
        g_drawDistance = d.scenery;
        g_forestDistance.store(d.forest);
        g_forestSprites.store(d.sprites);
        g_sceneryShown.store(d.scenery);
        std::fprintf(stderr, "[tf] draw distance: scenery x%g, forest x%g, far trees %s, grass x%g\n", d.scenery, d.forest,
                     d.sprites >= 1.0f ? "3D to the edge" : d.sprites <= 0.0f ? "the level's sprites" : "partly 3D", d.grass);
        if (g_drawDistance == 1.0f)
            return;
        g_setDetailDistance = runtime.lookupFunction(kSetDetailDistance);
        g_incidentalViewFactor = runtime.lookupFunction(kIncidentalViewFactor);
        if (!g_setDetailDistance || !g_incidentalViewFactor)
        {
            g_drawDistance = 1.0f;
            return;
        }
        runtime.registerFunction(kSetDetailDistance, setDetailDistance);
        runtime.registerFunction(kIncidentalViewFactor, incidentalViewFactor);
        g_drawDistanceHooks = true;
    }

    // The settings panel's settings of this game (ps2_settings.h): the frame rate, the draw distance,
    // and the picture's that are this port's own. The live ones are taken on the game thread at its
    // next frame (takeSettings, applyDrawDistance).
    void addSettings()
    {
        game_config::addSettings(); // the Cheats and Debug tabs (game_config.h)
        controller_settings::add(); // the Controller tab (DualSense / DualShock 4)
        dualsense::addSettings();   // ... and what the game shows on the pad (dualsense.h)
        discord_presence::addSettings(); // the Discord status (discord_presence.h)
        ultrawide::addSettings();   // Graphics: the picture's shape (ultrawide.h)
        native_sound::addSettings(); // Sound: the native sound server (native_sound.h)
        fast_load::addSettings();   // Gameplay: fast loading (fast_load.h)
        mod_menu::addSettings();    // Controls: driving with W / S (mod_menu.h)
        small_crosshair::addSettings(); // Display: the small crosshair (small_crosshair.h)
        crisp_text::addSettings();  // Display: sharp text and PS5 button symbols (crisp_text.h, TODO 119)
        playrec::addSettings();     // Gameplay: play recording, F2 (playrec.h, N12 b)
        using ps2_settings::Choice;
        using ps2_settings::Setting;
        const char *const times = "\xC3\x97";
        const auto onOff = []()
        { return std::vector<Choice>{{"0", "Off"}, {"1", "On"}}; };
        const auto gpu = []()
        { return ps2_settings::choice("PS2X_GPU") != "0"; };
        g_lockShown.store(g_pictureLock);
        {
            // A picture each refresh of the screen - moments between the frame before and this one,
            // stage 4 (not exact: offered with the native pixel path; the default since the user's OK
            // on 2026-10-08) - or 120 or 60 pictures a second. A frame rate of its own and the adaptive
            // rate are left to the environment variables (TF_FRAME_RATE, TF_FPS_LOCK=0).
            Setting s;
            s.key = "TF_FPS_LOCK";
            s.section = "Display";
            s.label = "Frame rate";
            s.order = 25;
            s.help = "Levels at 60 frames a second, shown as a picture each refresh of the screen, each the moment that "
                     "refresh shows between the frame before and this one: as smooth as the screen allows. Or 120 pictures - "
                     "each frame and the game's own in-between moment - or 60 (F4 switches between them). Where the graphics "
                     "card cannot draw them all in time, fewer are drawn; the game never slows down. The menus and movies "
                     "stay at the console's 30.";
            s.choices = {{"60", "60 a second"}, {"120", "120 a second"}, {"refresh", "The screen's refresh"}};
            s.defaultValue = "refresh";
            // (TODO 217) With the frame rate cap deciding the rates the picture lock does nothing: dimmed, saying so.
            s.available = []() { return !followRates(); };
            s.unavailableHelp = "Follows the frame rate limit now: the game shows a picture each frame, up to the limit. Only with TF_RATE_LADDER=1.";
            s.makeChoices = [gpu]()
            {
                if (followRates())
                    return std::vector<Choice>{{"follow", "Follows the frame rate limit"}}; // (TODO 217: what is in effect)
                std::vector<Choice> list = {{"60", "60 a second"}, {"120", "120 a second"}};
                // (moments need the GPU's native pixel path; the label says the screen's rate when known)
                if (g_momentsShown.load() || (gpu() && ps2_settings::choice("PS2X_GPU_NATIVE") != "0"))
                {
                    std::string label = "The screen's refresh";
                    ps2_host_display::Clock::time_point vblank{};
                    ps2_host_display::Clock::duration refresh{};
                    if (ps2_host_display::screenTiming(vblank, refresh) && refresh > std::chrono::milliseconds(2))
                        label += " (" + std::to_string(std::lround(1.0 / std::chrono::duration<double>(refresh).count())) + " a second)";
                    list.push_back(Choice{"refresh", label});
                }
                if (g_lockShown.load() == 0 && !g_momentsShown.load())
                    list.push_back(Choice{"0", "Free (TF_FPS_LOCK=0)"});
                return list;
            };
            s.current = []()
            {
                if (followRates())
                    return std::string("follow");
                return g_momentsShown.load() ? std::string("refresh") : std::to_string(g_lockShown.load());
            };
            s.apply = [](const std::string &value)
            {
                if (!g_levelHooks)
                    return false; // (started at the console's 30)
                const bool refresh = value == "refresh";
                const int lock = refresh ? 120 : std::atoi(value.c_str());
                g_wantLock.store(lock == 60 || lock == 120 ? lock : 0);
                g_wantMoments.store(refresh ? 1 : 0);
                g_lockShown.store(lock == 60 || lock == 120 ? lock : 0);
                g_momentsShown.store(refresh); // (the game takes it at its next frame)
                return true;
            };
            ps2_settings::add(s);
        }
        {
            // (TODO 160) The frame-rate cap: the levels' (and the HQ's) top rate, live.
            Setting c;
            c.key = "TF_FPS_CAP";
            c.section = "Graphics";
            c.label = "Frame rate cap";
            c.order = 5;
            c.help = "The most pictures a second the game draws. It draws as many as your PC can, up to this. With VSync on, also "
                     "no more than the screen's refresh; with VSync off, Unlimited means as many as your PC can draw. The game "
                     "itself always moves at its true speed, whatever is drawn.";
            c.choices = {{"30", "30"}, {"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}, {"165", "165"}, {"240", "240"}, {"0", "Unlimited"}};
            c.defaultValue = "240";
            c.available = []() { return followRates(); };
            c.unavailableHelp = "TF_RATE_LADDER=1 is set: the frame-rate ladder decides the rates (Game frame rate, Frame pacing).";
            c.apply = [](const std::string &value)
            {
                if (!g_levelHooks)
                    return false; // (started at the console's 30)
                g_wantCap.store(std::atoi(value.c_str()));
                return true;
            };
            ps2_settings::add(c);
        }
        {
            // (TODO 266) VSync: the presents wait for the screen's refresh (On: by the screen, as always - at its refresh,
            // at once with FreeSync / G-Sync) or go at once (Off: allowed to tear; the frame rate cap alone limits the
            // pictures, Unlimited none). Live: the presents at once (ps2_host_present::setSync), a level's rates from the
            // next frame (taken as a cap change), the HQ's when it is entered next.
            Setting v;
            v.key = "PS2X_PRESENT_SYNC";
            v.section = "Graphics";
            v.label = "VSync";
            v.order = 6;
            v.help = "On: each picture waits for your screen's refresh, so nothing tears, and the game draws no more pictures "
                     "than the screen shows. Off: each picture goes to the screen at once, as many as your PC can draw up to the "
                     "frame rate cap, with less delay to your input; the picture may tear.";
            v.choices = {{"on", "On"}, {"0", "Off"}};
            v.defaultValue = "on";
            v.current = []() { return std::string(ps2_host_present::sync() == 0 ? "0" : "on"); };
            v.apply = [](const std::string &value)
            {
                ps2_host_present::setSync(value == "0" ? 0 : (value == "1" ? 1 : -1));
                if (g_levelHooks)
                    g_wantCap.store(g_rateSetting >= kUnlimitedRate ? 0 : g_rateSetting); // (the rates again, without the refresh's top)
                return true;
            };
            ps2_settings::add(v);
        }
        {
            // (TODO 35) Frame pacing: how the levels' rates step - see pacingChoice. Taken when a level begins. (Only
            // with the ladder: TF_RATE_LADDER=1, TODO 160.)
            Setting s;
            s.key = "TF_FRAME_PACING";
            s.section = "Display";
            s.label = "Frame pacing";
            s.order = 25;
            s.help = "How the game's frame rate steps down when your PC cannot hold it. Even steps: only rates that divide "
                     "your screen's refresh (240 Hz: 240, 120, 80, 60), so every picture stays up the same time - the smooth "
                     "choice for a screen without FreeSync or G-Sync; between frames the camera still turns each refresh. "
                     "Any rate: every step (240, 200, 180, ...), a little under the refresh - for FreeSync / G-Sync, where "
                     "the screen waits for each picture. Auto: any rate when your screen is seen to refresh with the pictures, "
                     "else even steps. Takes effect when the next level begins.";
            s.choices = {{"auto", "Auto"}, {"even", "Even steps (no VRR)"}, {"any", "Any rate (VRR)"}};
            s.defaultValue = "auto";
            s.available = []() { return !followRates(); };
            s.unavailableHelp = "The frame rate cap decides the rates now (Graphics); the ladder only with TF_RATE_LADDER=1.";
            s.apply = [](const std::string &value)
            {
                g_pacingChoice.store(value == "even" ? 1 : value == "any" ? 2 : 0, std::memory_order_relaxed);
                return true;
            };
            ps2_settings::add(s);
        }
        {
            // Collecting the textures the game uses for a texture pack (patch 0011's dump; the pack is
            // made by "Make texture pack.bat" / transformers/tools/make_texture_pack.py).
            Setting s;
            s.key = "PS2X_TEXTURE_DUMP";
            s.section = "Graphics";
            s.label = "Collect textures for the HD pack";
            s.order = 60;
            s.help = "While on, every texture the game shows is saved once in texture_dumps\\. Then run 'Make texture pack.bat' "
                     "in the game's folder: it makes the HD replacements (from the remaster's upscales or an upscaler) that "
                     "'Texture pack' loads. Takes a restart.";
            s.choices = {{"", "Off"}, {"texture_dumps", "On"}};
            s.defaultValue = "";
            ps2_settings::add(s);
        }
        {
            // The rate the levels' game itself runs at (TF_FRAME_RATE, read at start: a restart).
            // The rate-dependent fixes are written for any rate R (scaled by 30/R).
            Setting s;
            s.key = "TF_FRAME_RATE";
            s.section = "Display";
            s.label = "Game frame rate";
            s.order = 24;
            s.help = "How many times a second the game itself moves on in levels (the menus and movies stay at the "
                     "console's 30). Above 60 needs a fast PC; the pictures shown follow 'Frame rate' below. Takes a restart.";
            s.choices = {{"30", "30 (the console's)"}, {"60", "60"},   {"120", "120"}, {"144", "144"},
                         {"160", "160"},               {"180", "180"}, {"200", "200"}, {"240", "240"}};
            s.defaultValue = "60";
            s.current = []() { return std::to_string(g_rateSetting); };
            s.available = []() { return !followRates(); }; // (TODO 160: the cap decides, Graphics)
            s.unavailableHelp = "Follows the frame rate limit now: the game updates as often as the limit allows, shown here. Only with TF_RATE_LADDER=1.";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "PS2X_WIDESCREEN";
            s.section = "Graphics";
            s.label = "Widescreen";
            s.order = 30;
            s.help = "The game's own 16:9 mode: a wider view, the HUD at the screen's sides. The game's Options menu switches it "
                     "too, until the next start.";
            s.choices = {{"1", "16:9"}, {"0", "4:3"}};
            s.defaultValue = "1";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "TF_DRAW_DISTANCE";
            s.section = "Graphics";
            s.label = "Draw distance";
            s.order = 40;
            s.help = "How far the detail reaches: 1x is the console's, everywhere. Above it the scenery's detail goes to 4x "
                     "(it costs almost nothing), the forest's trees to this factor (they are the cost: game time and graphics "
                     "card), drawn 3D to the edge, and the grass and the ground's close-up texture reach this factor too "
                     "(the ground at most 4x). Enemies, your Autobot and the pickups are drawn whole at any distance (Enemy "
                     "detail). The settings below set each one apart (Auto: from this).";
            for (const char *value : {"1", "1.5", "2", "3", "4"})
                s.choices.push_back(Choice{value, std::string(value) + times + (std::string(value) == "1" ? " (the console's)" : "")});
            s.defaultValue = "2";
            s.current = []()
            {
                char text[32];
                std::snprintf(text, sizeof(text), "%g", static_cast<double>(g_drawPresetShown.load()));
                return std::string(text);
            };
            s.apply = [](const std::string &value)
            {
                const float preset = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.25f, 16.0f);
                const DrawDistances d = resolveDistances(preset, ps2_settings::choice("TF_SCENERY_DISTANCE"),
                                                         ps2_settings::choice("TF_FOREST_DISTANCE"), ps2_settings::choice("TF_FOREST_SPRITES"),
                                                         ps2_settings::choice("TF_GRASS_DISTANCE"));
                if (d.scenery != g_sceneryShown.load() && !g_drawDistanceHooks)
                    return false; // (the scenery started at 1: its hooks are not in)
                g_drawPresetShown.store(preset);
                if (d.scenery != g_sceneryShown.load())
                {
                    g_wantDrawDistance.store(d.scenery);
                    g_sceneryShown.store(d.scenery);
                }
                g_forestDistance.store(d.forest);
                g_forestSprites.store(d.sprites);
                nr::setGrassReach(d.grass);
                g_terrainDetail.store(terrainDetailOf(ps2_settings::choice("TF_TERRAIN_DETAIL"), preset)); // (TODO 137: Auto follows)
                return true;
            };
            ps2_settings::add(s);
        }
        {
            const auto autoChoice = []() { return Choice{"", "Auto (from Draw distance)"}; };
            {
                Setting s;
                s.key = "TF_SCENERY_DISTANCE";
                s.section = "Graphics";
                s.label = "Scenery distance";
                s.order = 41;
                s.help = "How far the scenery's detail reaches - buildings' full models, the ruins, the detail of the "
                         "ground: 1x is the console's. Almost free.";
                s.choices.push_back(autoChoice());
                for (const char *value : {"1", "2", "3", "4"})
                    s.choices.push_back(Choice{value, std::string(value) + times});
                s.defaultValue = "";
                s.apply = [](const std::string &value)
                {
                    const DrawDistances d = resolveDistances(g_drawPresetShown.load(), value, ps2_settings::choice("TF_FOREST_DISTANCE"),
                                                             ps2_settings::choice("TF_FOREST_SPRITES"));
                    if (d.scenery == g_sceneryShown.load())
                        return true;
                    if (!g_drawDistanceHooks)
                        return false;
                    g_wantDrawDistance.store(d.scenery);
                    g_sceneryShown.store(d.scenery);
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                Setting s;
                s.key = "TF_FOREST_DISTANCE";
                s.section = "Graphics";
                s.label = "Forest distance";
                s.order = 42;
                s.help = "How far the forest's 3D trees reach: 1x is the console's. This is what more draw distance costs "
                         "(game time above all); when the PC falls behind, the far trees go first.";
                s.choices.push_back(autoChoice());
                for (const char *value : {"1", "1.5", "2", "3", "4"})
                    s.choices.push_back(Choice{value, std::string(value) + times});
                s.defaultValue = "";
                s.apply = [](const std::string &value)
                {
                    g_forestDistance.store(resolveDistances(g_drawPresetShown.load(), "", value, "").forest);
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                Setting s;
                s.key = "TF_FOREST_SPRITES";
                s.section = "Graphics";
                s.label = "Far trees";
                s.order = 43;
                s.help = "The farthest trees: 3D models to the edge of the forest (the same look, cheaper), or turned into flat "
                         "pictures as on the console.";
                s.choices.push_back(autoChoice());
                s.choices.push_back(Choice{"1", "3D to the edge"});
                s.choices.push_back(Choice{"0", "Flat (the console's)"});
                s.defaultValue = "";
                s.apply = [](const std::string &value)
                {
                    g_forestSprites.store(resolveDistances(g_drawPresetShown.load(), "", "", value).sprites);
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                Setting s;
                s.key = "TF_FOREST_FULL_DETAIL";
                s.section = "Graphics";
                s.label = "Trees";
                s.order = 43;
                s.help = "Full 3D at any distance: every tree in view is its 3D model with all its leaves, however far - no "
                         "flat far-away pictures and no trees changing as you walk. It costs game time and graphics card time "
                         "where a lot of forest is in view (from a big hilltop view it can cut the frame rate to a quarter). Off: as Forest "
                         "distance says - flat pictures beyond it, the leaves only on the nearest trees.";
                s.choices.push_back(Choice{"0", "Off"});
                s.choices.push_back(Choice{"1", "Full 3D at any distance"});
                s.defaultValue = "0";
                s.apply = [](const std::string &value)
                {
                    g_forestFull.store(value == "1");
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                Setting s;
                s.key = "TF_FOREST_ADAPTIVE";
                s.section = "Graphics";
                s.label = "Forest on slower PCs";
                s.order = 43;
                s.help = "Full: the forest always reaches as far as Forest distance says. Adaptive: when the frames fall "
                         "behind and the trees are what slows them, the far trees come in (to half the reach at most) and go "
                         "back out once there is room - for PCs that cannot hold the frame rate with the whole forest.";
                s.choices.push_back(Choice{"0", "Full"});
                s.choices.push_back(Choice{"1", "Adaptive"});
                s.defaultValue = "0";
                s.apply = [](const std::string &value)
                {
                    g_forestAdaptive.store(value == "1");
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                Setting s;
                s.key = "TF_GRASS_DISTANCE";
                s.section = "Graphics";
                s.label = "Grass distance";
                s.order = 44;
                s.help = "How far the grass's blades reach before they thin out: 1x is the console's (native renderer). Auto: "
                         "the Draw distance's factor.";
                s.choices.push_back(autoChoice());
                for (const char *value : {"1", "1.5", "2", "3", "4"})
                    s.choices.push_back(Choice{value, std::string(value) + times});
                s.defaultValue = "";
                s.apply = [](const std::string &value)
                {
                    nr::setGrassReach(resolveDistances(g_drawPresetShown.load(), "", "", "", value).grass);
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                // N8 item 5: the textures' mip levels for the screen's height (nr::setTextureSharpness). Auto: matched
                // to it; Off: the PS2's. (A number, from the environment only: that many levels - test runs.)
                Setting s;
                s.key = "TF_SHARP_TEXTURES";
                s.section = "Graphics";
                s.label = "Sharp textures";
                s.order = 45;
                s.help = "Native renderer: textures keep their detail into the distance on a high-resolution screen. The PS2 "
                         "chose the blurrier versions by distance for its 448 lines; Auto matches the choice to your screen's "
                         "height. Off: the PS2's.";
                s.choices = {Choice{"", "Auto"}, Choice{"0", "Off"}};
                s.defaultValue = "";
                s.apply = [](const std::string &value)
                {
                    nr::setTextureSharpness(textureSharpnessOf(value));
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                // N8 item 4 (the user's choice B): the enemies drawn finer at mid range, the animation the game's. Off by
                // default. Like the other detail settings it moves the run's emulated-time timeline (the finer drawing is
                // more guest code: a frame can land on another VBlank, and the random rolls follow) - no game rule changes
                // (Antarctica bisect 2026-11-03: no part posed, no random number drawn, by the drawing; Amazon's scenery 1 vs
                // 4 parts the timeline the same way).
                Setting s;
                s.key = "TF_ENEMY_DETAIL";
                s.section = "Graphics";
                s.label = "Enemy detail";
                s.order = 46;
                s.help = "Enemies drawn with their close-up detail (antennas, legs, armour, full shading). Full: whole, at "
                         "their finest model, at any distance. Their movement and the game's rules are the console's. "
                         "Off: the console's.";
                s.choices = {Choice{"0", "Off"}, Choice{"1", "One step finer"}, Choice{"4", "Full"}};
                s.defaultValue = "4";
                s.apply = [](const std::string &value)
                {
                    g_enemyDetail.store(enemyDetailOf(value));
                    return true;
                };
                ps2_settings::add(s);
            }
            {
                // N8 item 7: how far the ground's close-up detail texture reaches (staticSceneClear).
                Setting s;
                s.key = "TF_TERRAIN_DETAIL";
                s.section = "Graphics";
                s.label = "Ground detail distance";
                s.order = 47;
                s.help = "How far the ground keeps its close-up detail texture before it fades out: 1x is the console's. "
                         "Auto: the Draw distance's factor, at most 4x.";
                s.choices.push_back(autoChoice());
                for (const char *value : {"1", "2", "3", "4"})
                    s.choices.push_back(Choice{value, std::string(value) + times});
                s.defaultValue = "";
                s.apply = [](const std::string &value)
                {
                    g_terrainDetail.store(terrainDetailOf(value, g_drawPresetShown.load()));
                    return true;
                };
                ps2_settings::add(s);
            }
        }
        {
            Setting s;
            s.key = "PS2X_GPU_NATIVE";
            s.section = "Graphics";
            s.label = "Native pixel path";
            s.order = 80;
            s.help = "The graphics card's own depth test and blending for the scene: about half its time at 4K, the pictures "
                     "within a hair of the PS2's. Off: the GS's pixel arithmetic to the bit, slower.";
            s.choices = onOff();
            s.defaultValue = "1";
            s.available = gpu;
            s.unavailableHelp = "Only for the graphics card's renderer.";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "PS2X_GPU_TEXTURE_CACHE";
            s.section = "Graphics";
            s.label = "Texture cache";
            s.order = 85;
            s.help = "Textures the game reads from its own memory - without a texture pack, nearly all of them - decoded once "
                     "and filtered by the graphics card, instead of read a texel at a time for every pixel: without a pack, "
                     "about half the graphics card's time at 4K. Filtered within a hair of the PS2's.";
            s.choices = onOff();
            s.defaultValue = "1";
            s.available = [gpu]() { return gpu() && ps2_settings::choice("PS2X_GPU_NATIVE") != "0"; };
            s.unavailableHelp = "Needs the native pixel path.";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "PS2X_GPU_FULL_FIELDS";
            s.section = "Graphics";
            s.label = "Full-resolution fields";
            s.order = 90;
            s.help = "Both pictures of a frame drawn at the whole resolution down, not each at half of it: sharper, about "
                     "2.5 ms more of the graphics card a frame at 4K.";
            s.choices = onOff();
            s.defaultValue = "1";
            s.available = gpu;
            s.unavailableHelp = "Only for the graphics card's renderer.";
            ps2_settings::add(s);
        }
        {
            Setting s;
            s.key = "PS2X_GPU_MSAA";
            s.section = "Graphics";
            s.label = "Multisampling";
            s.order = 100;
            s.help = "Smooths the edges of the scene's geometry (the grass and leaves are their textures': FXAA smooths those). "
                     "Costs the graphics card more at higher counts.";
            s.choices = {{"0", "Off"}, {"2", std::string("2") + times}, {"4", std::string("4") + times}, {"8", std::string("8") + times}};
            s.defaultValue = "4";
            s.available = []()
            { return ps2_settings::choice("PS2X_GPU") != "0" && ps2_settings::choice("PS2X_GPU_NATIVE") != "0"; };
            s.unavailableHelp = "Needs the native pixel path.";
            ps2_settings::add(s);
        }
        ext::addSettings(); // the extensions' rows (port_ext.h: the enemy waves)
    }

    // The game's own work in a frame (TF_FPS): the EE cycles charged from the end of one wait
    // for a VBlank (0x25DE90) to the start of the next, idle skips left out. The cycle scale
    // applies: at 60 the work has to fit in 16.7 ms of it.
    constexpr double kEeCyclesPerMs = 294912.0;
    uint32_t g_waitEndCount = 0u;
    uint32_t g_skippedSinceWait = 0u; // idle skips in between
    uint64_t g_workCycleSum = 0u;
    uint32_t g_workCycleMax = 0u;
    uint32_t g_workFrames = 0u;

    uint32_t g_lastFrameWorkCycles = 0u; // (the slow-frame report)

    void noteFrameWork(uint32_t cycles)
    {
        g_lastFrameWorkCycles = cycles;
        g_workCycleSum += cycles;
        g_workCycleMax = std::max(g_workCycleMax, cycles);
        ++g_workFrames;
    }

    // TF_CALLERS=32e910 (debugging aid): counts the calls of that function by return address
    // and prints the counts every 2 s with the frames rendered meanwhile.
    uint32_t g_censusAddress = 0u;
    PS2Runtime::RecompiledFunction g_censusOriginal = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> g_censusCalls;
    uint32_t g_censusFrames = 0u;

    void censusCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        auto it = std::find_if(g_censusCalls.begin(), g_censusCalls.end(), [ra](const auto &c) { return c.first == ra; });
        if (it == g_censusCalls.end())
            g_censusCalls.emplace_back(ra, 1u);
        else
            ++it->second;
        g_censusOriginal(rdram, ctx, runtime);
    }

    void reportCensus()
    {
        using clock = std::chrono::steady_clock;
        static auto start = clock::now();
        ++g_censusFrames;
        if (clock::now() - start < std::chrono::seconds(2))
            return;
        std::sort(g_censusCalls.begin(), g_censusCalls.end());
        std::string line = "[tf:callers] " + std::to_string(g_censusFrames) + " frames:";
        char text[32];
        for (const auto &[ra, calls] : g_censusCalls)
        {
            std::snprintf(text, sizeof(text), " %08x=%u", ra, calls);
            line += text;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
        g_censusCalls.clear();
        g_censusFrames = 0u;
        start = clock::now();
    }

    // sub_0025D2B0 closes and kicks the display list once per rendered frame.
    // TF_FPS=1 prints the frame rate every 2 s, with the VBlank rate (below the level's rate
    // when the guest's own cycle count, not the host, is what holds frames back), the EE's
    // work a frame by its clock and the host time a frame the EE thread waited for the host's
    // clock (in real time the headroom: 0 when the host is what holds frames back).
    constexpr uint32_t kFrameKick = 0x0025D2B0u;
    PS2Runtime::RecompiledFunction g_frameKick = nullptr;
    bool g_printFps = false;
    // TF_FRAME_LOG=<file>: one line per frame: the VBlank tick, the host milliseconds since the
    // last frame, how much of that the EE thread was busy (the rest it waited for the VIF1 and
    // GS threads, or for the VBlank), the camera's position (0x731ED0), the player's, the view's
    // heading and the tick in game time (t: testTick's, 60 a second at every frame rate). In
    // turbo mode the time is what the frame cost: above 33.3 ms the game cannot keep 30 fps
    // there in real time.
    std::FILE *g_frameLog = nullptr;
    constexpr uint32_t kCameraPosition = 0x00731ED0u;

    void logFrame(const uint8_t *rdram, uint64_t tick)
    {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
#if defined(_WIN32)
        static double lastCpu = tfThreadCpuMs();
        const double cpu = tfThreadCpuMs();
        const double busy = cpu - lastCpu;
        lastCpu = cpu;
#else
        const double busy = 0.0;
#endif
        float camera[3] = {};
        if (const uint8_t *p = getConstMemPtr(rdram, kCameraPosition))
            std::memcpy(camera, p, sizeof(camera));
        // The player's position and the view's heading (the first row of the camera's
        // transform: where it looks), for comparing movement between runs.
        float player[3] = {}, forward[3] = {};
        static uint32_t lastAddress = 0u;
        if (const uint32_t address = playerPositionAddress(rdram))
        {
            if (address != lastAddress)
                std::fprintf(stderr, "[tf] tick %llu: player object at %08x, camera %08x, its transform %08x\n",
                             static_cast<unsigned long long>(tick), address - kPlayerPositionField, g_camera, g_cameraTransform);
            lastAddress = address;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(player, p, sizeof(player));
        }
        if (const uint8_t *p = g_cameraTransform ? getConstMemPtr(rdram, g_cameraTransform) : nullptr)
            std::memcpy(forward, p, sizeof(forward));
        // (the last column: the wall clock, seconds since 1970 - to line the frames up with other logs)
        const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(g_frameLog, "%llu,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%.2f,%.3f\n", static_cast<unsigned long long>(tick),
                     std::chrono::duration<double, std::milli>(now - last).count(), busy, camera[0], camera[1], camera[2],
                     player[0], player[1], player[2], std::atan2(forward[1], forward[0]), g_testTick, wall);
        std::fflush(g_frameLog); // the process ends with _Exit, which flushes nothing
        last = now;
    }

    // (TODO 112, a testing aid) TF_TEST_STALL=<game tick>,<ms>,<frames>: from that tick (TF_INPUT's count) the game's
    // thread sleeps <ms> in each of the next <frames> frames - a made hitch, to watch the adaptive ladder fall and
    // come back up (TF_RATE_LOG=1 says why it holds).
    void testStall(PS2Runtime *runtime)
    {
        static int from = -1, ms = 0, frames = 0;
        static bool parsed = false;
        if (!parsed)
        {
            parsed = true;
            if (const char *e = std::getenv("TF_TEST_STALL"))
                if (std::sscanf(e, "%d,%d,%d", &from, &ms, &frames) != 3)
                    from = -1;
        }
        if (from < 0 || frames <= 0 || testTick(runtime) < static_cast<uint64_t>(from))
            return;
        if (frames-- > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        if (frames == 0)
            std::fprintf(stderr, "[tf] TF_TEST_STALL: the made hitch is over\n");
    }

    // (TODO 112) TF_HITCH_MS=<ms>: every game frame longer than that (from one frame's start to the next) gets a line
    // telling where its time went - the game's thread's own CPU time, its waits for the clock (a VBlank, or the guest
    // waiting inside the frame: a semaphore, a movie picture), for the renderer (both frames in flight), the rest (the
    // thread not running: other threads or programs had the CPU) - with the render thread's waits for the GPU and what
    // the large images' loaders and the pipeline maker were doing. On at 50 ms by default when TF_FPS is on (the
    // playtest copy), 0 = off. Not while a level loads, in turbo mode or while a movie plays; at most 8 lines in 2 s.
    void reportSlowFrame(uint8_t *rdram, PS2Runtime *runtime)
    {
        static const double threshold = []()
        {
            if (const char *e = std::getenv("TF_HITCH_MS"))
                return std::atof(e);
            return std::getenv("TF_FPS") && *std::getenv("TF_FPS") == '1' ? 50.0 : 0.0;
        }();
        if (threshold <= 0.0)
            return;
        EeScheduler &ee = runtime->eeScheduler();
        const auto now = std::chrono::steady_clock::now();
        static auto last = now;
        static auto idleBefore = ee.idleHostTime();
#if defined(_WIN32)
        static double cpuBefore = tfThreadCpuMs();
        const double cpu = tfThreadCpuMs();
#else
        static double cpuBefore = 0.0;
        const double cpu = 0.0;
#endif
        static double renderWaitBefore = nr::gameWaitMs(), slotWaitBefore = nr::d3d::slotWaitMs();
        static uint64_t pipelinesBefore = nr::pipelinesMade(), compressedBefore = 0u, pngsBefore = 0u;
        static auto windowStart = now;
        static int linesInWindow = 0;
        const double intervalMs = std::chrono::duration<double, std::milli>(now - last).count();
        const double idleMs = std::chrono::duration<double, std::milli>(ee.idleHostTime() - idleBefore).count();
        const double cpuMs = cpu - cpuBefore, renderWait = nr::gameWaitMs() - renderWaitBefore,
                     slotWait = nr::d3d::slotWaitMs() - slotWaitBefore;
        const uint64_t pipelines = nr::pipelinesMade();
        unsigned queued = 0u, heavy = 0u;
        uint64_t compressed = 0u, pngs = 0u;
        nr::largeImageActivity(queued, heavy, compressed, pngs);
        const uint8_t *loading = getConstMemPtr(rdram, 0x006CD324u);
        const bool skip = ee.turboActive() || g_liveFmvStreams != 0 || (loading && *loading != 0u && !g_inHq);
        if (!skip && intervalMs > threshold)
        {
            if (now - windowStart >= std::chrono::seconds(2))
            {
                windowStart = now;
                linesInWindow = 0;
            }
            if (linesInWindow++ < 8)
                std::fprintf(stderr,
                             "[tf] slow frame %.1f ms at vblank %llu (%s at %d): game thread CPU %.1f ms, waited for the clock %.1f "
                             "(game work %.1f ms of EE time), for the renderer %.1f, not running %.1f; render thread waited for "
                             "the GPU %.1f ms; large images %u queued, %u decoding, +%llu compressed, +%llu PNGs; +%llu pipelines\n",
                             intervalMs, static_cast<unsigned long long>(ee.currentVSyncTick()), g_inHq ? "HQ" : g_levelRate ? "level" : "menus",
                             g_levelRate, cpuMs, idleMs, g_lastFrameWorkCycles / kEeCyclesPerMs, renderWait,
                             std::max(0.0, intervalMs - cpuMs - idleMs - renderWait), slotWait, queued, heavy,
                             static_cast<unsigned long long>(compressed - compressedBefore), static_cast<unsigned long long>(pngs - pngsBefore),
                             static_cast<unsigned long long>(pipelines - pipelinesBefore));
        }
        last = now;
        idleBefore = ee.idleHostTime();
        cpuBefore = cpu;
        renderWaitBefore = nr::gameWaitMs();
        slotWaitBefore = nr::d3d::slotWaitMs();
        pipelinesBefore = pipelines;
        compressedBefore = compressed;
        pngsBefore = pngs;
    }

    void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        testStall(runtime);
        reportSlowFrame(rdram, runtime);
        key_text::frame(rdram); // (the sticks' words for the keyboard: key_text.h)
        ps2_host_display::trace("frame kick", runtime->memory().gs().dispfb2);
        if (g_frameLog)
        {
            testTick(runtime); // (g_testTick for the log)
            logFrame(rdram, runtime->eeScheduler().currentVSyncTick());
        }
        if (!g_peeked.empty())
            reportPeekedWords(rdram, testTick(runtime));
        if (g_censusAddress != 0u)
            reportCensus();
        if (g_printFps)
        {
            using clock = std::chrono::steady_clock;
            static auto windowStart = clock::now();
            static uint32_t frames = 0;
            static uint64_t windowTick = 0;
            static uint64_t windowPictures = 0;
            static uint64_t windowLeftOut = 0;
            static auto windowIdle = runtime->eeScheduler().idleHostTime();
            // The 1% low (issue 12's thread placement bench): the frames of the last 30 s (the state load left out, as
            // bench.ps1 does), the average of the slowest 1% as a rate. A ring buffer, so a long session costs no more
            // than a short one and the report does not hitch the frames it measures.
            struct FrameTime
            {
                clock::time_point at;
                float ms;
            };
            constexpr size_t kRingFrames = 16384u; // (30 s at up to ~540 frames a second; faster, the window is shorter)
            constexpr auto kLowWindow = std::chrono::seconds(30);
            static std::vector<FrameTime> ring(kRingFrames);
            static std::vector<float> slowest = []
            {
                std::vector<float> v;
                v.reserve(kRingFrames);
                return v;
            }();
            static size_t ringNext = 0;
            static size_t ringCount = 0;
            static clock::time_point lastFrame{};
            static bool reportedOnce = false;
            const auto frameNow = clock::now();
            if (reportedOnce && lastFrame != clock::time_point{})
            {
                ring[ringNext] = FrameTime{frameNow, std::chrono::duration<float, std::milli>(frameNow - lastFrame).count()};
                ringNext = (ringNext + 1u) % kRingFrames;
                ringCount = std::min(ringCount + 1u, kRingFrames);
            }
            lastFrame = frameNow;
            ++frames;
            const double elapsed = std::chrono::duration<double>(frameNow - windowStart).count();
            if (elapsed >= 2.0)
            {
                const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
                ps2_host_display::Clock::time_point lastPicture{};
                const uint64_t pictures = ps2_host_display::pictureCount(lastPicture);
                std::cerr << "[tf] fps " << frames / elapsed << " vblank/s " << (tick - windowTick) / elapsed
                          << " pictures/s " << (pictures - windowPictures) / elapsed
                          << " EE work ms a frame "
                          << (g_workFrames ? static_cast<double>(g_workCycleSum) / g_workFrames / kEeCyclesPerMs : 0.0)
                          << " (at most " << g_workCycleMax / kEeCyclesPerMs << ")"
                          << " host idle ms a frame "
                          << std::chrono::duration<double, std::milli>(runtime->eeScheduler().idleHostTime() - windowIdle).count() /
                                 std::max(frames, 1u)
                          << " in-between left out " << ps2_host_display::secondFieldsLeftOut() - windowLeftOut;
                // (newest first, up to the first frame older than the window)
                slowest.clear();
                for (size_t i = 0; i < ringCount; ++i)
                {
                    const FrameTime &f = ring[(ringNext + kRingFrames - 1u - i) % kRingFrames];
                    if (frameNow - f.at > kLowWindow)
                        break;
                    slowest.push_back(f.ms);
                }
                if (slowest.size() >= 100u)
                {
                    const size_t count = slowest.size() / 100u;
                    std::nth_element(slowest.begin(), slowest.begin() + static_cast<std::ptrdiff_t>(count - 1u), slowest.end(), std::greater<float>());
                    double sum = 0.0;
                    for (size_t i = 0; i < count; ++i)
                        sum += slowest[i];
                    std::cerr << " 1% low " << (sum > 0.0 ? 1000.0 * static_cast<double>(count) / sum : 0.0);
                }
                std::cerr << std::endl;
                reportedOnce = true;
                frames = 0;
                windowTick = tick;
                windowPictures = pictures;
                windowLeftOut = ps2_host_display::secondFieldsLeftOut();
                windowIdle = runtime->eeScheduler().idleHostTime();
                windowStart = clock::now();
                g_workCycleSum = 0u;
                g_workCycleMax = 0u;
                g_workFrames = 0u;
            }
        }
        if (playrec::armed())
        {
            // (N12 b: the frame's check for a play recording or its replay - playrec.h)
            playrec::FrameFacts facts;
            facts.vsync = runtime->eeScheduler().currentVSyncTick();
            facts.gameTick = testTick(runtime);
            facts.rate = g_levelRate;
            facts.seed = gameSeed(rdram);
            if (const uint8_t *p = getConstMemPtr(rdram, kCameraPosition))
                std::memcpy(facts.camera, p, sizeof(facts.camera));
            if (const uint32_t at = playerPositionAddress(rdram))
                if (const uint8_t *p = getConstMemPtr(rdram, at))
                    std::memcpy(facts.player, p, sizeof(facts.player));
            float forward[3] = {};
            if (const uint8_t *p = g_cameraTransform ? getConstMemPtr(rdram, g_cameraTransform) : nullptr)
                std::memcpy(forward, p, sizeof(forward));
            facts.heading = std::atan2(forward[1], forward[0]);
            facts.photo = photo_mode::active();
            facts.menuOpen = mod_menu::isOpen();
            playrec::frame(runtime, facts);
        }
        updatePadOverride(rdram, runtime);
        mouseFollowsInvertOption(rdram); // (TODO 178)
        g_mouseLookRdram = rdram;
        mouseLook();
        updateDisplayAspect(rdram);
        applyFrameRate(rdram, ctx, runtime);
        applyDrawDistance(rdram, runtime);
        charDetailFrame(runtime);
        ++g_cullFrame, g_cullView = 0, g_cullFresh = true; // (TODO 99 check: the scene cull's views counted per frame)
        g_listsKicked.fetch_add(1u, std::memory_order_release); // (the list kicked now: tf::port::listsKicked)
        g_frameKick(rdram, ctx, runtime);
    }

    // (The wait for the VBlank at the end of a frame, 0x25DE90, and the main loop, 0x295310,
    // with its own wait for the frame's VBlanks, are decompiled - each with the EE clock
    // jumping to the next event instead of polling: decomp/timing/vblank.cpp, main_loop.cpp.)

    // Call tracer (debugging aid): TF_TRACE_FUNCS=133f40,133ce8 logs the
    // first calls to those functions with a0-a3, ra and the returned v0.
    // TF_TRACE_AFTER=<seconds> starts logging later; TF_TRACE_QWORDS=<n> also
    // prints n qwords (as floats) at a1 and a2 before the call and at a0 after.
    struct TracedFunction
    {
        uint32_t address = 0u;
        PS2Runtime::RecompiledFunction original = nullptr;
        uint32_t calls = 0u;
    };
    constexpr int kMaxTraced = 16;
    TracedFunction g_traced[kMaxTraced];
    double g_traceAfter = 0.0;
    int g_traceQwords = 0;

    void printQwords(const uint8_t *rdram, const char *label, uint32_t address)
    {
        for (int q = 0; q < g_traceQwords; ++q)
        {
            float v[4] = {};
            if (const uint8_t *p = getConstMemPtr(rdram, address + 16u * q)) // RAM or scratchpad
                std::memcpy(v, p, sizeof(v));
            std::fprintf(stderr, "[tf:mem]    %s+%02x (%g %g %g %g)\n", label, 16 * q, v[0], v[1], v[2], v[3]);
        }
    }

    // Printable text of at least two characters at an address, or nothing.
    std::string guestText(const uint8_t *rdram, uint32_t address, uint32_t length, bool terminated)
    {
        std::string text;
        if (address < 0x80000u || address >= PS2_RAM_SIZE || length > 200u)
            return text;
        for (uint32_t i = 0; i < (terminated ? 200u : length); ++i)
        {
            const uint8_t *p = getConstMemPtr(rdram, address + i);
            if (!p)
                return {};
            if (terminated && *p == 0u)
                break;
            if (*p < 0x20u || *p > 0x7Eu)
                return {};
            text.push_back(static_cast<char>(*p));
        }
        return text.size() >= 2u ? text : std::string{};
    }

    // TF_TRACE_STRINGS=1: what an argument of a traced call points at, when that is text:
    // a C string, or the game's own string objects (12 bytes: an odd first word means
    // length and pointer in the next two, otherwise the first byte is twice the length and
    // the text follows it) - up to four in a row, which shows a parsed file name.
    bool g_traceStrings = false;

    void printTextArgument(const uint8_t *rdram, const char *label, uint32_t address)
    {
        if (address < 0x80000u || address >= PS2_RAM_SIZE - 64u)
            return;
        std::string line;
        bool anyText = false;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const uint8_t *p = getConstMemPtr(rdram, address + 12u * i);
            if (!p)
                break;
            uint32_t words[3];
            std::memcpy(words, p, sizeof(words));
            std::string text;
            if (words[0] & 1u)
                text = guestText(rdram, words[2], words[1], false);
            else if ((words[0] & 0xFFu) <= 22u)
                text = guestText(rdram, address + 12u * i + 1u, (words[0] & 0xFFu) >> 1u, false);
            anyText = anyText || !text.empty();
            line += (i ? " | \"" : "\"") + text + "\"";
        }
        if (!anyText)
            line.clear();
        if (line.empty())
        {
            const std::string text = guestText(rdram, address, 0u, true);
            if (text.empty())
                return;
            line = "'" + text + "'";
        }
        std::fprintf(stderr, "[tf:text]   %s = %s\n", label, line.c_str());
    }

    template <int N>
    void tracedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        using clock = std::chrono::steady_clock;
        static const auto start = clock::now();
        TracedFunction &t = g_traced[N];
        if (std::chrono::duration<double>(clock::now() - start).count() < g_traceAfter)
        {
            t.original(rdram, ctx, runtime);
            return;
        }
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t call = t.calls++;
        if (call < 40u)
        {
            std::fprintf(stderr, "[tf:call] %08x(%08x, %08x, %08x, %08x) ra=%08x #%u\n", t.address, a0, a1, a2, a3, ra, call);
            printQwords(rdram, "a1", a1);
            printQwords(rdram, "a2", a2);
            if (g_traceStrings)
            {
                printTextArgument(rdram, "a0", a0);
                printTextArgument(rdram, "a1", a1);
                printTextArgument(rdram, "a2", a2);
                printTextArgument(rdram, "a3", a3);
            }
        }
        t.original(rdram, ctx, runtime);
        if (call < 40u)
        {
            std::fprintf(stderr, "[tf:ret]  %08x -> %08x\n", t.address, getRegU32(ctx, 2));
            printQwords(rdram, "a0", a0);
        }
    }

    template <int... N>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(N)> makeTracers(std::integer_sequence<int, N...>)
    {
        return {tracedCall<N>...};
    }

    // Call counter (debugging aid): TF_COUNT_FUNCS=2202e0,220350 prints how
    // often each function was called, once a second; with TF_COUNT_PER_TICK=1 once per VBlank
    // instead, with its tick of game time (testTick, as TF_INPUT's: at 60 frames with 120
    // pictures two VBlanks share a tick), so runs at any rate can be compared line by line.
    struct CountedFunction
    {
        uint32_t address = 0u;
        PS2Runtime::RecompiledFunction original = nullptr;
        uint32_t calls = 0u;
    };
    constexpr int kMaxCounted = 24;
    CountedFunction g_counted[kMaxCounted];
    int g_countedUsed = 0;
    bool g_countPerTick = false;
    uint64_t g_countTick = 0u, g_countGameTick = 0u;

    void reportCallCounts(PS2Runtime *runtime)
    {
        if (g_countPerTick)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (tick == g_countTick)
                return;
            std::string line = "[tf:count] tick " + std::to_string(g_countGameTick);
            char text[32];
            for (int i = 0; i < g_countedUsed; ++i)
            {
                std::snprintf(text, sizeof(text), " %x:%u", g_counted[i].address, g_counted[i].calls);
                line += text;
                g_counted[i].calls = 0u;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            g_countTick = tick;
            g_countGameTick = testTick(runtime);
            return;
        }
        using clock = std::chrono::steady_clock;
        static const auto start = clock::now();
        static auto windowStart = start;
        const auto now = clock::now();
        if (now - windowStart < std::chrono::seconds(1))
            return;
        windowStart = now;
        std::string line;
        char text[48];
        std::snprintf(text, sizeof(text), "[tf:count] t=%.1f", std::chrono::duration<double>(now - start).count());
        line += text;
        for (int i = 0; i < g_countedUsed; ++i)
        {
            std::snprintf(text, sizeof(text), " %x:%u", g_counted[i].address, g_counted[i].calls);
            line += text;
            g_counted[i].calls = 0u;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    template <int N>
    void countedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        reportCallCounts(runtime);
        ++g_counted[N].calls;
        g_counted[N].original(rdram, ctx, runtime);
    }

    template <int... N>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(N)> makeCounters(std::integer_sequence<int, N...>)
    {
        return {countedCall<N>...};
    }

    void installCallCounter(PS2Runtime &runtime)
    {
        const char *list = std::getenv("TF_COUNT_FUNCS");
        if (!list)
            return;
        if (const char *perTick = std::getenv("TF_COUNT_PER_TICK"))
            g_countPerTick = *perTick && *perTick != '0';
        static const auto counters = makeCounters(std::make_integer_sequence<int, kMaxCounted>{});
        for (const char *p = list; *p && g_countedUsed < kMaxCounted;)
        {
            char *end = nullptr;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p)
                break;
            if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address))
            {
                g_counted[g_countedUsed] = {address, original, 0u};
                runtime.registerFunction(address, counters[static_cast<size_t>(g_countedUsed)]);
                ++g_countedUsed;
            }
            p = (*end == ',') ? end + 1 : end;
        }
    }

    // TF_DIFF_FUNCS=<hex>,<hex>,... or @<file of hex addresses> with TF_DIFF_RANGE=<hex
    // address>,<hex bytes> (debugging aid): logs every call of those functions that left the
    // range different (as floats), with the call depth among them - the deepest is the writer
    // - and the VBlank tick. TF_DIFF_AFTER=<tick> starts later; TF_DIFF_MAX=<n> lines (400).
    std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> g_diffOriginals;
    uint32_t g_diffAddress = 0u, g_diffBytes = 0u;
    uint64_t g_diffAfter = 0u;
    int g_diffMax = 400, g_diffLogged = 0;
    thread_local int t_diffDepth = 0;

    void diffCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entry = ctx->pc;
        const auto it = g_diffOriginals.find(entry);
        if (it == g_diffOriginals.end())
        {
            std::fprintf(stderr, "[tf:diff] no original for %08x\n", entry);
            return;
        }
        uint8_t before[64] = {};
        const uint8_t *watched = getConstMemPtr(rdram, g_diffAddress);
        if (watched)
            std::memcpy(before, watched, g_diffBytes);
        const uint32_t ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4);
        const int depth = t_diffDepth++;
        it->second(rdram, ctx, runtime);
        --t_diffDepth;
        if (!watched || g_diffLogged >= g_diffMax || std::memcmp(before, watched, g_diffBytes) == 0)
            return;
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        if (tick < g_diffAfter)
            return;
        ++g_diffLogged;
        std::string line;
        char text[96];
        std::snprintf(text, sizeof(text), "[tf:diff] tick %llu depth %d %08x (a0 %08x ra %08x%s):",
                      static_cast<unsigned long long>(tick), depth, entry, a0, ra, ctx->pc == ra ? "" : ", unfinished");
        line += text;
        for (uint32_t i = 0; i + 4u <= g_diffBytes; i += 4u)
        {
            float b = 0.0f, a = 0.0f;
            std::memcpy(&b, before + i, 4u);
            std::memcpy(&a, watched + i, 4u);
            if (std::memcmp(before + i, watched + i, 4u) != 0)
            {
                std::snprintf(text, sizeof(text), " +%x %g>%g", i, b, a);
                line += text;
            }
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    void installDiffHooks(PS2Runtime &runtime)
    {
        const char *list = std::getenv("TF_DIFF_FUNCS");
        const char *range = std::getenv("TF_DIFF_RANGE");
        if (!list || !range)
            return;
        char *end = nullptr;
        g_diffAddress = static_cast<uint32_t>(std::strtoul(range, &end, 16));
        g_diffBytes = std::min<uint32_t>(64u, end && *end == ',' ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16)) : 4u);
        if (const char *after = std::getenv("TF_DIFF_AFTER"))
            g_diffAfter = std::strtoull(after, nullptr, 10);
        if (const char *most = std::getenv("TF_DIFF_MAX"))
            g_diffMax = std::atoi(most);
        std::string text = list;
        if (!text.empty() && text[0] == '@')
        {
            text.clear();
            if (std::FILE *f = std::fopen(list + 1, "r"))
            {
                char buffer[256];
                while (std::fgets(buffer, sizeof(buffer), f))
                    (text += buffer) += ',';
                std::fclose(f);
            }
        }
        for (const char *p = text.c_str(); *p;)
        {
            const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p)
            {
                ++p;
                continue;
            }
            if (address != 0u && !g_diffOriginals.count(address))
                if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address))
                {
                    g_diffOriginals[address] = original;
                    runtime.registerFunction(address, diffCall);
                }
            p = end;
        }
        std::fprintf(stderr, "[tf:diff] %zu functions watched for changes to %08x (+%u bytes)\n", g_diffOriginals.size(),
                     g_diffAddress, g_diffBytes);
    }

    // TF_STEALTH_LOG=1 (testing aid, TODO item 31): each stealth suit's creature (its constructor,
    // taStealthSuit::taStealthSuit 0x4E72F0, suit in a0, creature in a1) and its switches
    // (SetActive 0x4E7290, a1 on/off) with where the creature is (its actor at +0xF4, the
    // position as the player's, +0x620).
    PS2Runtime::RecompiledFunction g_suitCtor = nullptr, g_suitSetActive = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> g_suits; // (suit, creature)
    std::vector<std::pair<uint32_t, uint32_t>> g_suitStates; // (suit, last on/off)

    void suitCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_suits.emplace_back(getRegU32(ctx, 4), getRegU32(ctx, 5));
        std::fprintf(stderr, "[tf:stealth] suit %08x made for creature %08x\n", getRegU32(ctx, 4), getRegU32(ctx, 5));
        g_suitCtor(rdram, ctx, runtime);
    }

    void suitSetActive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t suit = getRegU32(ctx, 4), on = getRegU32(ctx, 5) & 0xFFu;
        auto state = std::find_if(g_suitStates.begin(), g_suitStates.end(), [suit](const auto &s) { return s.first == suit; });
        if (state == g_suitStates.end() || state->second != on)
        {
            if (state == g_suitStates.end())
                g_suitStates.emplace_back(suit, on);
            else
                state->second = on;
            const auto it = std::find_if(g_suits.begin(), g_suits.end(), [suit](const auto &s) { return s.first == suit; });
            const uint32_t creature = it != g_suits.end() ? it->second : 0u;
            uint32_t actor = 0u;
            float at[3] = {};
            if (const uint8_t *p = creature ? getConstMemPtr(rdram, creature + 0xF4u) : nullptr)
                std::memcpy(&actor, p, sizeof(actor));
            if (const uint8_t *p = actor ? getConstMemPtr(rdram, actor + kPlayerPositionField) : nullptr)
                std::memcpy(at, p, sizeof(at));
            std::fprintf(stderr, "[tf:stealth] tick %llu: suit %08x (creature %08x) %s at (%.1f, %.1f, %.1f)\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), suit, creature,
                         on ? "ON" : "off", at[0], at[1], at[2]);
        }
        g_suitSetActive(rdram, ctx, runtime);
    }

    void installCallTracer(PS2Runtime &runtime)
    {
        installDiffHooks(runtime);
        if (std::getenv("TF_STEALTH_LOG"))
        {
            if ((g_suitCtor = runtime.lookupFunction(0x004E72F0u)) != nullptr)
                runtime.registerFunction(0x004E72F0u, suitCtor);
            if ((g_suitSetActive = runtime.lookupFunction(0x004E7290u)) != nullptr)
                runtime.registerFunction(0x004E7290u, suitSetActive);
            std::fprintf(stderr, "[tf:stealth] hooks: constructor %s, SetActive %s\n", g_suitCtor ? "in" : "MISSING",
                         g_suitSetActive ? "in" : "MISSING");
        }
        if (const char *census = std::getenv("TF_CALLERS"); census && *census)
        {
            g_censusAddress = static_cast<uint32_t>(std::strtoul(census, nullptr, 16));
            g_censusOriginal = runtime.lookupFunction(g_censusAddress);
            if (g_censusOriginal)
                runtime.registerFunction(g_censusAddress, censusCall);
            else
                g_censusAddress = 0u;
        }
        const char *list = std::getenv("TF_TRACE_FUNCS");
        if (!list)
            return;
        if (const char *after = std::getenv("TF_TRACE_AFTER"))
            g_traceAfter = std::atof(after);
        if (const char *qwords = std::getenv("TF_TRACE_QWORDS"))
            g_traceQwords = std::clamp(std::atoi(qwords), 0, 16);
        if (const char *strings = std::getenv("TF_TRACE_STRINGS"))
            g_traceStrings = *strings && *strings != '0';
        static const auto tracers = makeTracers(std::make_integer_sequence<int, kMaxTraced>{});
        int count = 0;
        for (const char *p = list; *p && count < kMaxTraced;)
        {
            char *end = nullptr;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p)
                break;
            if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address))
            {
                g_traced[count] = {address, original, 0u};
                runtime.registerFunction(address, tracers[static_cast<size_t>(count)]);
                ++count;
            }
            else
            {
                std::fprintf(stderr, "[tf] TF_TRACE_FUNCS: no function at %08x\n", address);
            }
            p = (*end == ',') ? end + 1 : end;
        }
    }

    // Coverage (debugging aid): TF_COVERAGE=<file> wraps every function of work/functions.csv
    // (after all other hooks) and writes the start address of each one that ran at least
    // once, one per line, when the process exits. Calls arrive through dispatchGuestBranch
    // with ctx->pc on the function's start; a resumed call has ctx->pc inside the function,
    // so the function is found by its range.
    std::vector<uint32_t> g_coverageStarts;
    std::vector<PS2Runtime::RecompiledFunction> g_coverageOriginal;
    std::vector<uint8_t> g_coverageHit;
    std::string g_coveragePath;

    void coverageCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto it = std::upper_bound(g_coverageStarts.begin(), g_coverageStarts.end(), ctx->pc);
        const size_t index = static_cast<size_t>(it - g_coverageStarts.begin()) - 1u;
        g_coverageHit[index] = 1u;
        // (TF_CALL_LOG=<vblank>[:<count>], a test aid with TF_COVERAGE - TODO 150: every function entered from that VBlank,
        // its return address and COP0 Count, <count> of them (50,000): where two runs that should play the same part)
        static unsigned long long callFrom = 0ull;
        static long long callBudget = -1;
        if (callBudget < 0)
        {
            callBudget = 0;
            if (const char *e = std::getenv("TF_CALL_LOG"); e && *e)
            {
                char *end = nullptr;
                callFrom = std::strtoull(e, &end, 10);
                callBudget = (end && *end == ':') ? std::atoll(end + 1) : 50000;
            }
        }
        if (callBudget > 0 && runtime->eeScheduler().currentVSyncTick() >= callFrom)
        {
            --callBudget;
            // (TF_CALL_WATCH=<hex address>: the word there too, at each entry - where a value is written)
            // ("car": the first car's body velocity z, +0x68, as TF_CAR_LOG found it)
            static const char *watchText = std::getenv("TF_CALL_WATCH");
            static const bool watchCar = watchText && std::strcmp(watchText, "car") == 0;
            const uint32_t watch = watchCar ? (g_carLogBody.load() ? g_carLogBody.load() + 0x68u : 0u)
                                            : (watchText ? static_cast<uint32_t>(std::strtoul(watchText, nullptr, 16)) : 0u);
            uint32_t watched = 0u;
            if (const uint8_t *w = watch ? getConstMemPtr(rdram, watch) : nullptr)
                std::memcpy(&watched, w, sizeof(watched));
            std::fprintf(stderr, "[tf:enter] %08x %08x %u %08x\n", ctx->pc, getRegU32(ctx, 31), ctx->cop0_count, watched);
        }
        g_coverageOriginal[index](rdram, ctx, runtime);
    }

    void writeCoverage()
    {
        if (g_coveragePath.empty())
            return;
        if (FILE *out = std::fopen(g_coveragePath.c_str(), "w"))
        {
            for (size_t i = 0; i < g_coverageStarts.size(); ++i)
                if (g_coverageHit[i])
                    std::fprintf(out, "%08x\n", g_coverageStarts[i]);
            std::fclose(out);
        }
    }

    void installCoverage(PS2Runtime &runtime)
    {
        const char *path = std::getenv("TF_COVERAGE");
        if (!path || !*path)
            return;
        const char *listPath = std::getenv("TF_COVERAGE_LIST"); // (the list elsewhere: a worker has no work/ folder)
        FILE *list = std::fopen(listPath && *listPath ? listPath : "work/functions.csv", "r");
        if (!list)
        {
            std::fprintf(stderr, "[tf] TF_COVERAGE: work/functions.csv not found\n");
            return;
        }
        char line[256];
        std::vector<uint32_t> starts;
        while (std::fgets(line, sizeof(line), list))
            if (const char *comma = std::strchr(line, ','); comma && comma[1] == '0')
                starts.push_back(static_cast<uint32_t>(std::strtoul(comma + 1, nullptr, 16)));
        std::fclose(list);
        std::sort(starts.begin(), starts.end());
        for (uint32_t start : starts)
            if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(start))
            {
                g_coverageStarts.push_back(start);
                g_coverageOriginal.push_back(original);
            }
        g_coverageHit.assign(g_coverageStarts.size(), 0u);
        for (uint32_t start : g_coverageStarts)
            runtime.registerFunction(start, coverageCall);
        g_coveragePath = path;
        std::atexit(writeCoverage);
        std::fprintf(stderr, "[tf] TF_COVERAGE: %zu functions wrapped\n", g_coverageStarts.size());
    }

    // Call map (a decompilation aid): TF_CALLMAP=<file> wraps every function of
    // work/functions.csv like TF_COVERAGE. For each call that enters a function at its start it
    // records the vtables of the objects the first four arguments point to (up to four different
    // ones per argument), and for a call made by jalr the call site and the target. Written at
    // TF_QUIT_VSYNC or exit, one fact a line: "F start calls", "A start arg vtable",
    // "I site target count".
    struct CallMapFunction
    {
        uint32_t calls = 0u;
        uint32_t vtables[4][4] = {};
    };
    std::vector<uint32_t> g_callMapStarts;
    std::vector<PS2Runtime::RecompiledFunction> g_callMapOriginal;
    std::vector<CallMapFunction> g_callMapFunctions;
    std::unordered_map<uint64_t, uint32_t> g_callMapIndirect;
    std::string g_callMapPath;

    bool callMapWord(const uint8_t *rdram, uint32_t address, uint32_t &value)
    {
        const uint32_t physical = address & 0x1FFFFFFFu; // KSEG0/KSEG1 mirrors
        if ((physical & 3u) != 0u || physical < 0x00100000u || physical + 4u > PS2_RAM_SIZE)
            return false;
        std::memcpy(&value, rdram + physical, 4);
        return true;
    }

    // A CodeWarrior vtable: the type_info (or 0), the offset to the object's start (0, or
    // negative in a base's table), then code pointers.
    bool callMapIsVtable(const uint8_t *rdram, uint32_t vtable)
    {
        uint32_t typeInfo = 0u, offset = 0u, slot0 = 0u;
        if (!callMapWord(rdram, vtable, typeInfo) || !callMapWord(rdram, vtable + 4u, offset) ||
            !callMapWord(rdram, vtable + 8u, slot0))
            return false;
        const bool typeOk = typeInfo == 0u || (typeInfo >= 0x00600000u && typeInfo < 0x00800000u);
        const bool offsetOk = offset == 0u || (static_cast<int32_t>(offset) < 0 && static_cast<int32_t>(offset) > -0x10000);
        return typeOk && offsetOk && slot0 >= 0x00100000u && slot0 < 0x00600000u && (slot0 & 3u) == 0u;
    }

    void callMapCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto it = std::upper_bound(g_callMapStarts.begin(), g_callMapStarts.end(), ctx->pc);
        const size_t index = static_cast<size_t>(it - g_callMapStarts.begin()) - 1u;
        if (ctx->pc == g_callMapStarts[index])
        {
            CallMapFunction &f = g_callMapFunctions[index];
            if (f.calls != 0xFFFFFFFFu)
                ++f.calls;
            for (int arg = 0; arg < 4; ++arg)
            {
                uint32_t vtable = 0u;
                if (!callMapWord(rdram, getRegU32(ctx, 4 + arg), vtable) || !callMapIsVtable(rdram, vtable))
                    continue;
                uint32_t *seen = f.vtables[arg];
                for (int k = 0; k < 4; ++k)
                {
                    if (seen[k] == vtable)
                        break;
                    if (seen[k] == 0u)
                    {
                        seen[k] = vtable;
                        break;
                    }
                }
            }
            // Entered from a jalr: the word before the return address is the jalr, and its
            // register still holds this function (not the case for a tail call made by the
            // function the jalr called).
            const uint32_t site = getRegU32(ctx, 31) - 8u;
            uint32_t word = 0u;
            if (callMapWord(rdram, site, word) && (word >> 26) == 0u && (word & 0x3Fu) == 9u &&
                getRegU32(ctx, static_cast<int>((word >> 21) & 31u)) == ctx->pc)
                ++g_callMapIndirect[(static_cast<uint64_t>(site) << 32) | ctx->pc];
        }
        g_callMapOriginal[index](rdram, ctx, runtime);
    }

    void writeCallMap()
    {
        if (g_callMapPath.empty())
            return;
        if (FILE *out = std::fopen(g_callMapPath.c_str(), "w"))
        {
            std::fprintf(out, "# TF_CALLMAP 1\n");
            for (size_t i = 0; i < g_callMapStarts.size(); ++i)
            {
                const CallMapFunction &f = g_callMapFunctions[i];
                if (f.calls == 0u)
                    continue;
                std::fprintf(out, "F %08x %u\n", g_callMapStarts[i], f.calls);
                for (int arg = 0; arg < 4; ++arg)
                    for (int k = 0; k < 4 && f.vtables[arg][k] != 0u; ++k)
                        std::fprintf(out, "A %08x %d %08x\n", g_callMapStarts[i], arg, f.vtables[arg][k]);
            }
            for (const auto &[key, count] : g_callMapIndirect)
                std::fprintf(out, "I %08x %08x %u\n", static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key), count);
            std::fclose(out);
        }
        g_callMapPath.clear(); // once: the quit tick and the exit handler both get here
    }

    void installCallMap(PS2Runtime &runtime)
    {
        const char *path = std::getenv("TF_CALLMAP");
        if (!path || !*path)
            return;
        FILE *list = std::fopen("work/functions.csv", "r");
        if (!list)
        {
            std::fprintf(stderr, "[tf] TF_CALLMAP: work/functions.csv not found\n");
            return;
        }
        char line[256];
        std::vector<uint32_t> starts;
        while (std::fgets(line, sizeof(line), list))
            if (const char *comma = std::strchr(line, ','); comma && comma[1] == '0')
                starts.push_back(static_cast<uint32_t>(std::strtoul(comma + 1, nullptr, 16)));
        std::fclose(list);
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
        for (uint32_t start : starts)
            if (PS2Runtime::RecompiledFunction original = runtime.lookupFunction(start))
            {
                g_callMapStarts.push_back(start);
                g_callMapOriginal.push_back(original);
            }
        g_callMapFunctions.assign(g_callMapStarts.size(), CallMapFunction{});
        for (uint32_t start : g_callMapStarts)
            runtime.registerFunction(start, callMapCall);
        g_callMapPath = path;
        std::atexit(writeCallMap);
        std::fprintf(stderr, "[tf] TF_CALLMAP: %zu functions wrapped\n", g_callMapStarts.size());
    }

    void applyTransformers(PS2Runtime &runtime)
    {
        // The extensions (port_ext.h) install at six points of this function: Early here, AfterPhotoMode after photo
        // mode's install, BeforeGuestCode and AfterGuestCode around the added code's install, AfterSettings after the
        // core's settings, Late after the HQ animation tween (the last of the core's installs).
        if (!ext::install(ext::Phase::Early, runtime)) runtime.requestStop();
        ultrawide::install(runtime);
        sound_trace::install(runtime); // TF_PITCH_LOG=1 (sound_trace.cpp)
        cam_trace::install(runtime);   // TF_CAM_TRACE=1 (cam_trace.cpp)
        file_trace::install(runtime);  // TF_FILE_LOG=1 (file_trace.cpp)
        sirens_trace::install(runtime); // TF_SIRENS_LOG=<file> (sirens_trace.cpp)
        native_disc::install(runtime);  // TF_NATIVE_DISC=1: TwoFish's disc reads answered on the host (native_disc.cpp)
        native_sound::install(runtime); // TF_NATIVE_SOUND=1: the sound server answered on the EE (native_sound.cpp)
        distance_meter::install(runtime); // the distance meter (distance_meter.cpp)
        boss_cheat::install(runtime); // one-hit kills leave the bosses' fights to play out (boss_cheat.h, TODO 199)
        start_screens::install(runtime);  // boot straight to the main menu (start_screens.h, TODO 110)
        camera_mode::install(runtime);    // first / third person (camera_mode.cpp)
        photo_mode::install(runtime);     // the free camera, F8 (photo_mode.cpp)
        game_config::installHud(runtime); // the HUD setting and photo mode's hidden HUD at once (game_config.h, TODO 228)
        if (!ext::install(ext::Phase::AfterPhotoMode, runtime)) runtime.requestStop(); // (the jet bike: its addresses stay put)
        actor_guard::install(runtime);    // freed actors' child walks left out (actor_guard.h, TODO 51)
        revisit_music::install(runtime);  // a finished Antarctica's cliff music (revisit_music.h, TODO 83: an original PS2 bug)
        pool_growth::install(runtime);    // the game's look-and-sound pools x4 with the 256 MB RAM (pool_growth.h, N7 step 3)
        movement::install(runtime);       // the run on a modern stick (movement.h, TODO 41)
        ragdoll_step::install(runtime);   // the ragdolls stepped at the console's 1/30 s (ragdoll_step.cpp, TODO 182/172)
        test_place::install(runtime);     // TF_PLACE (a testing aid: test_place.h)
        playrec::install(runtime);        // play recordings: the pad read's hook (playrec.h, N12 b)
        playrec::bindForest(&g_forestFit);
        mod_menu::install(runtime);       // the menu keys' pause-screen watch (mod_menu.h)
        native_sound::installState();     // the native sound server's record for save states (after the rest: their addresses stay put)
        if (!ext::install(ext::Phase::BeforeGuestCode, runtime)) runtime.requestStop();
        guest_code::install(runtime);     // added guest code: C++ the game calls as its own (guest_code.h)
        if (!ext::install(ext::Phase::AfterGuestCode, runtime)) runtime.requestStop(); // (the mod host's place: it adds no guest code)
        if (const char *nodes = std::getenv("TF_SOUND_NODES"); !nodes || *nodes != '0')
            sound_nodes::install(runtime); // the sound nodes' frame counts at 30 a second (sound_nodes.cpp)
        g_printFps = std::getenv("TF_FPS") != nullptr;
        if (const char *path = std::getenv("TF_FRAME_LOG"); path && *path)
        {
            g_frameLog = std::fopen(path, "w");
            if (g_frameLog)
                std::fprintf(g_frameLog, "tick,ms,ee_ms,x,y,z,px,py,pz,yaw,t\n");
        }
        parseInputScript();
        parseWalkTargets();
        parsePokes();
        if (const char *god = std::getenv("TF_GOD"))
            g_godMode = *god && *god != '0';
        parseWarps();
        // (the mod host's player_position too: the extensions' installs ran above)
        // (always: besides the testing aids and the mod host's player_position, N9's camera turn for every picture needs the
        // camera object - view_turn.h)
        if ((g_getPosition = runtime.lookupFunction(kGetPosition)) != nullptr)
            runtime.registerFunction(kGetPosition, getPosition);
        parsePeekList();
        parseTestTurns();
        const char *skipFmv = std::getenv("TF_SKIP_FMV");
        g_skipFmv = skipFmv && skipFmv[0] != '\0' && skipFmv[0] != '0';
        // The movie player (fmvStreamPS2, 0x275A00) is built like Sony's sample: it keeps
        // the video stream in a ring of its own, which its stream callback (0x275650) fills
        // from sceMpegDemuxPss and its no-data callback (0x275280) hands to the decoder
        // through the IPU's DMA channel; it starts a movie once the ring is full and reads
        // on whenever there is room. A failed sceMpegGetPicture is its end of the movie once
        // a picture has been shown and the reported size matches the stream's; before that
        // it calls the fatal error handler (0x106620 -> 0x255E90).
        ps2_stubs::setMpegGuestBuffersStream(true);
        // The game is built with GCC: a variadic function saves a2..t3 with sd and walks
        // them 8 bytes at a time. Its string formatter (0x101490 -> vsprintf) read with
        // 4-byte steps got only its first argument right, so every name put together from
        // two parts ("%s%s": the cutscene scripts of a level, among others) ended in
        // "(null)" and those files were never found.
        ps2_stubs::setVarArgSlotBytes(8u);
        // At the end of every frame (0x25BF50) the game reads a counter that its VU1
        // microcode keeps in data memory (vector 0x0B, x), stores it times 8 at 0x6CF068 and
        // clears it. Nothing reads 0x6CF068. With VIF1 on its own thread that read would
        // hold the EE up until the frame's whole display list is through VU1, so the word
        // is declared unobserved: the read gives 0, the clear is queued.
        ps2vif1::setUnobservedWord(0x1100C0B0u);
        // VU1 batches whose routines are known run natively, bit for bit (vu1_native.cpp).
        tf::port::installVu1Native();

        g_frameKick = runtime.lookupFunction(kFrameKick);
        if (g_frameKick)
            runtime.registerFunction(kFrameKick, frameKick);
        g_errorScreen = runtime.lookupFunction(kErrorScreen);
        if (g_errorScreen)
            runtime.registerFunction(kErrorScreen, errorScreen);
        if (std::getenv("TF_SPAWN_TRACE"))
        {
            if ((g_stripEnemies = runtime.lookupFunction(kStripEnemies)) != nullptr)
                runtime.registerFunction(kStripEnemies, stripEnemies);
            if ((g_creatureAICtor = runtime.lookupFunction(kCreatureAICtor)) != nullptr)
                runtime.registerFunction(kCreatureAICtor, creatureAICtor);
        }
        // The decompiled functions (decomp/), in place of the recompiled ones - before the
        // debugging hooks below, which then wrap them like any other.
        decomp::install(runtime);
        menu_mouse::install(runtime); // the mouse in the HQ's screens (menu_mouse.h, TODO 175)
        // The native PC renderer (native_render/), wrapping the render calls (TF_NATIVE_RENDER).
        nr::install(runtime);
        nr::n5::install(runtime); // (N5: the render device native - docs/N5_PLAN.md)
#if defined(_WIN32)
        if (const char *guard = std::getenv("TF_GUARD_FRAME"))
        {
            if (std::sscanf(guard, "%x,%x,%x", &g_guardFrameFunction, &g_guardFrameSize, &g_guardFrameRa) == 3 &&
                (g_guardedFunction = runtime.lookupFunction(g_guardFrameFunction)) != nullptr)
                runtime.registerFunction(g_guardFrameFunction, guardedFrame);
            else
                std::fprintf(stderr, "[tf] TF_GUARD_FRAME: expected <function>,<frame size>,<ra offset> in hex\n");
        }
#endif
        g_createFmvStream = runtime.lookupFunction(kCreateFmvStream);
        g_fmvStreamDtor = runtime.lookupFunction(kFmvStreamDtor);
        if (g_createFmvStream && g_fmvStreamDtor)
        {
            runtime.registerFunction(kCreateFmvStream, createFmvStream);
            runtime.registerFunction(kFmvStreamDtor, destroyFmvStream);
            const char *pace = std::getenv("TF_HQ_MOVIE_PACE");
            if (!(pace && *pace == '0') && (g_fmvStreamUpdate = runtime.lookupFunction(kFmvStreamUpdate)) != nullptr)
                runtime.registerFunction(kFmvStreamUpdate, fmvStreamUpdate);
        }
        installDrawDistance(runtime);
        frustum_native::install(runtime); // mlxFrustum::Test in one native call (frustum_native.h, TODO 94; TF_FRUSTUM_NATIVE=0: off)
        if ((g_sceneNodeDtor = runtime.lookupFunction(kSceneNodeDtor)) != nullptr)
            runtime.registerFunction(kSceneNodeDtor, sceneNodeDtor); // (a child with no vtable: see above)
        installHeapWatch(runtime);
        // No time slice (the console's rule: a thread runs until it waits or a higher priority one
        // is woken). The game's memory manager picks its pools from ONE global, set for a span of
        // code by tdgMemoryAllocationContext (TDG_MemoryManager.cpp, func_00105720/func_001057D0):
        // the runtime's time slice (same-priority threads in turn every 65536 cycles) let another
        // thread allocate inside that span, from the first one's temporary pool. On the warp back
        // to the HQ the seated Optimus's rig was built in a load's 1 MB pool, which the load then
        // freed: the rig's nodes became free-tree blocks, and its destructor called through a zeroed
        // vtable - the warp that never ended (2026-10-23). TF_TIME_SLICE=0 (a test: not the cause after all).
        if (const char *slice = std::getenv("TF_TIME_SLICE"); slice && *slice == '0')
            runtime.eeScheduler().setTimeSliceEnabled(false);
        // A state loaded: the frame rate and its words set before the game runs on.
        runtime.eeScheduler().setStateLoadedHook(
            [&runtime]()
            {
                static R5900Context scratch{}; // (for the FPU's flags of the conversion's arithmetic)
                adoptLoadedState(runtime.memory().getRDRAM(), &scratch, &runtime);
                guest_code::writeAll(runtime.memory().getRDRAM()); // (the added data, whatever the state had)
                native_sound::stateLoaded(runtime.memory().getRDRAM()); // (its engine from the state's record)
                sound_nodes::stateLoaded(); // (the console frames they owe: none, as in a game started with the state)
            });
        // The frame rate (see "The frame rate"): TF_FRAME_RATE=<frames a second>, 60 by default;
        // 30 (or TF_60FPS=0) for the console's. TF_60FPS=force also plays the level of a state
        // made at 30 at it. TF_ADAPTIVE_RATE=0 keeps a rate above 60 when the host falls behind.
        {
            const char *fps60 = std::getenv("TF_60FPS");
            g_frameRate = fps60 && std::strcmp(fps60, "0") == 0 ? 30 : 60;
            g_force60 = fps60 && std::strcmp(fps60, "force") == 0;
            if (const char *rate = std::getenv("TF_FRAME_RATE"); rate && *rate)
                g_frameRate = std::clamp(std::atoi(rate), 30, 480);
            // (TODO 160: the cap - over TF_FRAME_RATE; 240 by default when neither is set; 0 unlimited)
            if (const char *cap = std::getenv("TF_FPS_CAP"); cap && *cap)
                g_frameRate = std::atoi(cap) <= 0 ? kUnlimitedRate : std::clamp(std::atoi(cap), 30, kUnlimitedRate);
            else if (!(std::getenv("TF_FRAME_RATE") && *std::getenv("TF_FRAME_RATE")) && !(fps60 && std::strcmp(fps60, "0") == 0) &&
                     followRates())
                g_frameRate = 240;
            g_rateSetting = g_frameRate;
            // (TODO 161 a: the fixed logic rate - over the cap, which then caps the pictures; not with the console's 30 asked
            // for, nor with the old ladder)
            {
                int logic = 0; // (TODO 165: following the cap - TF_LOGIC_RATE=<rate> fixes it, a test switch)
                if (const char *l = std::getenv("TF_LOGIC_RATE"); l && *l)
                    logic = std::atoi(l) <= 0 ? 0 : std::clamp(std::atoi(l), 30, kUnlimitedRate);
                g_logicRate.store(logic);
                if (logic > 0)
                    g_frameRate = logic;
            }
            if (const char *adapt = std::getenv("TF_ADAPTIVE_RATE"); adapt && *adapt)
                g_adaptiveRate = *adapt != '0';
            if (const char *inBetween = std::getenv("TF_IN_BETWEEN"); inBetween && *inBetween)
                g_inBetween = *inBetween != '0';
            if (const char *drop = std::getenv("TF_IN_BETWEEN_DROP"); drop && *drop)
                g_inBetweenDrop = *drop != '0';
            if (const char *turbo = std::getenv("PS2X_TURBO_VSYNCS"); turbo && std::strtoull(turbo, nullptr, 10) > 0u)
                g_inBetweenDrop = false; // (turbo: every picture drawn, the game as fast as the host goes)
            // A picture each refresh of the screen by default (moments, stage 4: the user's OK on
            // 2026-10-08 after playing at 144 with the native pixel path, the texture cache and 4x
            // multisampling) where the GPU's native pixel path makes them, else locked at 120 pictures
            // a second; TF_FPS_LOCK=120 or 60 locks them (F4 switches), TF_FPS_LOCK=0 or a
            // TF_FRAME_RATE of its own leaves them free (the adaptive rate).
            const char *lock = std::getenv("TF_FPS_LOCK");
            const char *rateSet = std::getenv("TF_FRAME_RATE");
            const char *gpuEnv = std::getenv("PS2X_GPU");
            const char *nativeEnv = std::getenv("PS2X_GPU_NATIVE");
            const bool nativePath = !(gpuEnv && *gpuEnv == '0') && !(nativeEnv && *nativeEnv == '0');
            const bool refresh = (lock && *lock) ? std::strcmp(lock, "refresh") == 0 : !(rateSet && *rateSet) && nativePath;
            const int pictures = refresh ? 120 : (lock && *lock ? std::atoi(lock) : (rateSet && *rateSet ? 0 : 120));
            // (moments: TF_FPS_LOCK=refresh, or TF_MOMENTS=1 whatever the lock - TF_MOMENTS=0 none)
            const char *momentsEnv = std::getenv("TF_MOMENTS");
            // A game frame rate above 60 asked for (TF_FRAME_RATE, the panel's "Game frame rate") wins
            // over the picture lock: a picture a frame, no in-between moments (the game itself is as
            // fast as the pictures or faster). (TODO 217) So does the frame rate cap whenever it decides the rates
            // (followRates: no TF_RATE_LADDER): the cap is THE setting, as in PC games - a picture rate kept in
            // settings.ini from an older version (TF_FPS_LOCK=60 / 120 / refresh, the default "refresh" too) no longer
            // pulls the levels and the HQ down to 60 (a player at a 165 cap stayed at 60).
            const bool fastGame = (followRates() || (rateSet && *rateSet)) && g_frameRate > 60;
            g_moments.store(momentsEnv && *momentsEnv ? *momentsEnv != '0' : (refresh && !fastGame));
            g_momentsShown.store(g_moments.load());
            if (fastGame && !(std::getenv("TF_IN_BETWEEN") && *std::getenv("TF_IN_BETWEEN")))
            {
                // (the in-between pictures would double the VBlanks at the fast rate: 240-480 a
                // second; they come on at half the rate, when the host falls behind)
                g_fastGameFallback = g_frameRate / 2 >= 60;
                g_inBetween = g_fastGameFallback;
            }
            if (fastGame && (pictures == 60 || pictures == 120))
                std::fprintf(stderr, "[tf] %s%d: levels at %d frames, a picture each (the picture lock TF_FPS_LOCK left out)\n",
                             rateSet && *rateSet ? "TF_FRAME_RATE=" : "the frame rate cap ", g_frameRate, g_frameRate);
            else if (g_frameRate > 30 && (pictures == 60 || pictures == 120))
            {
                if (g_frameRate != 60)
                    std::fprintf(stderr, "[tf] TF_FPS_LOCK=%d: levels at 60 frames (not TF_FRAME_RATE's %d)\n", pictures, g_frameRate);
                g_pictureLock = pictures;
                g_frameRate = 60;
            }
            else if (lock && *lock && pictures != 0)
                std::fprintf(stderr, "[tf] TF_FPS_LOCK=%s: 60, 120 or refresh (0: none)\n", lock);
            ps2_host_input::watchKey(kKeyF4);
        }
        // The main loop's executor (executorLoop): at every rate, for fast_load's HQ test; its rate
        // and picture rules only where they were before.
        if ((g_executorLoop = runtime.lookupFunction(kExecutorLoop)) != nullptr)
            runtime.registerFunction(kExecutorLoop, executorLoop);
        if ((g_preGameEnd = runtime.lookupFunction(kPreGameEnd)) != nullptr)
            runtime.registerFunction(kPreGameEnd, preGameEnd);
        // (TF_CAR_LOG at the console's 30: the cars' step wrapped for its log alone - g_levelRate 0 passes it through)
        if (g_frameRate <= 30 && std::getenv("TF_CAR_LOG") && *std::getenv("TF_CAR_LOG") == '1' &&
            (g_carsStep = runtime.lookupFunction(kCarsStep)) != nullptr)
            runtime.registerFunction(kCarsStep, carsStep);
        if (g_frameRate > 30)
        {
            g_gameBegin = runtime.lookupFunction(kGameBegin);
            g_gameEnd = runtime.lookupFunction(kGameEnd);
            if (g_gameBegin && g_gameEnd)
            {
                runtime.registerFunction(kGameBegin, gameBegin);
                runtime.registerFunction(kGameEnd, gameEnd);
                g_hqRate = -1; // (the HQ at the setting's rate, as a level: hq_rate.cpp makes its per-frame steps for it, TODO 56)
                if (const char *hq = std::getenv("TF_HQ_RATE"); hq && *hq)
                    g_hqRate = std::clamp(std::atoi(hq), 0, 240); // (30: the console's 30 with in-between pictures, as before)
                if ((g_airborneUpdate = runtime.lookupFunction(kAirborneUpdate)) != nullptr)
                    runtime.registerFunction(kAirborneUpdate, airborneUpdate);
                if ((g_controllerMove = runtime.lookupFunction(kControllerMove)) != nullptr)
                    runtime.registerFunction(kControllerMove, controllerMove);
                if ((g_abilityUpdate = runtime.lookupFunction(kAbilityUpdate)) != nullptr)
                    runtime.registerFunction(kAbilityUpdate, abilityUpdate);
                if ((g_turnToward = runtime.lookupFunction(kTurnToward)) != nullptr)
                    runtime.registerFunction(kTurnToward, turnToward);
                if ((g_axisAngle = runtime.lookupFunction(kAxisAngle)) != nullptr)
                    runtime.registerFunction(kAxisAngle, axisAngle);
                g_carsStep = runtime.lookupFunction(kCarsStep);
                g_carsPlace = runtime.lookupFunction(kCarsPlace);
                if (g_carsStep && g_carsPlace)
                {
                    runtime.registerFunction(kCarsStep, carsStep);
                    runtime.registerFunction(kCarsPlace, carsPlace);
                    if ((g_physicsStep = runtime.lookupFunction(kPhysicsStep)) != nullptr)
                        runtime.registerFunction(kPhysicsStep, physicsStep); // (TODO 154)
                }
                g_setWorldVelocity = runtime.lookupFunction(kSetWorldVelocity);
                g_speedFromVelocity = runtime.lookupFunction(kSpeedFromVelocity);
                if (g_setWorldVelocity && g_speedFromVelocity)
                {
                    runtime.registerFunction(kSetWorldVelocity, setWorldVelocity);
                    runtime.registerFunction(kSpeedFromVelocity, speedFromVelocity);
                }
            }
            else
            {
                g_frameRate = 30;
            }
        }
        g_levelHooks = g_frameRate > 30;
        if (!g_levelHooks)
            g_rateSetting = 30;
        if (ps2_host_display::widescreenTv())
        {
            if ((g_copyOptions = runtime.lookupFunction(kCopyOptions)) != nullptr)
                runtime.registerFunction(kCopyOptions, copyOptions);
            if ((g_applyProfileOptions = runtime.lookupFunction(kApplyProfileOptions)) != nullptr)
                runtime.registerFunction(kApplyProfileOptions, applyProfileOptions);
        }
        runtime.registerFunction(kSifLoadModuleBuffer, sifLoadModuleBufferFromIop);
        if ((g_setVelocity = runtime.lookupFunction(kSetVelocity)) != nullptr)
            runtime.registerFunction(kSetVelocity, setVelocityAtRate); // (TODO 153)
        if ((g_lerp = runtime.lookupFunction(kLerp)) != nullptr)
            runtime.registerFunction(kLerp, lerpAtRate); // (the rate sweep)
        if ((g_slerp = runtime.lookupFunction(kSlerp)) != nullptr)
            runtime.registerFunction(kSlerp, slerpAtRate);
        if ((g_optionByIndex = runtime.lookupFunction(kOptionByIndex)) != nullptr)
            runtime.registerFunction(kOptionByIndex, optionByIndex); // (TODO 178)
        if ((g_optionsDefaults[0] = runtime.lookupFunction(kOptionsDefaults[0])) != nullptr)
            runtime.registerFunction(kOptionsDefaults[0], optionsDefaults<0>); // (TODO 178)
        if ((g_optionsDefaults[1] = runtime.lookupFunction(kOptionsDefaults[1])) != nullptr)
            runtime.registerFunction(kOptionsDefaults[1], optionsDefaults<1>);
        if ((g_cameraTurn = runtime.lookupFunction(kCameraTurn)) != nullptr)
            runtime.registerFunction(kCameraTurn, cameraTurn); // (mouse look)
        ai_probe::install(runtime); // TF_AI_PROBE (a testing aid: ai_probe.h)
        installCallTracer(runtime);
        installCallCounter(runtime);
        installCoverage(runtime);
        installCallMap(runtime);
        addSettings();
        if (!ext::install(ext::Phase::AfterSettings, runtime)) runtime.requestStop(); // (the Decepticon HQ's first part)
        // The HQ's selected Autobot sits back down at the PC rate (TODO 107; Jenvery, PR #6): a
        // high-rate fade-in's first weight rounds to 0 and the game disabled the incoming idle.
        if (!hq_animation_tween::install(runtime))
            std::fprintf(stderr, "[hq animation tween] not installed\n");
        if (!ext::install(ext::Phase::Late, runtime)) runtime.requestStop(); // (their allocations after every published one)
        // The added code's layout in the save states' layout check (patch 0078): a state made by a build that added
        // other guest code (the public build without the jet bike, another order) is refused instead of loading into the
        // wrong addresses. kStatesLayout is the layout the states made so far have (the full build of 2026-11-08,
        // `mods` 986c30c): that one leaves the check as it was, so they keep loading. A change that only appends guest
        // code at the end may set it to the new layout; one that moves an allocation must not.
        {
            constexpr uint64_t kStatesLayout = 0xFDF7306D65F430FAull;
            const uint64_t layout = guest_code::layoutKey();
            if (layout != kStatesLayout)
            {
                PS2Runtime::setStateLayoutSalt(PS2Runtime::stateLayoutSalt() ^ layout ^ kStatesLayout);
                std::fprintf(stderr, "[guest code] added code layout %016llx (not the full build's %016llx): states of the other layout are refused\n",
                             static_cast<unsigned long long>(layout), static_cast<unsigned long long>(kStatesLayout));
            }
        }
        guest_code::writeAll(runtime.memory().getRDRAM());
    }

    // The PAL release (SLES-52388): only what does not depend on the USA executable's addresses,
    // until each hook's PAL address has been checked (transformers/config/versions/SLES-52388).
    constexpr uint32_t kPalElfCrc32 = 0xA0DBA384u;

    // The generic fatal error (0x106620, the same in both releases): who raised it.
    constexpr uint32_t kGenericFatal = 0x00106620u;
    PS2Runtime::RecompiledFunction g_genericFatal = nullptr;

    void genericFatal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[tf] generic fatal error raised (vsync %llu, ra=%08x, a0=%08x a1=%08x)\n",
                     static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), getRegU32(ctx, 31),
                     getRegU32(ctx, 4), getRegU32(ctx, 5));
#if defined(_WIN32)
        tfLogCallStack("generic fatal");
#endif
        g_genericFatal(rdram, ctx, runtime);
    }

    // The PAL frame hook (the game's frame kick, 0x25D2B0 in both releases): only the scripted test input
    // (TF_INPUT, TF_QUIT_VSYNC) for now; the USA hook's frame-rate, display and HUD work needs PAL addresses.
    void frameKickPal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        if (g_quitTick != 0u && tick >= g_quitTick)
            runtime->requestStop();
        uint8_t sticks = 0u, stickPercent = 100u;
        const uint16_t pressed = scriptedButtons(tick, sticks, stickPercent);
        const bool overriding = pressed != 0u || sticks != 0u;
        const auto axis = [sticks, stickPercent](unsigned low, unsigned high)
        {
            const int reach = 127 * stickPercent / 100;
            return static_cast<uint8_t>((sticks & (1u << low)) ? 0x80 - reach - (stickPercent == 100u ? 1 : 0)
                                                               : ((sticks & (1u << high)) ? 0x80 + reach : 0x80));
        };
        if (overriding)
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~pressed), axis(3, 1), axis(0, 2), axis(7, 5),
                                           axis(4, 6));
        else if (g_padOverridden)
            ps2_stubs::clearPadOverrideState();
        g_padOverridden = overriding;
        g_frameKick(rdram, ctx, runtime);
    }

    void applyTransformersPal(PS2Runtime &runtime)
    {
        parseInputScript(); // (TF_INPUT / TF_QUIT_VSYNC: the presses by VBlank tick)
        if ((g_frameKick = runtime.lookupFunction(kFrameKick)) != nullptr)
            runtime.registerFunction(kFrameKick, frameKickPal);
        ps2_stubs::setMpegGuestBuffersStream(true);
        ps2_stubs::setVarArgSlotBytes(8u); // (GCC's variadic slots: see applyTransformers)
        // (the SDK library is at the same addresses: the PAL analyzer names _sceSifLoadModuleBuffer here too)
        runtime.registerFunction(kSifLoadModuleBuffer, sifLoadModuleBufferFromIop);
        // (the fatal error screen starts at the same address in SLES_523.88: its message logged)
        if ((g_errorScreen = runtime.lookupFunction(kErrorScreen)) != nullptr)
            runtime.registerFunction(kErrorScreen, errorScreen);
        file_trace::install(runtime); // TF_FILE_LOG=1 (flFile::Open is at the same address)
        if ((g_genericFatal = runtime.lookupFunction(kGenericFatal)) != nullptr)
            runtime.registerFunction(kGenericFatal, genericFatal);
        // The sound server and TwoFish's disc reads on the host, as in the USA build. Their functions
        // (0x1F0470, 0x1F0170, 0x1E8B20, 0x192FF0) start at the same addresses in SLES_523.88, and so does
        // their data: with gp = 0x6D30F0 the PAL code reads the commander at 0x6CE8F4 and the EE clock at
        // 0x6CE5B0 / 0x6CB1A4. On the emulated server the second movie (PMH_LOGO) got no sound stream and
        // its player raised the generic fatal error.
        native_disc::install(runtime);
        native_sound::install(runtime);
        native_sound::installState();
        nr::installDisplayOnly(runtime); // (TF_NATIVE_RENDER=1: Direct3D 12 for what needs no game address)
    }
}

namespace tf::port
{
    bool momentPictures()
    {
        return g_moments.load(std::memory_order_relaxed) || g_hqMoments.load(std::memory_order_relaxed);
    }

    float inBetweenFactor() { return momentPictures() ? 0.0f : 0.5f; }

    bool posePictures() { return posePicturesWanted() && nativeOnePicture(); }

    // (TODO 88) The native renderer's pictures are the window's - one a frame - whatever the "Smooth pictures between
    // frames" setting: the menus' pictures between their frames go by this alone (nr_thread.cpp poses::menuWanted).
    bool nativePictures() { return nativeOnePicture(); }

    // (TODO 266) VSync off (the settings' VSync, PS2X_PRESENT_SYNC=0): the presents go at once, so the pictures are not
    // tied to the screen's refresh - the menus' pictures between their frames as many as the PC draws (nr_thread.cpp).
    bool vsyncOff() { return ps2_host_present::sync() == 0; }

    // (TODO 266) The settings' frame rate cap (0: Unlimited), for the menus' pictures with VSync off.
    int frameCap() { return g_rateSetting >= kUnlimitedRate ? 0 : g_rateSetting; }

    float oneFieldFactor() { return posePictures() ? 0.0f : 1.0f; }

    bool moviePlaying() { return g_liveFmvStreams != 0; }

    uint64_t testTickNow() { return static_cast<uint64_t>(g_testTick + 1e-6); }

    bool lateFramesTakeNextVBlank()
    {
        static const bool on = []()
        {
            const char *env = std::getenv("TF_LATE_VBLANK");
            return env && *env && *env != '0';
        }();
        return on && !decomp::consoleRules() && inBetweenNow();
    }

    int fieldOffset() { return decomp::consoleRules() ? 0 : g_fieldOffset; }

    void flipFieldOffset()
    {
        g_fieldOffset ^= 1;
        ps2_host_display::setFieldOffset(g_fieldOffset);
        static uint32_t flips = 0u;
        if ((++flips & 63u) == 1u)
            std::fprintf(stderr, "[tf] late frames started at the next VBlank: %u so far\n", flips);
    }

    uint64_t listsKicked() { return g_listsKicked.load(std::memory_order_acquire); }

    int frameRate()
    {
        return g_frameRate;
    }

    int levelRate()
    {
        return decomp::consoleRules() ? 0 : g_levelRate; // (the checker compares with the console's logic)
    }

    int pictureCap()
    {
        // (TODO 161 a: with the fixed logic rate the cap is the pictures'; following the PC it is the rate's own)
        if (g_logicRate.load(std::memory_order_relaxed) <= 0)
            return 0;
        const int cap = g_rateSetting;
        return cap >= kUnlimitedRate ? 0 : cap;
    }

    int fixedLogicRate() { return g_logicRate.load(std::memory_order_relaxed); }

    bool fullDetail()
    {
        static const bool ps2 = std::getenv("TF_DETAIL_PS2") && *std::getenv("TF_DETAIL_PS2") == '1';
        return !ps2 && !decomp::consoleRules();
    }

    bool inBetweenPictures()
    {
        return decomp::consoleRules() ? false : inBetweenNow();
    }

    int vblankRate()
    {
        return decomp::consoleRules() ? 0 : vblankRateNow();
    }

    void noteGameHalfRate(uint8_t on)
    {
        g_consoleHalfRate = on;
    }

    void vblankWaitBegins(uint32_t count)
    {
        if (g_waitEndCount != 0u)
        {
            noteFrameWork(count - g_waitEndCount - g_skippedSinceWait);
            tfNoteFrameWork(false, g_lastFrameWorkCycles / kEeCyclesPerMs);
        }
    }

    void vblankWaitEnds(uint32_t count)
    {
        g_waitEndCount = count;
        g_skippedSinceWait = 0u;
        tfNoteFrameWork(true, 0.0);
    }

    void idleSkipped(uint32_t cycles)
    {
        g_skippedSinceWait += cycles;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Transformers (USA)", "SLUS_206.68", kEntry, kElfCrc32, applyTransformers);
PS2_REGISTER_GAME_OVERRIDE("Transformers (Europe)", "SLES_523.88", kEntry, kPalElfCrc32, applyTransformersPal);
