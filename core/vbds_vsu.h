/* vbds_vsu.h - Virtual Boy sound chip. */
#pragma once
#include <stdint.h>
#include "vbds.h"

/* Output rate: 65457 / 4 = 16364.25 Hz, half the DS mixer rate (a DS
 * sound timer of 1024).  A sample every 20000000 / 65457 VSU clocks
 * = 305 + 35615 / 65457. */
#define VBDS_VSU_HZ_X4       65457u
#define VBDS_VSU_STEP_WHOLE  305
#define VBDS_VSU_STEP_REM    35615u
#define VBDS_VSU_STEP_DEN    65457u
#define VBDS_VSU_MAX_SAMPLES 1024    /* about three frames */

void vb_vsu_init(void);
uint8_t  vb_vsu_read8 (uint32_t addr);
uint16_t vb_vsu_read16(uint32_t addr);
uint32_t vb_vsu_read32(uint32_t addr);
VBDS_FAST_DECL void vb_vsu_write8(uint32_t addr, uint8_t v);
VBDS_FAST_DECL void vb_vsu_tick(uint64_t cpu_cycles);
void vb_vsu_end_frame(void);

/* Samples produced since the last call (signed 16-bit, left and right).
 * The pointers stay valid until the next emulated frame. */
unsigned vbds_vsu_take(const int16_t** left, const int16_t** right);

/* Testing: called with each sample's raw mix (before filtering). */
extern void (*vbds_vsu_raw_hook)(int l, int r);
/* Testing: every tick (kind 0, CPU cycles) and register write (kind 1). */
extern void (*vbds_vsu_trace)(int kind, uint32_t a, uint32_t b);
