#pragma once
// The native Sirens sound server (native_sound.cpp, the engine in native_sound_mixer.cpp): every
// command and frame packet the game's sound classes send to the IOP's SIRENS3 is answered on
// the host, the sound is mixed there (voices from the tone libraries, music and dialogue
// streams, movie audio, the reverb), the status buffer the game reads back is written from it,
// and the IOP is parked. On by default; TF_NATIVE_SOUND=0 (the Sound tab's "Sound engine") runs
// the original module on the emulated IOP and SPU2 instead.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>

class PS2Runtime;

namespace native_sound
{
    // From applyTransformers (it adds no guest code).
    void install(PS2Runtime &runtime);
    // A mod's bank in place of the game's: given the tone library the game asks for, the one to load
    // instead, or "" (the Playable Decepticons' vehicle engine banks). Set before the game runs.
    void setLibraryRedirect(std::function<std::string(const std::string &)> redirect);
    // A mod's own banks with each level's: called when a level's bank (.tla) has loaded, it names tone libraries
    // of the sound archive (the mods' copy) to load beside it, looked in after every bank of the game's (the
    // Playable Decepticons' STARSCREAM_SFX while Starscream plays). Set before the game runs.
    void setModLibraries(std::function<std::vector<std::string>()> libraries);
    // From applyTransformers, after everything else that adds guest code (the record's block, so
    // the addresses before it stay put): the engine's setup kept in guest memory, so a save state
    // carries it.
    void installState();
    // After a state was loaded: the engine rebuilt from its record (a state made on the IOP's
    // server, or before the record existed, has none: the sound comes back with the next level).
    void stateLoaded(uint8_t *rdram);
    // True when the native server is in place.
    bool active();
    // The mixer to the caller while the lock is held: the frame's mix (on the mixing thread) done
    // first, and no mix of the thread's own (keeping the output from running dry) meanwhile. For
    // anything that uses the mixer from outside the server (the port's own voices).
    std::unique_lock<std::mutex> lockMixer();
    // The Sound tab's "Sound engine" (TF_NATIVE_SOUND): native (default) or the IOP's.
    void addSettings();
}
