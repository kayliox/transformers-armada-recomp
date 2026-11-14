// A small crosshair (small_crosshair.h).
//
// The game's crosshair (taModelPredictionCrosshair, its Update 0x2FF730) is a model in the world: a
// big ring, plus an add-on shape for each Mini-Con weapon equipped (its sub-models at +0xA0 and
// +0xA4, chosen by the Mini-Con from the table at +0x3C, shown with bit 2 of their node flags). +0x14
// says whether it is shown at all. The add-on of the first is coloured with the colour the update
// builds from the floats at gp-0x700C, -0x7020, -0x7008 (0-255; gp = 0x6D30F0), or with the in-range
// colour (gp-0x6FF4 red, gp-0x70B8 green and blue, 0-1) while the targeting system's +0x2D0 is set
// (+0x2D2 in the view 6, the first-person one): the targeting system is [[the player +0x54] +0x7C],
// the player [[[[crosshair +0x10] +0x60] +0x24] +8], the view manager [[crosshair +0x10] +0x60] +0x28.
//
// Here the model is moved out of the picture (photo_mode's hook of vlSceneNode::SetPosition) and a
// small cross is drawn where it was (photo_mode::project of its world point), outlined in the add-on's
// colour while a Mini-Con weapon is equipped, in the in-range colour when it can fire.
//
// Decomp equivalent: the model not drawn and the cross drawn at the crosshair's place by DoRender,
// with the same colours and states.

#include "small_crosshair.h"

#include "camera_mode.h"
#include "crosshair_row.h" // (TODO 358: the crosshair's size)
#include "distance_meter.h"
#include "photo_mode.h"
#include "static_aim.h" // (TODO 322: the crosshair pinned)
#include "port_ext.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_display.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace small_crosshair
{
    namespace
    {
        std::atomic<bool> g_enabled{true};

        constexpr uint32_t kAddOnA = 0xA0u, kAddOnB = 0xA4u, kShown = 0x14u;
        constexpr uint32_t kAddOnRed = 0x006CC0E4u, kAddOnGreen = 0x006CC0D0u, kAddOnBlue = 0x006CC0E8u; // gp-0x700C, -0x7020, -0x7008
        constexpr uint32_t kInRangeRed = 0x006CC0FCu, kInRangeGreenBlue = 0x006CC038u;                   // gp-0x6FF4, -0x70B8

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (a >= 0x00100000u && a < PS2_RAM_SIZE)
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
            return v;
        }

        uint8_t byteAt(const uint8_t *rdram, uint32_t a)
        {
            const uint8_t *p = (a >= 0x00100000u && a < PS2_RAM_SIZE) ? getConstMemPtr(rdram, a) : nullptr;
            return p ? *p : 0u;
        }

        float floatAt(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t w = word(rdram, a);
            float f;
            std::memcpy(&f, &w, sizeof(f));
            return std::isfinite(f) ? f : 0.0f;
        }

        unsigned char channel(float v) { return static_cast<unsigned char>(std::clamp(v, 0.0f, 255.0f)); }

        // A bar of the cross: filled, with an outline around it.
        void bar(float x, float y, float w, float h, float outline, Color fill, Color edge)
        {
            DrawRectangleRec(Rectangle{x - outline, y - outline, w + 2.0f * outline, h + 2.0f * outline}, edge);
            DrawRectangleRec(Rectangle{x, y, w, h}, fill);
        }
    }

    void addSettings()
    {
        if (const char *env = std::getenv("TF_SMALL_CROSSHAIR"); env && *env)
            g_enabled.store(*env != '0');
        ps2_settings::Setting s;
        s.key = "TF_SMALL_CROSSHAIR";
        s.section = "Display";
        s.label = "Crosshair";
        s.order = 60;
        s.help = "Small: a small cross where the game aims, outlined in the Mini-Con weapon's colour while one is "
                 "equipped (its in-range colour when it can fire). Original: the game's big crosshair.";
        s.choices = {{"1", "Small"}, {"0", "Original"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_enabled.store(value != "0");
            photo_mode::setHideCrosshair(value != "0");
            return true;
        };
        ps2_settings::add(s);
        photo_mode::setHideCrosshair(g_enabled.load());
    }

    bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

    // TF_XHAIR_LOG=1 (tests): where the cross is, once a second, as a share of the window (TODO 292 B: TF_XHAIR_LOG=<ms> that
    // often instead) - and (TODO 322) where the game's crosshair model is in this frame's camera, the original crosshair's
    // place; with the original crosshair the small cross's is -1.
    void logPlaces(float x, float y)
    {
        static const double every = std::getenv("TF_XHAIR_LOG") ? (std::atof(std::getenv("TF_XHAIR_LOG")) > 1.0 ? std::atof(std::getenv("TF_XHAIR_LOG")) / 1000.0 : 1.0) : -1.0;
        if (every <= 0.0)
            return;
        static double last = 0.0;
        if (GetTime() - last < every)
            return;
        last = GetTime();
        const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
        float mx = -w, my = -h, mp[3];
        if (photo_mode::crosshair(mp))
            photo_mode::project(mp, w, h, ps2_host_display::displayAspect(), mx, my);
        std::fprintf(stderr, "[crosshair] at %.4f, %.4f of the window; the model at %.4f, %.4f\n", x / w, y / h, mx / w, my / h);
    }

    void draw(PS2Runtime &runtime)
    {
        if (!enabled() && !photo_mode::active() && !camera_mode::inVehicle())
            logPlaces(-static_cast<float>(GetScreenWidth()), -static_cast<float>(GetScreenHeight())); // (TODO 322, tests: the original crosshair's place)
        if (!enabled() || photo_mode::active() || camera_mode::inVehicle() || distance_meter::menuOpen(runtime)) // (none in the car or a menu)
            return;
        const uint32_t crosshair = distance_meter::crosshairObject(runtime);
        if (!crosshair)
            return;
        const uint8_t *rdram = runtime.memory().getRDRAM();
        if (!byteAt(rdram, crosshair + kShown))
            return;
        if (ext::hideCrosshair(rdram)) // (an extension's, port_ext.h: TODO 92, none on the jet bike)
            return;
        float point[3], x = 0.0f, y = 0.0f, share = 0.0f;
        if (static_aim::pinned(share))
        {
            // (TODO 322) Mouse and keyboard: one point of the window, never a projection - the picture's middle across, the
            // still crosshair's height up (the share of half the picture's height above its middle, where photo_mode puts the
            // game's model on the camera's ray). The same point every picture.
            const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
            const float aspect = ps2_host_display::displayAspect();
            if (!(aspect > 0.0f))
                return;
            const float picW = std::min(w, h * aspect), picH = picW / aspect;
            x = w * 0.5f;
            y = h * 0.5f - share * picH * 0.5f;
        }
        else if (!photo_mode::crosshair(point) ||
                 !photo_mode::project(point, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()),
                                      ps2_host_display::displayAspect(), x, y))
            return;
        logPlaces(x, y);

        // The Mini-Con weapon's state: equipped (an add-on), in range.
        const bool addOn = word(rdram, crosshair + kAddOnA) != 0u || word(rdram, crosshair + kAddOnB) != 0u;
        bool inRange = false;
        if (addOn)
        {
            const uint32_t view = word(rdram, word(rdram, crosshair + 0x10u) + 0x60u);
            const uint32_t player = word(rdram, word(rdram, view + 0x24u) + 8u);
            const uint32_t targeting = word(rdram, word(rdram, player + 0x54u) + 0x7Cu);
            const uint32_t manager = word(rdram, view + 0x28u);
            const uint32_t viewIndex = manager ? word(rdram, manager + 0x8Cu) : 0u;
            if (targeting)
                inRange = byteAt(rdram, targeting + 0x2D0u) != 0u || (viewIndex == 6u && byteAt(rdram, targeting + 0x2D2u) != 0u);
        }
        static const bool forceRange = std::getenv("TF_XHAIR_IN_RANGE") != nullptr; // (tests: the in-range colour)
        inRange = inRange || (addOn && forceRange);
        Color edge{0, 0, 0, 200};
        if (addOn && inRange)
            edge = Color{channel(floatAt(rdram, kInRangeRed) * 255.0f), channel(floatAt(rdram, kInRangeGreenBlue) * 255.0f),
                         channel(floatAt(rdram, kInRangeGreenBlue) * 255.0f), 255};
        else if (addOn)
            edge = Color{channel(floatAt(rdram, kAddOnRed)), channel(floatAt(rdram, kAddOnGreen)), channel(floatAt(rdram, kAddOnBlue)), 255};

        // The cross: four short bars around a gap, and a dot. (TODO 358) Its bars and gap times the Crosshair size row's
        // share (1, the default: as before), the thickness by its square root, each kept to a readable floor.
        const float h = static_cast<float>(GetScreenHeight());
        const float k = crosshair_row::size();
        float length = std::max(6.0f, h / 95.0f), thick = std::max(2.0f, h / 450.0f), gap = std::max(3.0f, h / 220.0f);
        if (k < 1.0f)
        {
            length = std::max(3.0f, length * k);
            thick = std::max(1.5f, thick * std::sqrt(k));
            gap = std::max(2.0f, gap * k);
        }
        const float outline = addOn ? std::max(1.5f, thick * 0.8f) : std::max(1.0f, thick * 0.5f);
        const Color fill{240, 246, 255, 235};
        bar(x - thick * 0.5f, y - gap - length, thick, length, outline, fill, edge); // up
        bar(x - thick * 0.5f, y + gap, thick, length, outline, fill, edge);          // down
        bar(x - gap - length, y - thick * 0.5f, length, thick, outline, fill, edge); // left
        bar(x + gap, y - thick * 0.5f, length, thick, outline, fill, edge);          // right
        bar(x - thick * 0.5f, y - thick * 0.5f, thick, thick, outline * 0.6f, fill, edge);
    }
}
