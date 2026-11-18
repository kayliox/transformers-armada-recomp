// The core's opening and settings (nr_core.h, S2a): the backend chosen and prepared, the settings handed to it. What the
// host decides (which API, the edges' smoothing, the window's lines, how pictures are handed over) comes in here as
// nr_core::Settings; the core calls its backend's own setters - the same calls, in the same order, as the host made
// before (runs\handover_2026-11-15\NR_CORE_SPEC.md 2).
#include "nr_core.h"
#include "nr_backend.h"
#include "nr_pace.h"

#include <cstdio>

namespace nr
{
    void chooseApi(int api); // (nr_backend.cpp: once, before the backend is first used)
}

namespace nr_core
{
    namespace
    {
        WindowEvents g_windowEvents;
    }

    void open(const Settings &settings)
    {
        if (settings.api >= 0)
            nr::chooseApi(settings.api);
        // (S11) What the environment said before (TF_NATIVE_LIVE, TF_PACING), from the host's settings - before the device is
        // prepared (it reads both once); -1 leaves the environment's say.
        if (settings.livePictures >= 0)
            nr::setLivePictures(settings.livePictures);
        if (settings.corePacing >= 0 && !nr::pace::setMode(settings.corePacing))
            std::fprintf(stderr, "[native render] open: Settings::corePacing %d not taken - the pacing was fixed before open()\n",
                         settings.corePacing);
        nr::backend().prepare();
        applySettings(settings);
    }

    void applySettings(const Settings &s)
    {
        nr::Backend &b = nr::backend();
        if (s.antiAliasing >= 0)
            b.setAntiAliasing(s.antiAliasing);
        if (s.supersampling >= 0)
            b.setSupersampling(s.supersampling != 0);
        if (s.speedupsForced >= 0)
            b.setSpeedupsForced(s.speedupsForced);
#if defined(__linux__)
        if (s.windowWidth >= 0 && s.windowHeight >= 0)
            b.setWindowSize(s.windowWidth, s.windowHeight);
#else
        if (s.windowLines >= 0)
            b.setWindowLines(s.windowLines);
#endif
        if (s.sharedPictures >= 0)
            b.setSharedPictures(s.sharedPictures != 0);
    }

    void setWindowEvents(const WindowEvents &events) { g_windowEvents = events; }
    const WindowEvents &windowEvents() { return g_windowEvents; }
}
