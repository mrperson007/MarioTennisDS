#!/bin/sh
# Generate generated-lean/ from your own ROM.
#   tools/regenerate.sh path/to/marios_tennis.vb
# Needs git and Python 3.10+.
set -e
rom=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")/.." && pwd)
work="$here/build/vbrecomp"
if [ ! -d "$work" ]; then
    git clone https://github.com/mstan/vbrecomp.git "$work"
    git -C "$work" checkout 8e8d3c68b0dcfd5fde5b8c95e7fb20c161be247c
    git -C "$work" apply "$here/tools/vbrecomp-lean.patch"
fi
cd "$work"
python3 -m recompiler.cli.vbrecomp_codegen --lean --rom "$rom" \
    --module marios_tennis --out "$here/generated-lean"
rm -f "$here/generated-lean/marios_tennis_sources.cmake"
cp runtime/include/vb_lean.h "$here/vbrecomp/include/vb_lean.h"
echo "generated-lean/ is ready."
