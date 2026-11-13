#pragma once
// A testing aid (test_place.cpp): TF_PLACE="tick:x,y,z[,heading];..." puts the player there (game
// units) through the game's own SetPosition at the first frame at or after the tick (TF_INPUT's: 60 a second of game time at every frame rate), and,
// with a heading (degrees, 0 = +x, counter-clockwise), turns the view that way. Unlike TF_WARP's
// write into the position, which the game may overwrite, this sticks. A tick written "+N" counts N
// ticks from the placement before it (the first such: from the level's first update), so a script
// moves the player at the same moments of the level whatever the load took. Applied in order.

//
// TF_PD_FORCE_RAGDOLL="tick[:collapse|grenade|rocket|death][,...]" (a testing aid, off when unset): at that tick the
// player falls into the ragdoll (state 10) the way the developers' ragdoll test in the actor update does it
// (debugRagdollMode + its two debug commands), once per listed tick. TF_PD_STATE_LOG=1 (on with the former):
// a "[tf:state]" line each time a transformer actor's state changes, with its tick.
// TF_PD_FORCE_STATE="tick:s<state>|e5|e21,..." (a testing aid, off when unset): at that tick the player goes to that state,
// or takes the flag that starts creature effect 5 (a hit's flash) or 21 (the water's hurt).

class PS2Runtime;

namespace test_place
{
    // From applyTransformers (it adds guest code: after jet_bike's, before the self-test).
    void install(PS2Runtime &runtime);
}
