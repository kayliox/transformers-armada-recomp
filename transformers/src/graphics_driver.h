#pragma once
// The graphics card's driver, checked once at the start (TODO 290). Intel's drivers from before 2023 draw this game
// wrong: a player's Iris Xe on 27.20.100.9565 (2021) showed the title menu as blue edges on black and the F1 menu
// garbled over noise (the boxes around its HUD were the texture stand-ins, TODO 290 (14), not the driver). The log says which driver it is, and the F1 menu (Graphics, About) says
// when it is that old, with the advice to update it.
#include <cstdint>
#include <string>

namespace graphics_driver
{
    // The first graphics card's maker (PCI vendor id: 0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel; 0 not known).
    uint32_t vendor();
    // (User decision 2026-11-11) Built-in graphics or a card with under 3.5 GB of video memory (the GTX 1650's 4 GB
    // and up are not): no HD textures are made or streamed on it. TF_LOW_END=1 / 0 says so for tests.
    bool lowEnd();
    // The card in a line for the HD texture tools (hd_textures\.gpu.json: card, its memory, low end or not).
    void writeGpuFile(const std::string &hdDir);
    // The warning for the F1 menu ("" when the driver is not known to be too old).
    const std::string &warning();
    // Logs the card and its driver (once).
    void logOnce();
    // A driver known to be too old: said once (for each driver version) in a Windows message box at the start, before the
    // game draws anything - readable where the driver breaks the game's own picture and its F1 menu.
    void showOnce();
}
