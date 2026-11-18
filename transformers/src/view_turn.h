#pragma once
// N9's camera turn for every picture (docs/TODO.md, N9 "The camera turn for every picture"; TODO 95). The game turns
// its camera once a frame from the mouse travel and the stick it read at the frame's start; a picture is shown a
// steady delay after its moment, and with pictures between frames (or a game rate below the screen's) several pictures
// show one frame. Here each picture gets the turn the game will make by the time it is shown: the mouse travel since the
// frame's read (exact - the next frame takes the same travel) and the stick's turn rate times the time since it (only
// while the stick is still held), as a turn of the view about the camera's target - the third-person camera orbits the
// player, so the player stays where he is on the screen - applied to the scene's 3D matrices by the GPU (nr_d3d.cpp's
// `turn`/`turnClip`). The game's logic keeps its own camera; nothing here writes the game's state.
//
// Every picture's turn is read at its moment plus a steady lead (the earliest a picture can be submitted, measured), so
// the turn advances evenly from picture to picture whatever the threads' times. TF_TURN=0 leaves it off.
#include <cstdint>
#include <memory>

namespace view_turn
{
    // The turn for every picture (TF_TURN, default on).
    bool enabled();
    // (TF_TURN_PICTURES, default on) With "Smooth pictures between frames" off (TF_POSES=0) and the game's rate at half the
    // screen's refresh or below (a heavy scene, a weaker PC, TF_FRAME_RATE under the screen's), a picture is still made for
    // each refresh of the frame - this frame's pose, only the camera's turn newer - so turning keeps the screen's rate.
    bool turnPicturesWanted();

    // ---- the game's thread ---------------------------------------------------------------------------------------
    // In place of ps2_host_input::takeMouseTravel (overrides.cpp): the travel since the last take, the take's moment and
    // the travel so far kept for the pictures (a thread reads the window's mouse every 1 ms meanwhile).
    bool takeMouseTravel(float &pixelsX, float &pixelsY);
    // (TODO 430) Photo mode's free camera: the mouse travel (pixels, after the sensitivity) since its last call, apart from the
    // game's takes; `reset` drops what came before (photo mode just turned on). False: the mouse is not the game's (not captured).
    bool takeFreeLookTravel(float &pixelsX, float &pixelsY, bool reset);
    // (TF_TURN_TEST, a measuring aid) A mouse moving at that many pixels a second (after the sensitivity) from now on, as
    // if the hand moved it - overrides.cpp sets it by game time; 0, 0 stops it.
    void setTestMouse(float pixelsPerSecondX, float pixelsPerSecondY);
    bool testMouseOn();
    // The third-person camera turned (0x46D410, its control in `control`): its angles after the turn, the mouse's part
    // of it (radians) and the frame's time step (s).
    void noteTurned(const uint8_t *rdram, uint32_t control, float mouseYaw, float mousePitch, float frameSeconds);
    // (TODO 396) The game's own field of view (degrees, as the camera sets it, before the port's changes), noted each time the
    // view's camera sets it (photo_mode's hook). aimNarrow(): the aiming view's zoomed steps and a scope - the field at or
    // below TF_AIM_FOV_NARROW (55 degrees; the steps measure 69.7, about 40.8 and under 29). The fix of the aiming view's stutter
    // is that the pictures' turn scales the mouse travel by the aiming zoom as the game's own frame does (noteMouseZoom); the
    // switches: TF_AIM_TURN=0 no turn for every picture at the narrow view, =1 as before the fix; TF_AIM_SMOOTH=1 (with
    // TF_AIM_TURN=0 only) the travel smoothed - measured, no gain, so off.
    void noteGameFov(float degrees);
    bool aimNarrow();
    // The aiming view's mouse zoom factor (the game's +0x20: 1, 0.57, 0.33) the frame's mouse travel was scaled by: the pictures' turn
    // has to scale the travel since the frame by the same, or it carries the view 1.75 / 3 times too far.
    void noteMouseZoom(float zoom);
    // (TODO 396) F1 > Gameplay > Scope sensitivity (TF_SCOPE_SENSITIVITY, percent, 100 = the game's): the mouse's speed in the
    // aiming view (R3's zoom steps and the sniper Mini-Con's scope) as a share of what it is; 1 outside the aiming view.
    // addSettings: the row, before the window loop starts.
    float aimSensitivity();
    void addSettings();
    // The frame's mouse travel (pixels) made even in time at the narrow aiming view: first-order, a time constant of
    // TF_AIM_SMOOTH_MS (4 ms of added delay on the mean); nothing is lost (what is owed comes out), `dtSeconds` the time
    // since the last take. Called at each take; resets itself when the view is not narrow.
    void smoothAimTravel(float &pixelsX, float &pixelsY, float dtSeconds);
    // The camera object and its transform (the listener update's 0x1AFD40 call, overrides.cpp).
    void noteCameraObject(uint32_t camera, uint32_t transform);
    // (TODO 292 A) The view's camera node as the camera manager places it (photo_mode's SetPosition hook): its transform
    // (+0x14) stands in where the sound listener's camera is not seen (Antarctica).
    void noteViewNode(uint32_t node);
    // A model-to-screen matrix sent (0x221DB0): the camera matrices it was made with (scratchpad 0x6A0 camera to screen,
    // 0x6E0 camera to the clipping space, 0xEC0 world to camera). The index of them for the draws made now. (TODO 343) The
    // matrix is the caller's copy the next draws are made with, and may be adjusted: the pinned crosshair's put on the
    // camera's axis.
    void noteMatrixSent(const uint8_t *rdram, float *sent);
    uint8_t cameraIndex();

    struct FrameCamera;
    // The frame's record (its draws done; nr_capture.cpp's endFrame), handed to the renderer with the frame.
    std::shared_ptr<const FrameCamera> endFrame(const uint8_t *rdram, uint64_t tick);

    // ---- the renderer -------------------------------------------------------------------------------------------
    // The camera index most of the scene's draws were made with (-1: none, no turn) - the turned draws.
    bool turnable(const FrameCamera &fc);
    // (TODO 101 b) The frame's camera jumped from the frame before's (a cut, a respawn, a warp): its known camera's angles or
    // place moved more than a frame's turn or walk can make.
    bool cameraCut(const FrameCamera &fc);
    // A picture's turn: `momentNs` its moment, `s` where its pose lies between the frame before's (0) and this frame's
    // (1), `camera` the frame's scene camera index. Sampled at the moment plus the lead (waiting up to `waitMs` for it).
    // `screen` and `clip`: the turn in GS screen space and in the clipping matrix's (row vectors), identity when none.
    // Returns the yaw the picture shows (the game's camera's, radians: the pacing log's turn steps).
    struct PictureTurn
    {
        float screen[16];
        float clip[16];
        bool any = false;       // not the identity
        bool valid = false;     // the shown yaw known
        double yawShown = 0.0;  // (radians, unwrapped: the pacing log's steps)
        int64_t sampleNs = 0;   // (the moment of the input it shows: the turn's reading, else the game's own)
    };
    void pictureTurn(const FrameCamera &fc, int camera, int64_t frameDueNs, int64_t spanNs, int64_t momentNs, float s, double waitMs, int leadSet,
                     PictureTurn &out);

    // The lead now (ms after a picture's moment that its turn is read at; -1 none yet): set 0 or 1 (pictureTurn's).
    double leadMs(int set);
    // The pacing log's turn steps (nr_present.cpp): picture `id`'s shown yaw recorded / looked up.
    void notePictureYaw(uint64_t id, bool valid, double yaw, int64_t sampleNs);
    bool pictureYaw(uint64_t id, double &yaw, int64_t &sampleNs);
    // The 2 s pacing line's addition: how evenly the shown view turned picture to picture (nr_present.cpp).
    void noteShownPicture(uint64_t id, int64_t shownNs, int64_t dueNs);
    void appendPacing(char *text, size_t size);
}
