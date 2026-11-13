/* lean_platform.h - binds lean generated code to the DS port's core.
 * Selected with -DVB_LEAN_PLATFORM_HEADER='"lean_platform.h"'. */
#ifndef VBDS_LEAN_PLATFORM_H
#define VBDS_LEAN_PLATFORM_H

#include "vbds.h"

/* Call the bus directly instead of through CPUState function pointers. */
#define VB_LEAN_READ8(cpu, a)      vb_read8(a)
#define VB_LEAN_READ16(cpu, a)     vb_read16(a)
#define VB_LEAN_READ32(cpu, a)     vb_read32(a)
#define VB_LEAN_WRITE8(cpu, a, v)  vb_write8((a), (v))
#define VB_LEAN_WRITE16(cpu, a, v) vb_write16((a), (v))
#define VB_LEAN_WRITE32(cpu, a, v) vb_write32((a), (v))


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
