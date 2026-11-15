/* main_nds.c - Nintendo DS front end for the Mario's Tennis recompilation.
 *
 * Top screen: the game.  The Virtual Boy picture is 384x224.  The core
 * reduces each row to 256 pixels as it draws (core/vbds_vip.c, page
 * output).  The DS hardware does the vertical 224->149 step: two bitmap
 * layers show the same picture at 1.5x vertical scale, one a row lower
 * than the other, and the blending unit averages them, so every source
 * row contributes and thin lines and small text stay visible.
 * Bottom screen: status text.
 * Sound: the emulated sound chip produces samples at half the DS mixer rate
 * (16364 Hz); they are streamed through two looping hardware
 * channels (left and right).
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
#include "vbds_vsu.h"

#define ROM_NAME "marios_tennis.vb"
#define ROM_SIZE 524288u

#define TOP_MARGIN ((192 - 149) / 2)
#define PAGE_ROWS 224
#define PAGE_BYTES (VBDS_PAGE_W * PAGE_ROWS)

/* 33.513982 MHz bus clock / 50.2734 Hz Virtual Boy frame rate. */
#define FRAME_TICKS (BUS_CLOCK / 50u - BUS_CLOCK / 9190u)

static u8* s_bitmap;
static int s_bg3;
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
    s_bg3 = bg3;
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

/* ---- Sound streaming ----
 * Two looping 16-bit channels play rings of AUD_N samples, one sample
 * every 2048 bus clocks, so the play position follows from the CPU timing
 * counter (timers 0/1 - timers 2/3 belong to the system tick scheduler and
 * must not be touched).  Each frame's samples are written about AUD_LEAD
 * samples ahead of the play position; the amount written is nudged by a
 * few samples so that lead holds.  The stretch just past the written audio
 * is kept silent, so running late gives a gap rather than old sound, and
 * if the lead is lost entirely the stream restarts AUD_LEAD ahead. */
#define AUD_N     4096u
#define AUD_LEAD  1200u
#define AUD_CH_L  0
#define AUD_CH_R  1
static s16 s_aud[2][AUD_N] __attribute__((aligned(32)));
static u32 s_aud_w;                       /* samples written (absolute) */
static u32 s_aud_resyncs;
static u64 s_aud_t0;                      /* bus clock when playback started */
static u32 s_clk_last, s_clk_hi;

/* 64-bit bus clock count from the 32-bit timing counter (it wraps every
 * 128 s; this is called every frame). */
static u64 bus_clock64(void) {
    u32 t = cpuGetTiming();
    if (t < s_clk_last) s_clk_hi++;
    s_clk_last = t;
    return ((u64)s_clk_hi << 32) | t;
}

static u32 audio_play_pos(void) {
    return (u32)((bus_clock64() - s_aud_t0) >> 11);
}

static void audio_init(void) {
    memset(s_aud, 0, sizeof(s_aud));
    DC_FlushRange(s_aud, sizeof(s_aud));
    soundInit();
    soundPowerOn();
    soundSetMixerVolume(127);
    soundPreparePcm(AUD_CH_L, 2047, 0, 1024, SoundMode_Repeat, SoundFmt_Pcm16,
                    s_aud[0], 0, AUD_N * 2 / 4);
    soundPreparePcm(AUD_CH_R, 2047, 127, 1024, SoundMode_Repeat, SoundFmt_Pcm16,
                    s_aud[1], 0, AUD_N * 2 / 4);
    soundStart((1u << AUD_CH_L) | (1u << AUD_CH_R));
    soundSynchronize();
    s_aud_t0 = bus_clock64();
    s_aud_w = AUD_LEAD;
}

static void audio_write(u32 at, const s16* l, const s16* r, unsigned n);

/* Start the stream again AUD_LEAD ahead of the play position, over
 * silence (after the opening screens are skipped). */
static void audio_restart(void) {
    u32 play = audio_play_pos();
    audio_write(play + 16, NULL, NULL, AUD_N - 32);
    s_aud_w = play + AUD_LEAD;
}

static void audio_write(u32 at, const s16* l, const s16* r, unsigned n) {
    for (int k = 0; k < 2; k++) {
        const s16* src = k ? r : l;
        u32 i = at % AUD_N, first = AUD_N - i < n ? AUD_N - i : n;
        if (src) memcpy(&s_aud[k][i], src, first * 2);
        else memset(&s_aud[k][i], 0, first * 2);
        DC_FlushRange(&s_aud[k][i], first * 2);
        if (first < n) {
            if (src) memcpy(&s_aud[k][0], src + first, (n - first) * 2);
            else memset(&s_aud[k][0], 0, (n - first) * 2);
            DC_FlushRange(&s_aud[k][0], (n - first) * 2);
        }
    }
}

static void audio_frame(void) {
    const int16_t *l, *r;
    unsigned n = vbds_vsu_take(&l, &r);
    u32 play = audio_play_pos();
    s32 lead = (s32)(s_aud_w - play);
    if (lead < 32 || lead > (s32)(AUD_N - 512)) {
        /* Lost the stream: silence up to the new write position. */
        /* A short gap; the rate adjustment restores the full lead. */
        s_aud_w = play + AUD_LEAD / 2;
        audio_write(play + 16, NULL, NULL, AUD_LEAD / 2 - 16);
        lead = AUD_LEAD / 2;
        s_aud_resyncs++;
    }
    if (!n) return;
    /* Write m samples resampled from n, a little more when behind. */
    /* At most 2 samples in 325 (0.6%): too little to hear. */
    s32 adj = ((s32)AUD_LEAD - lead) / 64;
    if (adj > 2) adj = 2; else if (adj < -2) adj = -2;
    unsigned m = (unsigned)((s32)n + adj);
    static s16 tmp[2][VBDS_VSU_MAX_SAMPLES + 16];
    if (m > VBDS_VSU_MAX_SAMPLES + 16) m = VBDS_VSU_MAX_SAMPLES + 16;
    if (m == n) {
        audio_write(s_aud_w, l, r, n);
    } else {
        u32 step = ((u32)n << 16) / m;
        for (unsigned i = 0, p = 0; i < m; i++, p += step) {
            unsigned j = p >> 16;
            if (j >= n) j = n - 1;
            tmp[0][i] = l[j]; tmp[1][i] = r[j];
        }
        audio_write(s_aud_w, tmp[0], tmp[1], m);
    }
    s_aud_w += m;
    audio_write(s_aud_w, NULL, NULL, 512);    /* silence past the end */
}

/* ---- Settings (bottom screen, touch to change) ---- */
typedef struct { u8 colour, bright, screen, volume, stats; } Settings;
static Settings s_set = { 0, 1, 0, 4, 0 };
enum { SET_COLOUR, SET_BRIGHT, SET_SCREEN, SET_VOLUME, SET_STATS, SET_COUNT };

static const struct { const char* name; u8 dark[3], light[3]; } s_colours[] = {
    { "Red",    {   0,   0,  0 }, { 255,   0,   0 } },
    { "Grey",   {   0,   0,  0 }, { 255, 255, 255 } },
    { "Green",  {  15,  56, 15 }, { 155, 188,  15 } },
    { "Amber",  {   0,   0,  0 }, { 255, 170,   0 } },
    { "Blue",   {   0,   0,  0 }, {  90, 170, 255 } },
};
#define NUM_COLOURS (int)(sizeof(s_colours) / sizeof(s_colours[0]))
static const char* const s_bright_names[] = { "Dim", "Normal", "Bright", "Brightest" };
static const u16 s_bright_scale[] = { 192, 256, 320, 384 };       /* /256 */
static const char* const s_screen_names[] = { "Smooth", "Sharp" };
static const char* const s_volume_names[] = { "Off", "25%", "50%", "75%", "100%" };
static const char* const s_onoff[] = { "Off", "On" };
static u8 s_option_count[SET_COUNT] = { NUM_COLOURS, 4, 2, 5, 2 };
static bool s_fat;
static bool s_palette_dirty = true;

#define SETTINGS_PATH "fat:/mariotennis.cfg"

static void settings_load(void) {
    if (!s_fat) return;
    FILE* f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;
    u8 b[8];
    if (fread(b, 1, 8, f) == 8 && b[0] == 'M' && b[1] == 'T' && b[2] == 1) {
        u8* v = &s_set.colour;
        for (int i = 0; i < SET_COUNT; i++)
            if (b[3 + i] < s_option_count[i]) v[i] = b[3 + i];
    }
    fclose(f);
}

static bool settings_save(void) {
    if (!s_fat) return false;
    FILE* f = fopen(SETTINGS_PATH, "wb");
    if (!f) return false;
    u8 b[8] = { 'M', 'T', 1, s_set.colour, s_set.bright, s_set.screen, s_set.volume, s_set.stats };
    bool ok = fwrite(b, 1, 8, f) == 8;
    fclose(f);
    return ok;
}

/* Put the settings into effect (palette is rebuilt at the next present). */
static void settings_apply(void) {
    s_palette_dirty = true;
    if (s_set.screen == 0) {
        bgShow(s_bg3);
        REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_DST_BG3;
    } else {
        bgHide(s_bg3);                  /* one layer: crisper, rows dropped */
        REG_BLDCNT = 0;
    }
    soundSetMixerVolume(s_set.volume * 127u / 4u);
}

static u16 colour_for_level(int level) {
    int l = level * s_bright_scale[s_set.bright] / 256;
    if (l > 255) l = 255;
    const u8* d = s_colours[s_set.colour].dark;
    const u8* t = s_colours[s_set.colour].light;
    int c[3];
    for (int k = 0; k < 3; k++) c[k] = d[k] + (t[k] - d[k]) * l / 255;
    return RGB15(c[0] >> 3, c[1] >> 3, c[2] >> 3);
}

static void present(void) {
    int slot = vbds_vip_display_slot();
    if (vbds_vip_page_stale(slot)) vbds_vip_refresh_page(slot);
    /* The game's brightness can change at any time; the palette is cheap. */
    BG_PALETTE[0] = colour_for_level(0);
    for (int i = 1; i < VBDS_PAGE_VALUES; i++)
        BG_PALETTE[i] = colour_for_level(vbds_vip_page_level(i));
    s_palette_dirty = false;
    DC_FlushRange(s_page[slot], PAGE_BYTES);
    dmaCopyWords(3, s_page[slot], s_bitmap, PAGE_BYTES);
}

static void menu_draw(const char* status) {
    static const char* const labels[SET_COUNT] = { "Colour", "Brightness", "Screen", "Volume", "Stats" };
    const char* values[SET_COUNT] = {
        s_colours[s_set.colour].name, s_bright_names[s_set.bright],
        s_screen_names[s_set.screen], s_volume_names[s_set.volume], s_onoff[s_set.stats],
    };
    iprintf("\x1b[0;0HMario's Tennis DS               \n"
            "Tap a setting to change it.     \n");
    for (int i = 0; i < SET_COUNT; i++)
        iprintf("\x1b[%d;0H  %-11s %-10s         ", 3 + 2 * i, labels[i], values[i]);
    if (status) iprintf("\x1b[13;0H%-32s", status);
    if (!s_set.stats)
        for (int r = 15; r < 24; r++) iprintf("\x1b[%d;0H%-32s", r, "");
}

/* A tap on a setting's row moves it to the next choice. */
static void menu_touch(void) {
    if (!(keysDown() & KEY_TOUCH)) return;
    touchPosition t;
    touchRead(&t);
    int row = t.py / 8;
    if (row < 2 || row > 11) return;
    int i = (row - 2) / 2;
    u8* v = &s_set.colour;
    v[i] = (u8)((v[i] + 1) % s_option_count[i]);
    settings_apply();
    menu_draw(settings_save() ? "Saved to the SD card.           "
                              : (s_fat ? "Could not save to the SD card.  "
                                       : "No SD card: not saved.          "));
}

static const u8* load_rom(void) {
    static const char* const paths[] = {
        "nitro:/" ROM_NAME, ROM_NAME, "fat:/" ROM_NAME, "sd:/" ROM_NAME,
        "fat:/roms/vb/" ROM_NAME, "sd:/roms/vb/" ROM_NAME,
    };
    bool nitro = nitroFSInit(NULL);
    bool fat = fatInitDefault();
    s_fat = fat;
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
    audio_init();
    settings_load();
    settings_apply();
    consoleClear();
    menu_draw("Starting...");
    vbds_tick_source = tick_source;

    /* Skip the opening screens: answer the precaution screen (A), the
     * alignment screen (START) and turn automatic pause off (RIGHT, A),
     * running flat out with the picture and sound held back.  The game
     * is deterministic, so these frame numbers always land. */
    static const struct { u32 frame; u16 mask; } boot_keys[] = {
        { 220, VB_PAD_A }, { 300, VB_PAD_START }, { 380, VB_PAD_LRIGHT }, { 420, VB_PAD_A },
    };
    const u32 boot_end = 440;
    while (vbds_cpu.frame < boot_end) {
        vbds_run_frame();
        for (unsigned i = 0; i < sizeof(boot_keys) / sizeof(boot_keys[0]); i++)
            if (vbds_cpu.frame == boot_keys[i].frame) vb_input_press(boot_keys[i].mask, 6);
        const int16_t *l, *r;
        vbds_vsu_take(&l, &r);
        /* Nothing is shown, but a page that needs rebuilding from the
         * framebuffer must be rebuilt when it would have been shown. */
        if (vbds_vip_take_dirty()) {
            int slot = vbds_vip_display_slot();
            if (vbds_vip_page_stale(slot)) vbds_vip_refresh_page(slot);
        }
    }
    present();
    audio_restart();
    menu_draw("");

    u32 deadline = cpuGetTiming();
    u32 window_start = deadline, work_ticks = 0, shown = 0, present_ticks = 0;

    while (pmMainLoop()) {
        scanKeys();
        vb_input_set_pad(read_pad());
        menu_touch();

        u32 t0 = cpuGetTiming();
        vbds_run_frame();
        audio_frame();
        if (vbds_vip_take_dirty()) {
            u32 p0 = cpuGetTiming();
            present();
            present_ticks += cpuGetTiming() - p0;
            shown++;
        } else if (s_palette_dirty) {
            present();
        }
        work_ticks += cpuGetTiming() - t0;

        /* Hold the Virtual Boy's 50.27 Hz.  Waiting is in whole DS frames
         * (59.8 Hz), so the deadline normally runs up to one DS frame
         * behind and the next frame starts at once; that is how 50 frames
         * fit into 60 refreshes.  Slow frames are caught up (the sound
         * stream depends on it); only beyond four frames behind is the
         * lost time dropped. */
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
            u32 dev = load > draw + pres + game ? load - draw - pres - game : 0;
            if (s_set.stats)
                iprintf("\x1b[15;0Hspeed %3lu%%   cpu %3lu%%          \n"
                        "draw  %3lu%%   screen %3lu%%       \n"
                        "game  %3lu%%   devices %3lu%%      \n"
                        "blocks %4lu/%-4lu shown %2lu/50   \n"
                        "sound lead %4ld gaps %-6lu    \n"
                        "frame %-8lu                  \n",
                        (unsigned long)speed, (unsigned long)load,
                        (unsigned long)draw, (unsigned long)pres,
                        (unsigned long)game, (unsigned long)dev,
                        (unsigned long)vbds_prof_blocks_drawn,
                        (unsigned long)(vbds_prof_blocks_drawn + vbds_prof_blocks_skipped),
                        (unsigned long)shown,
                        (long)(s32)(s_aud_w - audio_play_pos()), (unsigned long)s_aud_resyncs,
                        (unsigned long)vbds_cpu.frame);
            window_start = now; work_ticks = 0; shown = 0; present_ticks = 0;
            vbds_prof_draw_ticks = vbds_prof_game_ticks = 0;
            vbds_prof_blocks_drawn = vbds_prof_blocks_skipped = 0;
        }
    }
    return 0;
}
