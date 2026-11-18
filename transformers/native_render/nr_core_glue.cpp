// The glue between the renderer's core and the PC layer (nr_core S1): what the core asks its host for (nr_core.h), answered
// from the runtime: guest RAM and the GS upload skip's bookkeeping (nr_texture.cpp), the PNG loader, the prompt style and the
// developer's asset tree (nr_hires.cpp), thread placement (nr_d3d.cpp), the frame builder's hooks (nr_frame.cpp, S6).
// Registered before main by static initialisers.
#include "nr_core.h"
#include "nr_frame.h"
#include "nr_occlusion.h"
#include "nr_pace.h"
#include "flight_recorder.h"
#include "view_turn.h"

#include "dev_tree.h"
#include "key_text.h"

#include "raylib.h"

#include "runtime/ps2_host_display.h"
#include "runtime/ps2_host_input.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    const bool g_hiresHostSet = []
    {
        nr_core::HiresHost host;
        host.loadImage = [](const char *path, uint32_t &width, uint32_t &height, std::vector<uint32_t> &pixels) -> bool
        {
            Image image = LoadImage(path);
            if (!image.data)
                return false;
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            width = static_cast<uint32_t>(image.width);
            height = static_cast<uint32_t>(image.height);
            pixels.resize(static_cast<size_t>(width) * height);
            std::memcpy(pixels.data(), image.data, pixels.size() * 4u);
            UnloadImage(image);
            return true;
        };
        // (TODO 348 (b): in step with key_text's words for the sticks - the symbols switch when they do)
        host.keyboardPromptsShown = []() { return key_text::symbolsAsKeys(ps2_host_input::keyboardPromptsShown()); };
        host.lastPadIsPlayStation = []() { return ps2_host_input::lastPadStyle() == ps2_host_input::PadStyle::PlayStation; };
        host.keyboardPrompts = []() { return ps2_host_input::keyboardPrompts(); };
        host.xbox360Pads = []() { return ps2_host_input::xbox360Pads(); };
        host.applicationDirectory = []() -> const char * { return GetApplicationDirectory(); };
        host.devTree = devTree().string();
        nr_core::setHiresHost(host);
        return true;
    }();
    // (S2b) The pacing log's questions (nr_pacelog.cpp): the screen's refresh from the runtime's VBlank watcher, the view turn's
    // figures (N9), the hand-over's gate holds and cap drops (nr_pace.cpp).
    const bool g_presentHostSet = []
    {
        nr_core::PresentHost host;
        host.screenTiming = [](int64_t &refreshNs, int64_t &periodNs) -> bool
        {
            std::chrono::steady_clock::time_point vb{};
            std::chrono::steady_clock::duration period{};
            if (!ps2_host_display::screenTiming(vb, period))
                return false;
            refreshNs = std::chrono::duration_cast<std::chrono::nanoseconds>(vb.time_since_epoch()).count();
            periodNs = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
            return true;
        };
        host.screenVariable = []() -> bool { return ps2_host_display::screenVariable(); };
        host.appendTurnPacing = [](char *text, size_t size) { view_turn::appendPacing(text, size); };
        host.noteShownPicture = [](uint64_t turnId, int64_t shownNs, int64_t dueNs) { view_turn::noteShownPicture(turnId, shownNs, dueNs); };
        host.takeGateHolds = [](unsigned &pictures, double &heldMs, double &slicesMs)
        {
            const nr::pace::GateHolds gate = nr::pace::takeGateHolds();
            pictures = gate.pictures;
            heldMs = gate.heldMs;
            slicesMs = gate.slicesMs;
        };
        host.takeCapDrops = []() -> unsigned { return nr::pace::takeCapDrops(); };
        nr_core::setPresentHost(host);
        return true;
    }();
    // (S6) The frame builder's hooks (nr_frame.h FrameHost): a movie's band of rows (TODO 255), the flight recorder (TODO 405),
    // the sun's occlusion probe (roadmap N3a, nr_occlusion.cpp), the camera turn of each picture (N9, view_turn.h).
    const bool g_frameHostSet = []
    {
        nr::frame::FrameHost host;
        host.pictureBand = [](float &top, float &bottom) { ps2_host_display::pictureBand(top, bottom); };
        host.recorderOn = []() -> bool { return flight_recorder::enabled(); };
        host.recordFrame = [](uint64_t tick, uint32_t newTextures, uint32_t putOff, uint32_t standIns, uint32_t draws, uint64_t signature)
        { flight_recorder::renderFrame(tick, newTextures, putOff, standIns, draws, signature); };
        host.probeFor = [](uint64_t tick, float rect[4]) -> bool { return nr::occlusion::probeFor(tick, rect); };
        host.deliverProbe = [](uint64_t tick, const float *depth) { nr::occlusion::deliver(tick, depth); };
        host.pictureTurn = [](const view_turn::FrameCamera &fc, int camera, int64_t frameDueNs, int64_t spanNs, int64_t momentNs, float s,
                              double waitMs, int leadSet, nr::frame::PictureTurn &out)
        {
            view_turn::PictureTurn t;
            view_turn::pictureTurn(fc, camera, frameDueNs, spanNs, momentNs, s, waitMs, leadSet, t);
            std::memcpy(out.screen, t.screen, sizeof(out.screen));
            std::memcpy(out.clip, t.clip, sizeof(out.clip));
            out.valid = t.valid;
            out.yawShown = t.yawShown;
            out.sampleNs = t.sampleNs;
        };
        host.notePictureYaw = [](uint64_t id, bool valid, double yaw, int64_t sampleNs) { view_turn::notePictureYaw(id, valid, yaw, sampleNs); };
        nr::frame::setFrameHost(host);
        return true;
    }();
}
