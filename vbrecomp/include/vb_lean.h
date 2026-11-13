/* vb_lean.h - hooks used by code from the lean emitter
 * (recompiler/v810/lean.py, `vbrecomp-codegen --lean`).
 *
 * Every macro has a default that works with the stock desktop runtime.
 * A port to a small target defines VB_LEAN_PLATFORM_HEADER to a header that
 * pre-defines any of them, for example to count cycles in 32 bits or to
 * give work RAM a fast path that skips the bus function pointers.
 */
#ifndef VB_LEAN_H
#define VB_LEAN_H

#include "cpu_state.h"

#ifdef VB_LEAN_PLATFORM_HEADER
#include VB_LEAN_PLATFORM_HEADER
#endif

/* Add `n` guest cycles. Emitted once per basic block. */
#ifndef VB_LEAN_CHARGE
#define VB_LEAN_CHARGE(cpu, n) ((cpu)->cycles += (n))
#endif

/* True when the scheduler wants control back. */
#ifndef VB_LEAN_DEADLINE_HIT
#define VB_LEAN_DEADLINE_HIT(cpu) ((cpu)->cycles >= (cpu)->cycle_deadline)
#endif

/* Emitted on loop headers. Must leave the function with cpu->pc set to
 * `resume_pc` and cpu->yielded set when the deadline has passed. */
#ifndef VB_LEAN_YIELD_CHECK
#define VB_LEAN_YIELD_CHECK(cpu, resume_pc) \
    do { if (VB_LEAN_DEADLINE_HIT(cpu)) { \
        (cpu)->pc = (resume_pc); (cpu)->yielded = 1; return; } } while (0)
#endif

/* Emitted after CLI and LDSR-to-PSW: an interrupt may now be deliverable,
 * so hand control to the scheduler with cpu->pc at the next instruction. */
#ifndef VB_LEAN_IRQ_WINDOW
#define VB_LEAN_IRQ_WINDOW(cpu, next_pc) \
    do { (cpu)->pc = (next_pc); (cpu)->cycle_deadline = (cpu)->cycles; \
         (cpu)->yielded = 1; return; } while (0)
#endif

/* Emitted on the taken edge of a wait loop: a loop that reloads one memory
 * location at `addr`, has no other effect, and repeats (costing `iter`
 * cycles per pass) until that location changes.  If `addr` is plain RAM,
 * nothing can change it before the next scheduler event, so the remaining
 * passes are skipped.  The default lands on exactly the cycle count that
 * spinning would have reached. */
#ifndef VB_LEAN_IDLE_SPIN
#define VB_LEAN_IDLE_SPIN(cpu, iter, addr) \
    do { if (((((uint32_t)(addr)) >> 24) & 7u) == 5u && \
             (cpu)->cycles < (cpu)->cycle_deadline) { \
        uint64_t vb_left_ = (cpu)->cycle_deadline - (cpu)->cycles; \
        (cpu)->cycles += ((vb_left_ + (iter) - 1) / (iter)) * (iter); } } while (0)
#endif

/* Guest bus access. */
#ifndef VB_LEAN_READ8
#define VB_LEAN_READ8(cpu, a)      ((cpu)->read8(a))
#define VB_LEAN_READ16(cpu, a)     ((cpu)->read16(a))
#define VB_LEAN_READ32(cpu, a)     ((cpu)->read32(a))
#define VB_LEAN_WRITE8(cpu, a, v)  ((cpu)->write8((a), (v)))
#define VB_LEAN_WRITE16(cpu, a, v) ((cpu)->write16((a), (v)))
#define VB_LEAN_WRITE32(cpu, a, v) ((cpu)->write32((a), (v)))
#endif

/* Entries in the dispatch lookup cache; must be a power of two. */
#ifndef VB_LEAN_ROUTE_CACHE
#define VB_LEAN_ROUTE_CACHE 1024
#endif

#endif /* VB_LEAN_H */
