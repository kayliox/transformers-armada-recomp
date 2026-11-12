#pragma once
// (TODO 290 (17)) "I have HD textures (PCSX2 pack)": the player's own PCSX2 texture pack, chosen in F1 with Windows' folder
// picker and used where it is (TF_HD_PACK; nothing copied). Its images are found through the shipped name table
// (tools\hd_textures\pcsx2_names.json: PCSX2's image names -> our texture names, no image data) and drawn by the native
// renderer in place of an upscaled set (nr_hires.cpp). A folder of our own layout (an hd_textures\upscaled tree copied from
// another PC) is taken too (TF_HIRES_DIR). The only way to HD textures on built-in graphics (graphics_driver::lowEnd).
#include <string>

namespace hd_pack
{
    // The folder picker (on a thread of its own; the menu keeps drawing), then the folder checked and, when it is a pack,
    // chosen for the next start.
    void choose();
    // What the last choice found, for the menu ("" none yet).
    std::string status();
    // The pack in use now (TF_HD_PACK), "" none; and forgetting it.
    std::string current();
    void forget();
    // TF_HD_PACK and TF_HIRES_DIR as settings (no row of their own): a setting the settings file knows is what
    // ps2_settings::noteChanged writes, so a choice made here is there at the next start. Before the window loop.
    void addSettings();
}
