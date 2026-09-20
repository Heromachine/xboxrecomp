/*
 * AC97 bus-master control-register hook.
 *
 * The MCPX AC97 block at 0xFEC00000 is plain RAM in this runtime -- it has no
 * register model, and the APU emulation deliberately does not cover it (see
 * apu_mmio_hook.h: "AC97 (+0x400000), USB (+0x500000) and NIC (+0x700000)"
 * keep the plain-RAM treatment).
 *
 * That is survivable for everything except one idiom. Each bus-master channel
 * has a Control Register at +0x0B whose bit 1 is RR, "reset registers".
 * Software sets RR and waits for the hardware to clear it. Burnout's DSOUND
 * does exactly that at guest 0x00161631:
 *
 *     mov  byte [reg], 2      ; set RR
 *     mov  cl,  byte [reg]    ; read it back ONCE
 *     and  cl, 2
 *     test cl, cl
 *     jne  $-2                ; spin -- on the REGISTER, never re-reading
 *
 * The loop cannot re-read, so a value that is still 2 at that single read
 * hangs the title forever. On hardware RR is cleared by the reset itself and
 * never observed set. xemu models it the same way (hw/audio/ac97.c: a CR write
 * with CR_RR calls reset_bm_regs(), which sets r->cr &= CR_DONT_CLEAR_MASK --
 * RR is simply not stored).
 *
 * A background thread clearing the bit cannot fix this: it would have to win a
 * race against ~5 instructions, and losing once hangs the run permanently. The
 * write itself has to not persist RR, so the page is made read-only and stores
 * are completed by single-stepping -- no instruction decoder, so there is no
 * unsupported-encoding gap for a different title to fall into.
 *
 * MEASURED (Burnout, 2026-09-20). Without the guard the boot thread's native
 * RIP sits inside sub_00161614 forever; with it, the thread leaves that
 * function entirely, DSOUND arms a second 60 Hz interrupt (routine
 * 0x0015BC55, which never happened before) and the title runs on. The title
 * still does not reach a frame, but it stalls somewhere else, which is a
 * different problem. Both halves of that A/B were taken with the in-process
 * stall probe (XBOXRECOMP_STALL_PROBE); gdb cannot take either one, because
 * it intercepts the guard's SIGSEGV before the VEH sees it.
 *
 * A title that never calls ac97_hook_enable() is unaffected -- the file is
 * compiled and never invoked. Breakdown was re-run on this commit: 457 [APU]
 * lines, loudest sample 1.000, no AC97 lines at all.
 */

#ifndef XBOXRECOMP_AC97_MMIO_HOOK_H
#define XBOXRECOMP_AC97_MMIO_HOOK_H

#include <stdint.h>
#include <stdbool.h>

/* AC97 bus-master register file. The block base is 0xFEC00100; observed
 * channel descriptors sit at +0x00/0x10/0x40/0x70 and at +0x2000. The guarded
 * span covers both pages that hold them. */
#define AC97_MMIO_BASE      0xFEC00000u
#define AC97_MMIO_SIZE      0x00003000u

#define AC97_CR_RR          0x02u   /* reset-registers bit, self-clearing */

#if defined(_WIN32)
#include <windows.h>

/*
 * Service a fault in the AC97 window. Returns true if the exception was
 * handled and execution may continue.
 *
 * Call for BOTH access violations and single-step exceptions: a guarded store
 * is completed by stepping over it, so the step lands back here.
 */
bool ac97_hook_handle_fault(PCONTEXT ctx, uint32_t fault_va, int is_write,
                            int is_single_step);

#endif /* _WIN32 */

/* Enable guarding. Safe to call once memory layout is up; returns false if the
 * region could not be protected, in which case behaviour is exactly as before
 * (plain RAM) rather than broken. */
bool ac97_hook_enable(void);

/* Guarded stores seen, and RR bits actually cleared. A title that still hangs
 * with zero writes here is not being helped by the guard at all, which is a
 * different problem from one where RR is cleared and it hangs anyway. */
void ac97_hook_stats(unsigned *writes, unsigned *rr_cleared);

#endif /* XBOXRECOMP_AC97_MMIO_HOOK_H */
