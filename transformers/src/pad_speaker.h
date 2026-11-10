#pragma once
// The DualSense's own speaker (TODO 213): which of the game's sounds play there, and the sound handed to it.
//
// The native mixer (native_sound_mixer.cpp) mixes a second stereo bus for the pad besides the PC's:
// - "Game alerts" (TF_PAD_SPEAKER=alerts, the default): the Autobot's transformation, enemies noticing you, the
//   low-health beep and the HQ's calls (their beep and their voice message) play on the pad INSTEAD of the PC.
// - "Surround" (=surround): everything on both (TODO 214).
// - Off (=off).
// Only while a pad speaker is live (a transport has said so: setLive); with none, every sound stays on the PC, so
// nothing goes silent. The pad's sound is taken by the transport (take) at 48 kHz stereo.

#include <cstddef>
#include <cstdint>
#include <string>

namespace pad_speaker
{
    enum class Mode : uint8_t
    {
        Off,
        Alerts,
        Surround,
    };

    // Its settings (before the window loop starts: dualsense::addSettings).
    void addSettings();
    Mode mode();
    // The pad's loudness, 0..1 (TF_PAD_SPEAKER_VOLUME).
    float volume();
    // Surround: how long the PC's sound is held back so it plays with the controller's (ms). Over Bluetooth the
    // player's slider (TF_PAD_SPEAKER_DELAY); over USB measured: the pad's latency less the PC's buffer, never below 0.
    // pcBufferMs: the PC stream's target fill. Logged when it changes.
    int surroundDelayMs(int pcBufferMs);

    // A controller's speaker plays the game's sound now (the runtime's DualSense stream: ps2_sony_pad.h speakerLive;
    // or TF_PAD_SPEAKER_TEST). Any thread.
    bool live();

    // The game's sounds that go to the pad in "Game alerts": a voice's tone, a stream's file.
    bool alertTone(const std::string &tone);
    // The HQ call's voice messages: the dialogue streams ("audiocue") opened from the call's answer on - the message,
    // the question after it and the Autobot's reply - until 4 s after the last of them ended (real time).
    // (TODO 268) And the HQ's radio lines the scripts play with no call before them (first-time intros): always.
    void noteToneKeyed(const std::string &tone);
    bool alertStream(const std::string &file);
    void noteAlertStreamEnded();
    // (TODO 257) Once a game frame (mod_menu::frame): a level is paused. A call's message opened then or just after (the
    // pause menu's Show Objective closes the menu and plays it again), or one heard in a call before, goes to the
    // controller's speaker like the call itself.
    void setLevelPaused(bool paused);

    // The mixer, after each mix: the pad's frames (48 kHz stereo, interleaved), to the runtime's speaker stream.
    void push(const int16_t *frames, size_t count);
}
