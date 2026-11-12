#pragma once
// The IOP's tripwire (iop_tripwire.cpp): what the game still asks of the IOP, counted - the "emulation out" plan's
// step 1 (S1), a record only, nothing changed. With the native sound server and the native disc the IOP is parked once
// the sound server has started (native_sound.cpp); every call of the game's SIF library to the IOP is counted, before
// and after that, and each one after it is told. TF_IOP_TRIPWIRE=0: not installed.

class PS2Runtime;

namespace iop_tripwire
{
    // From applyTransformers, after the port's own replacements of the SIF library (it wraps what is there).
    void install(PS2Runtime &runtime);
    // The IOP parked (native_sound.cpp): the boot's record - the host's time to here, the IOP's own, the calls so far.
    void noteParked(PS2Runtime &runtime);
    // The session's whole count (main.cpp, once the game has ended: the process leaves by _Exit, no atexit runs).
    void summary();
}
