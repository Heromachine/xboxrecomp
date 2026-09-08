/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * The audio-side mirror of nv2a_mmio_hook.h. When recompiled code accesses
 * APU registers at 0xFE800000+, the access faults (the aperture is trapped by
 * xbox_ApuEnableTrapping()), this module decodes the faulting x86-64
 * instruction, routes it through the xemu-derived APU register handlers in
 * apu_core.c, and advances RIP past the instruction.
 *
 * apu_mmio_hook.c shipped without a header, which is why nothing ever called
 * it: the decoder, the register model and the library target all existed and
 * linked, but the entry point was invisible to any consumer. See HeroLab task
 * 164c98d1.
 */

#ifndef XBOXRECOMP_APU_MMIO_HOOK_H
#define XBOXRECOMP_APU_MMIO_HOOK_H

#include <stdint.h>
#include <stdbool.h>

#include "apu.h"

/*
 * The APU occupies the first 512 KB of the 8 MB MCPX aperture, but only the
 * first 192 KB of that is MODELLED, and only the modelled part may trap:
 *
 *   0x00000-0x1FFFF  general + FE registers   -- apu_core.c, "addr < 0x20000"
 *   0x20000-0x2FFFF  VP (voice processor)     -- mcpx_apu_vp_read/write
 *   0x30000+         GP, and EP at 0x50000    -- NOT MODELLED
 *
 * apu_core.c says so itself: "GP (0x30000) and EP (0x50000) regions ignored
 * for now". Those regions are also memory-like rather than register-like --
 * GPXMEM/GPYMEM/GPPMEM are scratch buffers the title memcpys through, and a
 * memcpy reaches them as `rep movsd` from inside a system DLL, which no
 * per-instruction MMIO decoder can service. Trapping them crashed the boot at
 * offset 0x30928 on the first attempt.
 *
 * So they keep the plain-RAM treatment the MCPX aperture comment in
 * xbox_memory_layout.c describes, along with AC97 (+0x400000), USB
 * (+0x500000) and NIC (+0x700000).
 *
 * XBOX_APU_SIZE in xbox_memory_layout.c is the VirtualProtect side of this
 * same number and must be kept equal to APU_MMIO_SIZE.
 */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00030000u

/*
 * The live APU instance. Defined in apu_mmio_hook.c and left NULL there;
 * the host program assigns it the result of mcpx_apu_init_standalone().
 * While it is NULL every fault decodes to "not handled", so routing faults
 * here before init is safe rather than fatal.
 */
extern MCPXAPUState *g_apu_state;

#if defined(_WIN32)
#include <windows.h>

/*
 * Handle an APU MMIO access fault.
 *
 * Called from the VEH handler when fault_xbox_va is in APU register space
 * (APU_MMIO_BASE .. APU_MMIO_BASE + APU_MMIO_SIZE).
 *
 * Returns true if the instruction was decoded and serviced. Returns false if
 * the decoder did not recognise it, or if g_apu_state is still NULL; as with
 * the NV2A hook, a false here must NOT be swallowed by the caller, because
 * re-executing the faulting instruction would simply fault again forever.
 */
bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);

#endif /* _WIN32 */

#endif /* XBOXRECOMP_APU_MMIO_HOOK_H */
