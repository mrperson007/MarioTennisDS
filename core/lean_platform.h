/* lean_platform.h - binds lean generated code to the DS port's core.
 * Selected with -DVB_LEAN_PLATFORM_HEADER='"lean_platform.h"'. */
#ifndef VBDS_LEAN_PLATFORM_H
#define VBDS_LEAN_PLATFORM_H

#include "vbds.h"

/* The hottest game functions (tools/generate.py HOT_FUNCTIONS): ARM code
 * in the instruction TCM on the DS, where the rest is Thumb in main RAM. */
#if defined(ARM9) && !defined(VBDS_NO_ITCM)
#define VBDS_HOT_FN __attribute__((section(".itcm"), target("arm")))
#else
#define VBDS_HOT_FN
#endif

/* Guest memory access from recompiled code.  Work RAM (region 5) and ROM
 * (region 7) have no side effects, so they are read and written inline;
 * everything else goes through the bus functions. */
typedef uint16_t vbds_u16a __attribute__((may_alias));
typedef uint32_t vbds_u32a __attribute__((may_alias));

static inline uint8_t vbds_r8(uint32_t a) {
    uint32_t r = (a >> 24) & 7u;
    if (r == 5u) return vbds_wram_ptr[a & 0xFFFFu];
    if (r == 7u) return vbds_rom_ptr[a & vbds_rom_mask];
    return vb_read8(a);
}
static inline uint16_t vbds_r16(uint32_t a) {
    uint32_t r = (a >> 24) & 7u;
    if (r == 5u) return *(const vbds_u16a*)(vbds_wram_ptr + (a & 0xFFFEu));
    if (r == 7u) return *(const vbds_u16a*)(vbds_rom_ptr + (a & vbds_rom_mask & ~1u));
    return vb_read16(a);
}
static inline uint32_t vbds_r32(uint32_t a) {
    uint32_t r = (a >> 24) & 7u;
    if (r == 5u) return *(const vbds_u32a*)(vbds_wram_ptr + (a & 0xFFFCu));
    if (r == 7u) return *(const vbds_u32a*)(vbds_rom_ptr + (a & vbds_rom_mask & ~3u));
    return vb_read32(a);
}
static inline void vbds_w8(uint32_t a, uint8_t v) {
    if (((a >> 24) & 7u) == 5u) vbds_wram_ptr[a & 0xFFFFu] = v;
    else vb_write8(a, v);
}
static inline void vbds_w16(uint32_t a, uint16_t v) {
    if (((a >> 24) & 7u) == 5u) *(vbds_u16a*)(vbds_wram_ptr + (a & 0xFFFEu)) = v;
    else vb_write16(a, v);
}
static inline void vbds_w32(uint32_t a, uint32_t v) {
    if (((a >> 24) & 7u) == 5u) *(vbds_u32a*)(vbds_wram_ptr + (a & 0xFFFCu)) = v;
    else vb_write32(a, v);
}

#define VB_LEAN_READ8(cpu, a)      vbds_r8(a)
#define VB_LEAN_READ16(cpu, a)     vbds_r16(a)
#define VB_LEAN_READ32(cpu, a)     vbds_r32(a)
#define VB_LEAN_WRITE8(cpu, a, v)  vbds_w8((a), (v))
#define VB_LEAN_WRITE16(cpu, a, v) vbds_w16((a), (v))
#define VB_LEAN_WRITE32(cpu, a, v) vbds_w32((a), (v))


/* Wait loops: jump straight to the scheduler's deadline.  The stock macro
 * lands on the exact cycle count spinning would reach, at the price of a
 * 64-bit division per skip, which is slow on an ARM9.  The difference is
 * at most one loop pass (under 12 cycles). */
#ifndef VBDS_FAST_IDLE
#ifdef ARM9
#define VBDS_FAST_IDLE 1
#else
#define VBDS_FAST_IDLE 0
#endif
#endif
#if VBDS_FAST_IDLE
#define VB_LEAN_IDLE_SPIN(cpu, iter, addr) \
    do { if (((((uint32_t)(addr)) >> 24) & 7u) == 5u && \
             (cpu)->cycles < (cpu)->cycle_deadline) \
        (cpu)->cycles = (cpu)->cycle_deadline; } while (0)
#endif

#endif
