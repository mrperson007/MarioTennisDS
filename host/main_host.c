/* main_host.c - PC test front end for the portable core.
 *
 *   vbds-host <rom> <frames>
 *
 * Environment (same conventions as the reference-runtime test patch):
 *   VB_TEST_INPUT="frame:hexmask:hold,..."  scripted pad presses
 *   VB_TEST_DUMP=path   at exit write WRAM (64 KB), then the left-eye
 *                       picture as 384x224 little-endian ARGB words
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "v810_interpreter.h"
#include "vbds.h"
#include "vbds_vip.h"
#include "vbds_scale.h"

void vbds_fatal(const char* what, uint32_t pc, uint32_t addr) {
    fprintf(stderr, "FATAL: %s (pc=%08X addr=%08X frame=%u)\n", what, pc, addr,
            vbds_cpu.frame);
    exit(3);
}

static void apply_script(const char* script, uint32_t frame) {
    for (const char* p = script; p && *p;) {
        unsigned f = 0, m = 0, h = 0; int n = 0;
        if (sscanf(p, "%u:%x:%u%n", &f, &m, &h, &n) < 3) break;
        if (f == frame) vb_input_press((uint16_t)m, (int)h);
        p += n; if (*p == ',') ++p;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s rom frames\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* rom = malloc((size_t)size);
    if (fread(rom, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    if (vbds_init(rom, (uint32_t)size) != 0) { fprintf(stderr, "ROM rejected\n"); return 2; }

    uint32_t frames = (uint32_t)strtoul(argv[2], NULL, 10);
    const char* script = getenv("VB_TEST_INPUT");
    while (vbds_cpu.frame < frames) {
        vbds_run_frame();
        apply_script(script, vbds_cpu.frame);
    }

    const char* dump = getenv("VB_TEST_DUMP");
    if (dump && (f = fopen(dump, "wb"))) {
        static uint32_t argb[384 * 224];
        const uint8_t* fb = vbds_vip_display_fb(0);
        for (int y = 0; y < 224; y++)
            for (int x = 0; x < 384; x++) {
                unsigned v = (fb[x * 64 + (y >> 2)] >> ((y & 3) * 2)) & 3u;
                argb[y * 384 + x] = 0xFF000000u | ((uint32_t)vbds_vip_level((int)v) << 16);
            }
        fwrite(vbds_wram(), 1, VBDS_WRAM_SIZE, f);
        fwrite(argb, 4, 384 * 224, f);
        fclose(f);
    }
    /* VB_TEST_SCALED=path: the DS picture (256x149 grey bytes). */
    const char* scaled = getenv("VB_TEST_SCALED");
    if (scaled && (f = fopen(scaled, "wb"))) {
        static uint8_t img[VBDS_SCALED_W * VBDS_SCALED_H];
        vbds_downscale(0, img, VBDS_SCALED_W);
        for (int i = 0; i < VBDS_SCALED_W * VBDS_SCALED_H; i++)
            img[i] = (uint8_t)vbds_scaled_level(img[i]);
        fwrite(img, 1, sizeof(img), f);
        fclose(f);
    }
    printf("vbds-host: frame=%u fallback=%llu stops/frame=%u drawn/frame=%.1f skipped/frame=%.1f\n",
           vbds_cpu.frame, (unsigned long long)vb_execution.fallback,
           vbds_prof_passes / (frames ? frames : 1),
           (double)vbds_prof_blocks_drawn / (frames ? frames : 1),
           (double)vbds_prof_blocks_skipped / (frames ? frames : 1));
    return 0;
}
