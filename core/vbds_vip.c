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

static inline void sig_update(uint32_t key, uint32_t old, uint32_t now) {
    uint32_t d = now - old;
    s_sig[0] += d * ((key * 0x9E3779B1u) | 1u);
    s_sig[1] += d * (((key ^ 0x5BD1E995u) * 0x85EBCA6Bu + 0xC2B2AE35u) | 1u);
}

static void fb_settle(int slot) {
    if (!s_fb_touched[slot]) return;
    s_fb_touched[slot] = 0;
    memset(s_block_valid[slot], 0, sizeof(s_block_valid[slot]));
}

static int slots_show_same_picture(void) {
    fb_settle(0); fb_settle(1);
    for (int i = 0; i < 28; i++) {
        if (!s_block_valid[0][i] || !s_block_valid[1][i]) return 0;
        if (s_block_sig[0][i][0] != s_block_sig[1][i][0]
            || s_block_sig[0][i][1] != s_block_sig[1][i][1]) return 0;
    }
    return 1;
}

#define SIG_KEY_REG 0x100000u       /* register keys sit above memory offsets */

void vbds_vip_set_right_eye(int enabled) {
    sig_update(SIG_KEY_REG + 0x100, (uint32_t)s_right_eye, (uint32_t)(enabled != 0));
    s_right_eye = enabled != 0;
}
int vbds_vip_take_dirty(void) { int d = s_dirty; s_dirty = 0; return d; }

static void recalc_gplt_cache(int which) {
    for (int i = 0; i < 4; ++i)
        s_gplt_cache[which][i] = (uint8_t)((s_gplt[which] >> (i * 2)) & 3u);
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

static void vip_draw_block_now(uint8_t block_no);

void vb_vip_init(void) {
    memset(s_low_store, 0, sizeof(s_low_store));
    memset(s_chr, 0, sizeof(s_chr));
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
    vip_check_irq();
}

/* ----------------- Column state machine ----------------- */

static void vip_advance_column(void) {
    /* Brightness "repeat" sample from the column table, every 4 columns
     * of an active display half (Beetle vip.c:1357-1366). */
    if ((s_display_region & 1) && !(s_column & 3)) {
        int lr = (s_display_region & 2) >> 1;
        uint32_t flat = 0x20000u + 0x1DFFEu
                      - (uint32_t)((s_column >> 2) * 2) - (lr ? 0u : 0x200u);
        uint8_t repeat = s_low[flat + 1];
        if (repeat != s_brt_repeat) {
            s_brt_repeat = repeat;
            recalc_brt_cache();
        }
    }

    s_column++;
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

    s_column_counter -= chunk_clocks;
    if (s_column_counter == 0) {
        s_column_counter = VIP_CYCLES_PER_COLUMN;
        vip_advance_column();
    }
}

void vb_vip_tick(uint64_t cycles) {
    s_update_span = cycles;
    int64_t clocks = (int64_t)cycles;
    while (clocks > 0) {
        int32_t chunk = clocks > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)clocks;
        if (s_drawing_counter > 0 && chunk > s_drawing_counter)
            chunk = s_drawing_counter;
        if (chunk > s_column_counter)
            chunk = s_column_counter;
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

static void vip_store(uint32_t o, uint32_t v, unsigned width) {
    int kind;
    uint8_t* p = vip_ptr(o, &kind);
    if (!kind) return;
    uint32_t old = 0;
    for (unsigned i = 0; i < width; i++) old |= (uint32_t)p[i] << (i * 8);
    if (old == v) return;
    if (kind == 2) {
        /* Key on the storage location so every alias of character RAM
         * updates the same terms. */
        uint32_t key = (uint32_t)(p - (p >= (uint8_t*)s_chr && p < (uint8_t*)s_chr + VIP_CHR_SIZE
                                       ? (uint8_t*)s_chr - 0x80000u : s_low));
        for (unsigned i = 0; i < width; i++) {
            uint8_t nb = (uint8_t)(v >> (i * 8));
            if (p[i] != nb) { sig_update(key + i, p[i], nb); p[i] = nb; }
        }
    } else {
        for (unsigned i = 0; i < width; i++) p[i] = (uint8_t)(v >> (i * 8));
        /* CPU drew into a framebuffer: slot 0 at 0x0000/0x10000, slot 1 at
         * 0x8000/0x18000. */
        int slot = (o >> 15) & 1;
        s_fb_touched[slot] = 1;
        if (slot == s_display_fb) s_dirty = 1;
    }
}

uint8_t vb_vip_read8(uint32_t addr) {
    uint32_t o = addr & (VIP_WINDOW_SIZE - 1u);
    if (vip_is_register(o)) {
        uint16_t r = vip_read_register16(o & ~1u);
        return (o & 1u) ? (uint8_t)(r >> 8) : (uint8_t)r;
    }
    int kind;
    return *vip_ptr(o, &kind);
}

uint16_t vb_vip_read16(uint32_t addr) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~1u;
    if (vip_is_register(o)) return vip_read_register16(o);
    int kind;
    const uint8_t* p = vip_ptr(o, &kind);
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t vb_vip_read32(uint32_t addr) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~3u;
    if (vip_is_register(o))
        return (uint32_t)vip_read_register16(o)
             | ((uint32_t)vip_read_register16(o + 2) << 16);
    int kind;
    const uint8_t* p = vip_ptr(o, &kind);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void vb_vip_write8(uint32_t addr, uint8_t v) {
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

void vb_vip_write16(uint32_t addr, uint16_t v) {
    uint32_t o = (addr & (VIP_WINDOW_SIZE - 1u)) & ~1u;
    if (vip_is_register(o)) { vip_write_register16(o, v); return; }
    vip_store(o, v, 2);
}

void vb_vip_write32(uint32_t addr, uint32_t v) {
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
static uint32_t s_row_store[2][8 * ROW_STRIDE / 4];    /* word-aligned */
#define s_row_buf ((uint8_t (*)[8 * ROW_STRIDE])s_row_store)

#define dram_u16() ((const uint16_t*)&s_low[0x20000u])

static inline int16_t sign_10(uint32_t v) { return (int16_t)(((v & 0x3FFu) ^ 0x200u) - 0x200u); }
static inline int16_t sign_11(uint32_t v) { return (int16_t)(((v & 0x7FFu) ^ 0x400u) - 0x400u); }
static inline int16_t sign_9(uint32_t v)  { return (int16_t)(((v & 0x1FFu) ^ 0x100u) - 0x100u); }

static void draw_bg(uint8_t* target, uint16_t real_y,
                    uint8_t bgmap_base_raw, int overplane, uint16_t overplane_char,
                    int32_t source_x, int32_t source_y,
                    uint32_t scx, uint32_t scy,
                    uint16_t dest_x, uint16_t dest_y,
                    uint16_t dest_width, uint16_t dest_height) {
    const uint16_t* chr = s_chr;
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

    for (int x = start_x; x <= final_x; x++) {
        uint32_t source_x_u = (uint32_t)source_x & source_x_mask;
        uint32_t bgsc = bgsc_overplane;

        if (source_x_u < source_x_size && (uint32_t)source_y < source_y_size) {
            uint32_t idx = (bgmap_base | ((source_x_u << 3) & ~0xFFFu)
                          | ((source_x_u >> 3) & 0x3Fu)) & 0xFFFFu;
            bgsc = bgm[idx];
        }

        uint32_t char_no    = bgsc & 0x7FFu;
        const uint8_t* pal  = s_gplt_cache[bgsc >> 14];
        uint32_t hflip_xor  = (bgsc & 0x2000u) ? 7u : 0u;
        uint32_t vflip_xor  = (bgsc & 0x1000u) ? 7u : 0u;
        uint32_t char_sub_y = vflip_xor ^ ((uint32_t)source_y & 0x7u);

        if (!(source_x_u & 7u) && (x + 7) <= final_x) {
            uint32_t pixels = chr[char_no * 8u + char_sub_y];
            if (pixels) {
                if (bgsc & 0x2000u) {
                    for (int sub = 0; sub < 8; ++sub) {
                        uint32_t v = (pixels >> (14 - sub * 2)) & 3u;
                        if (v) target[x + sub] = pal[v];
                    }
                } else {
                    for (int sub = 0; sub < 8; ++sub) {
                        uint32_t v = (pixels >> (sub * 2)) & 3u;
                        if (v) target[x + sub] = pal[v];
                    }
                }
            }
            x += 7;
            source_x += 8;
        } else {
            uint32_t char_sub_x = hflip_xor ^ (source_x_u & 0x7u);
            uint32_t pixel = (chr[char_no * 8u + char_sub_y] >> (char_sub_x * 2u)) & 0x3u;
            if (pixel) target[x] = pal[pixel];
            source_x++;
        }
    }
}

static void draw_affine(uint8_t* target, uint16_t real_y, int lr,
                        uint32_t param_base, uint32_t bgmap_base,
                        int overplane_mode, uint16_t overplane_char,
                        uint32_t scx, uint32_t scy,
                        uint16_t dest_x, uint16_t dest_y,
                        uint16_t dest_width, uint16_t dest_height) {
    const uint16_t* chr = s_chr;
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
        for (int x = start_x; x <= final_x; x++) {
            uint32_t bgsc = bgsc_overplane;
            source_x &= source_x_mask;
            if (source_x < (source_x_size << 9)) {
                uint32_t idx = (bgmap_base | ((source_x >> 6) & ~0xFFFu)
                              | ((source_x >> 12) & 0x3Fu)) & 0xFFFFu;
                bgsc = bgm[idx];
            }
            uint32_t hflip_xor = (uint32_t)(((int32_t)(bgsc << 18) >> 30) & 0xE);
            uint32_t vflip_xor = (uint32_t)(((int32_t)(bgsc << 19) >> 31) & 0x7);
            uint32_t char_sub_y = vflip_xor ^ ((source_y >> 9) & 0x7u);
            uint32_t char_sub_x = hflip_xor ^ ((source_x >> 8) & 0xEu);
            uint32_t pixel = (chr[((bgsc & 0x7FFu) * 8u) | char_sub_y] >> char_sub_x) & 0x3u;
            if (pixel) target[x] = s_gplt_cache[bgsc >> 14][pixel];
            source_x = (uint32_t)((int32_t)source_x + dx);
        }
    } else {
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
            uint32_t pixel = (chr[(bgsc & 0x7FFu) * 8u + char_sub_y] >> (char_sub_x * 2u)) & 0x3u;
            if (pixel) target[x] = s_gplt_cache[bgsc >> 14][pixel];
            source_x = (uint32_t)((int32_t)source_x + dx);
            source_y = (uint32_t)((int32_t)source_y + dy);
        }
    }
}

static void draw_obj(uint8_t* fb_lr[2], uint16_t y, const int lron[2]) {
    const uint16_t* chr = s_chr;
    const uint16_t* bgm = dram_u16();
    int32_t start_oam = s_spt[s_obj_search_which];
    int32_t end_oam   = s_obj_search_which ? s_spt[s_obj_search_which - 1] : 1023;
    int32_t oam = start_oam;

    do {
        const uint16_t* oam_ptr = &bgm[(0x1E000u + (uint32_t)oam * 8u) >> 1];
        uint32_t tile_y = (y - oam_ptr[2]) & 0xFFu;
        if (tile_y >= 8u) continue;

        uint32_t attr = oam_ptr[3];
        uint32_t char_sub_y = ((attr & 0x1000u) ? 7u : 0u) ^ tile_y;
        uint32_t pixels_save = chr[(attr & 0x7FFu) * 8u + char_sub_y];
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

static void vip_draw_block_into(uint8_t block_no, uint8_t* fb_l, uint8_t* fb_r) {
    const uint16_t* bgm = dram_u16();
    const int eyes = s_right_eye ? 2 : 1;
    const uint8_t bkcol = (uint8_t)(s_bkcol & 0x3u);

    for (int lr = 0; lr < eyes; lr++)
        memset(s_row_buf[lr], bkcol, 8 * ROW_STRIDE);

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
                        if (bgm_mode == 1u)
                            src_x += (uint16_t)(int16_t)bgm[
                                (param_base + (((real_y - gy) * 2u) | (uint32_t)lr))
                                & 0xFFFFu];
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
    for (int lr = 0; lr < eyes; lr++) {
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
    int slot = s_drawing_fb & 1;
    fb_settle(slot);
    if (s_block_valid[slot][block_no]
        && s_block_sig[slot][block_no][0] == s_sig[0]
        && s_block_sig[slot][block_no][1] == s_sig[1]) {
        vbds_prof_blocks_skipped++;
        return;                     /* already holds exactly this picture */
    }
    uint32_t t0 = vbds_tick_source ? vbds_tick_source() : 0;
    uint8_t* fb_l = &s_low[slot ? 0x8000u  : 0x0000u];
    uint8_t* fb_r = &s_low[slot ? 0x18000u : 0x10000u];
    vip_draw_block_into(block_no, fb_l, fb_r);
    s_block_sig[slot][block_no][0] = s_sig[0];
    s_block_sig[slot][block_no][1] = s_sig[1];
    s_block_valid[slot][block_no] = 1;
    if (slot == s_display_fb) s_dirty = 1;
    vbds_prof_blocks_drawn++;
    if (vbds_tick_source) vbds_prof_draw_ticks += vbds_tick_source() - t0;
}
