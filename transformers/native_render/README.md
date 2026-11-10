# The native PC renderer

Direct3D 12 in place of the emulated PS2 graphics hardware (the VIF1 unpacker, the VU1 vector unit
and the GS). The end state is a PC game with no PS2 hardware emulation and no PS2-specific blocks:
data the game keeps in PS2 form is converted once into PC form, and functions are rewritten
wherever that works better on a PC. This code is our own, written against the engine's own classes
and interfaces (their names and addresses are the game's).

## Where the game draws (the engine's platform layer)

- `vlRenderDevicePS2` (vtable 0x6B4450) implements `vlRenderDeviceBase`: frame begin/end (slots 3/4),
  render geometry set (25), 2D primitives (26-29), state (44, 46, 47), state blocks (38/39), transforms
  (52/53), viewport, clear, textures, render targets (73).
- Geometry sets (`vlGeometrySetPS2`) hold their vertices as a prebuilt VIF packet, also in the level
  files: positions (float or compressed 16-bit), RGBA5551 colours, float UVs, in strips of up to a
  VU1 batch, with drawing-kick masks. The engine's vertex iterator decodes the layout.
- Render state is GS register values (TEST, ALPHA, TEX0, CLAMP, FRAME, ZBUF...), written through one
  function that keeps a shadow (0x225D60); state blocks are ready-made register packets.
- A "shader" is a chain of VU1 microcode plus a constant block; what each routine computes is in
  `docs/RENDERER.md` and runs natively in `src/vu1_native.cpp` (the spec for the HLSL).
- Textures (`vlTexturePS2`) keep their texels and CLUTs in main memory; uploads to GS memory are
  DMA packets.

## Stages (each leaves the game working)

1. Capture + offscreen picture: the geometry sets, with the exact matrix sent to VU1, drawn by D3D12
   into a picture saved on request, beside the emulated one (`TF_NATIVE_RENDER=1`,
   `TF_NATIVE_SHOT=<VBlank>`, with `PS2X_SHOT_VSYNCS` at the same tick). **Done**: every frame is
   drawn (~3.7 ms for ~1,100 draws, GPU sync included); the picture is the display circuit's
   buffer, the whole frame composed as the GS composes it (3D field, its weave copy into the
   display buffer, glare, HUD).
2. The strips' drawing-kick masks, textures (CLUTs, formats), vertex colours, the shader combos
   (constant, lit, vertex colour), render state (blend, alpha test, depth, fog). **Done** except
   TEX1 (filtering, mip choice); top-byte formats (T8H/T4HL/T4HH), CLAMP, FBMSK, STQ. The depth
   fog: the game shuffles bits 8-15 of each pixel's Z24 into the top byte of its depth buffer's
   pages (the back display buffer's) and reads that through a palette in a full-field pass; here
   that pass takes the index from the target's own depth (`TF_NATIVE_FOG=0` off).
   GS-resident images (the light maps in the display buffers' top bytes, uploaded once from a
   freed buffer) are read from GS memory as a stopgap until loaded from the level's files.
3. Skinned (envelope) and tweened meshes, sprites, instances, 2D primitives and the HUD,
   render-to-texture (glare, blur, reflections), water, particles, grass. **Mostly done**: sprites,
   instances, grass, 2D and HUD (with the widescreen narrowing the PS2 path's front end does),
   render targets with their double buffering and SCANMSK field weave, the water. Left: skinned/tweened
   check, particles, the ground's lighting (the grass, ground and rocks about a fifth
   brighter than the emulated at Amazon's start; sky, models and HUD match; the blades are the
   same, 16,980 a field).
   The forest's sprite sets (2026-10-25): the game rewrites each tree's sprite size in the set's
   packet every frame (a speck where the tree is drawn on its own), so the sets' sprites are read
   again on the renderer's thread every frame - the giant dark palm at Amazon's start was a stale
   full-size sprite. The emulated path does not show these sets at all: the runtime's VIF unpacks
   V4-5 colours as raw 5-bit/1-bit values (alpha 1, the alpha test drops every sprite), where the
   hardware, and PCSX2 (its GS dumps: these sprites at 0x68-0x78 grey, alpha 0x80), shifts them
   to 8 bits; the native renderer draws them as the hardware does. A 24-bit frame (CT24, Z24)
   keeps its alpha: no draw into one writes the alpha channel.
   The shadow textures (2026-10-25): the frame's list is taken from the start of the packet
   buffer BeginFrame swaps in (it waits for the frame before's DMA, so the recompiled call
   returns before the swap: the pointer read after it was the old buffer's end, and the frame's
   first stretch - the clear and the blob shadows - was lost); the microcode primitives' sprites
   (`vlMicrocodePrimitive`, the blob shadows on routine 0x2cf0) are captured from their vertex
   calls with the list's GS state; the flat strips (0x3cc0, the player's shadow) take the screen
   their block writes (SCISSOR_1 / XYOFFSET_1); and a T8H read of pages a target of the frame
   drew (the shadow receivers) samples that target's green through the palette - what the game's
   16-bit copy moves into the top byte - in place of GS memory.
   The sprite routine and the primitives' strips (2026-10-26): the microcode primitives' sprites
   on 0x22a0/0x2740 are drawn (`TF_NATIVE_PARTICLES=0` off) - the single-tree billboards (ALPHA
   0x64, FIX fading by distance; the emulated path draws them since patch 0038) and the glows
   (0x48). A billboard samples the material of the geometry set drawn with the same TEX0: the
   trees' sprite textures reach GS memory by a path the upload map does not see, which still named
   what was there before (Deep Amazon's hillsides: temple walls, glyph tiles). Type-0 batches on the
   sprite routine are sprites too (the size their w as it is; Antarctica's snow, which as a mesh
   covered the screen), and the routine's path for q15.xy zero (traced on a captured batch) is the
   same sprite from the centre to the centre plus q18 * size * Q. The primitives' strips (begin
   0x220530, quads 0x220A10, end 0x220350) write their batches straight into the display list and
   were never drawn - the warp gates' cones, the dropships' glow, the lasers' beams: their end is
   wrapped and the strips read from the words written (`TF_NATIVE_STRIPS=0` off). ALPHA Cs * As
   (+ Cd) and Cs * FIX make the product in the pixel shader (the GS's factor goes to 2.0, the GPU's
   stops at 1.0).
   The glow (2026-10-26): after the field copies the frame's picture is shrunk into a 128 x 128
   buffer (block 0, FBW 2 - the scene buffer's top pages, free by then), a constant subtracted
   (the threshold, untextured: `Cd - Cs`), then through its ping-pong with block 256: a warp of the
   picture (an affine grid, CLAMP), a turn by 90 degrees, a pyramid down to 8 x 8 texels (64 x 64
   copies with region CLAMPs 127/63/31/15), red and blue each moved half a texel by copies of the
   buffer onto itself (FBMSK ffffff00 / ff00ffff), four enlargements by two back to 128, the turn
   back, a warp with a vignette's weights (gouraud colours), and three additions over the picture:
   a soft wash of the bright parts. What was wrong: (1) the buffers were kept at the picture's scale,
   so the pyramid kept five times the detail and the glow came out as sharp shapes (the green blob)
   - an effect's buffer (drawn only 2D, from other buffers) is now at the GS's own resolution
   (`TF_NATIVE_EFFECTSCALE=1`: scaled), and there the GPU's pixel centres are half a GS pixel off
   the GS's sample points (the copies lost their last row and column: the screen origin moves by
   half a pixel); (2) CLAMP was ignored for buffers read as textures (the sampler wrapped: the warp
   pulled texels from the far side) - now held to the texture's edge or region, in the buffer's
   texels and no further than the texture's own size (a buffer grown by another frame's scissor has
   rows nothing drew); (3) the copies of a buffer onto itself were left out - now drawn from a copy
   of it (32-bit views only: the shadow texture's CT16 shuffle stays with the top-byte path); (4) the
   threshold took off twice the GS's (untextured colour in units of 0x80): GS-exact by default now
   (`TF_NATIVE_UNTEXTURED=0`: as before). Checked stage by stage against a model of the chain with
   GS semantics (the buffers saved after each draw, `TF_NATIVE_SAVE_AFTER=<draw>[,<draw>]`) and the
   software renderer's local memory. NOTE: the emulated GPU renderer's glow is not GS-exact (red and
   blue copies of the bright parts a screen-width tenth apart, a red blob beside the laser) - the
   software renderer (`PS2X_GPU_MIRROR=1`, `shot_<t>.png`) shows the soft wash the native now draws;
   compare the glow with that one.
   The shadow receivers (2026-10-26): image 2011's `0x1df0` (a texture projected onto the mesh, the
   player's shadow texture B on the ground) was not a pass of its own - its draws took the mesh's own
   UVs into B, a black silhouette wherever those fell inside it (Amazon's start, beside the player);
   now ST from the position through q0-q3 and the colour q8 (alpha q8.w * bit 7). Their TEST's
   AFAIL is RGB_ONLY (a pixel failing the alpha test writes its colour, not its alpha): two passes.
   Left: the dropships' engine glows at Alaska are pink where the emulated are blue - not the glow
   pass (left out, the pink stays) but their textures (a red ball, T8, where the GS samples a blue one:
   a palette uploaded to CBP the converted texture does not know?); the player's shadow on Pacific
   Island's cliff is longer than the emulated; on the carrier's second map (10250) half of Optimus is
   drawn black (both resolved, below) - there with every switch of this change off too (`TF_NATIVE_AFAIL=0`,
   `TF_NATIVE_TARGETCLAMP=0`, `TF_NATIVE_SELFREAD=0`, `TF_NATIVE_EFFECTSCALE=1`,
   `TF_NATIVE_UNTEXTURED=0`, `TF_NATIVE_PROJECTION=0`), so not this change's. In the engine the glow
   is the supersampling scheme's "blur effects" (sun glare), run last in DownsampleToFrontBuffer
   before the HUD, its parameters (blur levels, passes, spectrum, rotate 90, highlight, feedback
   weight) per level from the weather database's BlurType entries; drawn here from the game's own
   draws, so a level's parameters come along. A bloom of our own from those parameters is the
   step after.
   The carrier's second map and the shadows (2026-10-26, checked against the software renderer):
   (1) half of Optimus black - not his materials: a display-list stretch was dropped. A set whose
   call adds its shader context (a CNT with the routine table, then CNTs of the data blocks) but
   not its own call - which the next stretch holds - ends its stretch inside the last CNT's data;
   the reader took the chain running past the end for a foreign chain and read nothing, so the
   routine table stayed the draw before's (a wheel's `0x1ac0` one light) for the next dozen lit
   parts: 0x1ac0 over the lit block's environment matrix, with context 1's TEX0 (the sphere map).
   Timing-dependent (the stretch ends where the packet pointer stands when the set returns; seen
   with the software renderer mirrored and in the surveys, not in every run). A CNT running over
   the end now reads to its end (`TF_NATIVE_RANGELOG`: "NOT READ" counts the dropped ones - 0 now).
   (2) The door (and the hull) as chrome: image 422f's routine at `0x2ec0`, other code making the
   same four copies as `0x2800` (`tools/vu1_trace.py --spec 4` on a captured batch,
   `PS2X_VU1_CAPTURE="1d10/q35 2ec0/q40 0480/qd@422f#4"`): unknown, it drew context 1 alone, the
   reflection. Identified as the lit mesh's draw routine (its bytes 0x2EC0-0x3290); 34-78% of
   VU1's work on this map, still the microcode's on the emulated path. (3) The player's shadow:
   against the software renderer (not the GPU renderer, whose shadow is the longer one) Pacific
   Island's was in place and of the same length, but hard-edged and too dark where the GS's is
   soft - the shadow scene softens its 128 x 128 by copies onto itself half a texel apart
   (ResampleSurface, bilinear), which at the picture's scale only shifted it. A target of at most
   256 x 256 read by a draw of the frame as 8 bits in the top byte (T8H/T4HL/T4HH: the player's
   shadow, the eco shadow map) is now kept at the GS's resolution, like the effects' buffers
   (`TF_NATIVE_SHADOWSCALE=1`: scaled) - the carrier's dark blobs on the rocks went with it.
   Debug: `TF_NATIVE_VIFLOG=<a>-<b>` (with `TF_NATIVE_RANGELOG`) prints the VIF codes, and an
   unpack's first words, of the stretches before draws a to b; `TF_NATIVE_DRAWLOG` adds prim2.
   A lost device (2026-10-26): a survey run on this map had the GPU hang (DXGI_ERROR_DEVICE_HUNG at a
   frame of 670 draws, tick 7890; the cause not found - not seen again in a dozen runs since) and the
   renderer went on: every texture made on the removed device failed and was made again each frame
   (8,408 "textures made" a profile window, the upscaled images' loader busy, the render thread at
   6.4 s a frame) until the machine went down (bugcheck 0xD1 in a kernel driver). Now a removed
   device stops the native renderer for good - no more GPU work, the emulated drawing back on
   (`TF_NATIVE_ONLY`) so the GS's picture shows - and a frame makes at most 128 new GPU textures and
   96 Mtexels (`TF_NATIVE_TEXCAP=<n>[,<Mtexels>]`), the rest put off to later frames with a warning
   line; a texture whose upload fails counts as refused, not made.
   The hang's hunt (2026-10-26, TODO 47): the debug layer, GPU-based validation and DRED on every
   map, Amazon live and native-only, a warp gate back to the HQ. Found and fixed: (1) a use of freed
   video memory that fits the hang (not proven to be it: the hazardous state was not reached on the
   routes tested) - the picture made again (the window's shape changes: 1440 x 1080 in the menus
   and loading screens, 2520 x 1080 in a level, nine times on a boot into a level) released the old
   picture at once and made the new one, and D3D12 hands the new resource object back at the old
   one's address every time (a test program: 199 of 199 remakes); the main target's entry
   (`m.colour != g.colour`) and its shader view (`srvFor`'s `srvOf == colour`) compare addresses,
   so a main target kept across a remake took the new picture for the old one and kept a view of
   the released texture - and in a level one pass every frame reads the picture (the profile's new
   count): freed video memory read, a page fault, which the AMD driver may report as DEVICE_HUNG.
   Now the old picture goes with the frame's releases, the main targets are told it is gone (a line
   "picture made ... while target ... held a view of the old one" tells when the old code would
   have gone wrong), and a made target's view is always written again. (2) Every draw into the picture had a pipeline made for the BGRA targets while the picture
   is RGBA (debug layer error 613 on every such draw - undefined on the GPU): the picture has its
   own pipelines. (3) A target that became the picture gave its shader view's slot back at once,
   while the frame in flight could still read it (a texture made the same frame rewrote it); its
   depth view, its DATE copy and its render-target place were lost for good (the 63 places then
   ran out). Now given back with the frame's fence. (4) A grown target whose new depth could not
   be made kept depth views of the released one: now not drawn at all. (5) The shared pictures:
   the one the window was about to take could be picked for drawing between its taking and its
   marking as held (marked under the hand-over's lock now). (6) A frame's fence is waited for in
   steps of a second with the device's state looked at: a removed device ends the wait (the
   frame's end then stops the renderer) instead of the render thread waiting for ever, and a frame
   the GPU has not finished in 3 s is told ("has not finished a frame" - the guards stop on it).
   Debug runs: `TF_NATIVE_DEBUG=1` (the layer), `=2` (with GPU-based validation; about 4x slower),
   each message id printed its first three times then at its 10th, 100th, 1000th...; the layer
   makes device creation fail (DXGI_ERROR_DEVICE_RESET) when the window starts fullscreen - run
   them with `PS2X_FULLSCREEN=0`. `TF_NATIVE_PROFILE` now counts the passes reading the picture.
   The water (2026-10-26). Two systems, both reaching the GS through VU1. (1) The rivers and pools
   (Deep Amazon, Amazon, Antarctica's and Alaska's water: taShader_FlowingWater, image e147's `0x1df0`
   then `0x2cc0`) are geometry sets whose routines were identified but drawn as a plain mesh - the
   base texture without its scroll and no reflection (a dark slab). Now two passes
   (`nr_passes.cpp`, `TF_NATIVE_WATER=0` the plain mesh): context 1 the ST moved by the current
   (q4.xy) with the vertex's colour, context 2 the sphere map of the eye's direction reflected about
   the normal the waves' slopes make, its colour times q21, CLAMP_2 from the block's ninth register
   (`Draw::clamp2`); both with the height moved by the two waves (`s - s^3` of `2 frac(p + 100000) - 1`,
   in the vertex shader, as `src/vu1_native.cpp drawWater`). Against the software renderer at Deep
   Amazon's river: the bluish reflection band, the bank's ripple and the Autobot's legs seen through
   the water as it draws them (the reflection sharper than the GS's, a 128 x 128 CT16 sphere map
   sampled with our own mips and filtering). The waves are small (heights 0.13 and 0.03, periods about a unit: what they move
   is the reflection, vertex by vertex). (2) The FFT ocean (vlxWaterEffect: the Mid-Atlantic, the
   carrier's maps; Pacific Island has it too but draws it only while loading) never makes a geometry
   set: its near patches' packets (a vlDynamicGrid's: strips of up to 12 quads, positions, colours
   and UVs REF'd from the simulation's arrays, MSCNT into the ocean block's routine) are CALLed into
   the list by 0x26F790 - wrapped, read from their words as the primitives' strips are, their state
   the list's (`TF_NATIVE_OCEAN=0` off). The carrier's second map now has its swell below the cliff
   where it had a flat slab. The far sea (Pacific Island's, seen from the cliffs) is not these
   patches nor a water routine - the effect's second loop hands four-corner pieces to the
   device's primitive renderer, which the capture had already: black in an earlier survey, it
   matches the emulated in the latest.
   Frame rate: the ocean's patches change every frame and go through the frame's ring, as the
   primitives do; so does any mesh of 256 vertices or fewer that has no buffer yet
   (`TF_NATIVE_SMALLRING=0`: buffers of their own). The carrier's second map draws ~110 geometry
   sets made anew each frame (6 to 190 vertices; new sets, new ids): a buffer each,
   CreateCommittedResource, cost 37 ms a frame (18 pictures/s; `TF_NATIVE_PROFILE` meshes 37.9,
   setup 18.6 - the GPU's fence two frames back); in the ring, 2.7 ms and 120 pictures/s at a 120
   target with the native picture live and the PS2 path off. Amazon, Pacific Island and Deep
   Amazon's river: 120 pictures/s, the render thread 3-5 ms (`TF_NATIVE_PROFILE` now also counts
   the buffers made and the ring's meshes).
   Debug: `TF_NATIVE_WATERLOG=<n>` / `TF_NATIVE_OCEANLOG=<n>` (the first n water / ocean draws after
   tick 9000: routines, blocks, state), `TF_NATIVE_GRIDLOG=<n>` (an ocean packet's words),
   `TF_NATIVE_MESHLOG=<n>` (buffers made). The ocean's own routine (0x2b00, the "ShaderAlphaFog"
   block's, ahead of 0x1410) is the fade by a plane - byte for byte 0x3660's code, at another address
   (TODO 147, 2026-11-04): each vertex's alpha clamp(q0.w + q0.xyz . position, 0, 128) from the plane the
   game sets per patch (traced: 0x42 at a near patch's corner, as the routine wrote). Unidentified, the
   patches kept their packet's alpha (0x80); now FadeByPlane, the sea blends as the GS's does - Mid-Atlantic,
   the changed water 3.9 -> 2.5 levels from the emulated drawing on average (`TF_NATIVE_OCEANFADE=0`: as
   before). Left: the GS's mip choice (TEX1) for the reflection; field B.
   Cybertron's city (TODO 162, 2026-11-05): the field of small grey and blue pieces is image a31d's
   `0x22c0`, alone in its table (entry pc 0x10, batch type 3, three vertices a batch, 22% of VU1's
   work there) - unknown, each batch's three vertices went out as one triangle: the dark shards, and
   no city. Traced on captured batches (`PS2X_VU1_CAPTURE="22c0/q35@a31d#3"`, tools/vu1_trace.py):
   each vertex is a building, a box - its centre the position's x, y, its half sizes the ST, its
   height the colour's alpha times 10, its foot the position's z times q12.w (0) - and a batch draws
   one face of each, the block's q10.w saying which (0 the side along x, 1 along y, 2 the roof; the
   game sends a set's buildings three times): a strip of four corners, the side the one facing the
   eye in q13 (the - side when the eye is below the centre along that axis, its top colour the
   building's times q10, else the + side's, times q11), the feet q16 (the red glow), the roof the
   colour times q10; ST the texture's repeats (q12: per unit of half size along the face and of
   height, at least one). Now `Routine::CityBlocks`: each vertex four corners (`cityBlocksOf`, cached
   by the mesh), placed by the vertex shader (mode.z 5) with the frame's face and eye. At 16500 the
   city matches the emulated picture (`TF_NATIVE_CITY=0`: as before).
   The atmosphere (2026-10-26): checked against the software renderer (`PS2X_GPU_MIRROR=1`) with its GS
   memory at the shot (`PS2X_GS_DUMP_VRAM_TICK`): the depth fog's per-pixel index survives there in the
   depth pages' top byte (the display buffer's CT24 writes leave it), and the fog / filter palettes at their
   CBP. (1) The depth fog never reached the middle distance: its quads sit at Z 0xFFFF with GEQUAL, to fog
   only what lies beyond (Z under 0xFFFF, where bits 8-15 are the palette index without wrapping), but the
   2D path kept Z / 16 while the meshes' depth is GS Z * 16 / 2^32 - a 2D draw tested sixteen times too far
   (`nr_gif.cpp`: GS Z as it is now). Antarctica's mountains and Deep Amazon's far hills now take the
   haze; the native fog index matches the GS's pixel for pixel (histogram). (2) The lens filters
   (taLensFilter, display list 0x18: Antarctica's grey grade - LUT 103..151 grey, tint (46,43,15), alpha
   0x38, blended over the scene) read the scene's own green, moved into its top byte by CopyGreenToAlpha,
   as T8H: a read of the target being drawn, which the top-byte path skipped, so it sampled GS memory
   converted from another moment. Now from a copy of the target as it is (`TF_NATIVE_LENSFILTER=0`: as
   before); the native result equals the GS formula to 0.2 of a unit. (3) Deep Amazon's "pink-yellow
   sun glow" is the emulated GPU renderer's glow bug (TODO 45): the software renderer shows a soft warm
   wash, which the native glow matches within a few units (the glow buffer 1.5 lower, following its
   input). Most of the native's darker picture there is the remaster's large images (sky, foliage: R -9
   with them, -3 without, `TF_HIRES=0`); the small sun disc in the native sky is in the remastered sky
   image, not a draw. The sun's own draws (disc, spikes, flare) reach both renderers alike: the occlusion
   read-back (page 384) holds the same bytes in both paths (docs/NOTES.md). Left: Antarctica's snow and ice
   are about 1.3x brighter and bluer than the GS's before any of this (the ground-lighting item above), and
   the GS's warm drift streaks on the snow (texture LOD, TEX1) are not drawn. Debug: `TF_NATIVE_FOGLOG`
   (the fog draws, their palette and quad Z, the pass decision), `TF_NATIVE_FOGDEBUG=1` (the fog pass shows
   its index in red, no depth test), `TF_NATIVE_DRAWVERTS` now prints a 2D vertex's Z.
   The field weave (2026-10-25): the scene buffer holds two fields (A rows 0-223, B rows 224-447;
   with one picture a frame B's scissor is a single pixel - GS log: 0-511,0-223 ofs y 29183, then
   0-0,0-0), woven into the display buffer by two copies (SCANMSK 3, then 2) on every map. The 3D
   is drawn once, field A's, so the copy of B is left out and A's fills every line, as the GS does
   with one field shown. Which copy goes was decided by the rows any 3D drew: the overlays drawn
   after the scene over the whole buffer at A's offset (the carrier's light shafts, Antarctica's
   snow, scissor 0-447) made B's copy run last, over A's - the carrier (both maps), Antarctica,
   the starship and Cybertron showed field B's black instead of the scene. A field now counts as
   drawn only by a draw whose scissor starts in it.
   The HQ (2026-10-25): the front end's scenes apply no state block - SetRenderState and SetTexture
   write ALPHA, TEST, TEX0 and CLAMP one by one through func_00225D60 (the register shadow at
   D_70000060) - so every HQ draw had no texture and no blend (the white room). Each register now
   comes from whichever wrote it last in the list's order, the state block applied last or a
   write of its own (func_00225D60 wrapped: a sequence number per register; `TF_NATIVE_SHADOWREGS=0`
   the block's only). The 2D's texture tables (uploads by GS block, SetTexture's bindings) were
   read by the renderer's thread as they stood at the frame's end, not where the list stood: the
   ring's label sheet (fbp 381) drew its font from the block the frame's later portraits were
   uploaded to. The tables' changes are now logged and the list cut at each (`TF_NATIVE_LISTCUT=0`
   no cuts), the renderer's thread replaying them into its own copy before each stretch. The HQ's
   images (60a8ddde9d123220, 6f9f0e96b0ab22f8) hold the one-light routine at 0x2100 (0x1ac0's code,
   the same bytes): identified, the holograms, light cones and wireframe car get their colour and
   alpha.
   The textures' bindings (2026-10-26): (1) a texture with several palettes ('_multi': the effects'
   colour variants, the enemies' recolours) is one texture shared by effects of different colours;
   its active palette (+0xE0) is whichever an effect set last, so a draw took the palette TEX0's CBP
   names (the level uploaded there), not the active one - Alaska's dropship engines (and the warp
   beams, the carrier's grunts, Deep Amazon's laser bolts) drew FX_blueredgreen1_multi's red palette
   where the GS draws its blue. (2) A texture group's uploads are recorded once when the group is
   placed (seen then, at the level's load) and sent by a DMA CALL whenever the group becomes current
   (vlTextureGroup::Load, 0x23C9F0): the upload table now follows those loads (the group's slot list,
   `TF_NATIVE_GROUPLOAD=0` not). (3) 8- and 4-bit levels packed into a group's CT24 pages' alpha
   (T8H/T4HL/T4HH; the level's own format put back after packing) are written as TEX0's format says.
   (4) Textures in GS memory alone (flag 0x1000: render surfaces' and depth buffers' aliases) are not
   converted (the renderer samples the target; counted "kept in GS memory alone"); bindings and
   uploads to freed textures (a vtable left, the levels garbage) count as unbound / unknown. The
   ~160 not converted on Antarctica and the Starship were before 45fd69a; now 0 on every map but one
   (Cybertron: a T8H view whose palette has no data). The 0x64 single-tree sprites (FIX = fade) are
   drawn by the GS into the scene buffer (GS draw log: fbp 0, field A's scissor), not the eco
   shadows - those are 0x2cf0 sprites (ALPHA 0x68, green) into the 256-wide targets at 112/224,
   drawn there already. Debug: `TF_NATIVE_LEFTLOG=<n>` (draws left out for want of a texture: on
   Antarctica the CT16 shuffle copies).
   The software picture's last differences (2026-10-26; the reference is `PS2X_GPU_MIRROR=1`'s
   software renderer). (1) Cybertron's bubble (the chrome oval at the bottom): image 6b20's routines
   `0x2030` + `0x25e0` were unknown, so the draw went out with vertex colour and alpha 1 under an
   additive blend. They move each vertex by two cubic waves (q4 amplitudes and frequencies, q5/q6
   directions and phases; the normal moves with it), take the sphere map of the view from the eye
   (q8) reflected about that normal, a constant colour (q10) and alpha `q11.w * max(N.L, 0) +
   q11.z` (L = q9) - 0 in most frames: a faint film, mostly nothing. (2) The Starship's light cones:
   image 83f1's `0x1e70` (55 draws) projects a 256 x 256 render target (CT16S, drawn by the 2D the
   same frame) from the vertex moved along its normal (`k = q0..q3 * (p + n * q4.w)`, ST `0.5 + 0.5 *
   k.xy / k.z`), coloured `min(q5 * (max(N.L, 0) + q6.w), 255)` with L = q6, alpha q5.w; and image
   e147's `0x2920` reflects the view through the screen's axes (q0-q2), alpha by `(1 - N.V)^2`, 16
   facing away. Both were drawn as plain meshes. (The sharp blue beams at 10250 are the emulated
   GPU renderer's own: the software picture has none, only the blue wash on the right the native
   picture already drew - blue 65 vs the software's 73 there.) (3) Alaska's purple patches on the snow are the
   remaster's own images, not a rendering fault: with `TF_HIRES=0` they go, and leaving out
   MOUNTSNOW2-5, ROCKPATCH, TRENCHSIDE, ROCKS and the like (`TF_HIRES_SKIP=<name>[,<name>]`, new)
   takes them away too - their upscales turn the game's soft grey rock under the snow into sharp,
   darker blots (about 2% of their texels dark purple where the game's have none), which the
   slope's lilac vertex colour and its magnification make into the patches; the game's own
   128/256-texel images blur them into the faint streak the software picture shows. Kept as the
   remaster made them (a question for the images). The large images' check (box-filtered back to
   the game's size) now also compares alpha: a mean difference over 24 rejects the image ("its
   alpha is not the game's" - LOADINGSCREENTOPLEFT 221, STRIPGLOW 226, GRAYX16 28; `TF_HIRES_ALPHA=0`
   keeps them). (TODO 229) It also rejects a black image, or one missing a colour the game's texture has
   (under a quarter of a channel whose mean is 24 or more: Real-ESRGAN's black images on GTX 16xx cards,
   which against a dark texture stayed under the mean difference of 20), and one whose red and blue
   exchanged fit far better; `TF_HIRES_DARKCHECK=0` / `TF_HIRES_SWAPCHECK=0` keep them. (4) Antarctica's
   warm snow streaks: the grass image's `0x0480` alone in the table is not the core's clip routine
   but vlShader_Drift - the ground's triangles sown with sprites drifting along the wind (q24, less
   its part along the normal) - drawn as a plain mesh before. Read off the microcode (RNEXT/RINIT
   modelled in a copy of tools/vu1_trace.py, which then gives every packet as VU1 sent them): per
   triangle a call (0x0c28) of three passes, each corner in turn as C, `n = q23.y * |(B-C) x (A-C)| /
   (1 + q23.w * max(C.w - q24.w, 0))` packets of 8 streaks (at most 8; the random unit restarted at
   1.34582 each pass; streak k of packet p at `C + u (A-C) + v (B-C) + wind * t`, (u, v) from q25 on,
   `t = frac(R - 1 + q23.x)`, the tip q2 further, alpha `(1 - (2t - 1)^2) * q23.z`), the triangle
   split in four (n less 2 each) when the largest excess over 8 is 4 or more. A model of it matches
   captured batches packet for packet (to 0.06 px). `TF_NATIVE_DRIFT=0` draws the plain mesh.
   (5) TEX1 (filtering and mip choice) per draw: MMAG / MMIN, LCM, L, K and MXL go to the shader;
   the GS's LOD (`(log2(1/Q) << L) + K`, or K alone with LCM 1) picks the level, a "nearest" mode
   rounds it, MXL caps it. The game draws almost everything with LCM 1, K 0 (level 0, bilinear),
   the lit meshes' context 2 with LCM 0 and MMIN nearest-mip-nearest (0xa0), Alaska's and the island's trees with MXL 2,
   K -8.5 (past level 0 only beyond w ~ 360). The levels past 0 are our own box-filtered chain, not
   levels the game may upload at MIPTBP (not read yet). Level 0 of an upscaled image is its full
   size, so a draw on level 0 keeps the derivative-picked mips of the larger image (no shimmer), a
   draw past level 0 samples the matching smaller level. Three samplers (anisotropic, linear,
   point). `TF_NATIVE_TEX1=0` the old filtering; `TF_NATIVE_TEX1LOG` (per frame: the TEX1 values in
   use), `TF_NATIVE_SKIPROUTINE=<hex>` (draws that ran that routine left out). Frame cost (Amazon,
   120 target): 120 pictures/s either way, the record step 1.70 -> 1.88 ms with TEX1 on.
   The core's clipping, the Gouraud colour, the half pixel and the game's mips (2026-10-26, against
   the software renderer, the same run's `emu_` and `native_` shots). (1) Antarctica's blue picture
   at 9900 (native ~ 0.95 x software + 21 in blue only, every pixel) was not a haze pass: traced by
   saving the scene buffer after each draw (`TF_NATIVE_SAVE_AFTER`) down to one draw - a 12-vertex
   blue additive light mesh (LightNormals, ALPHA Cs * FIX + Cd) whose corners straddle the camera,
   ten of twelve behind it. 0x1410 runs with q21.x above 0 (an integer), so VU1's 0x0480 clips each
   triangle against its own clipping matrix (the routine's q0-q3, |x|, |y|, |z| <= w); its near plane
   lies at w ~ 0.5 where the GPU's clipping alone kept everything from w ~ 0.03 - the sliver between
   came out as a screen-filling smear. Only in some states (the frames where that mesh is near the
   camera: this PC's and the other PC's 348-draw frame, not the test PC's 252-draw one). Now the
   draws that clip take 0x0480's matrix (`Pass::clip`, nr_passes.cpp: the table holding 0x0480 after
   the core, not instances) and are clipped as 0x0480 does: a geometry shader (`gsClip`) cuts each
   triangle by the six planes, new corners linear in clip space; a mesh whose box lies inside all six
   planes skips it (`boxInside`). `TF_NATIVE_CLIP=0` off, `TF_NATIVE_CLIPGS=0` the GPU's clip
   distances instead of the shader. 9900: blue 130.6 -> 116.5 (software 114.7), the fit 1.02 x + 0.
   (2) The Pacific's rock: perspective-correct vertex colour where the GS steps Gouraud colour
   linearly on the screen (perspective is for S, T, Q alone) - large lit triangles at a slant took
   their corners' colours in other proportions, dark bands down the cliff. The colour is now
   screen-linear (`noperspective`), but only over a triangle the GPU does not clip (the flag `whole`):
   a clipped one's screen-linear colour is extrapolated from behind the eye (a blue sheet over the
   sea), so it keeps the perspective-correct one - and the triangles 0x0480 clips are cut by
   `gsClip` first and then drawn screen-linear over the cut polygon, as the GS draws what VU1 sends.
   `TF_NATIVE_AFFINE=0`: perspective-correct, as before. With `TF_HIRES=0` at 9900: mean -1.3 ->
   -0.4 against the software's (40.8 vs 41.2), the fit's r 0.982 -> 0.989, the darkest band -5.2 ->
   -3.6 (the rest not found: the band's triangles lie close to the camera). (3) The half pixel: a GS
   pixel n shows its sample at n over [n, n+1), the GPU samples pixel centres - the half-pixel move
   was made only for targets at the GS's own resolution; at the picture's scale everything sat half a
   GS pixel up and left (measured: one GS pixel off the software's picture after the copies), and the
   copies of the 3D field (-0.5..511.5) left the picture's last two columns at the clear colour - the
   dark strip down the right edge of every level and menu. Now every target is moved
   (`TF_NATIVE_HALFPIXEL=0`: as before); the shift against the software's now 0 across, half a line
   down (field A's own). (4) The game's own mip levels: a vlTexturePS2 holds its image levels from
   +0x90 on, +0x54 of them; past the first, the levels a group uploads for MXL (MIPTBP1 in the state
   blocks is always 0: the levels go to the group's slots). Those levels (half the size each, the
   image's palette) are converted with the texture and become its first mips (`Texture::gameMips`;
   the large images take them in at their own level base + k, nr_hires.cpp `spliceGameMips`); below
   them the box filter. Alaska's PINE1-4, 27 textures on the Pacific; their level 1 differs from the
   box filter by about 4 per channel, 5 in alpha. `TF_NATIVE_GAMEMIPS=0` off; `TF_NATIVE_MIPLOG=<n>`.
   (5) Palettes of top-byte reads (the lens filters' seven) now as textureFor takes them: the one at
   CBP (`TF_NATIVE_FOGCBP=0`: the active one; `TF_NATIVE_PALLOG=<n>`). (6) Frame cost: the passes'
   constants are built on the stack and copied into the ring whole - every read of a field back from
   the upload heap (write-combined) cost about a microsecond, 0.8 ms a frame once the half pixel was
   applied to every pass; the record step is now 1.0-1.1 ms (1.9 before), the render thread 5.6 ms
   (6.5), Amazon at a 120 target 120 pictures/s. Debug: `TF_NATIVE_CLIPLOG` (with the DRAWLOG: q21,
   0x0480's matrix, each vertex through it). (7) Menus and 2D, surveyed with native shots in the front
   end (memory-card notice, legal screen, title menu, saved games, difficulty), the HQ (Autobot select,
   warp gate), the level's intro and the pause menu: each within about 1 of the software's mean, the
   same layout and text; the button prompts the same glyphs (with the remaster's images the cross
   reads grey where the game's reads blue-grey - the images, not the drawing; with `TF_HIRES=0` alike);
   the dark strip down the right edge was (3). The menus' shake (the user's report on the other PC,
   the difficulty screen): the emulated picture, not the native one - the title and its menus
   (taUIIntro) draw one full frame every two VBlanks in the GS's field mode, and at the console's rate
   the presenter showed a field a VBlank over the whole height (meant for frames whose fields are two
   moments), so the picture moved by half a line every VBlank (consecutive VBlanks of one frame: the
   GPU renderer's shot 2 px apart at its 4x scale, the software's 3.7 apart in content; the native
   picture still). Now taUIIntro's frames are shown whole (overrides.cpp executorLoop:
   `ps2_host_display::setShownField(0)` while it runs, the rate's rule back after; `TF_MENU_FIELDS=1`:
   as before): the two VBlanks of a frame identical (difference 0.00, shift 0); the HQ and levels as
   before (their own field rules). Not native yet: the movies (the warp tunnel between the
   front end and the HQ is an IPU movie: black natively, the GS's picture is shown for it); no loading
   screen showed in the runs (the fast load). Left: Deep Amazon's picture darker and less warm than the
   software's (R -14 at 9900 on the test PC, the same with all of the above off: the remaster's
   images); Antarctica's 10250 / 10700 within 4 of the software's. One GPU hang (DEVICE_HUNG) on the
   other PC (RX 5700 XT) on Cybertron after tick 11400 in the final survey (the renderer stopped, the
   emulated picture came back, as built for it; the watchdog's dump hang_1_20261026_234013 there) -
   not seen again in two repeats there, nor with the debug layer on the test PC or GPU-based
   validation on this PC: like TODO 47's, not reproduced.
4. The native picture shown live in the window. **Done** (`TF_NATIVE_LIVE=1`): rendered in the display's shape to fit
   the window's width and height (`TF_NATIVE_HEIGHT` fixes the lines instead), the GS buffers' targets scaled to it,
   handed to the window through the runtime's picture provider (patch 0036), the overlays on
   top; the GS's picture when the native renderer has none (movies). Two frames in flight; the
   frame's CPU work on a render thread (`nr_thread.cpp`): the display list is read there from
   guest memory (the game double-buffers its packet buffer; the game waits before reusing the
   one being read), the game's thread only records where each draw's stretch lies (~0.4 ms a
   frame). The picture is a D3D12 texture the window's GL reads itself (`nr_glshare.cpp`,
   EXT_memory_object_win32: no readback, no CPU copy); a CPU hand-over where the driver lacks it
   (`TF_NATIVE_SHARE=0`).
   The paced hand-over (2026-10-30, N9 / TODO 95; `TF_PACING`): a picture used to go to the window when the render
   thread began the frame after next and found its GPU work done (finishSlot), and the window showed the newest at
   each VBlank - with the in-between pictures two VBlanks a frame, the second a copy - so each picture stayed up for
   one, two or three VBlanks. Now a thread of its own (`pace::` in nr_d3d.cpp) waits for each picture's fence, then
   hands it over at its frame's moment (`Frame::dueNs`, the VBlank it was handed over at) plus a steady delay (the
   98th percentile of the last 2 s of readiness + 1 ms; whole VBlanks when they are timed by the screen's refresh),
   and the window shows it at once (`ps2_host_display::setPicturePacer`, PS2Recomp patch 0047). The pictures waiting
   for their time keep their shared texture (`g_published.pending`; six textures now). `[tf] pacing:` (nr_present.cpp)
   logs every 2 s how evenly they reach the screen. With it, a frame is one picture (overrides.cpp nativeOnePicture).
   Pictures between the game's frames (2026-10-30, N9's second stage, TODO 88; `TF_POSES`, the setting "Smooth
   pictures between frames"): the render thread (nr_thread.cpp `poses`) draws each frame once for every refresh its
   moments cover, each 3D draw of the scene's buffer posed between the frame before's pose and field A's for the
   picture's moment - the frame before's from the core's block (0x1410 / 0x2800 q0-q3, field B with the in-between
   factor 0), or the field A the same mesh had the frame before (the carry), draws with one matrix sharing one pose to
   the bit, the clipping matrix posed with it, draws without a pose moved by the camera's motion - and the menus' 2D
   (frames of 2D alone) blended vertex by vertex; each picture's `Frame::dueNs` is its moment and `Frame::pictureNs`
   its spacing, so the pacer above spaces them as the refreshes. How many: the screen's refresh, capped by the render
   thread's time (60%) and the GPU's (80%, the frame timestamps, now always taken for these pictures). Up to 24 shared
   textures (made as first needed) wait for their moments. The menus (2D alone) get their pictures between frames
   whatever the setting (TODO 88, 2026-11-03; `TF_MENU_POSES=0`: only with it), their render thread's time a picture
   counted without its waits for a frame slot and given 90% of a picture's time (the game's thread has next to nothing
   to do there): the RX 5700 XT's menus went from every other refresh to every refresh at 180 Hz. Checking aids: `TF_N9_CHECK`, `TF_N9_SHOTS`, `TF_N9_LIST`,
   `TF_N9_LIST2D`, `TF_POSES_ALT` (docs/DEBUGGING.md); `tools\n9_moments.py`, `tools\n9_ab.py`.
   One recording for all of a frame's pictures (2026-10-30, N9's third stage): the frame's list is recorded once and run
   for every picture (nr_d3d.cpp render's end; `Frame::pictures`) - each draw carries the frame before's pose
   (`Draw::matrixB`, `clipB`, `instB`, set by nr_thread.cpp `poses::assign`; the instance stream's IROWB0-3, the
   constants' clipRowsB) and the GPU blends them by the picture's moment (`posed` in the shader; b1 =
   `PictureConstants`, written by a small list of its own before each picture into the slot's 256 bytes; the lit models'
   lights in the mesh's space too, `lightsB`). A small list after each picture copies it into its shared texture and
   takes its last timestamp; before an extra picture a third sets every resource the frame's list moved back to the
   state it began with (`transition` notes them while the list is recorded). Meshes made anew every frame (the grass's
   blades, microcode primitives, the ocean) and the HUD's moving 2D are matched to the frame before's of their kind and
   blended vertex by vertex (a third vertex stream: POSB, COLORB, PSIZEB; `Draw::meshB`). The render thread's time for
   an extra picture: 0.04-0.08 ms (was 2-3). The pictures' cap (nr_thread.cpp render) counts the frame's own work once
   and each extra picture's (`d3d::gpuExtraMs`, `cpuPictureMs`), and tries one refresh more now and then. The pictures
   are the same as the second stage's (shots of the same frames: t = 0 and 1 identical, the in-betweens 0.003/255).
   An extra picture is submitted only once the picture before it has finished on the GPU (2026-11-02, found by
   Jenvery): D3D12 forbids submitting a command list again while an earlier submission of it may still run (the
   driver may patch a list as it is submitted; the debug layer's error 553, 100+ times in a run with the pictures
   between frames on). A thread of its own (`replay::` in nr_d3d.cpp) waits for each and submits it, so the render
   thread goes on recording the next frame meanwhile - into the other slot's frame list (`frameLists`, one a slot) -
   and the GPU stays as busy as before; every other use of the queue first waits for its jobs (`replay::drain`,
   counted as a wait for a frame slot: the GPU behind). `[native render] extra pictures` logs every 10 s how many
   there were. `TF_NATIVE_SERIAL_REPLAY=0`: back to back, as before; `=wait`: the render thread waits itself (as a
   frame saving a picture does), that wait left out of its times (`cpuPictureMs`, `replayWaitMs`).
   On AMD cards the core's clipping is the GPU's own clipper on every preset (`adapterVendor`; the clipping shader was
   4.4 of the RX 5700 XT's 8 ms a picture in Amazon, 0.4 of the GTX 1080's 3). Aids: `TF_POSES_REPLAY`,
   `TF_POSES_PROBE`, `TF_POSES_VERTICES`, `TF_POSES_HUD`, `TF_POSES_SKIP`, `TF_N9_TSLOG` (docs/DEBUGGING.md).
   Anti-aliasing (2026-10-28, TODO 64; `TF_NATIVE_AA`, default on, live in the F5 panel's Graphics
   tab as "Smooth edges (native renderer)"): an FXAA 3.11 "quality" pass (preset 39, 12 search
   steps; `kAaShader`, `runAa` in nr_d3d.cpp) over the picture. It runs once the picture holds the
   frame's 3D - a draw that is not a 2D GIF draw, or a copy from a target holding 3D (the scene
   buffer's field copies; the glow made from the picture) - just before the first draw into the
   picture that carries none, so the HUD, menus and text are drawn after it untouched; at the
   frame's end when no such draw comes; never on a picture with no 3D (the front end's menus). The
   picture is copied into `aaSource` and the pass drawn back into it, its alpha kept. A shot's log
   says where it ran ("anti-aliasing: applied before draw 1409 of 1466" in Amazon, 319 of 357 in the
   HQ - after the field copies, before the HUD). Cost (test PC, GTX 1080, 3360x1440): 0.03 ms of
   the render thread, 0.8-1.3 ms of the GPU. Tuning for tests: `TF_NATIVE_AA_SUBPIX` (0.25 since
   2026-11-03, was 0.5: TODO 64's comparison - the edges as smooth, the grass and leaves sharper; FXAA's own
   0.75 blurs the upscaled textures more), `TF_NATIVE_AA_EDGE` (0.125), `TF_NATIVE_AA_EDGEMIN`
   (0.0625); `TF_NATIVE_AASAVE=1` saves `<shot>_noaa.bmp` too (the picture just before the pass,
   without the 2D after it). SMAA 1x (2026-11-03, TODO 64 (c); `TF_NATIVE_AA=2` or the panel's "SMAA 1x" -
   the setting's choices are now Off / FXAA / SMAA 1x, FXAA the default): the reference SMAA (Jimenez et al.,
   MIT - `ext/smaa/`, with its LICENSE.txt and lookup textures, unchanged; `smaa_hlsl.inc` = SMAA.hlsl as
   string pieces, `make_inc.py`), its "high" preset with our porting macros (`kSmaaPrelude`, samplers s4/s5),
   in the same place as FXAA and from the same picture copy: luma edges into an R8G8 target, the blend weights
   into an RGBA8 one, then the picture blended, its alpha kept (`runAa`, `smaaTargetsFor`, `smaaLookups`).
   When it cannot be made it says so once and FXAA runs. Cost (test PC, GTX 1080, Amazon, median of 115 profile
   windows): 0.31 ms of the GPU at 1920x1080 (FXAA 0.20), 0.53 ms at 2560x1440 (FXAA 0.34). Detail kept vs AA off
   (frame 12500, the other PC): SMAA 0.95 / 0.97 in Amazon / Deep Amazon, FXAA 0.88 / 0.93 - it changes 2.1% of the
   pixels (FXAA 8.2%), the edges as smooth. The profile line counts its
   frames ("SMAA in n"). Found on the way: the scene buffer is drawn at `scaleY` = picture lines
   / 448, but the 3D is drawn once into field A's 224 rows, so at 1440 lines the 3D has 896 rows,
   enlarged x1.6 down by the field copy - the steps along edges come out ~1.6 lines tall and the
   picture softer up and down. Done (roadmap N10, 2026-11-02): a buffer a field copy reads gets picture lines / 224 rows a
   GS row (`scaleYField`), the field copied 1:1 (`TF_NATIVE_FIELDRES=0`: as before).
   Texture uploads off the render thread (2026-10-28, TODO 59; `TF_NATIVE_UPLOADER=0`: as before).
   A level's first seconds stalled the render thread: every new GPU texture was made on it - a
   default-heap texture, a committed staging buffer, the texels and a box-filtered mip chain copied
   in - about 1 ms for a game texture and far more for a 2K large image, up to the frame cap of 128
   textures / 96 Mtexels (`TF_NATIVE_TEXCAP`). Now an uploader thread (`up::` in nr_d3d.cpp) does
   all of that and runs the copies on a COPY queue of its own (the texture made in COMMON: promoted
   to COPY_DEST there, decayed once done, promoted to PIXEL_SHADER_RESOURCE by the frames that
   read it), waiting for its own fence; the render thread only gives a finished texture its
   descriptor slot (`takeUploaded`, once a frame). The game's own textures go first (a queue of
   their own; their draws wait the frame or two the copy takes), then the large images, during
   which the game's texture is drawn in the large one's place (`Texture::fallback`). Palettes
   (256 texels or fewer) stay on the render thread. `TF_NATIVE_UPLOADGAME=0`: the game's textures
   on the render thread again. The large images' loader (nr_hires.cpp) was one thread working
   through every request in order, the dropped ones too: a request held the game's texture, so
   the HQ's backlog was still being matched half a minute into a level, results nobody took, and
   the level's own images came late and all together. Requests now hold it weakly (gone = dropped
   at once) and a few loaders run below the game's threads in priority (`TF_HIRES_THREADS`,
   default a quarter of the hardware threads, 1 to 4; half, up to 8, since TODO 57; since TODO 102
   at most 6 and never more than the hardware threads less 4, in Windows' background mode - the
   lowest CPU, disk and memory priority, so a warp's burst of images never takes the PC from other
   programs - with the PNG decode + BC7 compression on at most half of them at once,
   `TF_HIRES_HEAVY`; `TF_HIRES_BACKGROUND=0` below normal as before). Since TODO 121 the loaders run
   below normal and take background mode only for a PNG's decode + compression: in it the whole time,
   the compressed cache's reads (a few ms each) waited 25 s and more whenever another program kept the
   PC busy (a compile beside the game) - the trees and the HQ's Autobots kept the game's small textures
   meanwhile (`TF_HIRES_BACKGROUND=all`: the whole time again). Since TODO 124 they run at the game's
   own priority outside background mode (below normal, one image from the cache took 0.4-0.5 s of wall
   time on a six-core PC with the game flat out, 7 ms at normal; `TF_HIRES_PRIO=<-2..2>` another), the
   texture caches' idle times count the host's seconds, not VBlanks (at 240 frames a second "30 s not
   drawn" was 7.5 s; `TF_NATIVE_TEXCLOCK=vblank` as before), a large image whose textures were all let
   go of (idle, or converted again) is held `TF_NATIVE_LARGEKEEP` seconds (600; 0 = not) or until the
   video memory runs short, and every 10 s a line tells how long textures in view waited for their large
   image ("large images in view: ...": median / 95% / longest, at once, asked for again, the loaders'
   queue and work; `TF_HIRES_WAITLOG=0` off). A frame over 50 ms on the render thread
   is now told with its parts ("slow frame at tick ...", "slow frame's parts: ...").
   The game's textures converted off its thread (2026-11-05, TODO 164): the HQ's 15-20 ms frames at 240 (each
   turn of the ring, each level-select picture) were 0.4-3.7 ms of EE time - the rest the conversion of the textures
   first drawn there, on the game's thread (`TF_PROFILE_SPIKES`: the copy and swizzle into a 4 MB GS image cleared
   whole each time, and the large image's content hash a byte at a time). Now the draw copies what the conversion
   reads of guest memory - each level's header, its texels, the palette (`captureTextureInputs`, a fraction of a
   millisecond) - and hands it to a pool of workers (`TF_NATIVE_TEXTHREADS`, default 3; 0: on the drawing thread as
   before); the draw holds the texture to come, the renderer waits for its pixels at the frame's end (a frame or
   more after the draw: rarely), and the rest - the prefetch's twin, the large image asked for, the counters - is
   done when it is back, at the next frame's start (`takeConverted`). A texture whose content has a large image
   loaded already gets it on its own frame (TODO 177, 2026-11-08): the worker looks its content key up in a
   map the game thread fills as large images come in (`g_liveLarge`) and leaves it in the texture's pending
   state; `textureFor` draws it - before, that frame drew the game's texture, a one-frame low-res flash on every
   texture the HQ uploads again while its camera moves (`TF_NATIVE_SAMEFRAME_LARGE=0`: as before). Only the pages a level reaches are cleared,
   and the content hash takes 8 bytes at a time. A light map read from GS memory is still converted at the draw.
   Every 300 frames a line tells the pool's work and the renderer's waits. Pictures bit for bit as before (the
   HQ's ring and level select, with and without the pool). Made ahead (stage 2): `TF_NATIVE_PREFETCH=1` converts each
   texture on the pool when the game uploads its image (most of a level's while its load screen shows) and asks
   for its large image then; `=scan` the old memory scans (with the state blocks' textures). Both stay off by
   default as they measured: on Amazon's opening the loaders had 297 jobs against 105 and the textures in view
   waited longer for their large images (median 199 ms against 69), on the HQ no better (63 against 66); the draws
   convert their own key anyway (another TEX0 than the upload's). The memory scan is a walk of all 256 MB of guest
   memory now (30 ms a scan).
   The shaders and pipelines kept across runs (2026-11-05, TODO 164 stage 3): beside the player's settings (the folder
   settings.ini is in) under "shader cache" (`TF_SHADER_CACHE=<folder>` another, `=0` none). Each compiled shader is kept
   by the hash of its source, entry point, target, defines and the compiler's version - a changed shader is another
   name, nothing stale is taken; the pipelines are kept in a D3D12 pipeline library (pipelines.bin), each by its key
   and its shaders' bytes, written 2 s after the last new one (beside, then renamed over: a run ending part-way
   leaves the old file). The keys this machine used (pipelines.txt) are made ahead at the next start on a thread of
   their own and handed to the render thread at a frame's start. A library the driver refuses - another driver or
   graphics card, or a damaged file - is made anew ("pipeline library: the one on disk refused ... made anew";
   `TF_PIPELINE_LIBRARY=0`: none). On a GTX 1080 test PC, the HQ: the start's shaders took 3.1 s compiled, 1 ms from the cache;
   at the second start 69 pipelines were made ahead in 22 ms and none was made while drawing.
   The first picture after a restart (2026-10-28, TODO 63): the settings' restart starts a new
   process that loads a state in the level; with the native renderer drawing alone the GS
   publishes no picture there, and the native one comes once Direct3D 12 is ready (2.9 s on the
   test PC) - meanwhile the window showed its own "no picture" texture, raylib's MAGENTA
   (ps2_runtime.cpp UploadFrame): the purple screen. Until the GS has shown a picture once, a frame
   with no native picture is now black (nr_present.cpp; "no picture from either renderer yet").
   The 2D's widescreen groups (2026-10-28, TODO 7): an element overlapping others now takes the
   anchor of its whole group's extent, transitively, as the PS2 path's front end decides it
   (gs_frontend.cpp closeOverlayFrame) - before, it took the anchor of the first element it
   overlapped, decided by that element's own extent: the Mini-Con ring's pieces and its highlight
   outline went their own ways, and the HQ's Autobot "More Info" card (TODO 54) put its description
   over the spec sheet in the right column, the left panel empty with two black bars
   (`TF_NATIVE_OLDGROUPS=1` brings the old grouping back: the card as reported, at 21:9).
   That alone left the ring where the emulated renderer puts it, in from the edge (2026-10-29,
   TODO 7 again): its highlight outline runs past the 4:3 picture's left edge (x -12 of 512), and
   a group touching the edge counts as a banner drawn from it, narrowed about the middle (the front
   end's rule, both renderers). The HUD's corners now differ from that rule: a group in the top or
   bottom 40% of the picture lying wholly in its left or right half - reaching past the edge or not -
   is pinned to that edge (`anchorOf`; `TF_NATIVE_HUDCORNERS=0`: the front end's rule).
   The HUD's corners kept pinned whatever 2D shows (2026-11-02, TODO 122): the Mini-Con ring and the
   top bars went back to their 4:3 places while the autosave notice showed (its last line of text a few
   rows above the ring joined the ring's group by the letters' line gap, and the group then reached the
   picture's middle), while the HUD came back from a menu or cutscene (still grouped with the panel
   sliding out), while a Mini-Con icon flew into the ring with its trail, and a comm call's lines left the
   left edge once one reached past the middle (Red Alert's tips; the first Mini-Con's three long lines never
   took it). Now (a) lines join across their gap only in one texture page; (b) a group holding pieces that
   stood still at an edge the frame before keeps that edge while it stays in the top or bottom 40% or in
   that half; (c) every corner piece is remembered (place + texture, `pins`): while it still lies in its
   corner it keeps its edge whatever it is grouped with, also on the frame it comes back; (d) what lies
   inside a full-width box in the top or bottom 40% and starts in its left quarter - a comm call's text -
   keeps to the left edge (the "Open" prompt in its bar, further in, stays about the middle).
   `TF_NATIVE_HUDSTICKY=0`: as before; `TF_NATIVE_GROUPLOG=1` logs the groups (extent, anchor, members,
   remembered pieces, texture page) and the full-width boxes each time they change.
   The text drawn outside the 4:3 picture (2026-11-02, TODO 123): the PS2 shows its 512 pixels across
   and the game draws what lies beyond them carelessly - a description sliding in, a line revealed letter
   by letter, the More Info card opening: the letters still off the screen are squashed into long strips
   (the Mini-Con panel's lines start 352 pixels left of the picture, each line's first piece a strip
   reaching to the visible text). Narrowed about the middle for a wider picture that part lands in the
   margins: long white streaks until the text settled (a 4:3 picture of the same moment has none). Each
   draw of a text page - a texture page drawn as 8 or more letter-sized pieces in the frame - now has a
   scissor where the 4:3 picture lands under its own narrowing: nothing the PS2 shows is cut, the panels
   around the text still reach into the margins. `TF_NATIVE_TEXTCLIP=0`: drawn whole. The front end's
   screens (2D alone - the Extras ticker and icons) are drawn by the emulated GS's front end
   (patch 0004 `squeezeOverlay`), not here.
   The HUD's thin box lines (2026-11-02, TODO 125): the GS's CLAMP held a coordinate half a texel from
   the texture's edge. The large images (the HD set) are drawn far smaller than their size - a Mini-Con
   icon of 1024 x 512 texels at about 90 pixels - and filtered from a smaller level, over several texels:
   held only half a texel in, the filter still reached round to the far edge. The icons are mirrored
   halves meeting at u = 1, and their quads' edges showed as thin vertical lines over the bars and through
   the icons (and in the Mini-Con ring). Now the coordinate is held by the pixel's footprint where that is
   larger than a texel; magnified - every texture at the PS2's size - nothing changes. Found by elimination:
   the lines stayed with the full-screen 8-pixel column pass left out (`TF_NATIVE_SKIP`) and with the large
   images uncompressed (`TF_HIRES_BC=0`), went with the large images off (`TF_HIRES=0`), and the emulated
   renderer (the game's own textures) has none.
   The More Info card's frame (2026-11-02, TODO 128): the card's two panels reach from the 4:3 picture's
   left edge to its right, and the vertices at the very edges stay at the wider picture's (so a banner
   drawn from the edge keeps its end there). The card is narrowed about the middle, so its outermost frame
   pieces - 32 pixels each - were stretched from the card out to the screen's edges: the right panel's
   edge drawn as wide light-blue bands, the left panel reaching the screen's left edge. A group reaching
   both edges of the 4:3 picture is now narrowed whole: the frame has the same thin edges on all sides
   (Optimus, Hot Shot and Red Alert checked at 2.4:1). The dimming behind it is a single full-width draw,
   not in a group: unchanged. `TF_NATIVE_CARDEDGES=0`: stretched, as before. `TF_NATIVE_GROUPLOG=2` also logs
   each 2D draw's anchor and the displayed buffer's 2D draws left unnarrowed (and why).
   The hint lines kept in the middle (2026-11-02, TODO 130): rule (d) of TODO 122 - text in a full-width box in
   the top or bottom 40% starting in its left quarter keeps to the left edge, for a comm call's lines - also took
   the HQ's centred hint line ("[X] View Mission Sites  [<][>] Change Menu", 91..420 of 512) to the left edge: its
   middle at a third of a 21:9 screen. A line as far from the box's left edge as from its right (within 6% of the
   width) now stays about the middle; a comm call's lines start at 32 and end at 376 at most, and still keep to the
   left edge. Checked at 16:9 and 21:9 with the pad's and the keyboard's symbols (`TF_KEY_PROMPTS`).
   The More Info card's left panel (2026-11-02, TODO 131): TODO 128 narrowed the whole card, the pieces at its
   two edges too, and the left panel and the title bar then ended in a hard cut at the 4:3 picture's edge. Those two
   are drawn from the left edge with their texture taken from inside (u 0.01-0.02, the panel's fill) - as if they
   went on past the PS2's screen - so they keep their end at the wide screen's edge as before; a piece whose texture
   ends at the edge (the right panel's frame, u 1) is the card's end and stays narrowed with it. The texture
   coordinate is s/q: the card's sprites come with the GIF's STQ. Checked at 16:9 and 21:9.
   The Mini-Con ring with Safeguard on (2026-11-02, TODO 122 / TODO 7): after a save was loaded with Safeguard
   equipped, the ring sat at its 4:3 place with a long red outline stretched back to the screen's edge, and pinned
   to the corner it was still drawn wrong - the red rim off the segments as a smeared arc, the left segments wider and
   paler. The game draws the ring the same in its 4:3 view and in widescreen (the native renderer's log of both: a
   181 x 181 ring texture from x -24, Safeguard's red sweep a fan of radius 90.5 centred at 78,360 from 172 to 278
   degrees, its colour pulsing red to pale in both). Three causes, all in the narrowing: (a) the swoosh rises to 58%
   of the picture's height (y 259 of 448), so the ring was not taken for a bottom corner - a group lying in one half
   and reaching into the bottom 40% from no higher than the middle now is (the autosave notice's centred paragraph,
   18..448 x 248..418, stays about the middle); (b) every vertex outside the 4:3 picture's edge band stayed at the
   wide picture's place (TODO 128's "a banner drawn from the edge keeps its end there"), also the ring's vertices past
   the edge - the left of the ring stretched while the rest was narrowed (about the middle: TODO 7's long outline); a
   draw pinned to an edge is now narrowed whole beyond that edge (its pivot is the edge: a vertex at it stays put);
   (c) the pivot was half a GS pixel right of the screen's origin (the half-pixel move), narrowed 2D sat 0.4 of a GS
   pixel right of its 4:3 place at 21:9. Measured against the game's 4:3 view (the same frames' pulse phase, the
   corner move and one scale only): the whole ring's overlap 0.79 before, 0.96 after at 16:9, 21:9 and 32:9.
   `TF_NATIVE_PINNEDEDGES=0`: (b) and (c) as before.
   The anchors in the frame they are decided (2026-11-03, TODO 136 / 138): the group anchors (with the kept edges
   and the remembered corner pieces) reached only the next frame - the list it matches its pieces against - and the
   frame itself drew each piece by its own place. An element's first picture, and every picture of a panel sliding
   in or out (its pieces' places new each frame), so took the anchor of whichever piece came first: the comm call's
   "(X) Confirm (O) Repeat" and "Respond" lines pinned left by their button symbol for a picture, then centred; the
   Mini-Con panel's first letters and two squares of its fill pinned left while the panel was narrowed about the
   middle (also the swap screen's two panels). Now the group's anchor is this frame's too, and the low-corner rule of
   TODO 122 (Safeguard) looks at a group's whole extent only, not each piece. `TF_NATIVE_GROUPNOW=0`: as before.
   Menu bars run on to the wide screen's edge (2026-11-03, TODO 145): the Stasis Lock menu's buttons and the pause
   OPTIONS screen's value panels are drawn from inside the picture to the PS2 screen's right edge - the right half of
   a 64-texel bar stretched across 400 pixels, which the TV cut off. Their menu reaches both 4:3 edges, so TODO 128
   narrowed it whole and they ended flat at the 4:3 picture's edge (78% of a 21:9 screen). A piece at an edge whose
   texture is stretched across it (more than twice its texels) now runs on to the wide screen's edge, as TODO 131's
   pieces do; the More Info card's frame pieces are drawn at their own size (32 pixels for 32 texels) and stay its
   ends. `TF_NATIVE_EDGELOG=1` logs each edge piece's vertices (x, u, v) and the decision on the saved frames.
   The Options screen's Music and SFX rows (2026-11-05, TODO 166) end in the same panels' box after the volume bars,
   its right half (32 texels) squeezed into the 20 pixels left to the PS2 screen's edge: drawn at less than three
   quarters of its texels it is a bar going on past the edge too, not a frame drawn at its size - it ran out as a
   thin sliver at the 4:3 picture's edge while the rows below reached the wide screen's; now all five rows do
   (21:9, `TF_NATIVE_SQUASHEDEDGE=0`: as before).
   Depth-only draws (2026-10-28, TODO 65/67): a draw whose FBMSK masks every colour bit but that
   writes Z was left out as "nothing written". An opened warp gate's portal is such a draw - a disc
   (FBMSK ffffffff, ZTE GEQUAL, ZMSK off) plugging the depth so the forest drawn after it fails
   there, the frame's clear left under the additive swirl (image 6b20's 0x2030 + 0x25e0 on WARP3,
   checked against VU1's own output: the routine's model was right). Left out, the forest showed
   through the portal with a faint reflective tint, and stepping into the gate showed the forest
   instead of the vortex the character warps into. `TF_NATIVE_DEPTHONLY=0`: left out, as before.
   A wrong large image (2026-10-29, TODO 72): the remaster's PROJ (the HQ band's glow - in the
   game a soft band of blue a few rows above its bottom edge) has a thin line on its very edge
   instead, drawn as a streak along the Drop Zones band; it passes the large images' checks (the
   image is almost all black, mean difference 0.51). A built-in list of such names keeps the game's
   own image (`TF_HIRES_BAD=0`: none; `TF_HIRES_SKIP` adds names). The names log
   (`TF_NATIVE_NAMES`) now gives each large image's "light kept" - the share of the game's light it
   has where the game has it (PROJ 30%); rejecting on it (`TF_HIRES_KEPT=1`) would also drop
   JUNGLE_FLOOR and the user's redrawn HUD icons, so it is a diagnostic only.
   Crisp text and PS5 button symbols (2026-11-02, TODO 119): the game's text is quads cut out of
   small font atlases (TFA2/FONTS, 256 x 256; each glyph's rectangle in its .VBN), so the sharp text
   is new atlases, not new drawing: tools\hd_textures\make_crisp_fonts.py makes them at 8x from the
   player's disc with every glyph in its own rectangle (the game draws the same quads - nothing
   moves): DECK (the menus, hints and prompts: ITC Avant Garde Gothic) drawn again from TeX Gyre
   Adventor (GUST Font License, bundled in tools\hd_textures\typeface) fitted into each original
   glyph's ink box and weight; USUZUFONT / HUDFONT / LITTLEFONT / TARGW24 rebuilt with sharp edges;
   the titles' 6MILL / UIFONT / BANKGOTHIC / WHITE18 left to the upscaled set (sharp already); the
   PlayStation symbols in DECK and USUZUFONT drawn in the DualSense's style. Three sets beside the
   upscaled tree in crisp_fonts\ (text_ps5, text_ps2, ps5); `TF_CRISP_TEXT=1|0` and
   `TF_BUTTON_SYMBOLS=ps5|ps2` (F1 Display "Sharp text" / "Button symbols", at the next start) pick
   one, nr_hires.cpp takes its image in place of the upscaled one (same name and native-twin match;
   its BC7 copy in the cache's crisp_<set> folder; no redrawn-art check - it is our own); off with the
   PS2's: none, the images as before. `TF_CRISP_DIR=<dir>`: another folder.
   Every font (the same day, the user's follow-ups): the fonts with no free twin - the titles' 6MILL /
   6MILL_COL, UIFONT, BANKGOTHIC, WHITE18, the HQ's USUZUFONT, HUDFONT, LITTLEFONT (AUTOSAVING), TARGW24 -
   are their own glyphs traced into vector outlines (tools\hd_textures\trace_glyphs.py: the coverage's
   middle contour, staircase straightened, stems snapped, corners kept, curves rounded; two-tone letters
   traced at both edges), each checked against the original at its size. Key prompts in the native
   renderer: the atlases with button symbols have a twin with the keyboard's keys (text_keys / keys sets, from
   controls.txt; the F1 Controls page makes them again on a rebind, --keys-only), loaded beside them
   (`Texture::keys`) and drawn instead while ps2_host_input::keyboardPromptsShown() (TF_KEY_PROMPTS and the
   keyboard / mouse the device last used) - nr_d3d.cpp textureFor, so the prompts follow the device from one
   picture to the next.
   The dash's after-image (2026-10-29, TODO 69): while the Slipstream dash runs the game draws the
   character a second time at the end of the frame with image 83f1's table `0x2220 + 0x1410 +
   0x0480` (batch type 4). `0x2220` (read off captured batches with tools/vu1_trace.py) moves each
   vertex whose normal faces its q0 by q0 (`min(max(q0 . N, 0) * 1e8, 1)`, a step) and replaces the
   colour: q2 there (128 grey, alpha 0), q1 elsewhere (alpha ~116) - a smear stretched backwards that
   fades out (`Routine::Smear`, mode.z 4 / colour mode 20; `TF_NATIVE_SMEAR=0`: a plain mesh). The
   kick is context 1, but the set's state block's TEX0_1 is a lit material's reflection map: the game
   writes TEX0_1 (the material's context-2 texture, the character's own), ALPHA_1 0x44 and TEST_1
   0x7001b into the display list itself (VIF DIRECT A+D after the block), which the GS draws with -
   the expanded frame takes those from the list's GIF state for these draws (`expandFrame`;
   `TF_NATIVE_SMEARSTATE=0`: the block's). Before, the character dashed as dark chrome (blue in
   Amazon, amber in Deep Amazon: the level's reflection map).
   The same on a destroyed enemy's flying pieces (2026-11-03, TODO 140; TODO 61's list override, `listOverrides`): the
   pieces' stretch of the list writes TEX0_1 for the creature's own texture with a fade-out ALPHA, but the chrome-lit
   enemies' (spider tank, Heavy Grunt) block has the reflection map as TEX0_1 and that texture as TEX0_2 - the
   override took the list's TEX0 only when it sampled the block's context-1 texture, so these pieces kept the
   reflection map and the block's blend: shiny blue-grey metal that never faded. A list TEX0 that samples the block's
   context-2 texture is now taken too, the draw's texture becoming that one (`TF_NATIVE_LISTSTATE2=0`: as before;
   `TF_NATIVE_LISTSTATELOG=<n>` also logs the list TEX0s not taken). Checked from the user's save states of a spider
   tank and a Heavy Grunt kill against the emulated drawing.
   Debug: `TF_NATIVE_BOUNDLOG=<n>[,<tbp>]` (a geometry set's bound texture beside what was last
   uploaded at its TBP and CBP), `TF_NATIVE_REUPLOAD=0` (a level uploaded again with other texels
   no longer converted again - `levelGeneration`), `TF_NATIVE_GSDUMP=<tick>,<tbp>,<tbw>,<psm>,<w>,<h>`
   (that region of GS memory as runs/gsdump_<tick>.ppm at the end of four frames; the EE's frame
   end, so the GS may be a frame behind).
   GPU cost (2026-10-29, TODO 62): `TF_NATIVE_PROFILE` prints the whole frame's GPU time and the
   render thread's wait for a frame slot; `TF_NATIVE_PASSPROFILE=1` adds a timestamp after every
   draw and sums the GPU time by kind of draw (3D with a large image, 3D through the clipping shader,
   2D, small targets, fog...) with the frame's costliest draws. Machines drift between runs (the
   other PC's GPU ran the same frames 5x slower in some runs), so A/Bs run in one run, by turns per
   300-frame window: `TF_NATIVE_CLIPFAST=alt` (the clipping shader's early way out for triangles
   wholly inside or outside - on by default, the same output; `=0` off) and
   `TF_NATIVE_ANISO_ALT=<n>` (another anisotropy, sampler s3); `TF_NATIVE_ANISO=<n>` sets it for a
   whole run. `TF_NATIVE_TEXLIST=1` (with `TF_NATIVE_TEX1LOG=1` on a saved frame) lists each
   textured 3D draw's textures, size and whether the large image is in.
5. The PS2 path off. **Begun** (`TF_NATIVE_ONLY=1`, patch 0037): while the native renderer draws a
   scene the GS draws no primitive; VU1 still runs (its programs carry register writes and events:
   skipping them hung a level load), uploads and read-backs go on (the game's per-frame read-back,
   the sun glare test, reads a 64x64 buffer only the EE writes, docs/NOTES.md). Menus and movies
   kept the emulated picture until stage 12 (roadmap N3b). Amazon at a 120 target: 114-117 fps (the
   emulated renderer alone 119.6). Next: VU1's work skipped where its register writes are known (stage
   6), the display list itself not built (N5).

6. VU1's drawing work left out (TODO N2, 2026-10-27; **on by default with the native renderer since
   2026-10-29** - commit 87a9ab3; `TF_NATIVE_NOVU1=0` turns it off, `=1` forces it): while the emulated drawing is off (a frame the native renderer draws),
   every batch run of a known dispatcher (MSCNT at 0x00f8 / 0x0010; the shared core's images and the
   grass image) is not run: no vertices, no PATH1 packets. Kept: the end pc 0x00f8 and the header's
   type cleared; the set-up programs (MSCAL 0 / 0x450, the grass image's) run as before; unpacks, VIF
   interrupts and DMA are unchanged (`vu1_native.cpp`, `novu1`). `TF_NATIVE_NOVU1=check` (slow) runs
   everything as before and keeps beside it the VU1 world in which those runs were skipped: the
   differing data words and registers; a run that is not skipped is run on the microcode in both worlds
   (PS2Recomp patch 0042: `Run::microcode`, the after-run hook) and its packets compared, with the
   register or word that makes the difference named; the EE's loads of differing words are reported
   (patch 0042's data observer); a quarter of the skipped runs' packets are read for the GS registers
   they write. Findings, Amazon (HQ, level load, level start; `runs\n2_chk4.err`): while skipping, 0
   packet differences (~180,000 runs skipped every 2 s, the set-up programs alike in both worlds); the
   skipped packets write only PRIM, RGBAQ, ST, XYZF2, XYOFFSET_1/2, SCISSOR_1/2, SCANMSK (and 0x23) -
   no IMAGE transfer, no SIGNAL / FINISH / LABEL, no FRAME / ZBUF / TEX0; differences appear only in
   the first batches run again after the emulated drawing comes back on (HQ -> level load, level
   start: ~5,000 runs) - their microcode reads lanes past its input that earlier batches' output left
   (e.g. q0b4.z, q165.w), so only the emulated GS's packets of those transition frames differ (the
   native picture is shown then; the GPU renderer's mesh path already leaves VU memory unlike the
   microcode's); the EE's loads of differing words (151) are VU1Module::Start's save of all 16 KB of
   VU1 data before the warp effect (0x26EEC0), restored as saved by Stop - not a use. The baseline the
   skip removes (Amazon level, 120 target, `PS2X_VIF_STATS`): ~555,000 VU1 runs and ~280 ms of the
   VIF1 thread a second (73 ms of it handing packets to the GS front end), plus the GS front end's
   processing of those packets. Not done yet: a run with `=1` (frame cost, pictures on vs off).

7. Game textures without stalls (TODO 76/77, 2026-10-29). An HQ screen change draws ~25-50 game textures
   for the first time in one frame; each was two committed resources (texture + staging buffer), 1-2 ms
   of driver time apiece on the test PC (48-93 ms frames, more with the video memory over budget).
   Now: placed resources in 64 MB heaps of our own (`pool`, first-fit, a range given back when the frame
   that let its texture go has finished; the next heap made ahead on a thread; `TF_NATIVE_TEXPOOL=0` off),
   staged through a ring in one upload buffer (24 MB a frame slot); mips box-filtered two channels at a
   time; a budget of ~3 ms of creation a frame (`TF_NATIVE_TEXBUDGET=<ms>`, 0 none) - past it a texture
   goes to the uploader and its draws take a stand-in (slot 0's white times the texture's average colour,
   the `standIn` constant), made for real by the next frame with budget left; releases always on the
   releasing thread. `TF_NATIVE_SLOWMS=<ms>`: the slow-frame lines' threshold (default 50).
8. Supersampling (`TF_NATIVE_SSAA`, a Graphics setting, off by default; `=alt` toggles it per
   `TF_NATIVE_PROFILE` window for an in-run A/B): the GS buffers' targets at 1.5x the picture's pixels each
   way, read into the picture through four bilinear taps a pixel (`downsample`, the `down` constant).
   Only where the 3D is drawn into a buffer copied into the picture (the levels' fields; a screen that
   draws its 3D straight into the displayed buffer is not supersampled).
9. The large images' video memory (TODO 94, 2026-10-30): (a) one large image per game texture's content - the
   capture's entries are by TEX0, and a character's texture came with ~12 (other GS pages per upload, both
   contexts' TFX), each a large image and a GPU copy of its own: the HQ held 13.6 GB of them. Now shared by
   name + size + texels (`TF_HIRES_SHARE=0`: as before). (b) BC7 (`ext/bc7enc`, mode 6, ~31 Mtexels a second a
   loader thread; `TF_HIRES_BC=0`: RGBA), a quarter of RGBA's memory and bandwidth. (c) A cap from the adapter's
   video memory (`d3d::largeImageCap`: under ~5 GB 2048 a side, under ~3 GB 1024; `TF_HIRES_MAX=<n>`;
   `TF_NATIVE_VRAM_MB=<n>` pretends another card) - the chain read from the level of that size. (d) A disk cache
   of the compressed chains beside the upscaled tree (`<upscaled>/../upscaled_bc7_cache`, `TF_HIRES_CACHE=<dir>`,
   `=0` none; per machine, keyed by the PNG's size and time): the first run decodes and compresses on the
   loaders (the game's own textures drawn meanwhile), later runs read ~5 ms an image. (e) Eviction: over the
   budget (`d3d::largeImageBudget`: the video memory less 1.5 GB; `TF_HIRES_BUDGET_MB`) the large images drawn
   least recently (none for `TF_HIRES_IDLE` s, default 5) are let go of, their game textures drawn, asked for
   again when next drawn. `TF_NATIVE_PROFILE` prints the large images' MB, the targets' MB and the cache's counts.
10. Quality presets (TODO 94, 2026-10-30): Graphics -> "Quality (native renderer)", `TF_NATIVE_QUALITY` =
   auto (the default: from the card's video memory - under ~3 GB Low, ~5 GB Medium, ~10 GB High, else Ultra) /
   low / medium / high / ultra; a restart. Sets anisotropy 4 / 8 / 16 / 16, the large images' largest side
   1024 / 2048 / 2048 / as made (never above the memory cap of stage 9), and on Low and Medium the GPU's own
   clipper for the core's clipping (`TF_NATIVE_CLIPGS=0`'s path: the same planes, the colour perspective-correct on
   the clipped triangles only) in place of the clipping shader. Each part yields to its own variable when that is set
   (`TF_NATIVE_ANISO`, `TF_HIRES_MAX`, `TF_NATIVE_CLIPGS`). Measuring aids, all by turns per profile window (in-run
   A/B): `TF_NATIVE_SKIPKIND=<kind>|all[a]` (a kind of draw not drawn, its state still set: its marginal cost),
   `TF_NATIVE_FLATPS=1` (a flat pixel shader), `TF_NATIVE_ONEPSO=<mode>` (pipelines unified: `y` blend + depth + no
   clipping shader, `W` depth writes on everywhere, `O` off everywhere, `g` no clipping shader...),
   `TF_NATIVE_CLIPGS=alt`, `TF_NATIVE_MESHVRAM=alt`, `TF_NATIVE_CBVRAM=alt` (meshes / constants in video memory -
   no effect measured, off), `TF_NATIVE_PSOLOG=1` (a saved frame's pipelines draw by draw); `TF_NATIVE_PROFILE`
   prints a frame's passes, pipeline changes, target changes, barriers and indices.
11. The GPU frame's cost, second pass (TODO 94, 2026-10-30). What a frame's GPU time went on, measured draw by draw:
   not the number of draws but the pixel shader - the one shader with every path in it ran at a fraction of the
   GPU's pixel rate (a do-nothing shader in its place: RX 5700 XT 7.9 -> 2.7 ms). (a) Shader variants: each draw
   takes a variant of the same shader compiled with only the paths it uses (`PSV_TEX` untextured / a game texture /
   anything, `PSV_LOD`, `PSV_CLAMP`, `PSV_ATEST`, `PSV_DATE`, `PSV_CULL`; `psv::psVariantOf` picks it from the
   draw's constants, the reads of render targets and the debug modes keep the whole shader). Compiled, and their
   pipelines made, on a thread of their own the first time a draw asks; the whole shader until then - the same
   pixels either way. A variant with no alpha test, destination alpha test or culling has no `discard`: the GPU can
   test depth before shading it. `TF_NATIVE_PSVARIANTS=0` / `alt`. (b) Instancing: an instanced draw's passes
   (the forest: 6-10 a mesh) are one draw - every draw reads its matrix and q0 from a per-instance vertex stream
   (`IROW0-3`, `IPZERO`; `kInstanceRing` entries a frame slot), the rest of its constants as before.
   `TF_NATIVE_INSTANCING=0` / `alt`. (c) A scissor, blend factor or pipeline the same as the previous draw's is not
   set again (`TF_NATIVE_STATEFILTER=0` / `alt`). Measured (Medium, 1920x1080, in-run A/B, before -> after): RX 5700
   XT Amazon 7.01 -> 5.63 ms, HQ 3.88 -> 2.87; GTX 1080 Amazon 5.42 -> 2.95, HQ 2.49 -> 1.28. Checked: with
   `TF_NATIVE_SHOTAB=1` a shot frame is drawn twice in the run, these three off (`<shot>_a.bmp`) then on (`_b`) -
   identical bit for bit (Amazon and the HQ, 10 frames). Measuring aids: `TF_NATIVE_DRAWCOST=<window>[,...]` (with
   `TF_NATIVE_PASSPROFILE`: every draw's GPU time averaged over those profile windows, printed with its state,
   target, texture, the barriers and copies before it; `TF_NATIVE_DRAWSTATS=1` adds its pixels shaded and triangles
   - those queries slow the frame, so not for times), `TF_NATIVE_ZWALT=<mode>,<mode>,...` (window w takes mode
   w % n: depth writes forced on by class of draw - 1 reading a target, 2 sprites, 4 2D, 8 the other 3D, 16 the
   draws with no depth test; 128 a do-nothing pixel shader, 256 one that only reads the interpolated values, 512 the
   variants without the alpha test). The old depth-write A/Bs (stage 10's `ONEPSO=W`) were a broken picture: forcing
   depth writes on made the game's full-screen 2D passes, drawn with no depth test before the 3D, write their depth
   over the whole field - everything after failed the depth test and the picture went black (`ZWALT` 16 shows it).
12. Calls that stop part-way (TODO 118, 2026-11-01). A recompiled function stops part-way when the EE scheduler's
   checkpoint falls inside it (a VBlank due, its time slice up - at one of its calls): the host stack returns to the
   dispatcher with pc inside it, and it is entered again later through that place's own entry in the function table,
   not its start. The capture's wrappers were registered at the start alone, so a RenderGeometrySet that stopped looked
   "culled" (its DMA call not added yet) and its end ran unseen: the set was missing from that frame. At 80-240 frames a
   second a VBlank falls at the same point of a frame for many frames in a row - Antarctica's near snow ground was left
   out for whole stretches (the sky and the dark sea under it showing through: the "black flicker"), back only in the
   frames where no VBlank fell inside that call; the emulated GS drew it every frame. RenderGeometrySet's and the matrix's
   wrappers (the two that read what their function wrote) are now registered at every place their function is entered
   again and keep a stopped call's arguments, by its stack pointer, until it ends (`OpenCall`, `wrapResumePoints`;
   `TF_NATIVE_RESUMEWRAP=0`: as before). TODO 127 (2026-11-02): the microcode primitives' two ends (sprites, glows,
   beams) and the texture group loads too - they also read the packet pointer after the call. A warp gate's opening
   shot showed its light rays as flat rectangles filled with a blocky picture (the glow's shrunk copy of the screen) in
   the pictures where the rays' strip end stopped in its padding loop: the strips were read from words not yet written,
   with the list's texture state of the glow passes. At a fixed 120 the first gate in Amazon showed 8 such pictures in
   every run on both workers with `TF_NATIVE_RESUMEWRAP=0`, none with the wrappers. A test aid:
   `TF_NATIVE_FORCEYIELD=<mask>` makes the wrapped calls stop part-way on purpose (a checkpoint asked for at their start:
   1 RenderGeometrySet, 2 the matrix, 4 the group loads, 8 the primitives' ends) - with the wrappers the pictures are
   the same as without it, with `TF_NATIVE_RESUMEWRAP=0` the gate's rays vanish. Checking aids: `TF_NATIVE_STREAM=<from>-<to>` (every frame handed over between those ticks
   saved without waiting for the GPU - a shot waits for its frame - shrunk `TF_NATIVE_STREAM_SHRINK` times (3) into
   `runs\stream\s_<sequence>_<tick>.bmp`), `TF_NATIVE_SETWATCH=<set, hex>` (every call for one geometry set: drawn or
   not, its box, the cull flag, the clip-space matrices).
13. The emulated drawing never back on, and the GS left with local memory only (roadmap N3b + N4, 2026-11-01/02;
   PS2Recomp patch 0061). (a) N3b: with the native renderer alone every frame of 2D alone - the whole front end, the
   memory-card and legal screens, the boot films, the loads, a level's film - used to switch the emulated drawing back
   on, and the window showed the GS's picture for them unless "smooth pictures" was on. The native renderer already
   drew them all (the game puts its menus and films through the render device: RenderRect sprites of the film's
   vlTexturePS2): the emulated drawing now starts on and goes off once, when Direct3D 12 is ready
   (`d3d::drawsPictures`), back only for a lost device (`TF_NATIVE_EMU2D=1`: the old rule); the window shows the
   native picture for every frame and keeps it up while none comes (nr_present.cpp). Native shots of the boot films
   are bit-identical to compare mode's; front end and HQ within ~5 levels of the GS's. (b) The one screen not drawn
   through the render device: the warp tunnel of the loads between the front end and the HQ and the HQ and a level -
   the loader's own GIF packets, no BeginFrame, the display switched to two other circuits (PMODE 8007). Patch 0061's
   `setGifObserver` hands the packets reaching the GS to `looseGif` (nr_capture.cpp): while the game's frames have
   stopped (over two VBlanks) and the display is not its frames', they are gathered up to each display flip and drawn
   as a frame of their own (`Frame::looseGif`, read as the list's 2D in `expandFrame`), the picture the buffer the last
   draw went to; their T4/T8 textures are read from GS memory (`textureFromGsMemory`, the game's own IMAGE uploads -
   kept by the GS with its drawing off); Z clamped to the depth format and all-negative Q flipped (the tunnel's VU1
   output: Z 2^31, Q < 0 - the GPU side clipped every triangle). The tunnel matches the GS's (2026-11-02, compare mode frame by frame, both tunnels: R/G within
   ~4-8 levels, B ~13 - the cloud's single pixels): PRMODECONT/PRMODE honoured (the flash blended), GS points drawn
   (one pixel each, the cloud), PMODE's circuit merge (circuit 2 shown: the buffer the game began its previous whole
   picture in), no widescreen narrowing for these frames, indexed textures always from GS memory. TODO 120: the
   scene's first frame after such a load has its 3D left out, as the GS shows black there; a frame of 2D that draws
   nothing into the buffer shown is not shown either - the window keeps the picture before (the native clear colour
   flashed dark grey; `TF_NATIVE_EMPTYFRAMES=1`: shown, as before). TODO 126: a 2D draw whose texture coordinates stay
   inside its texture (u, v within 0..1) is held to the edge texels like a CLAMP - at a higher resolution than the
   PS2's the pixels nearest the edge sampled past the last texel and the filter wrapped round: a dark line through
   every Extras gallery picture (a strip 512 texels wide and a column beside it), a light one under the Extras
   menus' title bar (`TF_NATIVE_EDGEHOLD=0`: wrapped, as before). TODO 27 (the tunnel's shake): its two 224-line field
   buffers are drawn half a line apart (XYOFFSET Y 1936 into FBP 0, 1936.5 into 0x46) - on the interlaced screen each
   field sits on its own lines; shown whole, every frame jumped half a field line (three pixels at 1440). A loose
   frame's half line is dropped (`TF_NATIVE_LOOSEFIELD=1`: kept). The tunnel's look itself - the teal wash, the
   radial streaks, the cloud of separate points - is the PS2's: the software GS at its own 640 x 224 draws the same.
   TODO 133 (the ground upside down / black slabs): where the replayed field A matrix differs from the one the capture saw
   sent for the set, the sent one is taken - Amazon's river reflects by mirrored copies of the ground drawn with the
   ordinary camera under the ground; the replay had kept a mirrored transform for them (the ground as a ceiling, the real
   ground black); the starship's corridor went black the same way at its warp gate (`TF_NATIVE_MATRIXPICK=replay`: as before).
   Checking aids: `TF_NATIVE_MATRIXCHECK=<n>` (3D draws whose replayed matrix differs from the sent one),
   `TF_NATIVE_MESHCHECK=<n>` (cached sets whose packet changed since decoded). `TF_NATIVE_LOOSE=0` off;
   `TF_NATIVE_LOOSE_SHOT`, `TF_NATIVE_LOOSELOG`, `TF_NATIVE_LOOSETEX`, `TF_NATIVE_EMULOG` debug. A read of the display
   buffer from GS memory (`gsDisplayedPicture`, for a film DMA'd straight to the display) is kept, off by default
   (`TF_NATIVE_GSMOVIE=1`): no screen of this game needs it. (c) N4: `ps2_host_display::gsNativeAlone()` (the
   emulated drawing off): the GS front end sends no draw state or CLUT loads, the GS thread (`GSThreadedBackend::run`)
   takes host-to-local uploads, local writes, transfers and read-backs only - no primitive, mesh, state, CLUT, picture
   or GPU-renderer work; back from it the GPU renderer reloads local memory. `PS2X_GS_NATIVE_IDLE=0`: the old path;
   `PS2X_GS_NATIVE_LOG=1/2`: what the GS thread took. What is left on it is the uploads (`GSCpuBackend::UploadImage`):
   the light maps and the tunnel's textures are read from GS memory.
14. The particle pipelines (TODO 144, 2026-11-03; `nr_particles.cpp`). The game's particle effects - smoke, sparks,
   the rings and trails of shots, the Mini-Cons' beams - are vlxParticleManager's 18 "vlxPPP_*" pipelines: not geometry
   sets and not microcode primitives, but a VU1 microcode image each (ELF 0x005E3BC0..0x005F9F50), uploaded by MPG when
   the render list switches to it, started by MSCAL there, fed its particles in UNPACK + MSCNT batches; the image itself
   expands every particle into its primitives and XGKICKs them. Nothing on the EE side makes the corners, so the capture
   had nothing to draw and the native renderer drew no particle effect at all. Now the renderer's thread runs those
   programs itself while it replays the list (`replayListRunning`, nr_shader.cpp): a MSCAL whose 64 bytes of code are a
   pipeline image's starts the mode, its MSCNTs run on the list tracker's own VU1 memory through PS2Recomp's VU1 engine
   (`ps2vu1fast::run` with a capture buffer: the XGKICK packets kept, not sent), and the packets go through the list's
   GIF reader at their place in the list, as the GS takes them - GS-space primitives with their Z, drawn with the list's
   state (`TF_NATIVE_PARTICLEFX=0`: not drawn). VU1's own runs go on as before. (b) Every particle is drawn twice, with
   a header of A+D writes per copy (SCISSOR_1, SCANMSK: field A's and field B's, the tables D_0061AB50 and D_0061AB80 that
   the game's field set-up rewrites); func_002516C0 copies them to VU data. Without betwixtMode (frames of one field: the
   PC port's levels whenever there are no in-between pictures - the native renderer's every level) it copied the FIRST
   header twice, and in that mode the first is the pass of one pixel (SCISSOR 0-0): both copies went into one pixel, in
   either renderer. Replaced (the same VIF words, written by our own code): the second copy always takes the second header,
   as betwixtMode's packet does (`TF_PARTICLE_FIELDS=0`: the game's copy). Debug: `TF_NATIVE_PARTICLELOG=<n>[,<tick>]`
   (the first n program runs from that tick: start, ITOP, packet bytes; with `TF_NATIVE_PARTICLEDUMP=1` their packets and
   the VU memory into `runs\particle_*.bin`), `TF_NATIVE_PARTICLEVIF=<n>` (the VIF commands of the first n particle
   stretches), `TF_NATIVE_PARTICLEDRAWLOG=<n>[,<tick>]` (the draws made of their packets: place, Z, state, alpha),
   `TF_NATIVE_PRIMDROPLOG=<frames>` (the microcode primitives left out, by routine table). Checked (Amazon, Hot Shot's
   Aurora against a wall, the user's card, live at 60, other PC, fx144v/w): with the pipelines on, the impact's purple puff
   is drawn where the frames before had nothing; their draws carry the second header's full scissor.
18. The rain (TODO 146, 2026-11-03; `nr_rain.cpp`). Mid-Atlantic after the boss had no rain: taRain draws one prebuilt
   42-vertex "rain patch" per visible leaf of its oct-tree (taShader_Rain::RenderRainPatches 0x00565480 calls the
   patch's microcode client - func_221B80, a DMA CALL in the list) with a routine table of one routine, the rain image
   (ELF 0x00602240, image e1472a8c96f62e62's 0x3950): no geometry set, so no draw. Its routine was read off captured
   batches (`PS2X_VU1_CAPTURE=3950/q35@e147#0`, `tools/vu1_trace.py`; a Python model matched the packets VU1 sent word
   for word but for Z's last bits): per patch two copies (the block's two field headers, two transforms), each a line
   strip of the patch's 14 drops, every drop a streak from the patch's mean position to where it is (copy 1) or from where
   it was to the mean (copy 2) plus the drop's offset, alpha falling with distance, ADC on a streak with an end outside
   the clip volume. Now built in C++ from the list tracker's VU1 memory at each of the rain's batches (the MSCAL / MSCNT
   whose table is that image's) and drawn through the list's GIF reader at its place, as the particle pipelines' packets
   are - no VU1 program run. Each pass's block (taShader_Rain::LoadContext's VU data: headers, transforms, the patches'
   positions) is put in the list itself (func_221CC0), and the pass's flush (func_23AB70 / func_23AA90) sends the list
   and starts it again from the same memory, so read later every pass had the last one's block (one set of patches drawn
   80 times): the capture copies each block before its memory can be written over (the next allocation, flush or the
   frame's end; `Frame::rainBlocks`), a rain MSCAL starts the next copy, its MSCNT batches are built from it. Cheaper than
   VU1's packets but drawn the same: the copy behind the pass of one pixel (SCISSOR 0-0, a frame of one field) left out,
   the drops the GS would not draw left out, the rest one line list a batch (`TF_NATIVE_RAINEXACT=1`: the packets exactly
   as VU1 sends them). A line under a GS pixel long is now drawn a pixel long, as the GS draws it (nr_gif.cpp; the far
   drops went missing). The drops sample the device's null texture (TEX0 at TBP 0x3FFE: white, alpha 0x7F in GS memory):
   a faint lightening of the sky - against the software renderer on 4d the same coverage (1.64 / 1.68 % of the sky's
   pixels lifted, by 8 / 7 on average). `TF_NATIVE_RAIN=0`: not drawn (as before); `TF_NATIVE_RAINLOG=<tick>`: that
   frame's rain passes and batches. Cost (this PC, RX 7900 XTX, 1080 lines, 4d in the rain, 30 profile windows each): the
   whole frame 1.98 ms of the GPU against 1.95 without the rain, the game's thread's capture 0.61 against 0.60 ms (the
   blocks' copies), 120 pictures a second held. (The first version - every packet VU1 sends, every pass with the same
   block - cost 1.6 ms of the GPU and 2-4 ms of the renderer's thread there.)

15. The warp tunnel of a load, native (TODO 27, 2026-11-03; `nr_warp.cpp`). The blue tunnel a load shows (the front end
   to the HQ, the HQ to a level, a warp gate back to the HQ) is a VU1 program of its own (0x606B00, 0x1420 bytes, its
   data 0x604900; taWarpTransitionEffect, the VBlank hook 0x26E990 runs it once a field) - not the render device's, so
   the capture saw only its GS packets, which stage 13 drew as a load screen: 900,000 vertices a picture made on the
   processor, each spark one GS pixel, the 640 x 224 picture stretched across a wide screen, and a picture only when a
   run of the program came - the EE waits for VU1 there, so during a load 11-28 pictures a second with gaps of up to
   0.6 s. Now the program is left out (src/vu1_native.cpp's hook: the run is ours, nothing sent; its inputs - the bar's
   progress (qw1022), the seed and the flash/fade bits (qw1023) - taken, qw20 the run count) and its work done on a
   thread of its own ("NativeWarp", above normal priority) from the real clock - a step of the program every 1/59.94 s, as the console's VBlanks
   ran it - with a picture for every refresh of the screen (at most the frame-rate setting), each at its own moment: the
   frame count, the emitters' clocks, the bar and the fade fractional between steps, so the motion is smooth at any rate
   and a stalled load does not stop it. What the program does (read from the microcode, checked step by step against a
   model of it): three passes (two additive, one subtractive) of a cone of four textured bands round 37 points, the
   textures (3 x 64 x 64, 4-bit, from its own upload packet) scrolled by the frame count; 20 emitters of sparks, each an
   iterated function system's orbit (the chaos game: six random affine maps - three axis turns, a scale, a move - VU1's
   random register choosing the map for each point, the same sequence for every emitter) of min(30000 c^2, 32000) + 10
   points, turned by 0.2 c + 0.004 F about the view axis and flown from z -10 towards the eye (+11 c) as its clock c goes
   up 0.004 a step, a new emitter at 1; the bar from the middle out to the load's progress (120 steps to each value); the
   opening flash (additive, 255 - 3 F) and the closing fade (subtractive, 8 a step). The orbits are made once an emitter
   (a mesh of 32,010 points) and moved by the GPU every picture (nr_d3d.cpp `drawCloud`, its own pipeline: a point an
   instance, a soft dot of a PS2 pixel's area at the picture's resolution - a pixel of the 224-line field, two lines of
   the shown frame; Gaussian, 0.4 of the PS2 pixel - adding in all what the one-pixel point added; against the emulated
   GS at scale 1 the pictures' colour within a few percent). Hor+ on a wide screen: the GS's x narrowed about the centre (the cone and
   the sparks reach out to the sides, the bar keeps the 4:3 picture's size), a fifth band nearer the eye continuing the
   cone for the corners of a picture wider than 16:9 (the first band's nearest row drawn on outward - continued, T would
   wrap round to the subtractive texture's brightest rows: a black rim). The load screen's own GS packets (the field
   buffers' clears) are not drawn meanwhile (looseGif), and the game's first frame of its own ends the tunnel's pictures.
   With the EE no longer waiting for VU1 the loads are shorter too: HQ to Amazon 218 VBlanks in 1.9 s on the test PC
   (13.0 s before). `TF_NATIVE_WARP=0`: the program and its packets as before; `=check`: as before, and the native steps
   beside the program's runs, compared run by run (clocks, places, colours, maps; `[native warp:check]`).
   `TF_NATIVE_WARP_SHOT=<frame>[,...]` (those frames of each tunnel, runs/native_warp_<tunnel>_<frame>.bmp),
   `TF_NATIVE_WARP_HZ=<n>` (pictures a second), `TF_NATIVE_WARP_DOT=<x>` (the dots' size), `TF_NATIVE_WARP_LOG=1` (a line
   a second).

16. The movies' pictures from the decoder (roadmap N6, 2026-11-03; `nr_movie.cpp`, PS2Recomp patch 0072). There is no
   emulated IPU in this port: Sony's libmpeg is replaced by the runtime's own (Kernel/Stubs/MPEG.cpp), which decodes the
   game's MPEG-2 with FFmpeg's avcodec (linked, the DLLs beside the exe) and swscale. What was left of the console was the
   picture's way to the screen: the decoder wrote each 640 x 384 picture into the game's vlTexturePS2 bitmap in the IPU's
   16-pixel macroblock columns, the game had the texture uploaded again (0x266570), the uploader snapshotted the 1 MB of
   texels, and the native renderer converted the texture back - the level laid out in a 4 MB GS-memory image and read out
   - for every picture, on the game's thread. Now sceMpegGetPicture hands each picture it serves to `nr_movie.cpp`
   (`runtime/ps2_mpeg_native.h`: RGBA, its number, the game's buffer, the VBlank), which keeps it by the buffer's
   address; when convertKey meets a texture whose level holds that buffer the picture becomes the texture directly (its
   own texels at the top left, its last row and column held across the rest of the power-of-two texture), named by a
   generation of its own. While the native renderer draws alone the game's copy is not written at all (a lost device's
   emulated GS still gets it) and its snapshot is skipped. The picture is dropped when the movie's vlTexturePS2 is
   destroyed (the game keeps one sceMpeg object from movie to movie). The colour: the chroma interpolated to every
   pixel across (swscale's fast path gave each pair of pixels one value), ~0.05 ms a picture more - bicubic and accurate
   rounding were measured at 1-3 ms a picture more and left out. Nothing in the game's movie logic changes: the same
   calls, the same picture at the same VBlank, the sound untouched - start, skip, end, the per-character films, the
   previews' own pace (TODO 113). Measured (TF_NATIVE_MOVIELOG, the user's settings at 240): the picture's way from the
   decoder to the texture took 6.5 ms a picture on the test PC (5.9 the conversion of the game's copy + 0.6 the copy; 1.45 -> 0.08 for the 512 x 256 previews)
   and 4.1 on this PC (3.7 + 0.4), now 0.4 on both (decoding 0.3-0.7 either way); a movie frame's moment-to-ready 17.0 ->
   10.8 ms (test PC), 8.9 -> 5.4 ms (this PC), the time between pictures as steady (spread ~0.9 ms). Every picture served
   was drawn, none twice: the logos and the attract film, the story intro (3,389 pictures), the Amazon film N1_1 (1,349),
   the Drop Zones' previews (30 a second at the HQ's 100-160 frames, three levels' previews in a row), a film skipped after its
   fifth picture; the sound blocks sent stay
   within -60..+95 ms of the pictures' time for whole films (no drift). `TF_NATIVE_MOVIES=check` (the game's copy written
   and converted too, compared picture by picture, with the old colour): 7,847 pictures (logos, attract, story intro, the
   512 x 256 preview, the Amazon film), 0 pixels differing.
   `TF_NATIVE_MOVIES=0`: the old path exactly (no sink, the old colour); `TF_MOVIE_COLOUR=old|accurate`;
   `TF_NATIVE_MOVIELOG=1`: every 2 s and per movie - pictures served / drawn / not drawn / made twice, their spacing in
   VBlanks, the sound against the pictures (slMusicStream::SendStreamData blocks, 0x1EB950), the host ms a picture.
   Not done (renderer side): the GPU texture is still made new for each picture, its mips built on the render thread;
   the game's upload of the (now unwritten) texels still goes to the GS thread.
   The level previews in HD (TODO 159, 2026-11-05). The HQ's level select draws its preview (FMV/LEVELS/N0x*.M2V,
   512 x 256 MPEG-2) across most of the screen, so it showed the movie at 3-4x: blur, the MPEG-2 blocks, and combing -
   the previews are interlaced (bottom field first, 394 of 400 frames by ffmpeg's idet) though flagged progressive.
   `transformers/tools/upscale_previews.py` makes each a 2048 x 1024 copy with the same frames, offline and kept on this
   machine (movies_hd/LEVELS/<name>.mkv, git-ignored): deinterlaced (bwdif), deblocked, Real-ESRGAN x4plus
   (realesrgan-ncnn-vulkan, any Vulkan GPU), blended 60/40 with a plain Lanczos 4x (x4plus alone invents ragged spikes on
   fine silhouettes - Greenland's pines; the blend keeps the detail without them; animevideov3 was smoother but lost the
   rock and snow detail), H.264 CRF 16 - the 8 previews in ~35 min on the 7900 XTX, 314 MB. At run time the copy's frame
   N is drawn for the game's picture N (nr_movie.cpp previewTexture): an ffmpeg of its own decodes it (hd_movies_decoder
   Decoder, started on a thread of its own - again at each loop), the frame moved into a large texture with the game-size
   one as its fallback; a picture whose frame is not there yet is drawn from the game's. On a GTX 1080 test PC, Antarctica's preview:
   86 pictures from the copy, 4 from the game's (its start), 0.07 ms a picture on the game's thread; the copy against the
   game's picture 2.5 levels on average (4x4 blocks: a frame off would show); the level select's frame times the same with
   it as without. `TF_HD_PREVIEWS=0`: the game's pictures; `TF_HD_MOVIES=<folder>` (movies_hd), `TF_FFMPEG=<ffmpeg.exe>`
   (on the PATH otherwise) as for the full-window HD movies.

17. The emulation's remnants in normal play (emurest, 2026-11-03). A census first (`TF_VU1_CENSUS=<s>`, vu1_native.cpp +
   nr_capture.cpp: every program run offered to VU1's hook by its code and timed - skipped, native or microcode - and what
   reaches or reads GS memory). On every map and the HQ: (a) on the emulated VU1 only the particle pipelines were left
   (1.5-47 ms/s of the VIF1 thread; 4d 47, Cybertron 35) and a few set-up runs; (b) GS memory took the game's texture
   streaming - 30,000-110,000 host-to-local transfers a second in a level (170-580 MB/s), the GS thread 12-29% busy with
   them - and nothing read it but the light maps (T8H views onto the top bytes of uploaded CT32 groups, 0-146 reads per
   100 s) - no local-to-host transfer at all. (1) PS2Recomp patch 0073: while the native renderer draws alone those
   uploads (colour and indexed formats) are not queued for the GS thread; each destination's last data is kept (a shadow,
   ~15 MB) and written to GS memory in order when anything needs it whole - the emulated drawing back on (a lost device:
   checked with TF_NATIVE_REMOVEDEVICE, the GS's pictures after it identical on and off), a local transfer, a snapshot -
   and a native reader's region before it reads (`gsupload::ensureWhole`, nr_texture.cpp: the light maps, TEX0 reads).
   The top-byte and depth formats are made as before; the texture flushes (16,000 a second) are left out too. GS thread
   at a fixed 60: Cybertron 10.2% -> 0.4%, the carrier 5% -> 1.5%, the HQ 8.9% -> 0%; the game thread unchanged; native
   shots identical on and off (maps 1, 4, 8). `PS2X_GS_SKIP_UPLOADS=0`: as before; `=check`: every upload made, the
   blocks a skip would leave stale marked and every reader checked against them (`[gs:skip] check`; maps 1, 2, 4, 8: two
   stale reads, both light maps - what ensureWhole is for). (2) The particle pipelines' batches on VU1 left out like N2's
   (vu1_native.cpp `particleBatch`: the MSCNTs resuming 16 bytes into a pipeline's image; the renderer replays them on
   its own VU1 memory): emulated microcode in a level 0.05-0.12 ms/s; `TF_NATIVE_NOVU1=check` finds differences only at
   the warp tunnel's start, as without the skip; `TF_NATIVE_PARTICLE_SKIP=0`: as before. Left for N5: the VIF1 thread's
   unpacks (8-23% of a core at 60), whose skipping cannot be checked cheaply (an UNPACK comes before the MSCNT that
   decides whether its run is skipped).

19. The display list without VIF1's work (roadmap N5.0 + N5.2, 2026-11-03; `nr_n5.cpp`, PS2Recomp patch 0074). N5.0, a
   census (`TF_N5_CENSUS=<s>`: the render device's 85 slots and the list helpers wrapped, the bytes each writes into the
   list counted at the kick, BeginDisplayListDmaTransfer 0x25D2B0): a level's list is ~495 KB a frame (Amazon, Deep
   Amazon), Cybertron 258 KB, the HQ 206 KB; SetupContextTransferPacket 120 KB, the shaders' dirty contexts 230 KB with
   what they call, LoadMicrocodeContext 76 KB, SetGSRegister 66 KB, RenderRect 35 KB, BlendSurfaceRect 21 KB; code outside
   the wrapped functions ~1.2 KB. N5.2: while the native renderer draws alone (and not in the warp tunnel or a loader
   screen of the game's own), VIF1 goes lite (`ps2vif1::setLite`): an UNPACK writes nothing into VU1 data memory and
   MSCAL / MSCNT start no program; MPG, DIRECT, the registers, MARK and the interrupts as before - the list is still built
   and kicked, the renderer still reads it from main memory. The scan of each transfer (patch 0005: the walk of every
   VIF code of the gathered chain - ~4.9 MB a frame in Deep Amazon with the geometry it points at - for the i-bit
   interrupts, MARK and the readbacks ahead) left the game's thread too (`ps2vif1::setLiteScan` answering 3): while lite
   its walk of the GIF data is left out (those GS registers only serve readbacks ahead of the queue, answered the plain
   way while lite and for 120 after), and the rest is done on the VIF1 thread as the transfer comes up - the interrupts
   and MARK then come when VIF1 gets there, as on the console (0.2-0.4 ms after the kick on average; the EE read
   VIF1_STAT/MARK ~once a frame and never before its transfer's scan, `[vif1] 10 s: ... scans on the VIF1 thread`), and
   a scan back on the game's thread first waits for the pending ones. Deep Amazon at a fixed 60: the scan 0.39-0.66 ms
   of the game thread a frame -> 0. `TF_N5=0`: as before; `TF_N5_SCAN=ee`: the scan on the game's thread (its GIF walk
   left out), `=0`: the scan as before (the rest lite); `TF_N5=check`: VIF1 as before, VU1 data memory as lite
   transfers would leave it kept beside the real one from each lite start (the EE's stores applied) and every EE read
   of it compared (`[n5:check]`, with the reader's guest stack), and the GS readbacks met while the scan would be lite
   and the EE's VIF1_STAT/MARK reads before VIF1 starts their transfer counted (an upper bound: VIF1 runs at its full
   cost there). Checked on every map and the HQ: the only reads that differ are VU1Module::Start's save of the 16 KB at
   the warp tunnel's start (sub_0026EEC0 via memcpy 0x109110), put back by Stop unread; no readback while lite. Native
   shots identical on and off (maps 2-8, 4d, the HQ; map 1's distant foliage differs run to run anyway), menus and
   movies within their own run-to-run differences; the GS's pictures after a lost device (`TF_NATIVE_REMOVEDEVICE`)
   identical on and off.

20. The device's functions native, the first group (roadmap N5.3, 2026-11-04; `nr_n5.cpp`, `nr_capture.cpp`). While the
   native renderer draws alone in the game's frames, these run as our own code and write no packet: what they set goes
   to the renderer directly (`Frame::nativeRegs`, applied to the list's GS state where the packet would have been - before
   the first of the list's own tags at or after the place the function ran, the particle and rain pipelines' packets
   included - and at the latest before the draw), and into the register stream (N5.1's `TF_N5_NOLIST=check`).
   SetGSRegister 0x00225D60 (the GS shadow D_70000060 as it keeps it), ApplyStateBlock (slot 39, 0x002591F0: the block's
   material colour and the shaders' dirty flags, the block's A+D registers instead of the DMA REF of it), and
   vlTextureChange::UseTexture 0x0023C340 (TEX0, MIPTBP1/2 and TEXA as it and its helpers compute them - GetGSFormat
   0x00267C90, the TEX0 maker 0x00267D30, the levels 0x00267C80 / 0x00267CE0 - into the shadow, the cache's state as it
   leaves it; any texture class but vlTexturePS2's own goes to the original). The recompiled originals stay: for the
   GS (a lost device, the game's own screens - the registers kept from it since written to it once from the shadow,
   through the original) and as the check's oracle. `TF_N5_DEVICE=0`: as before; `=check`: each call also run through the
   original - into the list, rewound after - and compared ("[n5:device]": 0 differing in millions of calls on maps 1, 3,
   8 and the HQ; an original that stops part-way at a VBlank is left to finish itself); `TF_N5_DEVICE_OFF=<names>`
   (setgsregister, applystateblock, usetexture): those left to the original. The per-draw register check then finds the
   draws' TEX0 / ALPHA / TEST as the calls set them; what still differs - FRAME, ZBUF, XYOFFSET - comes from
   SetHardwareRenderEnvironment 0x00264DB0, whose native version needs the render-state slots it calls (0xB8 / 0xC8).
21. The device's functions native, the second group (roadmap N5.3, 2026-11-04; `nr_n5.cpp`). SetHardwareRenderEnvironment
   0x00264DB0 native, with the render-state slots it calls (GetRenderState slot 48 0x00257F80, SetRenderState slot 44
   0x00258690 - as far as it uses them: the state table D_70000B80, and for state 0x10 the GS context's switch, with
   vlTextureChange::ContextChange 0x0023C660's TEX0 again from the shadow when the context holds a special texture): the
   environment made the current one (0x00713540, the one replaced saved at 0x00713650 for a null environment to put
   back), its FRAME_1/2 (FBMSK from render state 0xF per context), ZBUF_1/2, XYOFFSET_1/2, SCISSOR_1/2, PRMODE,
   PRMODECONT and TEXFLUSH into the shadow and to the renderer - no packet. Checked against the original
   (`TF_N5_DEVICE=check`, `renderenvironment` for `TF_N5_DEVICE_OFF`). One difference from the original, on purpose:
   where it switches to a context with a special texture (the front end), ContextChange calls the recompiled
   SetGSRegister directly and its packet (DMA CNT, GIF tag, A+D) lands inside the environment's DIRECT - the GS reads
   that tag's two qwords as A+D writes (a PRIM write among them); the native one writes the TEX0 only. With it the
   per-draw register check's FRAME / ZBUF / XYOFFSET differences are gone in the levels (map 1: every draw differed,
   now tens a 10 s; maps 3 and 8 up to a few thousand of ~1M - FRAME_2 / ZBUF_2 written into the list by something else,
   the next to find); the front end's ALPHA_2 ones are older and not
   the environment's. Every swap now gates on `framesDeferred()` (TODO 149: with `TF_NATIVE_THREAD=0` the threaded
   renderer's frame work runs on the game's thread and the native functions with it; `=old` back to the originals).
   The EE clock: the scheduler charges cycles at the recompiled code's safe points (32 a loop's back edge, 8 a call or a
   jump to another function, returns free), and the game's timers, VBlanks and time slices run on that clock - a native
   function that skips them moves every later event. Without the environment's ~1150 cycles a call map 3 came apart from
   the original's run (the same input, another fight, deterministically, from the first fights on); so every native
   device function now returns through `n5::deviceReturn`, charging what its original's path charges (SetGSRegister
   and ApplyStateBlock nothing; UseTexture 32 + 8 for a palette texture + 56 a mip level + 8 for TEXA; the environment
   1152, 704 for a null one, + 8 a context switch), measured against the originals in `TF_N5_DEVICE=check` ("EE cycles",
   at the clock's scale). `TF_N5_CYCLES=0`: not charged (the A/B).

Debug switches: `TF_NATIVE_PROFILE` (per-part times, video memory), `TF_NATIVE_DRED`,
`TF_NATIVE_DEBUG=1/2` (debug layer messages on stderr), `TF_NATIVE_ORDER` (the frame's draws by
target), `TF_NATIVE_COPYLOG`/`_COPYDEBUG`, `TF_NATIVE_SAVE_TARGET=<key>[,<key>]` (the first drawn), `TF_NATIVE_SAVE_AFTER=<draw>[,<draw>]` (the
target a draw drew into, as it is after it: `native_<t>_after<draw>.bmp`), `TF_NATIVE_DRAWVERTS=1` (with
`TF_NATIVE_DRAWLOG`: a 2D draw's every vertex, a 3D draw's CLAMP), `TF_NATIVE_STRIPLOG=<n>[,<tick>]`,
`TF_NATIVE_TARGETDRAWS=<key>[,<key>]` (`ffffffff`: every target; with each draw's CLAMP), `TF_NATIVE_TBPLOG=<tbp>`, `TF_NATIVE_TOPLOG` (top-byte
reads), `TF_NATIVE_RANGELOG=<tick>` (that frame's list stretches, geometry sets, microcode
primitives; with `TF_NATIVE_VIFLOG` its first stretch's VIF codes, with `TF_NATIVE_SLOTLOG=1` every
render-device slot call), `TF_NATIVE_FOGLOG`,
`TF_NATIVE_BIGSPRITES`, `TF_NATIVE_GRASSLOG`, `TF_NATIVE_ROWSLOG` (the draws that reach a target's rows past 224:
what decides which field copy weaves the picture), `TF_NATIVE_THREAD=0` (the renderer thread's same work - each frame
read whole and drawn - on the game's thread at the frame's end: the A/B for the renderer thread's reads of memory the game
has moved on from; with `TF_GRASS_GPU=0` pixel for pixel the threaded picture, TODO 149; `=old`: the older path, each
stretch read as the game sends it),
`TF_NATIVE_SYNC` (wait for the GPU every frame), `TF_NATIVE_OCCLOG=<ticks>` (the sun's visibility test: its copy's rect,
threshold, count and visibility, the host copy, the EE's time in the read-back, the native answer beside it; roadmap N3a),
`TF_NATIVE_OCCSHOT=<n>` (native shots of the first n frames the sun's test copies in), `TF_NATIVE_OCCLUSION=0` (the sun's
read-back from GS memory again instead of the native depth - nr_occlusion.cpp).

## Files

- `nr_types.h`: the plain data between the two sides (no Windows or runtime headers).
- `nr_capture.cpp`: the wrappers on the game's render calls (game thread, guest memory).
- `nr_d3d.cpp`: the Direct3D 12 side (D3D12 for ray tracing, the upscalers' frame generation, more headroom).
- `nr_install.h`: the entry point, called from `applyTransformers` after the decomp layer.
- `nr_thread.cpp`: the render thread (the frame's list read, grass, D3D12 recording).
- `nr_present.cpp`: the live picture's hand-over to the window (stage 4); `nr_glshare.cpp`: its shared
  texture on the GL side.
- `nr_vif.cpp`, `nr_shader.cpp`, `nr_gif.cpp`: our own VIF interpreter, the VU image and shader
  snapshots, the GIF reader (the 2D).
- `nr_warp.cpp`: the warp tunnel of a load drawn natively (stage 15): the tunnel's VU1 program left out, its work done from
  the real clock on a thread of its own, its sparks moved by the GPU (nr_d3d.cpp drawCloud).
- `nr_particles.cpp`: the particle pipelines' VU1 programs run on the renderer's thread (stage 14), and the pipelines'
  field headers' upload (func_002516C0) written by our own code.
- `nr_movie.cpp`: the movies' pictures from the decoder (stage 15, roadmap N6).
- `nr_rain.cpp`: the rain's batches (taShader_Rain's routine) made into their GIF packets in C++ (stage 18).
- `nr_n5.cpp`: roadmap N5 (stage 19): the display list's census (`TF_N5_CENSUS`) and VIF1's lite transfers while the
  native renderer draws alone, with its check mode.
- `nr_occlusion.cpp`: the sun's visibility test (roadmap N3a) answered from the 3D target's own depth - the game's
  per-frame read-back of its 64 x 64 occlusion buffer replaced, while the native renderer draws alone, by a 64 x 64
  probe of the depth read back a frame late (nr_d3d.cpp `runProbe`); the GS copy it once read never landed in this port. TODO 141
  (2026-11-03): the test's rect comes in the picture's 448 lines (this port's frames are not dual-field, so the game does not
  halve y) while the depth holds the scene once in field A's 224 rows - the probe read row 2y: a sun in the picture's upper
  half was tested against what lay twice as far down (the player, the ground) and vanished, one in the lower half against
  field B's empty rows (always visible, even behind the player). The rect's rows are halved for the probe (`TF_NATIVE_OCCFIELD=0`:
  as before).
- `nr_passes.cpp`, `nr_grass.cpp`, `nr_texture.cpp`, `nr_hires.cpp`: the routines as passes, the grass
  blades, texture conversion, the remaster's large images (loaded on a thread of their own).
