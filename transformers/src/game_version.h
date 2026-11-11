#pragma once
// The release this build is for (transformers/CMakeLists.txt: TF_GAME_PAL for SLES-52388).
//
// The addresses in this project's C++ are SLUS-20668's (USA). SLES_523.88 (PAL) has the same size and layout, the
// same data, the same VU1 microcode and the same SDK at the same addresses; its code differs in two places, which
// shift what follows them:
//   - up to 0x33B690             the same addresses;
//   - from 0x33FFD8 to 0x3E29A0  16 bytes lower;
//   - from 0x3EF1F0 to 0x5DCB80  32 bytes lower (the end of the main code region; the data after it is the same).
// Measured on every code address this project uses (transformers/tools/check_pal_addresses.py lists them): each PAL
// value is a function start in PAL's own function list (find_functions.py), or for a call site or a return address,
// the call at the shifted address - 262 function starts agree with these three steps.
// Between the steps (0x33B690 - 0x33FFD8, 0x3E29A0 - 0x3EF1F0) the shift is not known: an address there does not
// compile for PAL until it is checked and given with TF_VA.
//
// TF_GA(usa): a code address, as this build's release has it. Every code address above 0x33B690 is written with it
// (check_pal_addresses.py fails the PAL build on one that is not).
// TF_VA(usa, pal): an address whose PAL value is given by hand.

#include <cstdint>

namespace tf_game_version
{
    consteval uint32_t palCodeAddress(uint32_t usa)
    {
        if (usa <= 0x0033B690u || usa >= 0x005DCB80u)
            return usa;
        if (usa >= 0x0033FFD8u && usa <= 0x003E29A0u)
            return usa - 16u;
        if (usa >= 0x003EF1F0u)
            return usa - 32u;
        // (a consteval function that throws does not compile: the address needs checking by hand)
        throw "TF_GA: this address lies where the USA and PAL code differ - check it and use TF_VA(usa, pal)";
    }
}

#if defined(TF_GAME_PAL)
#define TF_GA(usa) (::tf_game_version::palCodeAddress(usa))
#define TF_VA(usa, pal) (pal)
#else
#define TF_GA(usa) (usa)
#define TF_VA(usa, pal) (usa)
#endif
