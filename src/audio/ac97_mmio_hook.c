/*
 * AC97 bus-master control-register hook. See ac97_mmio_hook.h for why this
 * exists and why a background thread cannot do the job.
 */

#include "ac97_mmio_hook.h"

#include <stdio.h>

#if defined(_WIN32)

extern ptrdiff_t g_xbox_mem_offset;

static void    *g_ac97_base = NULL;   /* host address of AC97_MMIO_BASE */
static bool     g_enabled   = false;

/* One guarded store is in flight per thread at a time: the fault unprotects
 * the region and sets the trap flag, and the resulting single-step re-protects
 * it. Thread-local so two threads storing at once cannot re-protect out from
 * under each other. */
static __declspec(thread) int g_stepping = 0;

/* Guarded-store counter. Kept permanently rather than deleted after the
 * investigation that needed it: "the guard is installed" and "the guard is
 * actually catching this title's stores" are different claims, and only the
 * second one explains a title that still hangs. Reported via
 * ac97_hook_stats(). */
static volatile LONG g_ac97_writes = 0;
static volatile LONG g_ac97_rr_cleared = 0;

void ac97_hook_stats(unsigned *writes, unsigned *rr_cleared)
{
    if (writes)     *writes     = (unsigned)g_ac97_writes;
    if (rr_cleared) *rr_cleared = (unsigned)g_ac97_rr_cleared;
}

/*
 * Clear RR wherever it could have been stored.
 *
 * Only the Control Register byte of each bus-master channel is touched, at
 * +0x0B of each 0x10-wide descriptor across the register file. Without an
 * instruction decoder we do not know which byte the store hit, so every CR in
 * range is sanitized; every other byte in the window is left exactly as the
 * guest wrote it.
 *
 * RR is the only bit cleared. CR's other bits (RPBM, IOCE, FEIE, LVBIE) are
 * real state the title reads back, and xemu preserves them across a reset via
 * CR_DONT_CLEAR_MASK, so clobbering them would trade one bug for a subtler one.
 */
static void ac97_sanitize(void)
{
    static const uint32_t FILE_BASES[] = { 0x0100u, 0x2100u };
    unsigned char *base = (unsigned char *)g_ac97_base;

    /*
     * Keep the voice/buffer acknowledgement asserted.
     *
     * Software writes a control bit to 0xFEC0012C and then polls 0xFEC00130
     * for bit 0x100 to come back set. That bit used to be maintained by the
     * NV2A ack thread's MCPX_IDLE table, but that thread is deliberately
     * stopped the moment any aperture starts trapping -- its plain stores
     * would themselves become faults. Nothing else was left to answer the
     * handshake, so a title that waits on it hangs with the guard installed
     * and working, which is exactly what happened here.
     *
     * Reads are not guarded, so the bit has to be physically present in RAM
     * rather than synthesised on read. Asserting it continuously is the same
     * honest answer MCPX_IDLE gave: nothing is queued for the APU to work
     * through in this runtime, so "idle" is true.
     */
    *(volatile uint32_t *)(base + 0x0130u) |= 0x00000100u;

    for (size_t f = 0; f < sizeof(FILE_BASES) / sizeof(FILE_BASES[0]); f++) {
        for (uint32_t ch = 0; ch < 0x100u; ch += 0x10u) {
            unsigned char *cr = base + FILE_BASES[f] + ch + 0x0Bu;
            if (*cr & AC97_CR_RR) {
                *cr = (unsigned char)(*cr & ~AC97_CR_RR);
                InterlockedIncrement(&g_ac97_rr_cleared);
            }
        }
    }
}

static bool ac97_protect(DWORD prot)
{
    DWORD old;
    return VirtualProtect(g_ac97_base, AC97_MMIO_SIZE, prot, &old) != 0;
}

bool ac97_hook_enable(void)
{
    if (g_enabled)
        return true;
    if (!g_xbox_mem_offset && AC97_MMIO_BASE == 0)
        return false;

    g_ac97_base = (void *)((uintptr_t)AC97_MMIO_BASE + g_xbox_mem_offset);

    /* Reads stay ordinary memory accesses -- only stores need servicing, and
     * making reads fault too would put every status poll through the VEH. */
    if (!ac97_protect(PAGE_READONLY)) {
        fprintf(stderr, "  WARNING: AC97 control-register guard could not be "
                "installed (error %lu); a title that waits on the RR bit will "
                "hang\n", GetLastError());
        return false;
    }

    g_enabled = true;
    /* Assert the ack before the guest runs, so the very first poll succeeds
     * even if it happens before any guarded store. */
    *(volatile uint32_t *)((unsigned char *)g_ac97_base + 0x0130u) |= 0x00000100u;
    fprintf(stderr, "  AC97 control registers: %u KB at Xbox VA 0x%08X guarded "
            "(RR self-clears)\n", AC97_MMIO_SIZE / 1024, AC97_MMIO_BASE);
    return true;
}

bool ac97_hook_handle_fault(PCONTEXT ctx, uint32_t fault_va, int is_write,
                            int is_single_step)
{
    if (!g_enabled)
        return false;

    if (is_single_step) {
        if (!g_stepping)
            return false;              /* somebody else's single-step */
        g_stepping = 0;
        ac97_sanitize();               /* store landed: drop RR, hold ack */
        ac97_protect(PAGE_READONLY);
        ctx->EFlags &= ~0x100u;        /* clear TF */
        return true;
    }

    if (fault_va < AC97_MMIO_BASE || fault_va >= AC97_MMIO_BASE + AC97_MMIO_SIZE)
        return false;
    if (!is_write)
        return false;                  /* reads are not guarded; a read fault
                                        * here is a real bug, not ours */

    /* Let the store execute for real, then sanitize on the way out. Stepping
     * rather than decoding means any encoding works -- byte, word, dword,
     * immediate or register source, with or without a REX prefix. */
    if (!ac97_protect(PAGE_READWRITE))
        return false;

    if (InterlockedIncrement(&g_ac97_writes) == 1) {
        /* Announce the first one. Whether this line ever appears is the
         * difference between "the guard is installed" and "the guard is doing
         * something for this title". */
        fprintf(stderr, "  [AC97] first guarded store at Xbox VA 0x%08X\n",
                fault_va);
        fflush(stderr);
    }
    g_stepping = 1;
    ctx->EFlags |= 0x100u;             /* set TF: trap after the next insn */
    return true;
}

#else  /* !_WIN32 */

bool ac97_hook_enable(void) { return false; }

#endif /* _WIN32 */
