/* vbds_vsu.c - Virtual Boy sound chip (VSU).
 *
 * Six channels: five play 32-step, 6-bit waveforms (the fifth adds
 * frequency sweep / modulation), the sixth is a noise generator.  The chip
 * runs at 5 MHz (CPU clock / 4).  Every channel has
 *   - a frequency counter: a wave channel steps its waveform every
 *     2048 - F clocks; the noise channel steps its 15-bit shift register
 *     every 10 * (2048 - F) clocks and its output is sampled every 120;
 *   - an effects clock every 4800 clocks (~1.04 kHz) which drives, in
 *     turn, the auto-stop interval (every 4th tick), the envelope (every
 *     16th tick) and, on the fifth channel, sweep / modulation (every tick
 *     or every 8th);
 * Behaviour follows the hardware as documented by the Virtual Boy
 * community (Sacred Tech Scroll) and as emulated by Mednafen; output is
 * checked against the original recompilation runtime's sound chip.
 *
 * The chip is advanced lazily: before each register write and at the end
 * of each frame the bus passes the CPU cycles elapsed.  Output samples are
 * point-sampled at half the DS mixer rate (16364 Hz), mixed, high-pass
 * filtered and collected per frame for the front end.
 */
#include "vbds_vsu.h"
#include "vbds.h"

#include <string.h>

typedef struct {
    uint8_t  ctl;           /* bit 7 playing, bit 5 interval enable, 4..0 interval */
    uint8_t  lvl_l, lvl_r;  /* 0..15 */
    uint8_t  ram;           /* waveform number (5+ = silent) */
    uint16_t freq;          /* 11-bit frequency register */
    int32_t  eff;           /* frequency in use (sweep / modulation change it) */
    uint16_t env_ctl;       /* low byte: start level, direction, step; high: flags */
    int32_t  env;           /* envelope level 0..15 */
    int32_t  pos;           /* waveform position 0..31 */
    int32_t  fcnt;          /* clocks until the next waveform / noise step */
    int32_t  icnt;          /* interval ticks left */
    int32_t  ecnt;          /* envelope ticks left */
    int32_t  fx_div;        /* clocks until the next effects tick */
    int32_t  iv_div;        /* effects ticks until the next interval tick */
    int32_t  en_div;        /* interval ticks until the next envelope tick */
} Chan;

static Chan     s_ch[6];
static uint8_t  s_wave[5][32];
static int8_t   s_mod[32];
static uint8_t  s_swp;              /* sweep / modulation control (channel 5) */
static int32_t  s_swp_cnt, s_swp_div, s_mod_pos;
static uint32_t s_lfsr;
static int32_t  s_noise_div;        /* clocks until the noise output is sampled */
static int32_t  s_noise_out;        /* 0 or 63 */
static const uint8_t s_taps[8] = { 14, 10, 13, 4, 8, 6, 9, 11 };

#define SAMPLE_WHOLE VBDS_VSU_STEP_WHOLE
#define SAMPLE_REM   VBDS_VSU_STEP_REM
#define SAMPLE_DEN   VBDS_VSU_STEP_DEN
static int32_t  s_cpu_residue;      /* CPU cycles not yet worth a VSU clock */
static int32_t  s_to_sample;        /* VSU clocks until the next sample */
static uint32_t s_sample_frac;
static int32_t  s_dc[2];            /* high-pass state (Q8) */
static int16_t  s_out[2][VBDS_VSU_MAX_SAMPLES];
static unsigned s_out_n;
void (*vbds_vsu_raw_hook)(int l, int r);
void (*vbds_vsu_trace)(int kind, uint32_t a, uint32_t b);

void vb_vsu_init(void) {
    memset(s_ch, 0, sizeof(s_ch));
    for (int c = 0; c < 6; c++) { s_ch[c].fx_div = 4800; s_ch[c].iv_div = 4; s_ch[c].en_div = 4; }
    memset(s_wave, 0, sizeof(s_wave));
    memset(s_mod, 0, sizeof(s_mod));
    s_swp = 0; s_swp_cnt = 0; s_swp_div = 1; s_mod_pos = 0;
    s_lfsr = 0; s_noise_div = 120; s_noise_out = 0;
    s_cpu_residue = 0;
    s_to_sample = SAMPLE_WHOLE; s_sample_frac = SAMPLE_REM;
    s_dc[0] = s_dc[1] = 0;
    s_out_n = 0;
}

uint8_t  vb_vsu_read8 (uint32_t addr) { (void)addr; return 0; }   /* write-only */
uint16_t vb_vsu_read16(uint32_t addr) { (void)addr; return 0; }
uint32_t vb_vsu_read32(uint32_t addr) { (void)addr; return 0; }

VBDS_FAST void vb_vsu_write8(uint32_t addr, uint8_t v) {
    if (vbds_vsu_trace) vbds_vsu_trace(1, addr, v);
    uint32_t a = addr & 0x7FFu;
    if (a & 3u) return;
    if (a < 0x280u) { s_wave[a >> 7][(a >> 2) & 31u] = v & 0x3Fu; return; }
    if (a < 0x400u) { s_mod[(a >> 2) & 31u] = (int8_t)v; return; }
    if (a >= 0x600u) return;
    int c = (int)((a >> 6) & 0xFu);
    if (c > 5) {
        if (a == 0x580u && (v & 1u))            /* stop all */
            for (int i = 0; i < 6; i++) s_ch[i].ctl &= (uint8_t)~0x80u;
        return;
    }
    Chan* ch = &s_ch[c];
    switch ((a >> 2) & 0xFu) {
        case 0:                                  /* play control */
            ch->ctl = v & (uint8_t)~0x40u;
            if (v & 0x80u) {
                ch->eff = ch->freq;
                ch->fcnt = (c == 5 ? 10 : 1) * (2048 - ch->eff);
                ch->icnt = (v & 0x1Fu) + 1;
                ch->ecnt = (ch->env_ctl & 7u) + 1;
                if (c == 4) {
                    s_swp_cnt = (s_swp >> 4) & 7u;
                    s_swp_div = (s_swp & 0x80u) ? 8 : 1;
                    s_mod_pos = 0;
                }
                ch->pos = 0;
                if (c == 5) s_lfsr = 1;
                ch->fx_div = 4800; ch->iv_div = 4; ch->en_div = 4;
            }
            break;
        case 1: ch->lvl_l = (v >> 4) & 0xFu; ch->lvl_r = v & 0xFu; break;
        case 2:
            ch->freq = (uint16_t)((ch->freq & 0x700u) | v);
            ch->eff  = (ch->eff & 0x700) | v;
            break;
        case 3:
            ch->freq = (uint16_t)((ch->freq & 0xFFu) | ((v & 7u) << 8));
            ch->eff  = (ch->eff & 0xFF) | ((v & 7) << 8);
            break;
        case 4:
            ch->env_ctl = (uint16_t)((ch->env_ctl & 0xFF00u) | v);
            ch->env = (v >> 4) & 0xFu;
            break;
        case 5:
            ch->env_ctl = (uint16_t)((ch->env_ctl & 0xFFu)
                        | ((v & (c >= 4 ? 0x73u : 0x03u)) << 8));
            if (c == 5) s_lfsr = 1;
            break;
        case 6: ch->ram = v & 0xFu; break;
        case 7: if (c == 4) s_swp = v; break;
        default: break;
    }
}

/* One effects tick (every 4800 clocks): interval, envelope, sweep / mod. */
VBDS_FAST static void effects_tick(int c, Chan* ch) {
    if (--ch->iv_div <= 0) {
        ch->iv_div += 4;
        if ((ch->ctl & 0x20u) && --ch->icnt == 0) ch->ctl &= (uint8_t)~0x80u;
        if (--ch->en_div <= 0) {
            ch->en_div += 4;
            if ((ch->env_ctl & 0x100u) && --ch->ecnt == 0) {
                ch->ecnt = (ch->env_ctl & 7u) + 1;
                int wrap = (ch->env_ctl & 0x200u) != 0;
                if (ch->env_ctl & 8u) { if (ch->env < 15 || wrap) ch->env = (ch->env + 1) & 15; }
                else                  { if (ch->env > 0  || wrap) ch->env = (ch->env - 1) & 15; }
            }
        }
    }
    if (c != 4) return;
    if (--s_swp_div > 0) return;
    s_swp_div += (s_swp & 0x80u) ? 8 : 1;
    uint32_t interval = (s_swp >> 4) & 7u;
    if (!interval || !(ch->env_ctl & 0x4000u)) return;
    if (s_swp_cnt) s_swp_cnt--;
    if (s_swp_cnt) return;
    s_swp_cnt = (int32_t)interval;
    if (ch->env_ctl & 0x1000u) {                 /* modulation */
        if (s_mod_pos < 32 || (ch->env_ctl & 0x2000u)) {
            s_mod_pos &= 31;
            ch->eff = (ch->freq + s_mod[s_mod_pos]) & 0x7FF;
            s_mod_pos++;
        }
    } else {                                     /* sweep */
        int32_t d = ch->eff >> (s_swp & 7u);
        int32_t nf = ch->eff + ((s_swp & 8u) ? d : -d);
        if (nf < 0) ch->eff = 0;
        else if (nf > 0x7FF) ch->ctl &= (uint8_t)~0x80u;
        else ch->eff = nf;
    }
}

/* Advance a wave channel by `clocks`. */
static inline __attribute__((always_inline)) int run_wave(int c, Chan* ch, int32_t clocks) {
    int fx = 0;
    while (clocks > 0) {
        int32_t n = clocks < ch->fx_div ? clocks : ch->fx_div;
        ch->fcnt -= n;
        if (ch->fcnt <= 0) {
            int32_t period = 2048 - ch->eff;
            int32_t steps = 1;
            ch->fcnt += period;
            if (ch->fcnt <= 0) {
                int32_t more = (-ch->fcnt) / period + 1;
                ch->fcnt += more * period;
                steps += more;
            }
            ch->pos = (ch->pos + steps) & 31;
        }
        ch->fx_div -= n;
        clocks -= n;
        if (ch->fx_div <= 0) { ch->fx_div += 4800; effects_tick(c, ch); fx = 1; }
    }
    return fx;
}

/* Advance the noise channel by `clocks`; its output is resampled every
 * 120 clocks. */
static inline __attribute__((always_inline)) int run_noise(Chan* ch, int32_t clocks) {
    int fx = 0;
    while (clocks > 0) {
        int32_t n = clocks;
        if (n > ch->fx_div) n = ch->fx_div;
        if (n > s_noise_div) n = s_noise_div;
        ch->fcnt -= n;
        while (ch->fcnt <= 0) {
            uint32_t tap = s_taps[(ch->env_ctl >> 12) & 7u];
            uint32_t fb = ((s_lfsr >> 7) ^ (s_lfsr >> tap) ^ 1u) & 1u;
            s_lfsr = ((s_lfsr << 1) & 0x7FFFu) | fb;
            ch->fcnt += 10 * (2048 - ch->eff);
        }
        s_noise_div -= n;
        if (s_noise_div <= 0) { s_noise_div = 120; s_noise_out = (s_lfsr & 1u) ? 63 : 0; }
        ch->fx_div -= n;
        clocks -= n;
        if (ch->fx_div <= 0) { ch->fx_div += 4800; effects_tick(5, ch); fx = 1; }
    }
    return fx;
}

static inline int gain(int env, int lvl) {
    int g = env * lvl;
    return g ? (g >> 3) + 1 : 0;
}

/* Filter collected samples into the output.  Waveforms are unsigned, so
 * the DC level is removed (~20 Hz high-pass); then the mix is scaled by
 * 16/3 (the game mixes well below six full channels; extreme peaks clip). */
static inline int16_t vsu_filter(int32_t v, int32_t* dc) {
    int32_t x = v << 8;
    *dc += (x - *dc) >> 8;
    int32_t y = (((x - *dc) >> 8) * 1365) >> 8;
    if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
    return (int16_t)y;
}

static void emit_samples(const int32_t* acc_l, const int32_t* acc_r, unsigned n) {
    if (vbds_vsu_raw_hook)
        for (unsigned i = 0; i < n; i++) vbds_vsu_raw_hook(acc_l[i], acc_r[i]);
    if (s_out_n + n > VBDS_VSU_MAX_SAMPLES) s_out_n = 0;   /* nobody collecting */
    int32_t dl = s_dc[0], dr = s_dc[1];
    int16_t* ol = &s_out[0][s_out_n];
    int16_t* or_ = &s_out[1][s_out_n];
    for (unsigned i = 0; i < n; i++) {
        ol[i] = vsu_filter(acc_l[i], &dl);
        or_[i] = vsu_filter(acc_r[i], &dr);
    }
    s_dc[0] = dl; s_dc[1] = dr;
    s_out_n += n;
}

/* Advance the chip by `clocks` VSU clocks.  The span is cut at output
 * sample instants; then each channel runs through the whole span on its
 * own, adding its output at every sample instant (channels do not
 * interact between register writes). */
#define SEG_MAX 256
static const uint8_t s_silent_wave[32];
VBDS_FAST static void vsu_run(int32_t clocks, uint32_t alive) {
    static int32_t acc_l[SEG_MAX], acc_r[SEG_MAX];
    static int16_t seg[SEG_MAX + 1];
    while (clocks > 0) {
        /* Segments: seg[0..nseg-1]; every one but possibly the last ends on
         * a sample instant. */
        unsigned nseg = 0, nsamp = 0;
        int last_is_sample = 0;
        while (clocks > 0 && nsamp < SEG_MAX) {
            int32_t n = clocks < s_to_sample ? clocks : s_to_sample;
            seg[nseg++] = (int16_t)n;
            clocks -= n;
            s_to_sample -= n;
            last_is_sample = s_to_sample == 0;
            if (last_is_sample) {
                nsamp++;
                s_to_sample = SAMPLE_WHOLE;
                s_sample_frac += SAMPLE_REM;
                if (s_sample_frac >= SAMPLE_DEN) { s_sample_frac -= SAMPLE_DEN; s_to_sample++; }
            }
        }
        memset(acc_l, 0, nsamp * sizeof(int32_t));
        memset(acc_r, 0, nsamp * sizeof(int32_t));
        for (uint32_t m = alive; m; m &= m - 1) {
            int c = __builtin_ctz(m);
            Chan* ch = &s_ch[c];
            const unsigned nout = last_is_sample ? nseg : nseg - 1;
            /* Gains change only on effects ticks. */
            int on = ch->ctl & 0x80u;
            int gl = on ? gain(ch->env, ch->lvl_l) : 0, gr = on ? gain(ch->env, ch->lvl_r) : 0;
            if (c == 5) {
                for (unsigned k = 0; k < nseg; k++) {
                    if (run_noise(ch, seg[k])) {
                        on = ch->ctl & 0x80u;
                        gl = on ? gain(ch->env, ch->lvl_l) : 0; gr = on ? gain(ch->env, ch->lvl_r) : 0;
                    }
                    if (k < nout) { acc_l[k] += s_noise_out * gl; acc_r[k] += s_noise_out * gr; }
                }
            } else {
                const uint8_t* wv = ch->ram < 5 ? s_wave[ch->ram] : s_silent_wave;
                /* Common case in registers: no effects tick inside the
                 * segment.  Otherwise the general step handles it. */
                int32_t fcnt = ch->fcnt, fxd = ch->fx_div, pos = ch->pos;
                int32_t period = 2048 - ch->eff;
                for (unsigned k = 0; k < nseg; k++) {
                    int32_t n = seg[k];
                    if (n < fxd) {
                        fxd -= n;
                        fcnt -= n;
                        if (fcnt <= 0) {
                            fcnt += period; pos++;
                            if (fcnt <= 0) {
                                int32_t more = (-fcnt) / period + 1;
                                fcnt += more * period; pos += more;
                            }
                            pos &= 31;
                        }
                    } else {
                        ch->fcnt = fcnt; ch->fx_div = fxd; ch->pos = pos;
                        run_wave(c, ch, n);
                        fcnt = ch->fcnt; fxd = ch->fx_div; pos = ch->pos;
                        period = 2048 - ch->eff;
                        on = ch->ctl & 0x80u;
                        gl = on ? gain(ch->env, ch->lvl_l) : 0; gr = on ? gain(ch->env, ch->lvl_r) : 0;
                    }
                    if (k < nout) { int w = wv[pos]; acc_l[k] += w * gl; acc_r[k] += w * gr; }
                }
                ch->fcnt = fcnt; ch->fx_div = fxd; ch->pos = pos;
            }
        }
        emit_samples(acc_l, acc_r, nsamp);
    }
}

VBDS_FAST void vb_vsu_tick(uint64_t cpu_cycles) {
    if (vbds_vsu_trace) vbds_vsu_trace(0, (uint32_t)cpu_cycles, 0);
    s_cpu_residue += (int32_t)cpu_cycles;
    int32_t clocks = s_cpu_residue >> 2;
    s_cpu_residue &= 3;
    /* A channel playing when the update starts runs to its end even if it
     * stops part way (its hidden counters keep going), like the reference.
     * Idle stretches still produce (silent) samples. */
    uint32_t alive = 0;
    for (int c = 0; c < 6; c++) if (s_ch[c].ctl & 0x80u) alive |= 1u << c;
    vsu_run(clocks, alive);
}

void vb_vsu_end_frame(void) {}

unsigned vbds_vsu_take(const int16_t** left, const int16_t** right) {
    unsigned n = s_out_n;
    *left = s_out[0]; *right = s_out[1];
    s_out_n = 0;
    return n;
}
