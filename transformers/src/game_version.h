#pragma once
// The release this build is for (transformers/CMakeLists.txt: TF_GAME_PAL for SLES-52388).
//
// TF_VA(usa, pal): a guest address that differs between the two executables. SLES_523.88 has the same size and
// layout as SLUS_206.68; its code sits at the same addresses up to about 0x2CE000, 16 bytes lower around
// 0x350000-0x380000 and 32 bytes lower from about 0x420000 on (transformers/config/versions/SLES-52388). Each PAL
// value is checked on its own: a function start in PAL's function list, or the instruction it names.
#if defined(TF_GAME_PAL)
#define TF_VA(usa, pal) (pal)
#else
#define TF_VA(usa, pal) (usa)
#endif
