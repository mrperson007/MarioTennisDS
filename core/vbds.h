/* vbds.h - portable core of the Mario's Tennis DS port.
 *
 * Everything here builds for any 32-bit little-endian target with a C11
 * compiler.  The front end (source/main_nds.c on DS, host/main_host.c on a
 * PC) supplies the ROM image, calls vbds_run_frame() once per Virtual Boy
 * frame and shows the framebuffer.
 */
#ifndef VBDS_H
#define VBDS_H

#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1: service the VIP only where guest-visible state can change (about 32
 * stops per frame).  0: every display column, exactly like the reference
 * runtime (1,536 stops per frame; used to check the core against it). */
#ifndef VBDS_COARSE_EVENTS
#define VBDS_COARSE_EVENTS 1
#endif

#define VBDS_CYCLES_PER_FRAME (259u * 384u * 4u)   /* 397,824 = 50.27 Hz */
#define VBDS_WRAM_SIZE 0x10000u

/* Start the machine on a ROM image that stays valid for the whole run.
 * `rom_size` must be a power of two.  Returns 0, or -1 if the image is not
 * the one the code was recompiled from. */
int vbds_init(const uint8_t* rom, uint32_t rom_size);

/* Run one Virtual Boy frame of guest time. */
void vbds_run_frame(void);

extern CPUState vbds_cpu;
const uint8_t* vbds_wram(void);

/* Guest bus. */
uint8_t  vb_read8 (uint32_t addr);
uint16_t vb_read16(uint32_t addr);
uint32_t vb_read32(uint32_t addr);
void     vb_write8 (uint32_t addr, uint8_t  v);
void     vb_write16(uint32_t addr, uint16_t v);
void     vb_write32(uint32_t addr, uint32_t v);

/* Guest cycle at which the current bus access happens. */
uint64_t vbds_access_cycle(void);

/* Called by the VIP at each game-frame start. */
void vbds_on_game_frame(void);

/* Optional profiling: if the front end sets a tick source, the time spent
 * drawing video blocks accumulates in vbds_prof_draw_ticks. */
extern uint32_t (*vbds_tick_source)(void);
extern uint32_t vbds_prof_draw_ticks;
extern uint32_t vbds_prof_blocks_drawn, vbds_prof_blocks_skipped;
extern uint32_t vbds_prof_passes;      /* scheduler stops */

/* Supplied by the front end: report an unrecoverable error and stop. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void vbds_fatal(const char* what, uint32_t pc, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
