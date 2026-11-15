/* main_nds.c - Nintendo DS front end for the Mario's Tennis recompilation.
 *
 * Top screen: the game.  The Virtual Boy picture is 384x224.  The core
 * reduces each row to 256 pixels as it draws (core/vbds_vip.c, page
 * output).  The DS hardware does the vertical 224->149 step: two bitmap
 * layers show the same picture at 1.5x vertical scale, one a row lower
 * than the other, and the blending unit averages them, so every source
 * row contributes and thin lines and small text stay visible.
 * Bottom screen: status text.
 *
 * The ROM is not part of the program.  It is read from NitroFS
 * (nitrofiles/marios_tennis.vb at build time) or from the SD card.
 */
#include <nds.h>
#include <fat.h>
#include <filesystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "v810_interpreter.h"
#include "vbds.h"
#include "vbds_vip.h"

#define ROM_NAME "marios_tennis.vb"
#define ROM_SIZE 524288u

#define TOP_MARGIN ((192 - 149) / 2)
#define PAGE_ROWS 224
#define PAGE_BYTES (VBDS_PAGE_W * PAGE_ROWS)

/* 33.513982 MHz bus clock / 50.2734 Hz Virtual Boy frame rate. */
#define FRAME_TICKS (BUS_CLOCK / 50u - BUS_CLOCK / 9190u)

static u8* s_bitmap;
/* The core writes each framebuffer slot's picture here as it draws. */
static u8  s_page[2][PAGE_BYTES] __attribute__((aligned(32)));

void vbds_fatal(const char* what, uint32_t pc, uint32_t addr) {
    consoleClear();
    iprintf("STOPPED\n\n%s\n\npc   %08lX\naddr %08lX\nframe %lu\n",
            what, (unsigned long)pc, (unsigned long)addr,
            (unsigned long)vbds_cpu.frame);
    for (;;) swiWaitForVBlank();
}

static void video_init(void) {
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    int bg2 = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
    int bg3 = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
    bgSetPriority(bg2, 0);
    bgSetPriority(bg3, 1);
    s_bitmap = (u8*)bgGetGfxPtr(bg2);
    /* Value 1 is opaque black; rows below the picture must be opaque so
     * the bottom row blends like every other row. */
    dmaFillWords(0x01010101u, s_bitmap, 256 * 256);

    /* Both layers: 1:1 across, 1.5x down, picture centred vertically.
     * Layer 3 samples one source row lower; the blend averages them. */
    const s32 y0 = -TOP_MARGIN * 0x180;
    REG_BG2PA = 0x100; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 0x180;
    REG_BG2X = 0;      REG_BG2Y = y0;
    REG_BG3PA = 0x100; REG_BG3PB = 0; REG_BG3PC = 0; REG_BG3PD = 0x180;
    REG_BG3X = 0;      REG_BG3Y = y0 + 0x100;
    REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_DST_BG3;
    REG_BLDALPHA = 8 | (8 << 8);

    vbds_vip_set_page_output(s_page[0], s_page[1], VBDS_PAGE_W);
    vbds_vip_set_pack(0);
}

static u32 tick_source(void) { return cpuGetTiming(); }

static void present(void) {
    int slot = vbds_vip_display_slot();
    if (vbds_vip_page_stale(slot)) vbds_vip_refresh_page(slot);
    BG_PALETTE[0] = 0;
    for (int i = 1; i < VBDS_PAGE_VALUES; i++)
        BG_PALETTE[i] = RGB15(vbds_vip_page_level(i) >> 3, 0, 0);
    DC_FlushRange(s_page[slot], PAGE_BYTES);
    dmaCopyWords(3, s_page[slot], s_bitmap, PAGE_BYTES);
}

static const u8* load_rom(void) {
    static const char* const paths[] = {
        "nitro:/" ROM_NAME, ROM_NAME, "fat:/" ROM_NAME, "sd:/" ROM_NAME,
        "fat:/roms/vb/" ROM_NAME, "sd:/roms/vb/" ROM_NAME,
    };
    bool nitro = nitroFSInit(NULL);
    bool fat = fatInitDefault();
    iprintf("nitrofs %s, sd card %s\n", nitro ? "ok" : "no", fat ? "ok" : "no");

    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE* f = fopen(paths[i], "rb");
        if (!f) continue;
        u8* rom = malloc(ROM_SIZE);
        size_t got = rom ? fread(rom, 1, ROM_SIZE, f) : 0;
        fclose(f);
        if (got == ROM_SIZE) {
            iprintf("rom: %s\n", paths[i]);
            return rom;
        }
        free(rom);
    }
    return NULL;
}

static u16 read_pad(void) {
    scanKeys();
    u32 k = keysHeld();
    u16 pad = 0;
    if (k & KEY_A)      pad |= VB_PAD_A;
    if (k & KEY_B)      pad |= VB_PAD_B;
    if (k & KEY_START)  pad |= VB_PAD_START;
    if (k & KEY_SELECT) pad |= VB_PAD_SELECT;
    if (k & KEY_L)      pad |= VB_PAD_LT;
    if (k & KEY_R)      pad |= VB_PAD_RT;
    if (k & KEY_UP)     pad |= VB_PAD_LUP;
    if (k & KEY_DOWN)   pad |= VB_PAD_LDOWN;
    if (k & KEY_LEFT)   pad |= VB_PAD_LLEFT;
    if (k & KEY_RIGHT)  pad |= VB_PAD_LRIGHT;
    /* Only two spare buttons for the Virtual Boy's second d-pad. */
    if (k & KEY_X)      pad |= VB_PAD_RUP;
    if (k & KEY_Y)      pad |= VB_PAD_RLEFT;
    return pad;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    consoleDemoInit();
    iprintf("Mario's Tennis recompiled\n\n");
    video_init();

    const u8* rom = load_rom();
    if (!rom)
        vbds_fatal("ROM not found. Put " ROM_NAME " in nitrofiles/ "
                   "and rebuild, or on the SD card.", 0, 0);
    if (vbds_init(rom, ROM_SIZE) != 0)
        vbds_fatal("This is not the ROM the program was built for.", 0, 0);

    cpuStartTiming(0);
    vbds_tick_source = tick_source;
    u32 deadline = cpuGetTiming();
    u32 window_start = deadline, work_ticks = 0, shown = 0, present_ticks = 0;

    while (pmMainLoop()) {
        vb_input_set_pad(read_pad());

        u32 t0 = cpuGetTiming();
        vbds_run_frame();
        if (vbds_vip_take_dirty()) {
            u32 p0 = cpuGetTiming();
            present();
            present_ticks += cpuGetTiming() - p0;
            shown++;
        }
        work_ticks += cpuGetTiming() - t0;

        /* Hold the Virtual Boy's 50.27 Hz; resynchronise if far behind. */
        deadline += FRAME_TICKS;
        if ((s32)(cpuGetTiming() - deadline) > (s32)(4 * FRAME_TICKS))
            deadline = cpuGetTiming();
        while ((s32)(cpuGetTiming() - deadline) < 0)
            swiWaitForVBlank();

        if (vbds_cpu.frame % 50 == 0) {
            u32 now = cpuGetTiming();
            u32 span = now - window_start;
            /* Speed and load in percent over the last 50 frames. */
            u32 speed = (u32)(((u64)50 * FRAME_TICKS * 100) / (span ? span : 1));
            u64 budget = (u64)50 * FRAME_TICKS;
            u32 load = (u32)(((u64)work_ticks * 100) / budget);
            u32 draw = (u32)(((u64)vbds_prof_draw_ticks * 100) / budget);
            u32 pres = (u32)(((u64)present_ticks * 100) / budget);
            u32 game = (u32)(((u64)vbds_prof_game_ticks * 100) / budget);
            iprintf("\x1b[6;0Hframe %-8lu\nspeed %3lu%%   \ncpu   %3lu%%   \n"
                    " video draw %3lu%%  \n screen     %3lu%%  \n game       %3lu%%  \n"
                    " devices    %3lu%%  \n"
                    "blocks %4lu/%-4lu  \nshown %2lu/50  \nfallback %-6lu\n",
                    (unsigned long)vbds_cpu.frame, (unsigned long)speed,
                    (unsigned long)load, (unsigned long)draw, (unsigned long)pres,
                    (unsigned long)game,
                    (unsigned long)(load > draw + pres + game ? load - draw - pres - game : 0),
                    (unsigned long)vbds_prof_blocks_drawn,
                    (unsigned long)(vbds_prof_blocks_drawn + vbds_prof_blocks_skipped),
                    (unsigned long)shown, (unsigned long)vb_execution.fallback);
            window_start = now; work_ticks = 0; shown = 0; present_ticks = 0;
            vbds_prof_draw_ticks = vbds_prof_game_ticks = 0;
            vbds_prof_blocks_drawn = vbds_prof_blocks_skipped = 0;
        }
    }
    return 0;
}
