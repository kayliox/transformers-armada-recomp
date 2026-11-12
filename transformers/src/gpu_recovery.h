#pragma once
// A lost graphics device (TODO 291): the native renderer's Direct3D 12 device removed while playing - a GPU hang the
// driver reset (DXGI_ERROR_DEVICE_HUNG / _RESET, a TDR), or a fault. Until now the native renderer stopped for the rest of
// the session and the emulated one drew the game, slowly. Now the game saves the moment it is at (a save state - the
// game's own memory, the GS's and the IOP's; nothing of the lost device), starts itself again from it with the same
// settings (the settings panel's "Restart now": ps2_settings::restartNow) and closes; the new process makes a new
// device, and the native renderer goes on where the player was, a few seconds later. It then says so for 15 s ("The
// graphics driver restarted. The game recovered."), and after a second time in the same play session suggests lower
// settings. After TF_GPU_RECOVERY_MAX restarts (default 2) in one play session the game no longer restarts: the
// emulated renderer goes on drawing, as before, and the message says so. The count rides through the restarts in
// TF_GPU_RESTARTS. The restarted game turns DRED on (TF_NATIVE_DRED=1: the device's last operations and the faulting
// address are logged if it happens again).
//
// A moment that cannot be saved (a host callback in progress: a movie, a load, the memory card being written) is tried
// again: the save state is asked for again every few seconds for up to 60 s, then the game goes on with the emulated
// renderer.
//
// Switches: TF_GPU_RECOVERY=0 (off: the emulated renderer goes on, as before), TF_GPU_RECOVERY_MAX=<n> (restarts in a
// play session before giving up; 0 = never), TF_GPU_RESTARTS=<n> (set by the game for the process it starts).
// A test: TF_NATIVE_REMOVEDEVICE=<vblank> removes the device for real at that tick (nr_d3d.cpp); the restarted game
// keeps the variable and its tick has passed, so it removes the device again at once - the whole chain of restarts and
// the final fallback tried in one run.

#include <string>

class PS2Runtime;

namespace gpu_recovery
{
    // Once a frame on the window thread (mod_menu::draw).
    void frame(PS2Runtime &runtime);
    // The message for the player ("" none) and how many seconds ago it was set (shown for 15 s).
    std::string message(double &ageSeconds);
}
