/* vbds_vip.c - compact Virtual Image Processor.
 *
 * Derived from vbrecomp runtime/src/vip.c (MIT, see vbrecomp/LICENSE), which
 * ports Beetle VB's vip.c / vip_draw.inc.  Register semantics, the column
 * state machine and the drawing rules are unchanged.  Removed: per-pixel
 * attribution, capture, overrides, recolour and observation rings.
 * Changed: storage is 288 KB instead of a flat 512 KB window, the right eye
 * is optional, and the event horizon can be coarse (see VBDS_COARSE_EVENTS).
 */
#include "vbds_vip.h"

#include <math.h>
#include <string.h>

#include "input.h"
#include "interrupts.h"
#include "vbds.h"

#define VIP_WINDOW_SIZE   0x80000u
#define VIP_REGISTER_BASE 0x5F800u
#define VIP_REGISTER_END  0x5F900u
#define VIP_LOW_SIZE      0x40000u      /* framebuffers + DRAM */
#define VIP_CHR_SIZE      0x8000u

#define VIP_CYCLES_PER_COLUMN 259
#define VIP_COLUMNS_PER_PHASE 384

#define XPCTRL_XP_RST 0x0001u
#define XPCTRL_XP_EN  0x0002u
#define DPCTRL_DPRST  0x0001u
#define DPCTRL_DISP   0x0002u

/* 0x00000-0x3FFFF: L/R framebuffers and DRAM.  Word-aligned so the
 * renderer may read DRAM as uint16_t. */
static uint32_t s_low_store[VIP_LOW_SIZE / 4];
static uint16_t s_chr[VIP_CHR_SIZE / 2];
/* The same characters stored row-major: s_chr_rows[y][c] is row y of
 * character c.  A picture row reads row y of many characters, which here
 * share cache lines (16 per line instead of 2) - this matters on the DS,
 * whose data cache is 4 KB. */
static uint16_t s_chr_rows[8][VIP_CHR_SIZE / 16];
static uint8_t  s_unmapped[4];
#define s_low ((uint8_t*)s_low_store)

static uint16_t s_intpnd, s_intenb, s_dpctrl, s_xpctrl, s_sbcmp, s_frmcyc, s_bkcol;
static uint8_t  s_brta, s_brtb, s_brtc, s_rest;
static uint16_t s_spt[4], s_gplt[4], s_jplt[4];
static uint8_t  s_gplt_cache[4][4], s_jplt_cache[4][4];
static int32_t  s_brt_cache[4];
static uint8_t  s_brt_repeat;
static uint8_t  s_color_lut[256];

static int32_t  s_column, s_column_counter, s_display_region;
static int      s_display_active, s_display_fb;
static int32_t  s_game_frame_counter, s_drawing_counter;
static int      s_drawing_active, s_drawing_fb;
static int32_t  s_drawing_block, s_sb_latch;
static int64_t  s_sbout_inactive_time;
static uint64_t s_update_span, s_vip_cycles;

static int s_dirty = 1;
static int s_right_eye;

/* Redraw avoidance.
 *
 * s_sig is a running 64-bit signature of everything the renderer reads
 * (DRAM, character RAM and the palette/object/background registers).  It
 * is a linear sum, sum(value[i] * K(i)), kept as two independent 32-bit
 * halves and updated on every write, so writing a value and then putting
 * the old one back returns the signature to where it was.  Games rebuild
 * their object tables every frame even when nothing moves; this lets a
 * still picture cost nothing.
 *
 * Each block of each framebuffer slot remembers the signature it was drawn
 * under and is redrawn only when the current one differs.  A CPU write
 * into a framebuffer invalidates every block of that slot. */
static uint32_t s_sig[2];
static uint32_t s_block_sig[2][28][2];
static uint8_t  s_block_valid[2][28];
static uint8_t  s_fb_touched[2];

/* Per-location multipliers must be unrelated to each other: with ones
 * linear in the location, structured edits (every sprite moving up a row
 * and right two pixels) can cancel out.  So the location is mixed with
 * the MurmurHash3 finalizer. */
static inline uint32_t mix32(uint32_t k) {
    k ^= k >> 16; k *= 0x85EBCA6Bu;
    k ^= k >> 13; k *= 0xC2B2AE35u;
    k ^= k >> 16;
    return k;
}
static inline uint32_t sig_key(uint32_t key) { return mix32(key) | 1u; }

static inline void sig_update(uint32_t key, uint32_t old, uint32_t now) {
    uint32_t d = now - old;
    s_sig[0] += d * sig_key(key);
    s_sig[1] += d * sig_key(key ^ 0x5BD1E995u);
}

static void fb_settle(int slot) {
    if (!s_fb_touched[slot]) return;
    s_fb_touched[slot] = 0;
    memset(s_block_valid[slot], 0, sizeof(s_block_valid[slot]));
}

/* Per-block dependencies.
 *
 * Background maps, parameter tables, sprite attributes and characters
 * change every frame somewhere, but usually only a few blocks of the
 * screen show the changed part.  So instead of the global signature,
 * those writes update narrower content signatures (same additive scheme):
 *  - each 128-byte page and each 8 KB segment of VIP memory 0x20000-0x3FFFF;
 *  - each group of 64 characters;
 *  - each block: the sprite entries whose rows touch it, and the
 *    attributes (other than the mode word) of every world whose window
 *    covers it.
 * A drawn block records the pages / segments and character groups it
 * read and their signatures.  It is current if the global signature, its
 * own block signature and every recorded signature still match.  Being
 * content-based, values that flip back and forth (double buffering)
 * leave each slot's blocks current.  Change stamps let the common case
 * skip the per-dependency comparison.
 *
 * Each drawn block also gets a content version, shared when it is known
 * to equal the other slot's block; the two slots show the same picture
 * when every block has the same version in both. */
#define DEP_MAX      96
#define DEP_OVERFLOW 0xFFu          /* depends on every map page */
#define DEP_SEG      0x8000u        /* entry names a segment, not a page */
static uint32_t s_clock = 1;
static uint32_t s_map_stamp, s_chr_stamp;
static uint32_t s_page_sig[1024];
static uint32_t s_seg_sig[16];
static uint32_t s_row_sig[28];       /* sprites and world windows, per block */
static uint32_t s_chr_sig[32];       /* character RAM, per 64 characters */
static uint32_t s_cur_chr;
static uint32_t s_block_time[2][28];
static uint32_t s_block_row_sig[2][28];
static uint32_t s_block_chr[2][28];  /* character groups the block used */
static uint32_t s_block_chr_sum[2][28];
static uint32_t s_block_ver[2][28];
static uint32_t s_ver;
static uint16_t s_block_dep[2][28][DEP_MAX];
static uint32_t s_block_dep_sig[2][28][DEP_MAX];
static uint8_t  s_block_dep_n[2][28];
static uint16_t s_cur_dep[DEP_MAX];
static unsigned s_cur_dep_n;


static inline void dep_add(uint32_t e) {
    unsigned n = s_cur_dep_n;
    if (n >= DEP_MAX) { s_cur_dep_n = DEP_OVERFLOW; return; }
    for (unsigned i = n, k = 0; i > 0 && k < 4; i--, k++)
        if (s_cur_dep[i - 1] == e) return;
    s_cur_dep[n] = (uint16_t)e;
    s_cur_dep_n = n + 1;
}
/* A map halfword index (as the renderer forms it, 0..0xFFFF). */
static inline void dep_cell(uint32_t idx) { dep_add((idx & 0xFFFFu) >> 6); }
static inline uint32_t dep_sig(uint32_t e) {
    return e & DEP_SEG ? s_seg_sig[e & 15u] : s_page_sig[e];
}

/* Blocks touched by a sprite at attribute row y. */
static uint32_t oam_blocks(uint32_t y) {
    uint32_t m = 0;
    for (uint32_t r = y & 0xFFu, i = 0; i < 8u; i++, r = (r + 1u) & 0xFFu)
        if (r < 224u) m |= 1u << (r >> 3);
    return m;
}
static inline int16_t sign_11(uint32_t v);
/* Blocks inside a world's window. */
static uint32_t world_blocks(const uint16_t* wp) {
    /* Rows r with (uint16_t)(r - gy) <= h, i.e. gy <= r <= gy + h. */
    int32_t lo = sign_11(wp[3]), hi = lo + (int32_t)(wp[8] & 0x3FFu);
    if (lo < 0) lo = 0;
    if (hi > 223) hi = 223;
    if (lo > hi) return 0;
    uint32_t first = (uint32_t)lo >> 3, last = (uint32_t)hi >> 3;
    return (uint32_t)((((uint64_t)2 << last) - 1u) & ~((1u << first) - 1u));
}
static void row_sig_add(uint32_t mask, uint32_t d) {
    for (; mask; mask &= mask - 1) s_row_sig[__builtin_ctz(mask)] += d;
}

static int block_current(int slot, int b) {
    if (!s_block_valid[slot][b]
        || s_block_sig[slot][b][0] != s_sig[0]
        || s_block_sig[slot][b][1] != s_sig[1]
        || s_block_row_sig[slot][b] != s_row_sig[b]) return 0;
    uint32_t t = s_block_time[slot][b];
    if (s_chr_stamp > t) {
        uint32_t sum = 0;
        for (uint32_t m = s_block_chr[slot][b]; m; m &= m - 1)
            sum += s_chr_sig[__builtin_ctz(m)];
        if (sum != s_block_chr_sum[slot][b]) return 0;
    }
    if (s_map_stamp <= t) return 1;
    unsigned n = s_block_dep_n[slot][b];
    if (n == DEP_OVERFLOW) return 0;
    const uint16_t* d = s_block_dep[slot][b];
    const uint32_t* v = s_block_dep_sig[slot][b];
    for (unsigned i = 0; i < n; i++)
        if (dep_sig(d[i]) != v[i]) return 0;
    /* Unchanged after all: move the block's time forward. */
    s_block_time[slot][b] = s_clock;
    return 1;
}

static int slots_show_same_picture(void) {
    fb_settle(0); fb_settle(1);
    for (int i = 0; i < 28; i++) {
        if (!s_block_valid[0][i] || !s_block_valid[1][i]) return 0;
        if (s_block_ver[0][i] != s_block_ver[1][i]) return 0;
    }
    return 1;
}

#define SIG_KEY_REG 0x100000u       /* register keys sit above memory offsets */

void vbds_vip_set_right_eye(int enabled) {
    sig_update(SIG_KEY_REG + 0x100, (uint32_t)s_right_eye, (uint32_t)(enabled != 0));
    s_right_eye = enabled != 0;
}
int vbds_vip_take_dirty(void) { int d = s_dirty; s_dirty = 0; return d; }

/* Page output for small screens.
 *
 * As each block is drawn, its rows of the left-eye picture are reduced
 * horizontally from 384 to 256 pixels and written, one byte per pixel,
 * into the page of the slot being drawn.  Every 3 source pixels a b c
 * become 2 output pixels from (a,b) and (b,c); each is the mean of the
 * 2:1-weighted average and the maximum of the pair, so 1-pixel lines are
 * never lost.  The 19 possible results are page values 1..19 (0 stays
 * free as "transparent"); vbds_vip_page_level() gives their brightness.
 * The front end does the vertical 224->149 step in hardware.  Pages hold
 * 224 rows.
 *
 * A front end shows the page of vbds_vip_display_slot().  A CPU write into
 * a framebuffer marks its page stale; vbds_vip_refresh_page() rebuilds it. */
static uint8_t* s_page[2];
static int      s_page_stride;
static uint8_t  s_page_stale[2];
VBDS_DTCM_BSS static uint16_t s_hpair[64];   /* (a<<4|b<<2|c) -> out0 | out1 << 8 */

static void page_init_tables(void) {
    for (int i = 0; i < 64; i++) {
        int a = i >> 4, b = (i >> 2) & 3, c = i & 3;
        int m0 = a > b ? a : b, m1 = b > c ? b : c;
        int o0 = 2 * a + b + 3 * m0, o1 = b + 2 * c + 3 * m1;     /* 0..18 */
        s_hpair[i] = (uint16_t)((o0 + 1) | ((o1 + 1) << 8));
    }
}

/* Reduce one row of 384 values (0..3) to 256 page values.  `src` is
 * word-aligned; each pass reads 12 pixels (4 triples) as three words. */
VBDS_FAST static void page_row(const uint8_t* src, uint8_t* dst) {
    const uint32_t* in = (const uint32_t*)src;
    uint32_t* out = (uint32_t*)dst;
    for (int i = 0; i < 32; i++, in += 3, out += 2) {
        uint32_t w0 = in[0], w1 = in[1], w2 = in[2];
        /* Pixels are bytes 0..3, so one multiply gathers three of them
         * (a<<4 | b<<2 | c lands in bits 16..21; nothing else does). */
        uint32_t t0 = ((w0 * 0x100401u) >> 16) & 63u;
        uint32_t t1 = ((w0 >> 20) & 0x30u) | (((w1 * 0x40100u) >> 16) & 15u);
        uint32_t t2 = ((w1 >> 12) & 0x30u) | ((w1 >> 22) & 0x0Cu) | (w2 & 3u);
        uint32_t t3 = (((w2 >> 8) * 0x100401u) >> 16) & 63u;
        out[0] = s_hpair[t0] | ((uint32_t)s_hpair[t1] << 16);
        out[1] = s_hpair[t2] | ((uint32_t)s_hpair[t3] << 16);
    }
}

void vbds_vip_set_page_output(uint8_t* page0, uint8_t* page1, int stride) {
    page_init_tables();
    s_page[0] = page0; s_page[1] = page1;
    s_page_stride = stride;
    s_page_stale[0] = s_page_stale[1] = 1;
}

int vbds_vip_page_level(int value) {
    if (value <= 0) return 0;
    int k = value - 1;                  /* level k/6 */
    int lo = k / 6, frac = k % 6;
    if (lo >= 3) return vbds_vip_level(3);
    int l0 = vbds_vip_level(lo), l1 = vbds_vip_level(lo + 1);
    return l0 + (l1 - l0) * frac / 6;
}

/* Packing drawn rows back into the 2-bit framebuffer only matters if the
 * game reads or writes framebuffer memory itself; Mario's Tennis does
 * neither, so a front end with page output can switch it off.  If the CPU
 * does write a framebuffer later, packing comes back on for good. */
static int s_pack = 1;
static int s_skip_copy = 1, s_skip_all = 1;
void vbds_vip_set_skipping(int enabled) { s_skip_copy = s_skip_all = enabled != 0; }
void vbds_vip_set_pack(int enabled) { s_pack = enabled != 0; }

int vbds_vip_display_slot(void) { return s_display_fb & 1; }
int vbds_vip_page_stale(int slot) { return s_page[slot & 1] && s_page_stale[slot & 1]; }

void vbds_vip_refresh_page(int slot) {
    slot &= 1;
    if (!s_page[slot]) return;
    const uint8_t* fb = &s_low[slot ? 0x8000u : 0x0000u];
    uint32_t row_store[96];
    uint8_t* row = (uint8_t*)row_store;
    for (int y = 0; y < 224; y++) {
        int shift = (y & 3) * 2;
        for (int x = 0; x < 384; x++) row[x] = (fb[x * 64 + (y >> 2)] >> shift) & 3;
        page_row(row, s_page[slot] + y * s_page_stride);
    }
    s_page_stale[slot] = 0;
}

/* Background tile rows, four pixels at a time: for a byte of 2-bit pixels,
 * s_bg4[palette][byte] holds the four palette-mapped values and
 * s_bg4_opaque[byte] a bit per non-zero pixel. */
VBDS_DTCM_BSS static uint32_t s_bg4[4][256];
VBDS_DTCM_BSS static uint8_t  s_bg4_opaque[256];

static void recalc_gplt_cache(int which) {
    for (int i = 0; i < 4; ++i)
        s_gplt_cache[which][i] = (uint8_t)((s_gplt[which] >> (i * 2)) & 3u);
    const uint8_t* pal = s_gplt_cache[which];
    for (int b = 0; b < 256; b++)
        s_bg4[which][b] = (uint32_t)pal[b & 3] | (uint32_t)pal[(b >> 2) & 3] << 8
                        | (uint32_t)pal[(b >> 4) & 3] << 16 | (uint32_t)pal[b >> 6] << 24;
}
static void recalc_jplt_cache(int which) {
    for (int i = 0; i < 4; ++i)
        s_jplt_cache[which][i] = (uint8_t)((s_jplt[which] >> (i * 2)) & 3u);
}

/* Beetle RecalcBrightnessCache (vip.c:172-223). */
static void recalc_brt_cache(void) {
    int32_t cumulative = (int32_t)s_brta + 1 + (int32_t)s_brtb + 1
                       + (int32_t)s_brtc + 1 + (int32_t)s_rest + 1 + 1;
    const int32_t max_time = 128;
    int32_t b[4] = { 0, 0, 0, 0 };
    int32_t before[4];
    memcpy(before, s_brt_cache, sizeof(before));
    for (int i = 0; i < s_brt_repeat + 1; ++i) {
        if (i * cumulative >= max_time) break;
        int32_t b1 = i * cumulative + (int32_t)s_brta;
        if (b1 > max_time) b1 = max_time;
        b1 -= i * cumulative;
        if (b1 < 0) b1 = 0;
        int32_t b2 = i * cumulative + (int32_t)s_brta + 1 + (int32_t)s_brtb;
        if (b2 > max_time) b2 = max_time;
        b2 -= i * cumulative + (int32_t)s_brta + 1;
        if (b2 < 0) b2 = 0;
        int32_t b3 = i * cumulative + (int32_t)s_brta + (int32_t)s_brtb
                   + (int32_t)s_brtc + 1;
        if (b3 > max_time) b3 = max_time;
        b3 -= i * cumulative + 1;
        if (b3 < 0) b3 = 0;
        b[1] += b1; b[2] += b2; b[3] += b3;
    }
    s_brt_cache[0] = 0;
    for (int i = 1; i < 4; ++i)
        s_brt_cache[i] = 255 * b[i] / max_time;
    if (memcmp(before, s_brt_cache, sizeof(before)) != 0) s_dirty = 1;
}

int vbds_vip_level(int v) {
    int32_t bv = s_brt_cache[v & 3];
    if (bv < 0) bv = 0;
    if (bv > 255) bv = 255;
    return s_color_lut[bv];
}

const uint8_t* vbds_vip_display_fb(int eye) {
    uint32_t base = eye ? (s_display_fb ? 0x18000u : 0x10000u)
                        : (s_display_fb ? 0x8000u  : 0x0000u);
    return &s_low[base];
}

static void vip_check_irq(void) {
    vb_irq_assert(VBIRQ_SOURCE_VIP, (s_intpnd & s_intenb) ? 1 : 0);
}

/* s_rev2bpp[b]: the four 2-bit pixels of byte b in reverse order. */
VBDS_DTCM_BSS static uint8_t s_rev2bpp[256];

static void vip_draw_block_now(uint8_t block_no);

void vb_vip_init(void) {
    for (int b = 0; b < 256; b++) {
        s_rev2bpp[b] = (uint8_t)(((b & 3) << 6) | ((b >> 2 & 3) << 4) | ((b >> 4 & 3) << 2) | (b >> 6));
        s_bg4_opaque[b] = (uint8_t)(((b & 3) ? 1 : 0) | ((b & 0x0C) ? 2 : 0) | ((b & 0x30) ? 4 : 0) | ((b & 0xC0) ? 8 : 0));
    }
    memset(s_low_store, 0, sizeof(s_low_store));
    memset(s_chr, 0, sizeof(s_chr));
    memset(s_chr_rows, 0, sizeof(s_chr_rows));
    s_intpnd = 0; s_intenb = 0;
    s_dpctrl = DPCTRL_DISP;
    s_xpctrl = 0; s_sbcmp = 0; s_frmcyc = 0; s_bkcol = 0;
    s_brta = s_brtb = s_brtc = s_rest = 0;
    s_brt_repeat = 0;
    /* Beetle MakeColorLUT: 1/2.2 gamma on the brightness scalar. */
    for (int i = 0; i < 256; ++i) {
        int v = (int)(pow((double)i / 255.0, 1.0 / 2.2) * 255.0);
        s_color_lut[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    recalc_brt_cache();
    for (int i = 0; i < 4; ++i) {
        s_spt[i] = 0; s_gplt[i] = 0; s_jplt[i] = 0;
        recalc_gplt_cache(i); recalc_jplt_cache(i);
    }
    s_column = 0;
    s_column_counter = VIP_CYCLES_PER_COLUMN;
    s_display_region = 0;
    s_display_active = 0; s_display_fb = 0;
    s_game_frame_counter = 0;
    s_drawing_counter = 0; s_drawing_active = 0; s_drawing_fb = 0;
    s_drawing_block = 0; s_sb_latch = 0;
    s_sbout_inactive_time = -1;
    s_vip_cycles = 0;
    s_dirty = 1;
    s_sig[0] = s_sig[1] = 0;
    memset(s_block_valid, 0, sizeof(s_block_valid));
    s_fb_touched[0] = s_fb_touched[1] = 0;
    s_right_eye = 0;
    /* Rebuild the narrow signatures from memory. */
    s_clock = 1; s_map_stamp = s_chr_stamp = 0;
    memset(s_page_sig, 0, sizeof(s_page_sig));
    memset(s_seg_sig, 0, sizeof(s_seg_sig));
    memset(s_row_sig, 0, sizeof(s_row_sig));
    memset(s_chr_sig, 0, sizeof(s_chr_sig));
    {
        const uint16_t* m = (const uint16_t*)&s_low[0x20000u];
        for (uint32_t i = 0; i < 0x10000u; i++) {
            uint32_t o = 0x20000u + i * 2u;
            if (o >= 0x3DC00u && o < 0x3E000u) continue;
            uint32_t d = m[i] * sig_key(o >> 1);
            s_page_sig[i >> 6] += d; s_seg_sig[i >> 12] += d;
        }
        for (uint32_t e = 0; e < 1024u; e++) {
            const uint16_t* ent = m + (0x1E000u + e * 8u) / 2u;
            uint32_t hsh = 0, key = 0x40000u + (uint32_t)(ent - (const uint16_t*)s_low);
            for (unsigned i = 0; i < 4u; i++) hsh += ent[i] * sig_key(key + i);
            row_sig_add(oam_blocks(ent[2]), hsh);
        }
        for (uint32_t w = 0; w < 32u; w++) {
            const uint16_t* ent = m + (0x1D800u + w * 32u) / 2u;
            uint32_t hsh = 0, key = 0x50000u + (uint32_t)(ent - (const uint16_t*)s_low);
            for (unsigned i = 1; i < 16u; i++) hsh += ent[i] * sig_key(key + i);
            row_sig_add(world_blocks(ent), hsh);
            sig_update(SIG_KEY_REG + 0x200 + w * 16u, 0, ent[0]);
        }
        const uint16_t* c = s_chr;
        for (uint32_t i = 0; i < VIP_CHR_SIZE / 2u; i++)
            s_chr_sig[i >> 9] += c[i] * sig_key(0x60000u + i);
    }
    vip_check_irq();
}

/* ----------------- Column state machine ----------------- */

/* Pass `k` column boundaries at once (never beyond the end of a phase).
 *
 * In an active display half the hardware samples a brightness "repeat"
 * value from the column table every 4 columns (Beetle vip.c:1357-1366).
 * Only the last sample of a span can still matter - nothing reads the
 * brightness in between - so only that one is taken. */
static void vip_advance_columns(int32_t k) {
    if (s_display_region & 1) {
        int32_t last = (s_column + k - 1) & ~3;
        if (last >= s_column) {
            int lr = (s_display_region & 2) >> 1;
            uint32_t flat = 0x20000u + 0x1DFFEu
                          - (uint32_t)((last >> 2) * 2) - (lr ? 0u : 0x200u);
            uint8_t repeat = s_low[flat + 1];
            if (repeat != s_brt_repeat) {
                s_brt_repeat = repeat;
                recalc_brt_cache();
            }
        }
    }

    s_column += k;
    if (s_column != VIP_COLUMNS_PER_PHASE) return;
    s_column = 0;

    if (s_display_active && (s_display_region & 1)) {
        s_intpnd |= (s_display_region & 2) ? VB_VIP_INT_RFB_END
                                           : VB_VIP_INT_LFB_END;
        vip_check_irq();
    }

    s_display_region = (s_display_region + 1) & 3;
    if (s_display_region != 0) return;

    s_display_active = (s_dpctrl & DPCTRL_DISP) ? 1 : 0;
    if (s_display_active) {
        s_intpnd |= VB_VIP_INT_FRAME_START;
        vip_check_irq();
    }

    s_game_frame_counter++;
    if (s_game_frame_counter > (int32_t)s_frmcyc) {
        s_intpnd |= VB_VIP_INT_GAME_START;
        vip_check_irq();
        vb_input_frame_advance();
        vbds_on_game_frame();
        if (s_xpctrl & XPCTRL_XP_EN) {
            s_display_fb      = s_drawing_fb;
            s_drawing_fb     ^= 1;
            s_drawing_block   = 0;
            s_drawing_active  = 1;
            s_drawing_counter = 1120 * 4;
            /* The picture changes only if the two slots were drawn from
             * different states. */
            if (!slots_show_same_picture()) s_dirty = 1;
        }
        s_game_frame_counter = 0;
    }
}

static void vip_advance_chunk(int32_t chunk_clocks) {
    s_vip_cycles += (uint64_t)chunk_clocks;

    if (s_drawing_counter > 0) {
        s_drawing_counter -= chunk_clocks;
        if (s_drawing_counter <= 0) {
            vip_draw_block_now((uint8_t)s_drawing_block);
            s_sbout_inactive_time = (int64_t)(s_vip_cycles + s_update_span) + 1120;
            s_sb_latch = s_drawing_block;
            s_drawing_block++;
            if (s_drawing_block == 28) {
                s_drawing_active = 0;
                s_intpnd |= VB_VIP_INT_XP_END;
                vip_check_irq();
            } else {
                s_drawing_counter += 1120 * 4;
            }
        }
    }

    if (chunk_clocks < s_column_counter) { s_column_counter -= chunk_clocks; return; }
    int32_t rest = chunk_clocks - s_column_counter;
    s_column_counter = VIP_CYCLES_PER_COLUMN - rest % VIP_CYCLES_PER_COLUMN;
    vip_advance_columns(1 + rest / VIP_CYCLES_PER_COLUMN);
}

VBDS_FAST void vb_vip_tick(uint64_t cycles) {
    s_update_span = cycles;
    int64_t clocks = (int64_t)cycles;
    while (clocks > 0) {
        int32_t chunk = clocks > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)clocks;
        if (s_drawing_counter > 0 && chunk > s_drawing_counter)
            chunk = s_drawing_counter;
        /* Many columns per step, but stop at the end of a display phase. */
        int32_t to_phase_end = s_column_counter
            + (VIP_COLUMNS_PER_PHASE - 1 - s_column) * VIP_CYCLES_PER_COLUMN;
        if (chunk > to_phase_end)
            chunk = to_phase_end;
        if (chunk <= 0) chunk = 1;
        vip_advance_chunk(chunk);
        clocks -= chunk;
    }
}

/* Cycles until the VIP next needs servicing.
 *
 * Stock behaviour (VBDS_COARSE_EVENTS=0): every column boundary, like the
 * reference runtime.  Coarse (default): only where guest-visible state can
 * change - the end of a 384-column display phase or the completion of a
 * drawing block.  That is about 32 stops per frame instead of 1,536. */
int32_t vb_vip_cycles_to_next_event(void) {
#if VBDS_COARSE_EVENTS
    int32_t n = s_column_counter
              + (VIP_COLUMNS_PER_PHASE - 1 - s_column) * VIP_CYCLES_PER_COLUMN;
    if (s_drawing_counter > 0 && s_drawing_counter < n) n = s_drawing_counter;
#else
    int32_t n = s_column_counter;
#endif
    return n < 1 ? 1 : n;
}

/* ----------------- Registers ----------------- */

static uint16_t vip_read_register16(uint32_t offset) {
    uint32_t sub = (offset - VIP_REGISTER_BASE) & 0xFEu;
    switch (sub) {
        case 0x00: return s_intpnd;
        case 0x02: return s_intenb;
        case 0x04: return 0;
        case 0x20: {
            uint16_t ret = s_dpctrl & 0x702u;
            if ((s_display_region & 1) && s_display_active) {
                unsigned dpbsy = 1u << ((s_display_region >> 1) & 1);
                if (s_display_fb) dpbsy <<= 2;
                ret |= (uint16_t)(dpbsy << 2);
            }
            ret |= 1u << 6;
            return ret;
        }
        case 0x22: return s_dpctrl;
        case 0x24: return (uint16_t)s_brta;
        case 0x26: return (uint16_t)s_brtb;
        case 0x28: return (uint16_t)s_brtc;
        case 0x2A: return (uint16_t)s_rest;
        case 0x2E: return s_frmcyc;
        case 0x30: return 0xFFFFu;
        case 0x40: {
            uint16_t ret = (uint16_t)(s_xpctrl & 0x2u);
            if (s_drawing_active)
                ret |= (uint16_t)((1 + s_drawing_fb) << 2);
            if ((int64_t)vbds_access_cycle() < s_sbout_inactive_time) {
                ret |= 0x8000u;
                ret |= (uint16_t)((s_sb_latch & 0xFFu) << 8);
            }
            return ret;
        }
        case 0x42: return s_xpctrl;
        case 0x44: return 2;
        case 0x48: case 0x4A: case 0x4C: case 0x4E: return s_spt[(sub >> 1) & 3];
        case 0x60: case 0x62: case 0x64: case 0x66: return s_gplt[(sub >> 1) & 3];
        case 0x68: case 0x6A: case 0x6C: case 0x6E: return s_jplt[(sub >> 1) & 3];
        case 0x70: return s_bkcol;
        default:   return 0;
    }
}

static void vip_write_register16(uint32_t offset, uint16_t v) {
    uint32_t sub = (offset - VIP_REGISTER_BASE) & 0xFEu;
    switch (sub) {
        case 0x02:
            s_intenb = v & VB_VIP_INT_MASK_VALID;
            vip_check_irq();
            break;
        case 0x04:
            s_intpnd &= ~v;
            vip_check_irq();
            break;
        case 0x22:
            s_dpctrl = v & 0x703u;
            if (v & DPCTRL_DPRST) {
                s_display_active = 0;
                s_intpnd &= (uint16_t)~(VB_VIP_INT_TIME_ERR | VB_VIP_INT_FRAME_START
                                      | VB_VIP_INT_GAME_START | VB_VIP_INT_RFB_END
                                      | VB_VIP_INT_LFB_END | VB_VIP_INT_SCANERR);
                vip_check_irq();
            }
            break;
        case 0x24: s_brta = (uint8_t)v; recalc_brt_cache(); break;
        case 0x26: s_brtb = (uint8_t)v; recalc_brt_cache(); break;
        case 0x28: s_brtc = (uint8_t)v; recalc_brt_cache(); break;
        case 0x2A: s_rest = (uint8_t)v; recalc_brt_cache(); break;
        case 0x2E: s_frmcyc = v & 0xFu; break;
        case 0x42:
            s_xpctrl = v & 0x0002u;
            s_sbcmp  = (uint16_t)((v >> 8) & 0x1Fu);
            if (v & XPCTRL_XP_RST) {
                s_drawing_fb      = s_display_fb;
                s_display_fb     ^= 1;
                s_drawing_active  = 0;
                s_drawing_counter = 0;
                s_dirty = 1;
                s_intpnd &= (uint16_t)~(VB_VIP_INT_SB_HIT | VB_VIP_INT_XP_END
                                      | VB_VIP_INT_TIME_ERR);
                vip_check_irq();
            }
            break;
        case 0x48: case 0x4A: case 0x4C: case 0x4E:
            sig_update(SIG_KEY_REG + sub, s_spt[(sub >> 1) & 3], v & 0x3FFu);
            s_spt[(sub >> 1) & 3] = v & 0x3FFu;
            break;
        case 0x60: case 0x62: case 0x64: case 0x66: {
            int which = (sub >> 1) & 3;
            sig_update(SIG_KEY_REG + sub, s_gplt[which], v & 0xFCu);
            s_gplt[which] = v & 0xFCu;
            recalc_gplt_cache(which);
            break;
        }
        case 0x68: case 0x6A: case 0x6C: case 0x6E: {
            int which = (sub >> 1) & 3;
            sig_update(SIG_KEY_REG + sub, s_jplt[which], v & 0xFCu);
            s_jplt[which] = v & 0xFCu;
            recalc_jplt_cache(which);
            break;
        }
        case 0x70:
            sig_update(SIG_KEY_REG + sub, s_bkcol, v & 0x3u);
            s_bkcol = v & 0x3u;
            break;
        default: break;
    }
}

/* ----------------- Bus surface ----------------- */

static inline int vip_is_register(uint32_t o) {
    return o >= VIP_REGISTER_BASE && o < VIP_REGISTER_END;
}

/* Storage for a non-register offset.  Character RAM is reachable through
 * four alias windows below 0x20000 and through 0x70000-0x7FFFF; all share
 * one 32 KB array (Beetle vip.c:670-686, 715-716).  Offsets with nothing
 * behind them read as zero and ignore writes.  `*kind`: 0 nothing behind
 * it, 1 framebuffer, 2 data the renderer reads. */
static inline uint8_t* vip_ptr(uint32_t o, int* kind) {
    if (o < 0x20000u) {
        if ((o & 0x7FFFu) >= 0x6000u) {
            *kind = 2;
            return (uint8_t*)s_chr + ((o & 0x1FFFu) | ((o >> 2) & 0x6000u));
        }
        *kind = 1;
        return &s_low[o];
    }
    if (o < VIP_LOW_SIZE) { *kind = 2; return &s_low[o]; }
    if (o >= 0x70000u) { *kind = 2; return (uint8_t*)s_chr + (o & 0x7FFFu); }
    *kind = 0;
    return s_unmapped;
}

VBDS_FAST static void vip_store(uint32_t o, uint32_t v, unsigned width) {
    int kind;
    uint8_t* p = vip_ptr(o, &kind);
    if (!kind) return;
    uint32_t old = 0;
    for (unsigned i = 0; i < width; i++) old |= (uint32_t)p[i] << (i * 8);
    if (old == v) return;
    if (kind == 2 && o >= 0x20000u && o < 0x40000u
        && (o < 0x3DC00u || o >= 0x3E000u)) {
        /* Maps, parameter tables, world attributes, sprite attributes. */
        const int oam = o >= 0x3E000u, world = o >= 0x3D800u && o < 0x3DC00u;
        uint16_t* h = (uint16_t*)&s_low[o & ~1u];
        const unsigned nh = width == 4 ? 2u : 1u;
        uint32_t hw_old[2] = { h[0], nh == 2 ? h[1] : 0 };
        uint16_t* ent = 0;
        uint32_t ekey = 0, emask_old = 0, w0_old = 0;
        unsigned ewords = 0;
        if (oam | world) {
            ent = (uint16_t*)&s_low[oam ? (o & ~7u) : (o & ~31u)];
            ekey = (oam ? 0x40000u : 0x50000u) + (uint32_t)(ent - (uint16_t*)s_low);
            ewords = oam ? 4u : 16u;
            emask_old = oam ? oam_blocks(ent[2]) : world_blocks(ent);
            w0_old = ent[0];
        }
        for (unsigned i = 0; i < width; i++) p[i] = (uint8_t)(v >> (i * 8));
        /* Page and segment signatures (and the entry's), per halfword. */
        uint32_t page = (o - 0x20000u) >> 7, seg = (o - 0x20000u) >> 13;
        uint32_t edelta = 0;
        for (unsigned i = 0; i < nh; i++) {
            uint32_t d = (uint32_t)(h[i] - hw_old[i]);
            if (!d) continue;
            uint32_t pd = d * sig_key((o >> 1) + i);
            s_page_sig[page] += pd;
            s_seg_sig[seg] += pd;
            if (ent) {
                unsigned idx = (unsigned)(h + i - ent);
                if (idx >= (unsigned)world) edelta += d * sig_key(ekey + idx);
            }
        }
        s_map_stamp = ++s_clock;
        if (ent) {
            if (world && ent[0] != w0_old)
                sig_update(SIG_KEY_REG + 0x200 + (uint32_t)(ent - (uint16_t*)&s_low[0x3D800u]),
                           w0_old, ent[0]);
            uint32_t emask = oam ? oam_blocks(ent[2]) : world_blocks(ent);
            if (emask == emask_old) {
                row_sig_add(emask, edelta);
            } else {
                uint32_t ehash = 0;
                for (unsigned i = world; i < ewords; i++) ehash += ent[i] * sig_key(ekey + i);
                row_sig_add(emask_old, 0u - (ehash - edelta));
                row_sig_add(emask, ehash);
            }
        }
    } else if (kind == 2 && p >= (uint8_t*)s_chr && p < (uint8_t*)s_chr + VIP_CHR_SIZE) {
        uint32_t off = (uint32_t)(p - (uint8_t*)s_chr) & ~1u;
        uint16_t* h = (uint16_t*)((uint8_t*)s_chr + off);
        uint32_t hw_old[2] = { h[0], width == 4 ? h[1] : 0 };
        for (unsigned i = 0; i < width; i++) p[i] = (uint8_t)(v >> (i * 8));
        for (unsigned i = 0; i < (width == 4 ? 2u : 1u); i++) {
            uint32_t oi = off + 2u * i;
            s_chr_rows[(oi >> 1) & 7u][oi >> 4] = h[i];
            s_chr_sig[off >> 10] += (uint32_t)(h[i] - hw_old[i]) * sig_key(0x60000u + (off >> 1) + i);
        }
        s_chr_stamp = ++s_clock;
    } else if (kind == 2) {
        /* Key on the storage location so every alias of character RAM
         * updates the same terms. */
        uint32_t key = (uint32_t)(p - (p >= (uint8_t*)s_chr && p < (uint8_t*)s_chr + VIP_CHR_SIZE
                                       ? (uint8_t*)s_chr - 0x80000u : s_low));
        /* One signature term per halfword, whatever the access width. */
        if (width == 1) {
            uint8_t* h = p - (key & 1u);
            uint32_t before = (uint32_t)h[0] | (uint32_t)h[1] << 8;
            p[0] = (uint8_t)v;
            sig_update(key >> 1, before, (uint32_t)h[0] | (uint32_t)h[1] << 8);
        } else {
            for (unsigned i = 0; i < width; i += 2) {
                uint32_t before = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
                uint32_t after = (v >> (i * 8)) & 0xFFFFu;
                if (before != after) {
                    sig_update((key + i) >> 1, before, after);
                    p[i] = (uint8_t)after; p[i + 1] = (uint8_t)(after >> 8);
                }
            }
        }
    } else {
        for (unsigned i = 0; i < width; i++) p[i] = (uint8_t)(v >> (i * 8));
        /* CPU drew into a framebuffer: slot 0 at 0x0000/0x10000, slot 1 at
         * 0x8000/0x18000. */
        int slot = (o >> 15) & 1;
        s_fb_touched[slot] = 1;
        if (o < 0x10000u) s_page_stale[slot] = 1;
        s_pack = 1;
        if (slot == s_display_fb) s_dirty = 1;
    }
}

VBDS_FAST uint8_t vb_vip_read8(uint32_t addr) {
    uint32_t o = addr & (VIP_WINDOW_SIZE - 1u);
    if (vip_is_register(o)) {
        uint16_t r = vip_read_register16(o & ~1u);
        return (o & 1u) ? (uint8_t)(r >> 8) : (uint8_t)r;
    }
    int kind;
    return *vip_ptr(o, &kind);
}

VBDS_FAST uint16_t vb_vip_read16(uint32_t addr) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~1u;
    if (vip_is_register(o)) return vip_read_register16(o);
    int kind;
    const uint8_t* p = vip_ptr(o, &kind);
    return (uint16_t)(p[0] | (p[1] << 8));
}

VBDS_FAST uint32_t vb_vip_read32(uint32_t addr) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~3u;
    if (vip_is_register(o))
        return (uint32_t)vip_read_register16(o)
             | ((uint32_t)vip_read_register16(o + 2) << 16);
    int kind;
    const uint8_t* p = vip_ptr(o, &kind);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

VBDS_FAST void vb_vip_write8(uint32_t addr, uint8_t v) {
    uint32_t o = addr & (VIP_WINDOW_SIZE - 1u);
    if (vip_is_register(o)) {
        uint16_t current = vip_read_register16(o & ~1u);
        uint16_t merged = (o & 1u)
            ? (uint16_t)((current & 0x00FFu) | ((uint16_t)v << 8))
            : (uint16_t)((current & 0xFF00u) | v);
        vip_write_register16(o & ~1u, merged);
        return;
    }
    vip_store(o, v, 1);
}

VBDS_FAST void vb_vip_write16(uint32_t addr, uint16_t v) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~1u;
    if (vip_is_register(o)) { vip_write_register16(o, v); return; }
    vip_store(o, v, 2);
}

VBDS_FAST void vb_vip_write32(uint32_t addr, uint32_t v) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~3u;
    if (vip_is_register(o)) {
        vip_write_register16(o, (uint16_t)v);
        vip_write_register16(o + 2, (uint16_t)(v >> 16));
        return;
    }
    vip_store(o, v, 4);
}

/* ----------------- Renderer -----------------
 * Port of Beetle vip_draw.inc via vbrecomp vip.c: DrawBG (normal / H-bias),
 * DrawAffine, DrawOBJ and the 8-row block walk over 32 worlds. */

#define BGM_AFFINE 0x2u
#define BGM_OBJ    0x3u
#define ROW_STRIDE 512
#define PAD_LEFT   8

static int s_obj_search_which;
/* Rows of the block being drawn, one byte per pixel.  The left eye's (the
 * one shown on the DS) lives in the data TCM. */
VBDS_DTCM_BSS static uint32_t s_row_l[8 * ROW_STRIDE / 4];
static uint32_t s_row_r[8 * ROW_STRIDE / 4];
static uint32_t* const s_row_store[2] = { s_row_l, s_row_r };
static uint8_t* const s_row_buf[2] = { (uint8_t*)s_row_l, (uint8_t*)s_row_r };

#define dram_u16() ((const uint16_t*)&s_low[0x20000u])

static inline int16_t sign_10(uint32_t v) { return (int16_t)(((v & 0x3FFu) ^ 0x200u) - 0x200u); }
static inline int16_t sign_11(uint32_t v) { return (int16_t)(((v & 0x7FFu) ^ 0x400u) - 0x400u); }
static inline int16_t sign_9(uint32_t v)  { return (int16_t)(((v & 0x1FFu) ^ 0x100u) - 0x100u); }

VBDS_FAST static void draw_bg(uint8_t* target, uint16_t real_y,
                    uint8_t bgmap_base_raw, int overplane, uint16_t overplane_char,
                    int32_t source_x, int32_t source_y,
                    uint32_t scx, uint32_t scy,
                    uint16_t dest_x, uint16_t dest_y,
                    uint16_t dest_width, uint16_t dest_height) {
    const uint16_t* bgm = dram_u16();
    uint32_t bgmap_base = (uint32_t)bgmap_base_raw << 12;
    int32_t start_x, final_x;
    const uint32_t bgsc_overplane = bgm[overplane_char];
    const uint32_t source_x_size = 512u << scx;
    const uint32_t source_y_size = 512u << scy;
    const uint32_t source_x_mask = overplane ? 0x1FFFu : (source_x_size - 1u);
    const uint32_t source_y_mask = overplane ? 0x1FFFu : (source_y_size - 1u);

    if ((uint16_t)(real_y - dest_y) > dest_height) return;

    dest_x = (uint16_t)sign_10((uint32_t)dest_x);
    if (dest_x & 0x8000u) source_x -= (int16_t)dest_x;

    start_x = (int16_t)dest_x;
    final_x = (int16_t)dest_x + dest_width;
    if (start_x < 0)   start_x = 0;
    if (final_x > 383) final_x = 383;
    if (start_x > final_x) return;

    source_y &= (int32_t)source_y_mask;
    bgmap_base |= (((uint32_t)source_y >> 3) & 0x3Fu) * 0x40u
                | (((uint32_t)source_y << 3) & ~0xFFFu) << scx;

    dep_cell(overplane_char);
    for (uint32_t sx = 0; sx < (1u << scx); sx++)
        dep_cell(bgmap_base | sx << 12);

    uint32_t used = 0;
    for (int x = start_x; x <= final_x; x++) {
        uint32_t source_x_u = (uint32_t)source_x & source_x_mask;
        uint32_t bgsc = bgsc_overplane;

        if (source_x_u < source_x_size && (uint32_t)source_y < source_y_size) {
            uint32_t idx = (bgmap_base | ((source_x_u << 3) & ~0xFFFu)
                          | ((source_x_u >> 3) & 0x3Fu)) & 0xFFFFu;
            bgsc = bgm[idx];
        }

        uint32_t char_no    = bgsc & 0x7FFu;
        used |= 1u << (char_no >> 6);
        const uint8_t* pal  = s_gplt_cache[bgsc >> 14];
        uint32_t hflip_xor  = (bgsc & 0x2000u) ? 7u : 0u;
        uint32_t vflip_xor  = (bgsc & 0x1000u) ? 7u : 0u;
        uint32_t char_sub_y = vflip_xor ^ ((uint32_t)source_y & 0x7u);

        if (!(source_x_u & 7u) && (x + 7) <= final_x) {
            uint32_t pixels = s_chr_rows[char_sub_y][char_no];
            if (pixels) {
                if (bgsc & 0x2000u)
                    pixels = (uint32_t)(s_rev2bpp[pixels & 0xFFu] << 8) | s_rev2bpp[pixels >> 8];
                const uint32_t* lut = s_bg4[bgsc >> 14];
                uint8_t* t = target + x;
                for (int half = 0; half < 2; half++, t += 4, pixels >>= 8) {
                    uint32_t b = pixels & 0xFFu, m = s_bg4_opaque[b], w = lut[b];
                    if (m == 0xFu) {
                        if (!((uintptr_t)t & 3u)) {
                            *(uint32_t*)(void*)t = w;          /* little-endian */
                        } else {
                            t[0] = (uint8_t)w; t[1] = (uint8_t)(w >> 8);
                            t[2] = (uint8_t)(w >> 16); t[3] = (uint8_t)(w >> 24);
                        }
                    } else if (m) {
                        if (m & 1u) t[0] = (uint8_t)w;
                        if (m & 2u) t[1] = (uint8_t)(w >> 8);
                        if (m & 4u) t[2] = (uint8_t)(w >> 16);
                        if (m & 8u) t[3] = (uint8_t)(w >> 24);
                    }
                }
            }
            x += 7;
            source_x += 8;
        } else {
            uint32_t char_sub_x = hflip_xor ^ (source_x_u & 0x7u);
            uint32_t pixel = (s_chr_rows[char_sub_y][char_no] >> (char_sub_x * 2u)) & 0x3u;
            if (pixel) target[x] = pal[pixel];
            source_x++;
        }
    }
    s_cur_chr |= used;
}

VBDS_FAST static void draw_affine(uint8_t* target, uint16_t real_y, int lr,
                        uint32_t param_base, uint32_t bgmap_base,
                        int overplane_mode, uint16_t overplane_char,
                        uint32_t scx, uint32_t scy,
                        uint16_t dest_x, uint16_t dest_y,
                        uint16_t dest_width, uint16_t dest_height) {
    const uint16_t* bgm = dram_u16();
    const uint32_t source_x_size = 512u << scx;
    const uint32_t source_y_size = 512u << scy;
    const uint16_t* pp = &bgm[(param_base + 8u * (real_y - dest_y)) & 0xFFFFu];
    int16_t mx = (int16_t)pp[0];
    int16_t mp = (int16_t)pp[1];
    int16_t my = (int16_t)pp[2];
    int16_t dx = (int16_t)pp[3];
    int16_t dy = (int16_t)pp[4];
    uint32_t source_x, source_y, source_x_mask, source_y_mask;
    int32_t start_x, final_x;
    const uint32_t bgsc_overplane = bgm[overplane_char];

    dest_x = (uint16_t)sign_10((uint32_t)dest_x);
    if ((uint16_t)(real_y - dest_y) > dest_height) return;

    dep_cell(param_base + 8u * (real_y - dest_y));
    dep_cell(overplane_char);

    source_x = (uint32_t)((int32_t)mx << 6);
    source_y = (uint32_t)((int32_t)my << 6);

    if (dest_x & 0x8000u) {
        source_x += (uint32_t)((int32_t)dx * (65536 - (int32_t)dest_x));
        source_y += (uint32_t)((int32_t)dy * (65536 - (int32_t)dest_x));
    }
    if (mp >= 0 && lr) {
        source_x += (uint32_t)((int32_t)dx * mp);
        source_y += (uint32_t)((int32_t)dy * mp);
    } else if (mp < 0 && !lr) {
        source_x += (uint32_t)((int32_t)dx * -mp);
        source_y += (uint32_t)((int32_t)dy * -mp);
    }

    if (overplane_mode) {
        source_x_mask = 0x3FFFFFFu;
        source_y_mask = 0x3FFFFFFu;
    } else {
        source_x_mask = (source_x_size << 9) - 1u;
        source_y_mask = (source_y_size << 9) - 1u;
    }

    start_x = (int16_t)dest_x;
    final_x = (int16_t)dest_x + dest_width;
    if (start_x < 0) start_x = 0;
    if (final_x > 383) final_x = 383;

    if (dy == 0) {
        source_y &= source_y_mask;
        if (source_y >= (source_y_size << 9)) return;
        bgmap_base |= (((source_y >> 6) & ~0xFFFu) << scx)
                    | (((source_y >> 12) & 0x3Fu) * 0x40u);
        for (uint32_t k = 0; k < (1u << scx); k++)
            dep_cell(bgmap_base | k << 12);
        /* Walk the row one map cell (8 source pixels) at a time: look up the
         * cell and its character row once, then step through it.  Cells are
         * 4096 source units wide and every mask and size limit is a multiple
         * of that, so a cell is entirely inside or outside the map. */
        const uint32_t xlimit = source_x_size << 9;
        const uint32_t sub_y = (source_y >> 9) & 0x7u;
        /* 2^32 / |dx|, so the steps left in a cell need no division. */
        const uint32_t adx = (uint32_t)(dx < 0 ? -dx : dx);
        const uint32_t recip = adx ? (uint32_t)(0xFFFFFFFFu / adx) : 0;
        uint32_t sx = source_x & source_x_mask;
        uint32_t used = 0;
        int x = start_x;
        while (x <= final_x) {
            const uint32_t cell = sx >> 12;
            /* Output pixels until sx leaves this cell: the smallest n with
             * sx + n*dx outside [cell*4096, cell*4096 + 4096). */
            int n;
            if (!adx) {
                n = final_x - x + 1;
            } else {
                uint32_t dist = dx > 0 ? ((cell + 1) << 12) - sx + adx - 1
                                       : sx - (cell << 12) + adx;
                n = (int)(((uint64_t)dist * recip) >> 32);
                if ((uint32_t)n * adx > dist) n--;          /* estimate may be 1 high */
                if ((uint32_t)(n + 1) * adx <= dist) n++;   /* ...or 1 low */
                if (n < 1) n = 1;
                if (n > final_x - x + 1) n = final_x - x + 1;
            }
            uint32_t bgsc = bgsc_overplane;
            if (sx < xlimit)
                bgsc = bgm[(bgmap_base | ((sx >> 6) & ~0xFFFu)
                           | ((sx >> 12) & 0x3Fu)) & 0xFFFFu];
            used |= 1u << ((bgsc & 0x7FFu) >> 6);
            uint32_t row = s_chr_rows[((bgsc & 0x1000u) ? 7u : 0u) ^ sub_y][bgsc & 0x7FFu];
            if (row) {
                if (bgsc & 0x2000u)
                    row = (uint32_t)(s_rev2bpp[row & 0xFFu] << 8) | s_rev2bpp[row >> 8];
                const uint8_t* pal = s_gplt_cache[bgsc >> 14];
                uint32_t p = sx;
                uint8_t* t = target + x;
                if (((row | (row >> 1)) & 0x5555u) == 0x5555u) {
                    /* No transparent pixel in this cell row: look up the 8
                     * colours once, then store without testing. */
                    uint8_t e[8];
                    for (int j = 0; j < 8; j++) e[j] = pal[(row >> (2 * j)) & 3u];
                    for (int k = 0; k < n; k++) { t[k] = e[(p >> 9) & 7u]; p += (uint32_t)dx; }
                } else {
                    for (int k = 0; k < n; k++) {
                        uint32_t v = (row >> ((p >> 8) & 0xEu)) & 0x3u;
                        if (v) t[k] = pal[v];
                        p += (uint32_t)dx;
                    }
                }
            }
            x += n;
            sx = (uint32_t)((int32_t)sx + (int32_t)n * dx) & source_x_mask;
        }
        s_cur_chr |= used;
    } else {
        for (uint32_t k = 0; k < (1u << (scx + scy)); k++)
            dep_add(DEP_SEG | (((bgmap_base >> 12) | k) & 15u));
        for (int x = start_x; x <= final_x; x++) {
            uint32_t bgsc = bgsc_overplane;
            source_x &= source_x_mask;
            source_y &= source_y_mask;
            if (source_x < (source_x_size << 9) && source_y < (source_y_size << 9)) {
                uint32_t m_index   = ((source_x >> 6) & ~0xFFFu)
                                   + (((source_y >> 6) & ~0xFFFu) << scx);
                uint32_t sub_index = ((source_x >> 12) & 0x3Fu)
                                   + (((source_y >> 12) & 0x3Fu) * 0x40u);
                bgsc = bgm[(bgmap_base | m_index | sub_index) & 0xFFFFu];
            }
            uint32_t hflip_xor  = (bgsc & 0x2000u) ? 7u : 0u;
            uint32_t vflip_xor  = (bgsc & 0x1000u) ? 7u : 0u;
            uint32_t char_sub_y = vflip_xor ^ ((source_y >> 9) & 0x7u);
            uint32_t char_sub_x = hflip_xor ^ ((source_x >> 9) & 0x7u);
            s_cur_chr |= 1u << ((bgsc & 0x7FFu) >> 6);
            uint32_t pixel = (s_chr_rows[char_sub_y][bgsc & 0x7FFu] >> (char_sub_x * 2u)) & 0x3u;
            if (pixel) target[x] = s_gplt_cache[bgsc >> 14][pixel];
            source_x = (uint32_t)((int32_t)source_x + dx);
            source_y = (uint32_t)((int32_t)source_y + dy);
        }
    }
}

VBDS_FAST static void draw_obj(uint8_t* fb_lr[2], uint16_t y, const int lron[2]) {
    const uint16_t* bgm = dram_u16();
    int32_t start_oam = s_spt[s_obj_search_which];
    int32_t end_oam   = s_obj_search_which ? s_spt[s_obj_search_which - 1] : 1023;
    int32_t oam = start_oam;

    do {
        const uint16_t* oam_ptr = &bgm[(0x1E000u + (uint32_t)oam * 8u) >> 1];
        uint32_t tile_y = (y - oam_ptr[2]) & 0xFFu;
        if (tile_y >= 8u) continue;

        uint32_t attr = oam_ptr[3];
        s_cur_chr |= 1u << ((attr & 0x7FFu) >> 6);
        uint32_t char_sub_y = ((attr & 0x1000u) ? 7u : 0u) ^ tile_y;
        uint32_t pixels_save = s_chr_rows[char_sub_y][attr & 0x7FFu];
        if (!pixels_save) continue;

        uint32_t jx = oam_ptr[0];
        uint32_t jp = oam_ptr[1] & 0x3FFFu;
        const uint8_t* pal = s_jplt_cache[attr >> 14];
        int jlron[2] = { (int)(oam_ptr[1] & 0x8000u), (int)(oam_ptr[1] & 0x4000u) };

        for (int lr = 0; lr < 2; lr++) {
            if (!(jlron[lr] && lron[lr])) continue;
            uint32_t pixels = pixels_save;
            uint32_t raw = (jx + (lr ? jp : (uint32_t)-(int32_t)jp)) & 0x3FFu;
            int32_t x = (int32_t)((raw ^ 0x200u) - 0x200u);
            if (x >= -7 && x < 384) {
                uint8_t* tgt = &fb_lr[lr][x];
                if (attr & 0x2000u) {
                    tgt += 7;
                    for (int m = 8; m; m--) {
                        if (pixels & 3u) *tgt = pal[pixels & 3u];
                        tgt--; pixels >>= 2;
                    }
                } else {
                    for (int m = 8; m; m--) {
                        if (pixels & 3u) *tgt = pal[pixels & 3u];
                        tgt++; pixels >>= 2;
                    }
                }
            }
        }
    } while ((oam = (oam - 1) & 1023) != end_oam);
}

VBDS_FAST static void vip_draw_block_into(uint8_t block_no, uint8_t* fb_l, uint8_t* fb_r) {
    const uint16_t* bgm = dram_u16();
    const int eyes = s_right_eye ? 2 : 1;
    const uint8_t bkcol = (uint8_t)(s_bkcol & 0x3u);

    for (int lr = 0; lr < eyes; lr++)
        memset(s_row_buf[lr], bkcol, 8 * ROW_STRIDE);
    s_cur_dep_n = 0;
    s_cur_chr = 0;

    s_obj_search_which = 3;

    for (int world = 31; world >= 0; world--) {
        const uint16_t* wp = &bgm[(0x1D800u + (uint32_t)world * 0x20u) >> 1];

        uint32_t bgmap_base = wp[0] & 0xFu;
        int      over       = (wp[0] & 0x80u) != 0;
        uint32_t scy        = (wp[0] >> 8) & 3u;
        uint32_t scx        = (wp[0] >> 10) & 3u;
        uint32_t bgm_mode   = (wp[0] >> 12) & 3u;
        int      lron[2]    = { (int)(wp[0] & 0x8000u),
                                s_right_eye ? (int)(wp[0] & 0x4000u) : 0 };
        uint16_t gx         = (uint16_t)sign_11(wp[1]);
        uint16_t gp         = (uint16_t)sign_9(wp[2]);
        uint16_t gy         = (uint16_t)sign_11(wp[3]);
        uint16_t mx         = wp[4];
        uint16_t mp         = (uint16_t)sign_9(wp[5]);
        uint16_t my         = wp[6];
        uint16_t window_w   = (uint16_t)sign_11(wp[7]);
        uint16_t window_h   = wp[8] & 0x3FFu;
        uint32_t param_base = wp[9] & 0xFFF0u;
        uint16_t over_chr   = wp[10];

        if (wp[0] & 0x40u) break;       /* END world */

        if (bgm_mode == BGM_OBJ || lron[0] || lron[1]) {
            for (int y = 0; y < 8; y++) {
                uint16_t real_y = (uint16_t)(block_no * 8 + y);
                uint8_t* fb[2] = {
                    &s_row_buf[0][PAD_LEFT + y * ROW_STRIDE],
                    &s_row_buf[1][PAD_LEFT + y * ROW_STRIDE],
                };
                if (bgm_mode == BGM_OBJ) {
                    draw_obj(fb, real_y, lron);
                    continue;
                }
                for (int lr = 0; lr < eyes; lr++) {
                    if (!lron[lr]) continue;
                    uint16_t dst_x = gx + (uint16_t)(lr ? gp : -gp);
                    if (bgm_mode == BGM_AFFINE) {
                        draw_affine(fb[lr], real_y, lr, param_base,
                                    bgmap_base * 4096u, over, over_chr,
                                    scx, scy, dst_x, gy, window_w, window_h);
                    } else {
                        uint16_t src_x = mx + (uint16_t)(lr ? mp : -mp);
                        uint16_t src_y = my + (uint16_t)(real_y - gy);
                        if (bgm_mode == 1u) {
                            uint32_t pi = param_base + (((real_y - gy) * 2u) | (uint32_t)lr);
                            dep_cell(pi);
                            src_x += (uint16_t)(int16_t)bgm[pi & 0xFFFFu];
                        }
                        draw_bg(fb[lr], real_y, (uint8_t)bgmap_base, over, over_chr,
                                (int32_t)(int16_t)src_x, (int32_t)(int16_t)src_y,
                                scx, scy, dst_x, gy, window_w, window_h);
                    }
                }
            }
        }

        if (bgm_mode == BGM_OBJ && s_obj_search_which) s_obj_search_which--;
    }

    /* Pack 8 rows of byte pixels into two 2bpp bytes per column, four
     * columns at a time: each row contributes one word of four pixels. */
    for (int lr = 0; s_pack && lr < eyes; lr++) {
        uint8_t* fb_target = (lr == 0 ? fb_l : fb_r) + block_no * 2;
        const uint32_t* src = &s_row_store[lr][PAD_LEFT / 4];
        for (int x = 0; x < 384; x += 4, src++, fb_target += 256) {
            uint32_t top = src[0] | (src[ROW_STRIDE / 4] << 2)
                         | (src[ROW_STRIDE / 2] << 4) | (src[ROW_STRIDE * 3 / 4] << 6);
            uint32_t bot = src[ROW_STRIDE] | (src[ROW_STRIDE * 5 / 4] << 2)
                         | (src[ROW_STRIDE * 6 / 4] << 4) | (src[ROW_STRIDE * 7 / 4] << 6);
            fb_target[0]   = (uint8_t)top;         fb_target[1]   = (uint8_t)bot;
            fb_target[64]  = (uint8_t)(top >> 8);  fb_target[65]  = (uint8_t)(bot >> 8);
            fb_target[128] = (uint8_t)(top >> 16); fb_target[129] = (uint8_t)(bot >> 16);
            fb_target[192] = (uint8_t)(top >> 24); fb_target[193] = (uint8_t)(bot >> 24);
        }
    }
}

static void vip_draw_block_now(uint8_t block_no) {
    int slot = s_drawing_fb & 1, other = slot ^ 1;
    fb_settle(slot); fb_settle(other);
    if (s_skip_all && block_current(slot, block_no)) {
        vbds_prof_blocks_skipped++;
        return;                     /* already holds exactly this picture */
    }
    if (block_current(other, block_no)) {
        if (!s_pack && s_page[slot] && s_page[other] && s_skip_copy) {
            /* The other slot already has this block: copy its page rows. */
            for (int y = 0; y < 8; y++) {
                int off = (block_no * 8 + y) * s_page_stride;
                memcpy(s_page[slot] + off, s_page[other] + off, 256);
            }
            s_block_sig[slot][block_no][0] = s_sig[0];
            s_block_sig[slot][block_no][1] = s_sig[1];
            s_block_time[slot][block_no] = s_block_time[other][block_no];
            s_block_row_sig[slot][block_no] = s_block_row_sig[other][block_no];
            s_block_dep_n[slot][block_no] = s_block_dep_n[other][block_no];
            memcpy(s_block_dep[slot][block_no], s_block_dep[other][block_no],
                   sizeof(s_block_dep[0][0]));
            memcpy(s_block_dep_sig[slot][block_no], s_block_dep_sig[other][block_no],
                   sizeof(s_block_dep_sig[0][0]));
            s_block_ver[slot][block_no] = s_block_ver[other][block_no];
            s_block_chr[slot][block_no] = s_block_chr[other][block_no];
            s_block_chr_sum[slot][block_no] = s_block_chr_sum[other][block_no];
            s_block_valid[slot][block_no] = 1;
            if (slot == s_display_fb) s_dirty = 1;
            vbds_prof_blocks_skipped++;
            return;
        }
        s_block_ver[slot][block_no] = s_block_ver[other][block_no];
    } else {
        s_block_ver[slot][block_no] = ++s_ver;
    }
    uint32_t t0 = vbds_tick_source ? vbds_tick_source() : 0;
    uint8_t* fb_l = &s_low[slot ? 0x8000u  : 0x0000u];
    uint8_t* fb_r = &s_low[slot ? 0x18000u : 0x10000u];
    vip_draw_block_into(block_no, fb_l, fb_r);
    if (s_page[slot])
        for (int y = 0; y < 8; y++)
            page_row(&s_row_buf[0][PAD_LEFT + y * ROW_STRIDE],
                     s_page[slot] + (block_no * 8 + y) * s_page_stride);
    s_block_sig[slot][block_no][0] = s_sig[0];
    s_block_sig[slot][block_no][1] = s_sig[1];
    s_block_time[slot][block_no] = s_clock;
    s_block_row_sig[slot][block_no] = s_row_sig[block_no];
    s_block_chr[slot][block_no] = s_cur_chr;
    {
        uint32_t sum = 0;
        for (uint32_t m = s_cur_chr; m; m &= m - 1) sum += s_chr_sig[__builtin_ctz(m)];
        s_block_chr_sum[slot][block_no] = sum;
    }
    s_block_dep_n[slot][block_no] = (uint8_t)s_cur_dep_n;
    if (s_cur_dep_n != DEP_OVERFLOW) {
        memcpy(s_block_dep[slot][block_no], s_cur_dep, s_cur_dep_n * sizeof(uint16_t));
        for (unsigned i = 0; i < s_cur_dep_n; i++)
            s_block_dep_sig[slot][block_no][i] = dep_sig(s_cur_dep[i]);
    }
    s_block_valid[slot][block_no] = 1;
    if (slot == s_display_fb) s_dirty = 1;
    vbds_prof_blocks_drawn++;
    if (vbds_tick_source) vbds_prof_draw_ticks += vbds_tick_source() - t0;
}
