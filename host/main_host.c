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

#include "vbds_vsu.h"
static FILE *s_vsulog, *s_vsuraw;
static void vsu_trace(int kind, uint32_t a, uint32_t b) { fprintf(s_vsulog, "%c %u %u\n", kind ? 'W' : 'T', a, b); }
static void vsu_raw(int l, int r) { int16_t v[2] = { (int16_t)l, (int16_t)r }; fwrite(v, 2, 2, s_vsuraw); }

#include <time.h>
static uint32_t host_ticks(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000000ull + ts.tv_nsec);
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

    static uint8_t pages[2][256 * 256];
    int check_pages = getenv("VB_TEST_CHECK_PAGES") || getenv("VB_TEST_PREVIEW");
    unsigned long page_checks = 0, page_bad = 0;
    if (check_pages) vbds_vip_set_page_output(pages[0], pages[1], 256);
    if (getenv("VB_TEST_NOPACK")) { vbds_vip_set_page_output(pages[0], pages[1], 256); vbds_vip_set_pack(0); }

    if (getenv("VB_TEST_NOSKIP")) vbds_vip_set_skipping(0);
    /* VB_TEST_SHOWN=path: per frame, a hash of what a DS front end that
     * copies the page only when told it changed would be showing. */
    FILE* shown_f = getenv("VB_TEST_SHOWN") ? fopen(getenv("VB_TEST_SHOWN"), "w") : NULL;
    static uint8_t shown[256 * 224];

    /* Sound: VB_TEST_VSULOG (tick / write trace), VB_TEST_VSURAW (raw mix
     * per sample), VB_TEST_WAV (the filtered output as a .wav file). */
    if (getenv("VB_TEST_VSULOG")) { s_vsulog = fopen(getenv("VB_TEST_VSULOG"), "w"); vbds_vsu_trace = vsu_trace; }
    if (getenv("VB_TEST_VSURAW")) { s_vsuraw = fopen(getenv("VB_TEST_VSURAW"), "wb"); vbds_vsu_raw_hook = vsu_raw; }
    FILE* wav = getenv("VB_TEST_WAV") ? fopen(getenv("VB_TEST_WAV"), "wb") : NULL;
    uint32_t wav_samples = 0;
    if (wav) fseek(wav, 44, SEEK_SET);

    /* VB_TEST_FRAMEPROF=N: every N frames print time per frame (us) for
     * the frame, drawing and game code, and blocks drawn. */
    unsigned fp_every = getenv("VB_TEST_FRAMEPROF") ? (unsigned)atoi(getenv("VB_TEST_FRAMEPROF")) : 0;
    uint32_t fp_t = 0, fp_total = 0;
    if (fp_every) vbds_tick_source = host_ticks;

    uint32_t frames = (uint32_t)strtoul(argv[2], NULL, 10);
    const char* script = getenv("VB_TEST_INPUT");
    while (vbds_cpu.frame < frames) {
        if (fp_every) fp_t = host_ticks();
        vbds_run_frame();
        if (fp_every) {
            fp_total += host_ticks() - fp_t;
            if (vbds_cpu.frame % fp_every == 0) {
                printf("F %u total %u draw %u game %u blocks %u\n", vbds_cpu.frame,
                       fp_total / fp_every / 1000, vbds_prof_draw_ticks / fp_every / 1000,
                       vbds_prof_game_ticks / fp_every / 1000, vbds_prof_blocks_drawn / fp_every);
                fp_total = 0; vbds_prof_draw_ticks = vbds_prof_game_ticks = 0;
                vbds_prof_blocks_drawn = vbds_prof_blocks_skipped = 0;
            }
        }
        if (check_pages) {
            int slot = vbds_vip_display_slot();
            if (vbds_vip_page_stale(slot)) vbds_vip_refresh_page(slot);
            /* Independent reference: the same 3->2 reduction straight from
             * the framebuffer. */
            const uint8_t* fb = vbds_vip_display_fb(0);
            int bad = 0;
            for (int y = 0; y < 224 && !bad; y++)
                for (int t = 0; t < 128; t++) {
                    int px[3];
                    for (int k = 0; k < 3; k++)
                        px[k] = (fb[(t * 3 + k) * 64 + (y >> 2)] >> ((y & 3) * 2)) & 3;
                    int m0 = px[0] > px[1] ? px[0] : px[1], m1 = px[1] > px[2] ? px[1] : px[2];
                    int o0 = 2 * px[0] + px[1] + 3 * m0 + 1, o1 = px[1] + 2 * px[2] + 3 * m1 + 1;
                    if (pages[slot][y * 256 + t * 2] != o0 || pages[slot][y * 256 + t * 2 + 1] != o1) { bad = 1; break; }
                }
            page_checks++; page_bad += bad;
        }
        {
            const int16_t *al, *ar;
            unsigned an = vbds_vsu_take(&al, &ar);
            for (unsigned i = 0; wav && i < an; i++) { int16_t v[2] = { al[i], ar[i] }; fwrite(v, 2, 2, wav); }
            wav_samples += an;
        }
        if (shown_f) {
            int slot = vbds_vip_display_slot();
            /* VB_TEST_HOLD=N: like the DS start-up skip, show nothing (and
             * drop the change flag) for the first N frames. */
            static long hold = -1;
            if (hold < 0) hold = getenv("VB_TEST_HOLD") ? atol(getenv("VB_TEST_HOLD")) : 0;
            if ((long)vbds_cpu.frame < hold) {
                /* Pages must still be brought up to date as they would be. */
                if (vbds_vip_take_dirty() && vbds_vip_page_stale(slot)) vbds_vip_refresh_page(slot);
            }
            else if (vbds_vip_take_dirty() || (long)vbds_cpu.frame == hold) {
                if (vbds_vip_page_stale(slot)) vbds_vip_refresh_page(slot);
                memcpy(shown, pages[slot], sizeof(shown));
            }
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < sizeof(shown); i++) h = (h ^ shown[i]) * 1099511628211ull;
            fprintf(shown_f, "%016llx\n", (unsigned long long)h);
        }
        apply_script(script, vbds_cpu.frame);
    }
    if (shown_f) fclose(shown_f);
    if (s_vsulog) fclose(s_vsulog);
    if (s_vsuraw) fclose(s_vsuraw);
    if (wav) {
        uint32_t bytes = wav_samples * 4u, rate = 16364, v32;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); v32 = 36 + bytes; fwrite(&v32, 4, 1, wav);
        fwrite("WAVEfmt ", 1, 8, wav); v32 = 16; fwrite(&v32, 4, 1, wav);
        uint16_t fmt[2] = { 1, 2 }; fwrite(fmt, 2, 2, wav);
        fwrite(&rate, 4, 1, wav); v32 = rate * 4u; fwrite(&v32, 4, 1, wav);
        uint16_t blk[2] = { 4, 16 }; fwrite(blk, 2, 2, wav);
        fwrite("data", 1, 4, wav); fwrite(&bytes, 4, 1, wav);
        fclose(wav);
    }

    if (getenv("VB_TEST_CHECK_PAGES"))
        printf("page checks %lu, mismatches %lu\n", page_checks, page_bad);
    const char* preview = getenv("VB_TEST_PREVIEW");
    if (preview && (f = fopen(preview, "wb"))) {
        /* What the DS shows: two layers sampling the page at 1.5x, the
         * second offset by one pixel, averaged by the blending hardware. */
        const uint8_t* pg = pages[vbds_vip_display_slot()];
        static uint8_t img[256 * 192];
        for (int sy = 0; sy < 192; sy++) {
            int v = ((sy - 21) * 0x180) >> 8;
            for (int sx = 0; sx < 256; sx++) {
                int sum = 0;
                for (int l = 0; l < 2; l++) {
                    int r = v + l;
                    int idx = (r >= 0 && r < 256) ? pg[r * 256 + sx] : 0;
                    sum += vbds_vip_page_level(idx) >> 3;      /* 5-bit DS colour */
                }
                img[sy * 256 + sx] = (uint8_t)(((sum * 8) >> 4) << 3);
            }
        }
        fwrite(img, 1, sizeof(img), f);
        fclose(f);
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
