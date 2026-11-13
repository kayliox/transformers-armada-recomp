// The forest's 3D trees drawn natively (TODO 287, lane F). nr_forest.cpp.
//
// Phase 0 (this file's present state): measure first. TF_NATIVE_FOREST=measure wraps the mesh surfaces' render
// (vlMeshSurface::vfunc_03, 0x1FFC70 - a tree's submit when called from the forest's code: RenderGeometrySet and its
// capture) and the forest's instance drawing (ecoPromotionList::Render, 0x429510 - its runs counted) and still calls
// the originals; every 30 s (TF_NATIVE_FOREST_SECONDS) it logs ("[forest:native]") what native clones could save: the
// trees' submit time per run and per second, submits per run, the callers that submit most, distinct surfaces (the
// would-be templates) and the share of submits whose surface was met before in the window (the clonable ceiling).
// Unset or 0: nothing installed.
//
// Phase 1 (after phase 0's numbers, lane A's review of the renderer hooks): each forest submit pushes one clone Draw at
// its own place in the frame's draw list instead of running the original path (design: docs/TODO.md 287).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace nr::forest
{
    // At start-up (game thread), at the end of the game's overrides (after any other wrapper of the same functions:
    // these chain onto whatever is registered then).
    void install(PS2Runtime &runtime);
    // Whether TF_NATIVE_FOREST asks for anything (measure in phase 0).
    bool enabled();
}
