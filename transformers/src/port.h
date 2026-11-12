#pragma once
// What the PC port's own state the game hooks (overrides.cpp) and the decompiled code
// (decomp/) share.

#include <cstdint>

class PS2Runtime;

namespace tf::port
{
    // The frame rate the PC port runs levels at: TF_FRAME_RATE, 60 by default (120, 144, ...).
    // 30 is the console's rate, frames of two fields (docs/NOTES.md, "Frame rates").
    int frameRate();
    // The rate the level that runs now runs at, a frame and a VBlank every 1/R s - frameRate()
    // from the level's begin to its end - or 0: the menus and the intro, a level at the
    // console's 30, and the checker's run of the console's logic (decomp::consoleRules).
    int levelRate();
    // (TODO 290) The creature effects' update that runs now (0x533010) steps a frame of the console's 30 a second
    // (overrides.cpp effectsUpdate): its frame time is that 30th of a second (FramePeriod).
    bool effectsAtConsoleStep();
    // The governor's detail scale pinned at 1 (TODO 160: a console's load balancer; TF_DETAIL_PS2=1 leaves it to the load).
    bool fullDetail();
    // In-between pictures (TF_IN_BETWEEN, on by default): the level that runs now draws each
    // frame as the console does at 30 - two fields, the second with the moving parts of the
    // scene halfway between the frame before and this one - and both are shown, a VBlank every
    // 1/(2R) s. Not while a movie plays: its pictures are timed in the console's VBlanks.
    bool inBetweenPictures();
    // The in-between field's moment (the governor's blend factor, 0x6CD6F0): 0.5, halfway from the
    // frame before to this one, as the console draws it - or 0 with moment pictures (TF_MOMENTS=1,
    // docs/RENDERER.md stage 4): the second field is then the frame before's pose, and any moment
    // between the two is the GPU's to make.
    float inBetweenFactor();
    bool momentPictures();
    // (N9 stage 2) The in-between factor while a frame draws one field (the governor's 1.0 on the console: the second
    // pass at this frame's pose): 0 while the native renderer makes pictures between frames (nr_d3d.cpp poses) - the
    // second pass then carries the frame before's pose, beside field A's - else 1.
    float oneFieldFactor();
    // (N9 stage 2) The native renderer makes pictures between the game's frames (the setting, TF_POSES).
    bool posePictures();
    // A movie plays (the game's FMV streams: the intro, the episodes' films, the extras' movies).
    bool moviePlaying();
    // The testing aids' tick (TF_INPUT, TF_GOTO, TF_PLACE): the console's VBlanks, 60 a second of game time at every
    // frame rate, as of the frame's input (overrides.cpp testTick).
    uint64_t testTickNow();
    // (TODO 292) The mouse turns the view now: from its first travel taken until the pad's right stick moves (overrides.cpp).
    bool mouseTurnsView();
    // Display lists kicked so far (0x25D2B0): VIF1 and VU1 work on the newest one's batches until
    // the next kick, which waits for them - so it numbers the frame a batch belongs to.
    uint64_t listsKicked();
    // The VBlanks a second of the level that runs now: levelRate(), twice that with in-between
    // pictures (0 as levelRate()).
    int vblankRate();
    // The game set its half-rate byte (0x32E8A0): what it asked for, to give back when the
    // level ends.
    void noteGameHalfRate(uint8_t on);
    // Late frames with in-between pictures (TF_LATE_VBLANK=1, a test while it is checked): a frame
    // of two fields has to start on its field, so a frame late by a little waited a whole frame
    // more. With this a frame already two VBlanks old starts at the next VBlank, on the other
    // field: the port's field offset flips (the game's own field is left as it is) and the
    // pictures are shown by it (ps2_host_display::setFieldOffset), the frame's earlier field
    // still first.
    bool lateFramesTakeNextVBlank();
    int fieldOffset();
    void flipFieldOffset();

    // The frame statistics of TF_FPS: a wait for the VBlank begins or ends (EE cycle count);
    // cycles the EE clock jumped over in the main loop's wait (not the game's work).
    void vblankWaitBegins(uint32_t count);
    void vblankWaitEnds(uint32_t count);
    void idleSkipped(uint32_t cycles);
    // (TODO 291) A frame of the main loop (0x295310) begins: the game's frames of TF_FPS's 1% low and of TF_HITCH_MS
    // are timed from one to the next here, not by the display list's kick - the kick runs on a thread of its own,
    // most frames at the next VBlank but now and then straight after the frame's drawing, which paired a 3 ms
    // "frame" with a 30 ms one while the frames and the pictures were even.
    void frameBegins(uint8_t *rdram, PS2Runtime *runtime);

    // The game's VU1 routines done natively (vu1_native.cpp, docs/RENDERER.md).
    void installVu1Native();
}
