#pragma once
// Transformers saves in and out of the port's memory card (the mod menu's Saves tab, F1).
//
// The game keeps one save directory on the card, four save slots inside its main file:
//   NTSC (SLUS-20668): "BASLUS-20668 TRANSFM"      PAL (SLES-52388): "BESLES-52388 TRANSFM"
// each holding a file of the same name (2,904 bytes, FISH "~TA": ~Date, ~Progress, ~Options,
// ~Autobot per slot), icon.sys and Icon.ico. The two releases write the same data: a PAL save
// renamed to the NTSC names loads in this port (tested 2026-10-21: a our earlier recomp PAL save
// continued in the HQ), so converting a region is renaming the directory and its main file.
//
// Read from: a save folder (this port's card, a PS2 folder memory card, a PCSX2 folder memory card),
// .psu (EMS), .max (Action Replay Max, LZARI-compressed), .psv (PS3) and a whole PCSX2 card
// image (.ps2, the standard Sony memory card file system). Written as a folder or a .psu.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace save_io
{
    enum class Region
    {
        Unknown,
        Ntsc,
        Pal,
    };

    struct File
    {
        std::string name;
        std::vector<uint8_t> data;
    };

    struct Save
    {
        std::string directory; // e.g. "BASLUS-20668 TRANSFM"
        std::vector<File> files;
        Region region = Region::Unknown;
    };

    constexpr const char *kNtscDirectory = "BASLUS-20668 TRANSFM";
    constexpr const char *kPalDirectory = "BESLES-52388 TRANSFM";

    // The region of the saves on this port's own memory card: its release's (the game reads only its own).
#if defined(TF_GAME_PAL)
    constexpr Region kCardRegion = Region::Pal;
    constexpr const char *kCardDirectory = kPalDirectory;
#else
    constexpr Region kCardRegion = Region::Ntsc;
    constexpr const char *kCardDirectory = kNtscDirectory;
#endif

    Region regionOf(const std::string &directory);
    const char *regionName(Region region);

    // The Transformers saves in a file or folder (a card image can hold both regions' saves).
    // `error` says why when nothing was found.
    std::vector<Save> load(const std::filesystem::path &path, std::string &error);

    // The same save with the region's directory and main file names.
    Save convert(const Save &save, Region to);

    // Its slots as the Saved Games screen dates them: "2026-10-13 06:42" per used slot.
    std::vector<std::string> slotDates(const Save &save);

    bool writeFolder(const Save &save, const std::filesystem::path &parent, std::string &error); // parent/<directory>/...
    bool writePsu(const Save &save, const std::filesystem::path &file, std::string &error);
}
