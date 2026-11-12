# Game addresses and the European disc (TF_GA)

The port builds for two discs: Transformers USA (SLUS-20668) and, experimentally, the European disc (SLES-52388).
Their code is the same except in two places, which shift the code after them (`transformers/src/game_version.h`):

| USA code address | European address |
|---|---|
| up to 0x33B690, and from 0x5DCB80 on (all data) | the same |
| 0x33FFD8 to 0x3E29A0 | 16 bytes lower |
| 0x3EF1F0 to 0x5DCB80 | 32 bytes lower |
| 0x33B690 to 0x33FFD8, 0x3E29A0 to 0x3EF1F0 | not known: checked by hand |

## The rule for everyone who adds or changes C++ (both lanes, contributors)

1. A **code** address (a function, a call site, a return address, an instruction patch) in the range 0x33B690 to
   0x5DCB80 is written `TF_GA(0x...u)`. In the USA build it is the number itself; in the European build it is moved by
   the table above.
2. If the address lies in one of the two "not known" gaps, the build stops with a message: find the European address
   (the function list of the European executable, `find_functions.py`) and write `TF_VA(usa, pal)`.
3. Data addresses (0x5DCB80 and up) and addresses below 0x33B690 need nothing.
4. Before committing, run `python transformers/tools/check_pal_addresses.py`. The European build fails on an address
   that is not wrapped; the USA build runs the same check and only warns, so you see it at once.
5. Code that only makes sense on one disc goes under `#if defined(TF_GAME_PAL)` (example: the 50 / 60 Hz menu).

## The private build

The private tree's mods (`overlay/`) use USA addresses that are not wrapped. The European disc is built without them:
`build.ps1` leaves the overlay out for any disc other than the USA one, as `-Public` does. Mods on the European disc are
out of scope for now.
