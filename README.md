# Mario's Tennis for Nintendo DS (work in progress)

A Nintendo DS build of [Mario's Tennis Virtual Boy Recomp](https://github.com/mstan/MarioTennisVirtualBoyRecomp):
the Virtual Boy game, statically recompiled to C, running on a DS with a new
small-footprint runtime.

**No ROM is included.** You need your own dump of Mario's Tennis
(Japan/USA, CRC32 `7CE7460D`).

## Status

- Boots, shows the warning and title screens, and plays a match in melonDS.
- **Too slow to play yet:** about 34% of full speed during a rally on an
  original DS. The bottom screen shows live speed and CPU figures.
- Left eye only, reduced from 384x224 to 256x149 with averaging so small
  text stays readable.
- No sound yet.
- Not yet tried on real hardware.

The game logic is checked against the original PC runtime: work RAM and
the picture are byte-identical in every test session (up to 12,000 frames
with random input).

## Building

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
   "NDS Development", and Python 3.10+.
2. Generate the recompiled game code from your ROM (this repo does not
   ship it, because it is derived from the ROM):
   ```
   tools/regenerate.sh path/to/marios_tennis.vb
   ```
   Run it from the devkitPro MSYS2 shell on Windows.
3. Copy your ROM to `nitrofiles/marios_tennis.vb`.
4. Run `make` (or double-click `build.bat` on Windows).

The result, `mariotennis-ds.nds`, contains your ROM. Do not share it.

## Controls

| DS | Virtual Boy |
|---|---|
| D-pad | Left D-pad |
| A, B | A, B |
| L, R | L, R |
| Start, Select | Start, Select |
| X, Y | Right D-pad up, left |

## How it works

- `tools/vbrecomp-lean.patch` adds a `--lean` mode to the
  [vbrecomp](https://github.com/mstan/vbrecomp) recompiler (at `8e8d3c6`).
  The stock output is cycle-exact and about 6 MB for the DS's CPU; lean
  output is about 0.5 MB. It charges cycles per block instead of per
  instruction, drops flag updates nothing reads, and marks wait loops so
  the runtime can skip them.
- `core/` is the runtime: memory map and scheduler (`vbds_bus.c`,
  `vbds_core.c`), a compact video chip (`vbds_vip.c`, from vbrecomp's port
  of Beetle VB, with redraw skipping for unchanged areas), the 2/3
  downscaler (`vbds_scale.c`) and a silent sound stub.
- `source/main_nds.c` is the DS front end.
- `vbrecomp/` holds unmodified CPU, timer, pad and interrupt files from
  vbrecomp.
- `host/` is a PC front end used to compare the core with the original
  runtime (`host/build.sh out` on Linux).

## Credits

- [mstan](https://github.com/mstan) for vbrecomp and the Mario's Tennis
  recompilation.
- Beetle VB / Mednafen, the reference the video chip is ported from.
- Mario's Tennis is (c) 1995 Nintendo.
