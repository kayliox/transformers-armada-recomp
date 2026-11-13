#pragma once
// (TODO 339) The old Ultra preset's forest moved to the new Ultra's, once (2026-11-11's move, made one-time).
// ultra_migrate.cpp.

namespace ultra_migrate
{
    // Before settings.ini is read (it rewrites the file): a file without the line TF_ULTRA_MIGRATE_DONE=1 is looked at
    // once. If it holds everything the old Ultra preset wrote for the distances - Draw distance, scenery, forest, grass
    // and ground at 4x, far trees 3D to the edge, 8x smooth edges (with "Trees: full 3D" on or not) - its forest goes to
    // the new Ultra's: Auto (2x), far trees Auto, full 3D off (the forest and far-tree lines taken out, full 3D 0).
    // Either way the line TF_ULTRA_MIGRATE_DONE=1 is added, and a file with it is never changed again: values the player
    // sets by hand afterwards stay (wayneng1997: Forest 4x and far trees 3D to the edge went back to Auto at every start).
    // Only the file's values count: a row the launch's environment sets otherwise (a test run) leaves the file alone.
    // No settings file: nothing (the first start that has one marks it). TF_ULTRA_MIGRATE=0: never.
    void run();
}
