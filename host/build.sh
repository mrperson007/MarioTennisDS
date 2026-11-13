#!/bin/sh
# Build the portable core for a PC. usage: host/build.sh <out> [extra cc flags]
set -e
cd "$(dirname "$0")/.."
out=$1; shift
cc -std=gnu11 -O2 -w "$@" -DVB_LEAN_PLATFORM_HEADER='"lean_platform.h"' \
   -Icore -Ivbrecomp/include -Igenerated-lean \
   core/*.c vbrecomp/src/*.c generated-lean/*.c host/main_host.c -lm -o "$out"
