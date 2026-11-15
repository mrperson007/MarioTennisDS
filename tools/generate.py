"""Find your Mario's Tennis ROM and generate the recompiled game code.

Run by build.bat / make; you normally never call it yourself.

  1. Looks for the ROM: a .vb file, or a .zip containing one, in this
     folder, nitrofiles/ or roms/.  The file name does not matter.
  2. Checks that it is the right dump (CRC32 7CE7460D, 512 KB).
  3. Copies it to nitrofiles/marios_tennis.vb (packed into the .nds).
  4. Writes generated-lean/ with the recompiler in tools/vbrecomp, unless
     it is already up to date.

Needs Python 3.10 or newer, nothing else.
"""
import os
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "vbrecomp"))

ROM_CRC = 0x7CE7460D
ROM_SIZE = 524288
GEN_VERSION = "lean-2"          # bump when the recompiler or marking changes

# The game functions that take the most time (measured over several play
# sessions).  On the DS they are compiled as ARM code into the fast
# instruction memory (VBDS_HOT_FN in core/lean_platform.h); about 9 KB.
HOT_FUNCTIONS = [
    "FFF838E4", "FFF8135C", "FFF841C2", "FFF9242A",
    "FFF80186", "FFF801B2", "FFF80144", "FFF8016C",
]
OUT_DIR = os.path.join(ROOT, "generated-lean")
ROM_DEST = os.path.join(ROOT, "nitrofiles", "marios_tennis.vb")
STAMP = os.path.join(OUT_DIR, "generated.stamp")


def candidates():
    for folder in (ROOT, os.path.join(ROOT, "nitrofiles"), os.path.join(ROOT, "roms")):
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            path = os.path.join(folder, name)
            low = name.lower()
            if not os.path.isfile(path):
                continue
            if low.endswith(".vb"):
                with open(path, "rb") as f:
                    yield path, f.read()
            elif low.endswith(".zip"):
                try:
                    with zipfile.ZipFile(path) as z:
                        for inner in z.namelist():
                            if inner.lower().endswith(".vb"):
                                yield f"{path} ({inner})", z.read(inner)
                except zipfile.BadZipFile:
                    pass


def find_rom():
    wrong = []
    for where, data in candidates():
        if len(data) == ROM_SIZE and zlib.crc32(data) == ROM_CRC:
            return where, data
        wrong.append(f"{where}: CRC32 {zlib.crc32(data):08X}, {len(data)} bytes")
    print("\nCould not find the Mario's Tennis ROM.")
    print("Put your ROM (.vb, or a .zip containing it) in this folder:")
    print(f"  {ROOT}")
    print("It must be Mario's Tennis (Japan, USA), CRC32 7CE7460D.")
    if wrong:
        print("\nThese files were checked but are a different dump:")
        for w in wrong:
            print("  " + w)
    sys.exit(1)


def main():
    if sys.version_info < (3, 10):
        print("Python 3.10 or newer is needed (found %d.%d)." % sys.version_info[:2])
        sys.exit(1)

    where, rom = find_rom()
    print(f"ROM: {where}")
    os.makedirs(os.path.dirname(ROM_DEST), exist_ok=True)
    current = open(ROM_DEST, "rb").read() if os.path.exists(ROM_DEST) else b""
    if current != rom:
        with open(ROM_DEST, "wb") as f:
            f.write(rom)

    stamp = f"{GEN_VERSION} {ROM_CRC:08X}\n"
    if os.path.exists(STAMP) and open(STAMP).read() == stamp:
        print("Game code: already generated.")
        return

    print("Generating game code from the ROM...")
    from recompiler.v810.analysis import RomImage
    from recompiler.v810.lean import recompile_rom_lean

    result = recompile_rom_lean(RomImage.from_bytes(rom), module_name="marios_tennis",
                                unit_instruction_limit=8192)
    os.makedirs(OUT_DIR, exist_ok=True)
    for old in os.listdir(OUT_DIR):
        os.remove(os.path.join(OUT_DIR, old))
    for rel, text in result.files.items():
        name = os.path.basename(rel)
        if name.endswith(".cmake"):
            continue
        for addr in HOT_FUNCTIONS:
            text = text.replace(f"\nvoid vb_fn_{addr}(CPUState* cpu) {{",
                                f"\nVBDS_HOT_FN void vb_fn_{addr}(CPUState* cpu) {{")
        with open(os.path.join(OUT_DIR, name), "w", newline="\n") as f:
            f.write(text)
    with open(STAMP, "w") as f:
        f.write(stamp)
    print(f"Game code: {result.function_count} functions written to generated-lean/.")


if __name__ == "__main__":
    main()
