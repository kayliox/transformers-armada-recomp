// nr_submit.h - inside the core (nr_core S10): a frame the host records itself, nr_core::submit (nr_core.h), on its way through
// the frame queue (nr_queue.cpp) to the core's drawing thread, which draws it here (nr_submit.cpp).
#pragma once

#include "nr_core.h"

namespace nr
{
    struct Job;

    struct SubmittedFrame
    {
        nr_core::Record record;    // (empty: a task of the core's own - endSubmits' letting go)
        nr_core::FrameInfo info;
        bool release = false;      // endSubmits: the picture target and the swap chain let go of, no frame drawn
    };

    // The drawing thread's part (nr_queue.cpp drawOne): the device started, the frame begun, recorded, ended, presented.
    void drawSubmitted(Job &job);
}
