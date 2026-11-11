#pragma once

// (TODO 279, release A) F1 > About > "Make a report": one zip a player can send when something goes wrong.
//
// In it: report.txt (the game's version, Windows, processor, memory, graphics cards, where the folders are, the
// names of the crash dumps found), the game's newest logs, the setup's and the updater's logs, settings.ini.
// Never in it: saves, save states, memory dumps, the disc's files or anything made from them.
// It is written to the logs folder (logs\ of the new folder layout, else runs\ in the game folder) as
// report_<date>_<time>.zip.
//
// TF_REPORT_AT=<tick> (tests): the report made at that VBlank, the folder not opened.

#include <filesystem>
#include <string>

namespace support_report
{
    // Makes the zip. `made` gets its path; the text returned is the line for the menu (what was made, or why not).
    std::string make(std::filesystem::path &made);
    // The folder opened in Explorer (the menu's buttons). False when it is not there.
    bool openFolder(const std::filesystem::path &folder);
}
