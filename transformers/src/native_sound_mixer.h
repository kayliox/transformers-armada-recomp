#pragma once
// The native sound server's engine (native_sound_mixer.cpp): what SIRENS3 does with the game's
// commands, done on the host - voices playing the tone libraries' samples, file and movie
// streams, the reverb - mixed at 48 kHz stereo into the port's audio stream.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace native_sound::mixer
{
    // Where the disc is (the .MS files, SOUNDE.ZIP). Once, before anything else.
    void setDisc(const std::filesystem::path &cdRoot);
    // The disc the files are read from from now on (the mods' copy, once it is set up); nothing reloaded.
    void useDisc(const std::filesystem::path &cdRoot);

    // ---- tone libraries --------------------------------------------------------------
    int32_t createLibrary();                             // the handle command 23 returns
    void destroyLibrary(int32_t handle);
    bool loadLibrary(int32_t handle, const std::string &path); // the zip mounted by command 40
    // A library of the port's own, looked in last of all (empty path: none): the full bank of a
    // level revisited with its trimmed one (native_sound.cpp). In save states.
    void setFallbackLibrary(const std::string &path);
    // A mod's library (the mounted archive), looked in after every library of the game's and the fallback; loaded
    // again when asked again (each level). False when it is not there.
    bool loadModLibrary(const std::string &path);
    void mountArchive(const std::string &archive);       // command 40 op 0

    // ---- voices (EE handles) -----------------------------------------------------------
    void createVoice(int32_t handle);
    void destroyVoice(int32_t handle);
    void setTone(int32_t handle, const std::string &name);
    // Tag 0x22: SPU2 15-bit volume words, the pitch factor, the priority, the reverb send and
    // the dry level.
    void updateVoice(int32_t handle, uint16_t volLeft, uint16_t volRight, float pitch, int32_t priority, float send, float dry);
    void play(int32_t handle);
    void stop(int32_t handle);
    void rewind(int32_t handle);
    void queuePlay(int32_t handle);  // a key-on held for the packet's channel assignment
    void assignChannels();           // the held key-ons, the most important first
    void endPacket();                // (TODO 290 (10)) a frame packet's end: an engine loop keyed off at a transformation fades
    void forgetPlay(int32_t handle); // a held key-on dropped (the voice stopped or destroyed)
    void silenceVoices();
    // (TODO 369) Photo mode: while held, no voice is mixed or moves on - each keeps its place in its data, its envelope and
    // its loop - and the streams (the music) play on. Any thread.
    void holdVoices(bool held);
    void setReservedChannels(int channels);

    // ---- streams ---------------------------------------------------------------------
    int32_t createStream(bool pcm, bool eeFed);
    void destroyStream(int32_t handle);
    void openStream(int32_t handle, const std::string &name);
    void playStream(int32_t handle);
    void stopStream(int32_t handle);
    void closeStream(int32_t handle);
    void switchTune(int32_t handle, bool immediately, const std::string &tune);
    void setTunePart(int32_t handle, int32_t part);
    void setStreamLoudness(int32_t handle, uint32_t loudness);
    void setStreamChannelLoudness(int32_t handle, uint32_t channel, uint32_t left, uint32_t right);
    void setStreamPitch(int32_t handle, float pitch, float loudness);
    void setStreamLooping(int32_t handle, bool looping);
    void flushStream(int32_t handle);
    // Movie audio: the EE's 8 KB chunk (SendStreamData's transfer), and how many chunks the
    // stream has consumed since the last call (the credits the server grants back).
    void feedStream(int32_t handle, const uint8_t *data, uint32_t size);
    int takeConsumedChunks(int32_t handle);

    // ---- effects ---------------------------------------------------------------------
    void setReverbPreset(const std::string &name);
    void setReverbLevel(float level);
    void setReverbDelay(float delay);
    void setReverbFeedback(float feedback);

    // ---- what the status reports ---------------------------------------------------------
    std::vector<int32_t> playingVoices();               // sorted
    std::vector<std::pair<int32_t, int32_t>> streams(); // handle, next tune-part index (-1: finished)

    // ---- the port's own sounds (not the game's: the jet bike's engine) ----------------------
    // A looping tone (or an instrument) of a library, played at `gain` (0..1, panned by `pan`, -1
    // left .. 1 right) and `pitch` (a factor), started when it is not playing; called every frame
    // it plays. The library is read from `folder` (a mod's PORT folder: files the port reads
    // itself, kept off the game's disc so its layout and save states stay as they are), or from
    // the disc's sound archive when `folder` is empty. Its voice is not in the playing list the
    // game reads. False when the library or the tone is not there (once: not tried again).
    bool hostLoop(int id, const std::filesystem::path &folder, const std::string &library, const std::string &tone, float gain, float pitch,
                  float pan = 0.0f);
    void hostStop(int id); // its release runs

    // ---- save states ---------------------------------------------------------------------
    // What the server holds that the game relies on, as text: the mounted archive, the tone
    // libraries by handle, the voices with their tones and last update, the streams with what
    // they play (file or album, tune, part), the reverb, the reserved channels. A state keeps it
    // in guest memory (native_sound.cpp); loadSetup rebuilds the engine from it (voices silent,
    // streams from the start of their part: what was playing at the moment is not kept).
    std::string saveSetup();
    void loadSetup(const std::string &text);

    // Mixes up to the game's time `seconds` (the EE clock) and hands the frames to the port's
    // audio stream.
    void render(double seconds);
    // Mixes `frames` more beyond the game's time and hands them over: the output about to run
    // dry while the game is behind real time (a hitch, a frame rate the host does not hold). The
    // time a hitch costs the game is lost for good (the EE scheduler does not catch it up), so the
    // game's later mixes are not shortened by them. Not before the first render.
    void renderAhead(size_t frames);
    void reset();
}
