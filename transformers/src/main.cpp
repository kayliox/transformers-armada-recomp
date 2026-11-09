// Transformers (PS2, SLUS-20668) static recompilation entry point.
//
// The game data is never shipped with this project. tools/extract_iso.py
// unpacks the user's own disc image into a folder (by default `game/` in the
// repository), and this launcher boots SLUS_206.68 from there.

#include "ThreadNaming.h"
#include "ps2_runtime.h"
#include "raylib.h"
#include "runtime/ps2_audio_stream.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_sony_pad.h"

#include "port_ext.h"
#include "hd_movies.h"
#include "hd_progress.h"
#include "mod_menu.h"
#include "nr_install.h"
#include "menu_mouse.h"
#include "playrec.h"
#include "start_screens.h"
#include "save_io.h"
#include "ultrawide.h"
#include "discord_presence.h"
#include "updates.h"
#include "self_log.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

extern PS2Runtime *g_tfRuntime; // profiler.cpp
#if defined(_WIN32)
void tfShowWindowWithoutFocus(void *window, int x, int y); // profiler.cpp
#endif
void tfNoteWindow(void *window); // profiler.cpp (TF_TEST_WINDOW)
void tfKeepFullSpeedInBackground(); // profiler.cpp (TODO 197: no Windows throttling with the window left)

namespace
{
    // The game started straight from a launcher that does not open it in its own folder (Steam's "Add a non-Steam
    // game" under Proton, a shortcut to the exe; a player's report): the game folder - the one above the exe that
    // holds settings.example.ini - becomes the current folder, and settings.ini is made on the first start, as
    // Play.bat does. Started from Play.bat (or with the game folder current) nothing changes.
    void enterGameFolder()
    {
        std::error_code e;
        auto isGameFolder = [&](const fs::path &d)
        { return fs::exists(d / "settings.example.ini", e) || fs::exists(d / "overlay" / "settings.example.ini", e); };
        if (!isGameFolder(fs::current_path(e)))
        {
            for (fs::path d = fs::path(GetApplicationDirectory()); !d.empty(); d = d.parent_path())
            {
                if (isGameFolder(d))
                {
                    fs::current_path(d, e);
                    break;
                }
                if (d == d.parent_path())
                    break;
            }
        }
        const fs::path here = fs::current_path(e);
        if (!fs::exists(here / "settings.ini", e))
            for (const fs::path &example : {here / "overlay" / "settings.example.ini", here / "settings.example.ini"})
                if (fs::exists(example, e) && fs::copy_file(example, here / "settings.ini", e))
                    break;
    }

    // TF_MONITOR=<n>: open the window centred on monitor n (0 = primary; full screen there
    // too) without taking keyboard focus. The test scripts use it to keep runs off the main
    // screen.
    int requestedMonitor()
    {
        const char *env = std::getenv("TF_MONITOR");
        return (env && *env) ? std::atoi(env) : -1;
    }

    void showWindowOnMonitor(int monitor)
    {
        if (monitor >= GetMonitorCount())
            monitor = 0;
        const Vector2 origin = GetMonitorPosition(monitor);
        const int x = static_cast<int>(origin.x) + (GetMonitorWidth(monitor) - GetScreenWidth()) / 2;
        const int y = static_cast<int>(origin.y) + (GetMonitorHeight(monitor) - GetScreenHeight()) / 2;
#if defined(_WIN32)
        tfShowWindowWithoutFocus(GetWindowHandle(), x, y);
#else
        SetWindowPosition(x, y);
        ClearWindowState(FLAG_WINDOW_HIDDEN);
#endif
    }

    // A normal start: a window two thirds of the screen's height, in its middle (the
    // runtime's 640x480 is a stamp on a 4K screen), 4:3 or for a widescreen TV
    // (PS2X_WIDESCREEN) 16:9. PS2X_WINDOW and full screen are left alone.
    void sizeWindowForScreen()
    {
        if (std::getenv("PS2X_WINDOW") || IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
            return;
        const int monitor = GetCurrentMonitor();
        const int height = std::max(480, GetMonitorHeight(monitor) * 2 / 3);
        const int width = ps2_host_display::widescreenTv() ? height * 16 / 9 : height * 4 / 3;
        if (height <= GetScreenHeight())
            return;
        SetWindowSize(width, height);
        const Vector2 origin = GetMonitorPosition(monitor);
        SetWindowPosition(static_cast<int>(origin.x) + (GetMonitorWidth(monitor) - width) / 2,
                          static_cast<int>(origin.y) + (GetMonitorHeight(monitor) - height) / 2);
    }

    constexpr const char *kElfName = "SLUS_206.68";
    constexpr uint32_t kElfCrc32 = 0x74C982C0u;

    uint32_t crc32File(const fs::path &path)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }

        std::ifstream in(path, std::ios::binary);
        std::vector<char> buf(1 << 16);
        uint32_t crc = 0xFFFFFFFFu;
        while (in.read(buf.data(), buf.size()) || in.gcount() > 0)
        {
            for (std::streamsize i = 0; i < in.gcount(); ++i)
                crc = table[(crc ^ static_cast<uint8_t>(buf[i])) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    // Accepts the ELF itself or the folder that holds the extracted disc.
    fs::path elfFromCandidate(const fs::path &candidate)
    {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec))
            return candidate;
        const fs::path inside = candidate / kElfName;
        if (fs::is_regular_file(inside, ec))
            return inside;
        return {};
    }

    fs::path findGameElf(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
            return elfFromCandidate(argv[1]);

        if (const char *env = std::getenv("TRANSFORMERS_GAME_DIR"))
            return elfFromCandidate(env);

        // Look for a `game` folder next to the executable or in any parent
        // (the build tree lives inside the repository, which holds `game/`).
        std::error_code ec;
        fs::path dir = fs::absolute(argv[0], ec).parent_path();
        for (int depth = 0; depth < 6 && !dir.empty(); ++depth)
        {
            if (fs::path elf = elfFromCandidate(dir / "game"); !elf.empty())
                return elf;
            if (dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        return {};
    }

    // (TODO 167) A state loaded at start (PS2X_LOAD_STATE - a start-up state, a settings restart's) lands in a level
    // at once. The native renderer's Direct3D 12 is made on its own thread from the start (nr_install.h), and until it
    // is ready the frames are the emulated GS's: in a level those ran at ~20 fps, the sun's visibility test waiting
    // for the GS's read-back each frame (nr_occlusion.cpp answers it only while the native renderer draws alone) -
    // ~20 s of it on the test PC. So such a state is loaded once the native renderer draws (the wait the play
    // recordings have, TODO 150), its drawing alone from the first frame. A load at a later tick
    // (PS2X_LOAD_STATE_TICK) comes after a boot, by which time it is ready.
    void waitForRendererBeforeStartState()
    {
        const char *state = std::getenv("PS2X_LOAD_STATE");
        const char *tick = std::getenv("PS2X_LOAD_STATE_TICK");
        if (!state || !*state || (tick && std::strtoull(tick, nullptr, 10) != 0u) || !nr::rendererStarting())
            return;
        const auto t0 = std::chrono::steady_clock::now();
        while (nr::rendererStarting() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool ready = !nr::rendererStarting();
        if (ready && ps2_host_display::emulatedDrawing())
            ps2_host_display::setEmulatedDrawing(false);
        std::fprintf(stderr, "[states] the start-up state loaded once the native renderer %s (%.1f s waited)\n",
                     ready ? "was ready: it draws alone from the first frame" : "had not got ready after 60 s: the GS draws until it is",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

#if defined(__linux__)
    // The emulated renderer's GPU path needs GL_ARB_fragment_shader_interlock (gs_gpu.cpp); without it the GS draws on
    // the CPU. Of Mesa's GL drivers only iris and Zink have it (docs/features.txt): AMD's radeonsi (the Steam Deck) and
    // virgl do not, while Zink over a Vulkan driver with VK_EXT_fragment_shader_interlock (RADV, Venus) does. A hidden
    // window in a child process asks, before this process makes any GL.
    int glProbe()
    {
        SetTraceLogLevel(LOG_NONE);
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        InitWindow(16, 16, "gl probe");
        const bool interlock = IsWindowReady() && glfwExtensionSupported("GL_ARB_fragment_shader_interlock");
        CloseWindow();
        return interlock ? 0 : 1;
    }

    bool glHasInterlock()
    {
        char self[] = "/proc/self/exe", flag[] = "--gl-probe";
        char *args[] = {self, flag, nullptr};
        pid_t pid = 0;
        int status = 0;
        return posix_spawn(&pid, self, nullptr, nullptr, args, environ) == 0 && waitpid(pid, &status, 0) == pid &&
               WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    // Zink when the default driver lacks the interlock and Zink has it. TF_ZINK=0: never, =1: always; a
    // MESA_LOADER_DRIVER_OVERRIDE already set wins.
    void chooseGlDriver()
    {
        const char *force = std::getenv("TF_ZINK");
        if (std::getenv("MESA_LOADER_DRIVER_OVERRIDE") || (force && *force == '0'))
            return;
        const bool forced = force && *force == '1';
        if (!forced && glHasInterlock())
            return;
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        if (!forced && !glHasInterlock())
        {
            unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
            std::fprintf(stderr, "[gl] no OpenGL driver here has GL_ARB_fragment_shader_interlock: the GS draws on the CPU\n");
            return;
        }
        // Zink's command reordering garbles the F1 menu (ImGui through raylib's batches: stripes, wrong triangles;
        // seen with Mesa 26.1 on Venus and on lavapipe); a ZINK_DEBUG already set wins.
        setenv("ZINK_DEBUG", "noreorder", 0);
        std::fprintf(stderr, "[gl] OpenGL through Zink (%s): the GPU renderer's pixel interlock (TF_ZINK=0: the default driver)\n",
                     forced ? "TF_ZINK=1" : "the default driver lacks the interlock");
    }
#endif
}

int main(int argc, char *argv[])
{
#if defined(__linux__)
    if (argc > 1 && std::string(argv[1]) == "--gl-probe") // (chooseGlDriver's child)
        return glProbe();
#endif
    // build.ps1, after linking: the native renderer's shaders compiled into the table the next link builds in
    // (nr_install.h). Nothing else runs: no game folder, window or device.
    if (argc >= 3 && std::strcmp(argv[1], "--bake-shaders") == 0)
        return nr::bakeShaders(argv[2]);
    // (TODO 186) The game's own log, runs\play_<date>.err / .out, when its output goes nowhere else (an installed
    // copy's Play.bat): first, so that everything below is in it.
    enterGameFolder(); // (first: the log, settings.ini and the game's files are all found from the game folder)
    self_log::start();
    tfKeepFullSpeedInBackground(); // (TODO 197: movies and the clock at full speed with the game alt-tabbed)
    // The runtime logs a lot from the game thread. Unbuffered, every token is a
    // separate write to the console or file, which cost most of the frame time;
    // buffer both streams and flush them from a background thread instead, so
    // the logs are still (nearly) complete if the game is killed.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    std::setvbuf(stderr, nullptr, _IOFBF, 1 << 16);
    std::thread([]()
                {
                    ThreadNaming::SetCurrentThreadName("StdioFlush");
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        std::fflush(stdout);
                        std::fflush(stderr);
                    } })
        .detach();

    std::set_terminate([]()
                       {
                           std::cerr << "[terminate] unhandled exception" << std::endl;
                           std::abort(); });

    // The environment the game was started with (before settings.ini fills it): what the updater and
    // a start again get (updates.h, TODO 104).
    updates::noteLaunchEnvironment();
    // A play recording's replay (TF_PLAY_REPLAY, playrec.h): its recorded settings into the environment, settings.ini
    // left out, hidden, silent, turbo.
    playrec::prepareReplay();
    // The settings panel's choices (settings.ini, F5 while playing) into the environment before
    // anything reads it: a variable already set wins (ps2_settings.h).
    ps2_settings::loadFile();
    // Respect saved display choices; a first launch from the couch starts full screen.
    if (const char *mode = std::getenv("RECOMPLAUNCHER_MODE"); mode && std::string(mode) == "bigpicture" &&
        !std::getenv("PS2X_FULLSCREEN"))
    {
#if defined(_WIN32)
        _putenv_s("PS2X_FULLSCREEN", "1");
#else
        setenv("PS2X_FULLSCREEN", "1", 0);
#endif
    }
    // (TODO 219) An HD set still being made: the sign that the game runs (the upscale waits), the window that carries on
    // with it when nothing does, and the watcher that tells the renderer and the F1 menu (hd_progress.h).
    hd_progress::start();
#if defined(__linux__)
    chooseGlDriver(); // (after settings.ini: TF_ZINK there counts; before any GL)
#endif
    // (TODO 163) The renderer the settings choose by default: the native one (N1, the port's own - Emulated stays a
    // choice). Set before anything reads TF_RENDERER (the scale below, the native renderer's install, VU1's); a
    // settings.ini line or a variable already set wins.
#if defined(_WIN32)
    if (!std::getenv("TF_RENDERER"))
    {
        _putenv_s("TF_RENDERER", "native");
    }
#elif defined(TF_NATIVE_RENDERER)
    if (!std::getenv("TF_RENDERER")) // (Linux with vkd3d: the native renderer as on Windows)
        setenv("TF_RENDERER", "native", 1);
#else
    // Linux has no native renderer in this build (nr_none.cpp): always the emulated one, whatever settings.ini says.
    setenv("TF_RENDERER", "emulated", 1);
#endif
    // A scripted test run's start screens said in the environment, so a play recording keeps them (start_screens.h).
    start_screens::noteScriptedRun();
    // Play recording on (TF_PLAY_RECORDING / TF_PLAY_RECORD): VIF1 on the game's thread for the session (playrec.h).
    playrec::prepareRecord();

    // (N7) Main RAM: 256 MB (the most the EE's address map allows), the game's memory manager sized to it. TF_RAM_MB=32
    // gives the game the PS2's 32 MB again with the same build (the runtime's PS2X_GUEST_RAM_MB: its heap ceiling, the
    // port's added data and the game's probe as on the console) - an A/B and a fallback.
    if (const char *ramMb = std::getenv("TF_RAM_MB"); ramMb && *ramMb && !std::getenv("PS2X_GUEST_RAM_MB"))
    {
#if defined(_WIN32)
        _putenv_s("PS2X_GUEST_RAM_MB", ramMb);
#else
        setenv("PS2X_GUEST_RAM_MB", ramMb, 1);
#endif
    }

    // (TODO 117) With the native renderer the emulated GS renderer draws only the menus and the movies (the native one
    // draws the HQ and the levels), and what it draws is shown in a window of the screen's size: its Resolution
    // (PS2X_GPU_SCALE) above the screen's own (Auto: 3 on a 1440-line screen, 5 on 4K) costs the graphics card
    // without a sharper picture - at 8 with 8x multisampling the menus waited for the graphics card every frame, and
    // with another program in front taking the card the menus fell from 30 frames to 11-13 and the game with them (it
    // runs a step a frame): 43% speed, measured on the test PC and the other PC. At the screen's size they held 30.
    // A Resolution of 1-4 is kept; above it, Auto. TF_NATIVE_GS_SCALE=keep: the setting as it is (tests).
    {
        const char *renderer = std::getenv("TF_RENDERER");
        const char *scale = std::getenv("PS2X_GPU_SCALE");
        const char *keep = std::getenv("TF_NATIVE_GS_SCALE");
        if (renderer && std::string(renderer) == "native" && !std::getenv("TF_NATIVE_RENDER") && scale && *scale &&
            std::atoi(scale) > 4 && !(keep && std::string(keep) == "keep"))
        {
            std::fprintf(stderr, "[tf] the menus and movies (the emulated renderer, with the native one) drawn at the screen's size "
                                 "(Auto), not %sx: the graphics card's work for them a fraction (TODO 117)\n", scale);
#if defined(_WIN32)
            _putenv_s("PS2X_GPU_SCALE", "auto");
#else
            setenv("PS2X_GPU_SCALE", "auto", 1);
#endif
        }
    }

    // The port plays 16:9: the game's own widescreen mode, as for a widescreen TV
    // (PS2X_WIDESCREEN; =0 plays 4:3 as the console's default TV setting has it).
    if (!std::getenv("PS2X_WIDESCREEN"))
    {
#if defined(_WIN32)
        _putenv_s("PS2X_WIDESCREEN", "1");
#else
        setenv("PS2X_WIDESCREEN", "1", 1);
#endif
    }

    // TF_SAVE_TEST=<file>;<file>...: read each save (save_io.h), print it, write it as a .psu and
    // read that back, then exit (checks the Saves tab's formats without the game).
    if (const char *test = std::getenv("TF_SAVE_TEST"))
    {
        std::string list = test;
        size_t start = 0;
        while (start <= list.size())
        {
            const size_t end = std::min(list.find(';', start), list.size());
            const fs::path path = list.substr(start, end - start);
            start = end + 1;
            if (path.empty())
                continue;
            std::string error;
            const auto saves = save_io::load(path, error);
            std::printf("[save test] %s: %zu save(s)%s%s\n", path.string().c_str(), saves.size(), error.empty() ? "" : " - ",
                        error.c_str());
            for (const auto &save : saves)
            {
                std::printf("[save test]   %s (%s), %zu files:", save.directory.c_str(), save_io::regionName(save.region),
                            save.files.size());
                for (const auto &f : save.files)
                    std::printf(" %s=%zu", f.name.c_str(), f.data.size());
                std::printf("\n");
                for (const auto &date : save_io::slotDates(save))
                    std::printf("[save test]     slot %s\n", date.c_str());
                const fs::path psu = fs::temp_directory_path() / "tf_save_test.psu";
                std::string e2;
                save_io::writePsu(save_io::convert(save, save_io::Region::Pal), psu, e2);
                const auto back = save_io::load(psu, e2);
                bool same = back.size() == 1 && back[0].files.size() == save.files.size() && back[0].region == save_io::Region::Pal;
                for (size_t i = 0; same && i < save.files.size(); ++i)
                    same = back[0].files[i].data == save.files[i].data;
                std::printf("[save test]     PAL .psu round trip: %s\n", same ? "identical" : "DIFFERS");
            }
        }
        std::fflush(stdout);
        return 0;
    }

    try
    {
        const fs::path elfPath = findGameElf(argc, argv);
        if (elfPath.empty())
        {
            std::cerr << "Could not find " << kElfName << ".\n"
                      << "Extract your Transformers (USA) disc first:\n"
                      << "  python transformers/tools/extract_iso.py \"Transformers (USA).iso\" game\n"
                      << "then run this program again, or pass the game folder as the first argument." << std::endl;
            return 1;
        }

        const uint32_t crc = crc32File(elfPath);
        if (crc != kElfCrc32)
        {
            std::cerr << "Warning: " << elfPath.string() << " has CRC32 " << std::hex << crc
                      << ", expected " << kElfCrc32 << std::dec
                      << ". This build only supports Transformers (USA, SLUS-20668)." << std::endl;
        }

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI)
        // The runtime's debugger: hidden until Ctrl+F1 (TF_DEBUG_UI=1 shows it from the start). F1
        // is the mod menu (mod_menu.h), drawn with the same ImGui.
        PS2DebugPanel debugPanel;
        debugPanel.setVisible(std::getenv("TF_DEBUG_UI") != nullptr);
        // TF_NO_OVERLAY=1 (a test): no ImGui at all - no mod menu, no debugger, no HD movies over the picture.
        if (const char *noOverlay = std::getenv("TF_NO_OVERLAY"); !(noOverlay && *noOverlay == '1'))
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &, void *userData)
            { static_cast<PS2DebugPanel *>(userData)->initialize(); },
            [](PS2Runtime &rt, void *userData)
            {
                auto *panel = static_cast<PS2DebugPanel *>(userData);
                ultrawide::noteWindow(GetScreenWidth(), GetScreenHeight()); // (TF_ASPECT=screen)
                hd_movies::draw(rt); // (an upscaled movie over the picture: hd_movies.h)
                mod_menu::draw(rt);
                playrec::draw(); // (the REC mark and its messages: playrec.h)
                panel->draw(rt);
                // The panels need the cursor: the game only gets the mouse while both are hidden.
                menu_mouse::windowFrame(GetScreenWidth(), GetScreenHeight()); // (the pause menu's cursor: menu_mouse.h)
                ps2_host_input::setMouseCaptureAllowed(!panel->isVisible() && !mod_menu::isOpen() && !menu_mouse::wantsCursor());
            },
            [](PS2Runtime &, void *userData)
            { static_cast<PS2DebugPanel *>(userData)->shutdown(); },
            &debugPanel);
#endif

        const int monitor = requestedMonitor();
        if (monitor >= 0)
        {
            SetConfigFlags(FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED); // shown once placed
            // The runtime puts the window there as soon as it exists (PS2X_MONITOR), so that
            // full screen and the GPU's resolution go by that monitor, not the primary.
            const std::string index = std::to_string(monitor);
#if defined(_WIN32)
            _putenv_s("PS2X_MONITOR", index.c_str());
#else
            setenv("PS2X_MONITOR", index.c_str(), 1);
#endif
        }
        if (!runtime.initialize("Transformers"))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }
        // PS2X_WINDOW_HIDDEN=1 (test machines): the window stays hidden whatever TF_MONITOR says -
        // showing it here put test runs on the screen of the PC they ran on (2026-10-26).
        const char *hidden = std::getenv("PS2X_WINDOW_HIDDEN");
        if (hidden && *hidden == '1')
        {
        }
        else if (monitor >= 0)
            showWindowOnMonitor(monitor);
        else
            sizeWindowForScreen();

#if defined(_WIN32)
        g_tfRuntime = &runtime;
#endif
        tfNoteWindow(GetWindowHandle()); // (the window's state for TODO 117: tfWindowAway, TF_TEST_WINDOW)
        if (!runtime.loadELF(elfPath.string()))
        {
            std::cerr << "Failed to load " << elfPath.string() << std::endl;
            return 1;
        }
        // TF_MC_ROOT=<folder>: memory card 1 lives there instead of next to the ELF (test
        // runs use a scratch card, so they neither depend on nor change real saves).
        if (const char *mcRoot = std::getenv("TF_MC_ROOT"); mcRoot && *mcRoot)
        {
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.mcRoot = fs::absolute(mcRoot).lexically_normal();
            PS2Runtime::setIoPaths(paths);
        }
        // TF_HOST_ROOT=<folder>: what the game sees as host0: (a development kit's PC).
        // The sound driver reads its tracing options from host0:./SIRENS3.INI.
        if (const char *hostRoot = std::getenv("TF_HOST_ROOT"); hostRoot && *hostRoot)
        {
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.hostRoot = fs::absolute(hostRoot).lexically_normal();
            PS2Runtime::setIoPaths(paths);
        }

        // An extension's disc (port_ext.h; the mod loader's: a modded copy built from the Mods folder).
        // The executable and the memory card stay in game\.
        if (const fs::path disc = ext::prepareDisc(elfPath.parent_path()); !disc.empty())
        {
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.cdRoot = disc;
            PS2Runtime::setIoPaths(paths);
            std::cout << "[mods] the disc is read from " << disc.string() << std::endl;
        }
        playrec::noteMods(ext::loadoutTags()); // (the extensions' changes, port_ext.h: kept in a recording, compared in its replay)
        playrec::beforeRun();                   // (a recording from the boot: the memory card copied in)
        waitForRendererBeforeStartState();      // (TODO 167: a start-up state's first frames not the GS's)

        // An update from the F1 menu left the moment it was made at (updates.h): loaded before the
        // game's first instruction, when this build can take it.
        updates::beginResume(runtime);

        runtime.run();
        playrec::finish("the game ended");
        // (a resume the runtime refused stopped it: the game once more, normally - TODO 104)
        updates::afterRun(runtime);

        ps2x::sonypad::shutdown();    // a DualSense's motors off, its LEDs back to the pad (ps2_sony_pad.h)
        discord_presence::shutdown(); // the Discord status cleared (discord_presence.h)
        ps2_audio_stream::shutdown(); // completes a PS2X_AUDIO_DUMP file
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
    std::_Exit(1);
}
