# Mario's Tennis for Nintendo DS (work in progress)

A Nintendo DS build of [Mario's Tennis Virtual Boy Recomp](https://github.com/mstan/MarioTennisVirtualBoyRecomp):
the Virtual Boy game, statically recompiled to C, running on a DS with a new
small-footprint runtime.

**No ROM is included.** You need your own dump of Mario's Tennis
(Japan/USA, CRC32 `7CE7460D`).

## Status

- Boots, shows the warning and title screens, and plays a match in melonDS.
-It runs at 100% speed now in Melonds
- Left eye only, reduced from 384x224 to 256x149 with averaging so small
  text stays readable.
- Full sound.
- Works on real hardware.

The game logic is checked against the original PC runtime: work RAM and
the picture are byte-identical in every test session (up to 12,000 frames
with random input).

## Building

You need, once:
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with "NDS Development"
- [Python](https://www.python.org/downloads/) 3.10 or newer (tick "Add
  python.exe to PATH" when installing)

Then:
1. Put your ROM in this folder. A `.vb` file or a `.zip` containing it both
   work, and the name does not matter.
2. Double-click `build.bat`.

It finds and checks the ROM, generates the recompiled game code from it,
and builds `mariotennis-ds.nds`. That file contains your ROM, so do not
share it.

On Linux or macOS, run `python3 tools/generate.py` and then `make`.

## Controls

| DS | Virtual Boy |
|---|---|
| D-pad | Left D-pad |
| A, B | A, B |
| L, R | L, R |
| Start, Select | Start, Select |
| X, Y | Right D-pad up, left |

## How it works

- `tools/vbrecomp/` is the [vbrecomp](https://github.com/mstan/vbrecomp)
  recompiler (at `8e8d3c6`) with an added `--lean` mode (`lean.py`), and
  `tools/generate.py` runs it on your ROM.  The stock output is cycle-exact and about 6 MB for the DS's CPU; lean
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
