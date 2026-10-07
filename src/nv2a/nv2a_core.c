/*
 * NV2A GPU Core - Standalone register handlers
 *
 * Adapted from xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * Contains: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC register handlers,
 * nv2a_update_irq, DMA helpers, block dispatch table, and standalone init.
 */

#include "nv2a_state.h"
#include "nv2a_pgraph_d3d11.h"
#include "../d3d/d3d8_xbox.h"   /* d3d8_PresentFrame, called at FLIP_STALL */

/* ============================================================
 * Global state
 * ============================================================ */

static NV2AState *g_nv2a = NULL;
static MemoryRegion g_vram_region;
static MemoryRegion g_ramin_region;

/* PRAMIN window, matching xemu's subregion at MMIO 0x700000. */
#define NV_PRAMIN_OFFSET 0x700000u
#define NV_PRAMIN_SIZE   0x100000u

NV2AState *nv2a_get_state(void) {
    return g_nv2a;
}

/* ============================================================
 * IRQ aggregation (from xemu nv2a.c)
 * ============================================================ */

void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts) {
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* ============================================================
 * DMA helpers (from xemu nv2a.c)
 * ============================================================ */

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

/* Resolve an object HANDLE to its RAMIN instance address through the RAMHT.
 *
 * The parameter a title passes to SET_CONTEXT_DMA_* is a handle, not a RAMIN
 * offset -- Burnout binds its semaphore object with handle 8, and RAMIN offset
 * 8 is in the middle of the hash table itself. The table maps it:
 *   handle 0x00000008 -> context 0x8000011B -> instance 0x11B << 4 = 0x11B0
 * and RAMIN 0x11B0 holds the object {limit 0x20, address 0x03FFD000}, which is
 * the physical address of the dword the title polls.
 *
 * Hash and layout are xemu's (hw/xbox/nv2a/pfifo.c ramht_hash/ramht_lookup).
 * Returns 0 if the handle is not found, which callers must treat as "cannot
 * resolve" rather than "RAMIN offset 0".
 */
uint32_t nv_ramht_instance(NV2AState *d, uint32_t handle)
{
    uint32_t ramht_reg = d->pfifo.regs[NV_PFIFO_RAMHT / 4];
    unsigned ramht_size = 1u << (GET_MASK(ramht_reg, NV_PFIFO_RAMHT_SIZE) + 12);
    unsigned bits = (unsigned)ctz32(ramht_size) - 1;
    hwaddr base = (hwaddr)GET_MASK(ramht_reg, NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;
    uint32_t h = handle, hash = 0;
    unsigned channel_id;
    hwaddr entry;

    if (!d->ramin_ptr || bits == 0 || bits > 31)
        return 0;

    while (h) {
        hash ^= (h & ((1u << bits) - 1u));
        h >>= bits;
    }
    channel_id = GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1 / 4],
                          NV_PFIFO_CACHE1_PUSH1_CHID);
    hash ^= channel_id << (bits - 4);

    entry = base + (hwaddr)hash * 8;
    if (entry + 8 > NV_PRAMIN_SIZE)
        return 0;

    {
        const uint8_t *p = d->ramin_ptr + entry;
        uint32_t entry_handle  = ldl_le_p((const uint32_t *)p);
        uint32_t entry_context = ldl_le_p((const uint32_t *)(p + 4));
        if (entry_handle != handle || !(entry_context & NV_RAMHT_STATUS))
            return 0;
        return (entry_context & NV_RAMHT_INSTANCE) << 4;
    }
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    uint32_t ram_size = 0;
    uint8_t *ram;

    /* Resolve against GUEST RAM, not d->vram_ptr.
     *
     * On real hardware there is no separate VRAM: GPU memory IS system memory,
     * and a DMA object's address is a physical address into it. d->vram_ptr is
     * a private VirtualAlloc block disconnected from the guest, so resolving
     * there returned a pointer into memory the title has never seen -- which
     * is why the caution above pfifo_pull said this path wanted pointing at
     * guest RAM before it could return anything real. It does now.
     *
     * Same 26-bit wrap as pb_read32: the Xbox memory controller wraps every
     * physical address modulo installed RAM, so mask rather than reject. */
    ram = (uint8_t *)nv2a_get_guest_ram(&ram_size);
    if (!ram || ram_size == 0) {
        *len = 0;
        return NULL;
    }

    dma.address &= 0x07FFFFFF;

    *len = dma.limit;
    return ram + (dma.address & (ram_size - 1));
}

/* ============================================================
 * PMC - card master control (from xemu pmc.c)
 * ============================================================ */

uint64_t pmc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* NV2A, A03, Rev 0 */
        r = 0x02A000A3;
        break;
    case NV_PMC_INTR_0:
        r = d->pmc.pending_interrupts;
        break;
    case NV_PMC_INTR_EN_0:
        r = d->pmc.enabled_interrupts;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PMC, addr, size, r);
    return r;
}

void pmc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PMC, addr, size, val);

    switch (addr) {
    case NV_PMC_INTR_0:
        /* Bounded trace: a registered ISR acknowledging an interrupt shows up
         * here (write-1-to-clear). Silence means nothing is being serviced. */
        {
            static int logged;
            if (logged < 8) {
                logged++;
                fprintf(stderr, "  [NV2A] PMC_INTR_0 write 0x%08X (pending was 0x%08X)\n",
                        (uint32_t)val, d->pmc.pending_interrupts);
                fflush(stderr);
            }
        }
        d->pmc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PMC_INTR_EN_0:
        /* The question this answers: does the title EVER enable GPU
         * interrupts? sub_001C6890, the vblank ISR the delivery thread
         * invokes at 60 Hz, returns FALSE immediately whenever this reads 0 --
         * so if it is never written non-zero, that ISR is inert by design at
         * this stage and the vblank -> DPC chain cannot fire for this title. */
        {
            static int logged;
            if (logged < 8) {
                logged++;
                fprintf(stderr, "  [NV2A] PMC_INTR_EN_0 write 0x%08X\n", (uint32_t)val);
                fflush(stderr);
            }
        }
        d->pmc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PBUS - bus control (from xemu pbus.c)
 * ============================================================ */

uint64_t pbus_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    uint64_t r = 0;
    switch (addr) {
    case NV_PBUS_PCI_NV_0:
        r = pci_get_long(d->config + PCI_VENDOR_ID);
        break;
    case NV_PBUS_PCI_NV_1:
        r = pci_get_long(d->config + PCI_COMMAND);
        break;
    case NV_PBUS_PCI_NV_2:
        r = pci_get_long(d->config + PCI_CLASS_REVISION);
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PBUS, addr, size, r);
    return r;
}

void pbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    nv2a_reg_log_write(NV_PBUS, addr, size, val);

    switch (addr) {
    case NV_PBUS_PCI_NV_1:
        pci_set_long(d->config + PCI_COMMAND, val);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PTIMER - time measurement (from xemu ptimer.c)
 * ============================================================ */

static uint64_t ptimer_get_clock(NV2AState *d)
{
    if (d->ptimer.numerator == 0) return 0;
    return muldiv64(muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                             d->pramdac.core_clock_freq,
                             NANOSECONDS_PER_SECOND),
                    d->ptimer.denominator,
                    d->ptimer.numerator);
}

uint64_t ptimer_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PTIMER_INTR_0:
        r = d->ptimer.pending_interrupts;
        break;
    case NV_PTIMER_INTR_EN_0:
        r = d->ptimer.enabled_interrupts;
        break;
    case NV_PTIMER_NUMERATOR:
        r = d->ptimer.numerator;
        break;
    case NV_PTIMER_DENOMINATOR:
        r = d->ptimer.denominator;
        break;
    case NV_PTIMER_TIME_0:
        r = (ptimer_get_clock(d) & 0x7ffffff) << 5;
        break;
    case NV_PTIMER_TIME_1:
        r = (ptimer_get_clock(d) >> 27) & 0x1fffffff;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PTIMER, addr, size, r);
    return r;
}

void ptimer_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PTIMER, addr, size, val);

    switch (addr) {
    case NV_PTIMER_INTR_0:
        d->ptimer.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_INTR_EN_0:
        d->ptimer.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_DENOMINATOR:
        d->ptimer.denominator = val;
        break;
    case NV_PTIMER_NUMERATOR:
        d->ptimer.numerator = val;
        break;
    case NV_PTIMER_ALARM_0:
        d->ptimer.alarm_time = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PFB - framebuffer / memory control (from xemu pfb.c)
 * ============================================================ */

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = memory_region_size(d->vram);
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * PCRTC - CRT controller (from xemu pcrtc.c)
 * ============================================================ */

uint64_t pcrtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PCRTC_INTR_0:
        r = d->pcrtc.pending_interrupts;
        break;
    case NV_PCRTC_INTR_EN_0:
        r = d->pcrtc.enabled_interrupts;
        break;
    case NV_PCRTC_START:
        r = d->pcrtc.start;
        break;
    case NV_PCRTC_RASTER:
        r = d->pcrtc.raster++;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PCRTC, addr, size, r);
    return r;
}

void pcrtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PCRTC, addr, size, val);

    switch (addr) {
    case NV_PCRTC_INTR_0:
        d->pcrtc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_INTR_EN_0:
        d->pcrtc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_START:
        val &= 0x07FFFFFF;
        d->pcrtc.start = val;
        NV2A_DPRINTF("PCRTC_START - %x %x %x %x\n",
                d->vram_ptr[val+64], d->vram_ptr[val+64+1],
                d->vram_ptr[val+64+2], d->vram_ptr[val+64+3]);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PRAMDAC - RAMDAC / PLL control (from xemu pramdac.c)
 * ============================================================ */

uint64_t pramdac_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr & ~3) {
    case NV_PRAMDAC_NVPLL_COEFF:
        r = d->pramdac.core_clock_coeff;
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        r = d->pramdac.memory_clock_coeff;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        r = d->pramdac.video_clock_coeff;
        break;
    case NV_PRAMDAC_PLL_TEST_COUNTER:
        /* emulated PLLs locked instantly */
        r = NV_PRAMDAC_PLL_TEST_COUNTER_VPLL2_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_NVPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_MPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_VPLL_LOCK;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        r = d->pramdac.general_control;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        r = d->pramdac.fp_vdisplay_end;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        r = d->pramdac.fp_vcrtc;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        r = d->pramdac.fp_vsync_end;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        r = d->pramdac.fp_vvalid_end;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        r = d->pramdac.fp_hdisplay_end;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        r = d->pramdac.fp_hcrtc;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        r = d->pramdac.fp_hvalid_end;
        break;
    default:
        break;
    }

    /* Handle unaligned access */
    r >>= 32 - 8 * size - 8 * (addr & 3);

    nv2a_reg_log_read(NV_PRAMDAC, addr, size, r);
    return r;
}

void pramdac_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t m, n, p;

    nv2a_reg_log_write(NV_PRAMDAC, addr, size, val);

    switch (addr) {
    case NV_PRAMDAC_NVPLL_COEFF:
        d->pramdac.core_clock_coeff = val;

        m = val & NV_PRAMDAC_NVPLL_COEFF_MDIV;
        n = (val & NV_PRAMDAC_NVPLL_COEFF_NDIV) >> 8;
        p = (val & NV_PRAMDAC_NVPLL_COEFF_PDIV) >> 16;

        if (m == 0) {
            d->pramdac.core_clock_freq = 0;
        } else {
            d->pramdac.core_clock_freq = (NV2A_CRYSTAL_FREQ * n)
                                          / (1 << p) / m;
        }
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        d->pramdac.memory_clock_coeff = val;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        d->pramdac.video_clock_coeff = val;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        d->pramdac.general_control = val;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        d->pramdac.fp_vdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        d->pramdac.fp_vcrtc = val;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        d->pramdac.fp_vsync_end = val;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        d->pramdac.fp_vvalid_end = val;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        d->pramdac.fp_hdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        d->pramdac.fp_hcrtc = val;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        d->pramdac.fp_hvalid_end = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PVIDEO - video overlay (stub)
 * ============================================================ */

uint64_t pvideo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pvideo.regs[addr];
    nv2a_reg_log_read(NV_PVIDEO, addr, size, r);
    return r;
}

void pvideo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PVIDEO, addr, size, val);
    d->pvideo.regs[addr] = val;
}

/* ============================================================
 * PGRAPH - graphics engine (stub for Phase 1)
 * ============================================================ */

uint64_t pgraph_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = addr == NV_PGRAPH_PATT_COLOR0
        ? __atomic_load_n(&d->pgraph.regs[addr], __ATOMIC_ACQUIRE)
        : d->pgraph.regs[addr];
    nv2a_reg_log_read(NV_PGRAPH, addr, size, r);
    return r;
}

void pgraph_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PGRAPH, addr, size, val);
    d->pgraph.regs[addr] = val;
}

/* ============================================================
 * PGRAPH method dispatch
 * Called when push buffer commands are parsed.
 * Routes method calls into PGRAPH register writes.
 * ============================================================ */

static uint32_t g_pgraph_method_count = 0;
static uint32_t g_pgraph_draw_count = 0;
static uint32_t g_pgraph_clear_count = 0;
static uint32_t g_pgraph_flip_count = 0;
static uint32_t g_pgraph_inline_verts = 0;
static int g_pgraph_in_begin = 0;

/* NV097 method constants for dispatch */
#define M_NO_OPERATION          0x0100
#define M_SET_SURFACE_FORMAT    0x0208
#define M_SET_SURFACE_PITCH     0x020C
#define M_SET_SURFACE_COLOR_OFF 0x0210
#define M_SET_SURFACE_ZETA_OFF  0x0214
#define M_SET_SURFACE_CLIP_H    0x0200
#define M_SET_SURFACE_CLIP_V    0x0204
#define M_CLEAR_SURFACE         0x01D0
#define M_SET_COLOR_CLEAR_VALUE 0x01D4
#define M_SET_BEGIN_END         0x17FC
#define M_INLINE_ARRAY          0x1818
/* The flip group, confirmed against what Breakdown actually emits: 0x0120/24/28
 * are written 0, 1, 2 back to back during device init (SET_FLIP_READ,
 * SET_FLIP_WRITE, SET_FLIP_MODULO for a double buffer), and 0x012C then 0x0130
 * arrive together at the end of the first frame. These were previously 0x0114
 * and 0x0118 -- the whole group written down 0x18 low -- so FLIP_STALL never
 * matched, no frame ever ended, and 0x0110 (which is WAIT_FOR_IDLE) was
 * miscounted as a flip. */
#define M_FLIP_INCREMENT_WRITE  0x012C
#define M_FLIP_STALL            0x0130
/* Back-end write semaphore. Looked up in nv2a_regs.h, not inferred:
 * NV097_SET_CONTEXT_DMA_SEMAPHORE 0x01A4, NV097_SET_SEMAPHORE_OFFSET 0x1D6C,
 * NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70. */
#define M_SET_CONTEXT_DMA_SEMAPHORE 0x01A4
/* Occlusion-query reports: NV097_SET_CONTEXT_DMA_REPORT 0x01A8,
 * NV097_CLEAR_REPORT_VALUE 0x17C8, NV097_SET_ZPASS_PIXEL_COUNT_ENABLE 0x17CC,
 * NV097_GET_REPORT 0x17D0 (nv2a_regs.h). */
#define M_SET_CONTEXT_DMA_REPORT       0x01A8
#define M_CLEAR_REPORT_VALUE           0x17C8
#define M_SET_ZPASS_PIXEL_COUNT_ENABLE 0x17CC
#define M_GET_REPORT                   0x17D0
static uint32_t g_dma_report;     /* report DMA object handle */
static uint32_t g_zpass_result;   /* samples since CLEAR_REPORT_VALUE */
#define M_SET_SEMAPHORE_OFFSET      0x1D6C
#define M_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70

#define M_SET_VIEWPORT_OFFSET   0x0A20
#define M_SET_VIEWPORT_SCALE    0x0AF0

void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param)
{
    g_pgraph_method_count++;

    /* SET_OBJECT carries a RAMHT handle. The object context's low byte is
     * the graphics class, as in xemu's pgraph_method SET_OBJECT path. */
    if (method == 0 && subchannel < 8) {
        uint32_t instance = nv_ramht_instance(d, param);
        if (instance && instance + 4 <= NV_PRAMIN_SIZE)
            d->pgraph.subchannel_class[subchannel] =
                (uint8_t)ldl_le_p((uint32_t *)(d->ramin_ptr + instance));
    }

    /* NV044_SET_MONOCHROME_COLOR0 is also visible at PGRAPH PATT_COLOR0.
     * Breakdown polls bits 2..6 there as a completion marker. */
    if (subchannel < 8 &&
        d->pgraph.subchannel_class[subchannel] == NV_CONTEXT_PATTERN &&
        method == NV044_SET_MONOCHROME_COLOR0)
        __atomic_store_n(&d->pgraph.regs[NV_PGRAPH_PATT_COLOR0],
                         param, __ATOMIC_RELEASE);

    /* XBOXRECOMP_METHODTRACE=<frame> prints every method of that ONE frame.
     *
     * Scoped to a frame because the interesting question is always "what did
     * the title ask for in a frame that looked wrong", and a whole-run trace
     * buries that in millions of lines. Frame numbers match the [PGRAPH] Frame
     * counter, so pick one from a run and re-run to see inside it.
     *
     * Decode the result against nv2a_regs.h -- reading these numbers from
     * memory is how four wrong constants got into this file once before. */
    { static int want = -2;
      if (want == -2) {
          const char *e = getenv("XBOXRECOMP_METHODTRACE");
          want = e && *e ? atoi(e) : -1;
      }
      if (want >= 0 && (int)g_pgraph_flip_count == want)
          fprintf(stderr, "[MTRACE] %u sub=%u 0x%04X = 0x%08X\n",
                  g_pgraph_method_count, subchannel, method, param); }

    /* Frame boundary, handled before the translator gets a say.
     *
     * FLIP_STALL is where the title says "that was a frame, show it" -- on
     * hardware the pusher blocks here until the CRTC has flipped. The D3D11
     * translator's default case swallows 0x0114/0x0118 and returns handled, so
     * the legacy counters further down never see them; presenting has to
     * happen here or it does not happen at all. Flush first: the translator
     * batches draws and the last one of the frame is still pending. */
    if (method == M_FLIP_STALL) {
        pgraph_d3d11_flush();
        g_pgraph_flip_count++;
        if (g_pgraph_flip_count <= 5 || (g_pgraph_flip_count % 300) == 0) {
            /* Counts come from the translator, not the legacy counters below.
             * Those live past a `return` for every method the translator
             * claims, which is now nearly all of them -- they read 0 forever
             * and say nothing about whether anything was drawn. */
            PgraphD3D11Stats st;
            pgraph_d3d11_get_stats(&st);
            fprintf(stderr, "[PGRAPH] Frame %u: %u methods (%u ignored), "
                    "%u draws, %u verts, %u clears, %u array draws, "
                    "%u array gather failures, scanout 0x%08X\n",
                    g_pgraph_flip_count, g_pgraph_method_count,
                    st.methods_ignored, st.draw_calls, st.vertices_submitted,
                    st.clears, st.array_draws, st.array_gather_failures,
                    (unsigned)d->pcrtc.start);
            fflush(stderr);
        }
        d3d8_PresentFrame();
        return;
    }

    /*
     * Back-end write semaphore: the GPU's answer to "how far have you got?".
     *
     * The driver keeps its own submission counter and will not reuse a buffer
     * until the GPU reports having passed a given value, so a semaphore that
     * never advances stops the title dead -- Burnout spins in its own D3D at
     * sub_0012D3B0 polling the semaphore dword, needing 9, seeing 3 forever.
     *
     * Handled BEFORE the translator, which claims these methods and returns
     * "handled", exactly as FLIP_STALL has to be.
     *
     * Semantics are xemu's (hw/xbox/nv2a/pgraph/pgraph.c,
     * NV097_BACK_END_WRITE_SEMAPHORE_RELEASE): map the bound DMA object, add
     * the semaphore offset, store the parameter there as a little-endian
     * 32-bit word. xemu updates surfaces first; the equivalent here is
     * flushing the translator's batched draws, so the release cannot be
     * observed before the work it accounts for.
     */
    /* Occlusion-query reports (xemu pgraph.c CLEAR_REPORT_VALUE /
     * SET_ZPASS_PIXEL_COUNT_ENABLE / GET_REPORT, report layout from
     * pgraph_write_zpass_pixel_cnt_report: u64 timestamp, u32 count, u32 done
     * at the report DMA object + offset). Unhandled before, so every report
     * the title polled read whatever was in memory -- see
     * pgraph_d3d11_zpass_collect(). */
    if (method == M_SET_CONTEXT_DMA_REPORT) {
        g_dma_report = param;
        d->pgraph.regs[method / 4] = param;
        return;
    }
    if (method == M_CLEAR_REPORT_VALUE) {
        pgraph_d3d11_zpass_clear();
        g_zpass_result = 0;
        return;
    }
    if (method == M_SET_ZPASS_PIXEL_COUNT_ENABLE) {
        pgraph_d3d11_zpass_enable(param != 0);
        return;
    }
    if (method == M_GET_REPORT) {
        hwaddr len = 0;
        uint8_t *rep;
        uint32_t offset = param & 0x00FFFFFF;
        static unsigned logged;

        g_zpass_result += pgraph_d3d11_zpass_collect();
        { uint32_t inst = nv_ramht_instance(d, g_dma_report);
          rep = inst ? (uint8_t *)nv_dma_map(d, inst, &len) : NULL; }
        if (rep && offset + 16 <= len) {
            uint64_t ts = 0x0011223344556677ull;   /* xemu's placeholder */
            uint8_t *p = rep + offset;
            int i;
            for (i = 0; i < 8; i++) p[i] = (uint8_t)(ts >> (8 * i));
            for (i = 0; i < 4; i++) p[8 + i] = (uint8_t)(g_zpass_result >> (8 * i));
            for (i = 0; i < 4; i++) p[12 + i] = 0;
        }
        if (logged < 40 || (logged % 5000) == 0)
            fprintf(stderr, "[REPORT] GET_REPORT type %u offset 0x%06X -> %u samples%s\n",
                    param >> 24, offset, g_zpass_result,
                    rep ? "" : " (report DMA object did not resolve)");
        logged++;
        return;
    }
    if (method == M_SET_CONTEXT_DMA_SEMAPHORE) {
        d->pgraph.dma_semaphore = param;
        d->pgraph.regs[method / 4] = param;
        return;
    }
    if (method == M_SET_SEMAPHORE_OFFSET) {
        d->pgraph.semaphore_offset = param;
        d->pgraph.regs[method / 4] = param;
        return;
    }
    if (method == M_BACK_END_WRITE_SEMAPHORE_RELEASE) {
        hwaddr len = 0;
        uint8_t *sem;

        pgraph_d3d11_flush();

        { uint32_t inst = nv_ramht_instance(d, d->pgraph.dma_semaphore);
          sem = inst ? (uint8_t *)nv_dma_map(d, inst, &len) : NULL; }
        if (sem && d->pgraph.semaphore_offset + 4 <= len) {
            uint8_t *p = sem + d->pgraph.semaphore_offset;
            /* Byte-wise, so an unaligned offset cannot fault on a host that
             * cares, and little-endian explicitly rather than by memcpy. */
            p[0] = (uint8_t)(param      );
            p[1] = (uint8_t)(param >>  8);
            p[2] = (uint8_t)(param >> 16);
            p[3] = (uint8_t)(param >> 24);
        } else {
            static int warned;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[PGRAPH] semaphore release 0x%08X dropped: "
                        "dma 0x%08X offset 0x%X maps %s (len %llu)\n",
                        param, d->pgraph.dma_semaphore,
                        d->pgraph.semaphore_offset, sem ? "short" : "NULL",
                        (unsigned long long)len);
                fflush(stderr);
            }
        }
        d->pgraph.regs[method / 4] = param;
        return;
    }

    /* Sample translator cost without timing every one of millions of methods. */
    static int profile_on = -1;
    static LARGE_INTEGER profile_frequency;
    static uint64_t profile_samples[7], profile_ticks[7];
    static uint64_t profile_method_samples[0x2000 / 4];
    static uint64_t profile_method_ticks[0x2000 / 4];
    static ULONGLONG profile_last_report;
    if (profile_on < 0) {
        const char *env = getenv("XBOXRECOMP_PGRAPH_PROFILE");
        profile_on = env && strcmp(env, "1") == 0;
        if (profile_on) QueryPerformanceFrequency(&profile_frequency);
    }
    bool profile_sample = profile_on &&
                          (g_pgraph_method_count % 101u) == 0;
    LARGE_INTEGER profile_start, profile_end;
    if (profile_sample) QueryPerformanceCounter(&profile_start);
    int handled = pgraph_d3d11_method(subchannel, method, param);
    if (profile_sample) {
        int category = method >= NV097_SET_TRANSFORM_CONSTANT &&
                       method < NV097_SET_TRANSFORM_CONSTANT + 0x80 ? 0 :
                       method >= NV097_SET_TRANSFORM_PROGRAM &&
                       method < NV097_SET_TRANSFORM_PROGRAM + 0x80 ? 1 :
                       method == NV097_ARRAY_ELEMENT16 ||
                       method == NV097_ARRAY_ELEMENT32 ? 2 :
                       method == NV097_SET_BEGIN_END ? 3 :
                       method == NV097_DRAW_ARRAYS ? 4 :
                       method == NV097_INLINE_ARRAY ? 5 : 6;
        QueryPerformanceCounter(&profile_end);
        profile_samples[category]++;
        profile_ticks[category] +=
            profile_end.QuadPart - profile_start.QuadPart;
        if (method < 0x2000 && !(method & 3)) {
            profile_method_samples[method / 4]++;
            profile_method_ticks[method / 4] +=
                profile_end.QuadPart - profile_start.QuadPart;
        }
        ULONGLONG now = GetTickCount64();
        if (!profile_last_report) profile_last_report = now;
        if (now - profile_last_report >= 5000) {
            fprintf(stderr, "[PGRAPH METHOD PROFILE]");
            for (int i = 0; i < 7; i++)
                fprintf(stderr, " c%d=%llu/%.3fus", i,
                        (unsigned long long)profile_samples[i],
                        profile_samples[i] ?
                            (double)profile_ticks[i] * 1000000.0 /
                            profile_frequency.QuadPart / profile_samples[i] : 0.0);
            fprintf(stderr, "\n");
            for (int rank = 0; rank < 8; rank++) {
                uint64_t best = 0;
                int best_method = -1;
                for (int i = 0; i < 0x2000 / 4; i++)
                    if (profile_method_ticks[i] > best) {
                        best = profile_method_ticks[i];
                        best_method = i;
                    }
                if (best_method < 0) break;
                fprintf(stderr, "[PGRAPH METHOD TOP] %04X samples=%llu "
                        "avg_us=%.3f est_ms=%.1f\n", best_method * 4,
                        (unsigned long long)profile_method_samples[best_method],
                        (double)best * 1000000.0 /
                            profile_frequency.QuadPart /
                            profile_method_samples[best_method],
                        (double)best * 101000.0 /
                            profile_frequency.QuadPart);
                profile_method_ticks[best_method] = 0;
            }
            memset(profile_samples, 0, sizeof(profile_samples));
            memset(profile_ticks, 0, sizeof(profile_ticks));
            memset(profile_method_samples, 0, sizeof(profile_method_samples));
            memset(profile_method_ticks, 0, sizeof(profile_method_ticks));
            profile_last_report = now;
        }
    }

    /* Route through D3D11 translator first */
    if (handled) {
        /* Handled by D3D11 translator — still store in regs for state queries */
        if (method < 0x2000 * 4) {
            d->pgraph.regs[method / 4] = param;
        }
        return;
    }

    /* Log unhandled methods (first 20 + periodic) */
    if (g_pgraph_method_count <= 20 || (g_pgraph_method_count % 5000) == 0) {
        fprintf(stderr, "[PGRAPH] #%u UNHANDLED sub=%u 0x%04X = 0x%08X\n",
                g_pgraph_method_count, subchannel, method, param);
    }

    /* Store method parameters in PGRAPH register space */
    if (method < 0x2000 * 4) {
        d->pgraph.regs[method / 4] = param;
    }

    /* Track high-level operations (legacy counters) */
    switch (method) {
    case M_CLEAR_SURFACE:
        g_pgraph_clear_count++;
        break;

    case M_SET_BEGIN_END:
        if (param != 0) {
            g_pgraph_in_begin = 1;
            g_pgraph_draw_count++;
        } else {
            g_pgraph_in_begin = 0;
        }
        break;

    case M_INLINE_ARRAY:
        if (g_pgraph_in_begin) {
            g_pgraph_inline_verts++;
        }
        break;

    /* M_FLIP_INCREMENT_WRITE used to count frames here. It never arrives: the
     * D3D11 translator claims 0x0114 in its ignore list, so this switch is
     * unreachable for it. Frames are counted at FLIP_STALL above instead. */

    default:
        break;
    }
}

/* ============================================================
 * PFIFO - command FIFO (stub for Phase 1)
 * Full PFIFO with push buffer processing comes in Phase 2-3.
 * ============================================================ */

uint64_t pfifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFIFO_INTR_0:
        r = d->pfifo.pending_interrupts;
        break;
    case NV_PFIFO_INTR_EN_0:
        r = d->pfifo.enabled_interrupts;
        break;

    /* Queue-empty status. LOW_MARK means "at or below the low water mark",
     * i.e. drained. Nothing here queues runout entries or pushes methods into
     * CACHE1, so empty is the truthful answer -- the same reasoning that makes
     * the interrupt-pending registers read 0 because nothing raises
     * interrupts, not a convenience.
     *
     * Falling through to the regs[] default instead returns 0, which reads as
     * "not empty" forever and hangs any title that waits for the FIFO to
     * drain. That is not hypothetical: it is why enabling the NV2A backend
     * regressed Breakdown's boot on 2026-09-06. The ack thread in
     * xbox_memory_layout.c (NV2A_IDLE) had been forcing these two bits set
     * from outside, and trapping the aperture necessarily stops that thread,
     * so the answer has to come from here instead. */
    case NV_PFIFO_RUNOUT_STATUS:
        r = NV_PFIFO_RUNOUT_STATUS_LOW_MARK;
        break;
    case NV_PFIFO_CACHE1_STATUS:
        r = NV_PFIFO_CACHE1_STATUS_LOW_MARK;
        break;

    default:
        r = d->pfifo.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFIFO, addr, size, r);
    return r;
}

void pfifo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFIFO, addr, size, val);

    switch (addr) {
    case NV_PFIFO_INTR_0:
        d->pfifo.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_INTR_EN_0:
        d->pfifo.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        d->pfifo.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * Stub handler for unimplemented blocks
 * ============================================================ */

uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub read: addr=0x%llx size=%d\n",
                 (unsigned long long)addr, size);
    return 0;
}

void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub write: addr=0x%llx val=0x%llx size=%d\n",
                 (unsigned long long)addr, (unsigned long long)val, size);
}

/* ============================================================
 * NV_USER - PFIFO channel push buffer pointers, and the puller
 *
 * Software submits work by writing DMA_PUT, then spins reading DMA_GET until
 * it matches: "you have consumed everything I submitted". Left as a stub this
 * read 0 forever and any title driving its own push buffer hangs here --
 * Breakdown does exactly that, spinning ~250,000 reads/second on 0x800044.
 *
 * The puller below walks guest RAM from GET to PUT, decodes the command
 * stream, and dispatches methods to pgraph. GET advances as commands are
 * actually consumed, which is both what hardware does and what makes the
 * title's spin terminate honestly.
 *
 * Before it existed, DMA_PUT simply slammed GET to PUT: the commands were
 * reported consumed without being executed. That kept the title running and
 * drew nothing. If guest RAM was never handed over (nv2a_set_guest_ram), the
 * puller cannot read the stream and that old acknowledgement is still the
 * fallback -- boot survives, the screen stays black.
 * ============================================================ */

#define NV_USER_DMA_PUT 0x40u
#define NV_USER_DMA_GET 0x44u
#define NV_USER_REF     0x48u

/* Host address of guest physical 0, and how much RAM is behind it. */
static uint8_t *g_guest_ram = NULL;
static uint32_t g_guest_ram_size = 0;
static bool g_pfifo_async_enabled = false;
static volatile LONG g_pfifo_async_stage;
static volatile LONG g_pfifo_async_channel = -1;
static volatile LONG g_pfifo_async_puts;
static volatile LONG g_pfifo_async_acked_puts;
static volatile LONG g_pfifo_async_throttles;
static LONG g_pfifo_async_max_pending_puts = 8;
static volatile uint64_t g_pfifo_async_wait_ms;
static volatile uint64_t g_pfifo_async_wait_max_ms;
static volatile uint64_t g_pfifo_async_decode_ms;
static volatile LONG g_pfifo_async_wakes;
static volatile LONG g_pfifo_async_batches;

/* MMIO producer and PFIFO consumer exchange the pointers without holding the
 * decoder lock. A release when publishing GET also orders completed methods
 * before the guest observes that the command was consumed. */
static inline uint32_t pfifo_load(const uint32_t *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void pfifo_store(uint32_t *ptr, uint32_t value)
{
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

void nv2a_set_guest_ram(void *base, uint32_t size)
{
    g_guest_ram = (uint8_t *)base;
    g_guest_ram_size = size;
    fprintf(stderr, "[NV2A] push buffer puller armed: guest RAM %u MB at %p\n",
            size / (1024u * 1024u), base);
    fflush(stderr);
}

/* Accessor for anything else that needs guest RAM's base and size, so the
 * 0x80000000/64MB convention lives in one place rather than being copied.
 * Returns NULL if guest RAM was never handed over (nv2a_set_guest_ram()
 * not yet called).
 *
 * CAUTION: this is the base the PUSH BUFFER uses, and "whole of RAM, zero
 * base" is scoped to that object alone -- see the pfifo_pull() comment. It
 * does NOT generalise to textures. That was measured the hard way: every
 * byte read through here at a SET_TEXTURE_OFFSET address came back 0x00,
 * for both formats this title uses, with a live-vs-cached comparison ruling
 * out a read-before-write race. Real texture addressing is DMA-object base
 * plus offset, the object chosen by a field in the format register
 * (xemu pgraph/texture.c:85-99):
 *     dma_select = GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CONTEXT_DMA);
 *     data = nv_dma_map(d, dma_select ? pg->dma_b : pg->dma_a, &len);
 *     data += offset;
 * This fork tracks neither SET_CONTEXT_DMA_A (0x0184) nor _B (0x0188).
 * And nv_dma_map() above resolves against d->vram_ptr, a private
 * VirtualAlloc block disconnected from guest RAM -- on real hardware there
 * is no separate VRAM, GPU memory IS system memory -- so that path wants
 * pointing at guest RAM before it can return anything real either. */
const uint8_t *nv2a_get_guest_ram(uint32_t *out_size)
{
    if (out_size)
        *out_size = g_guest_ram_size;
    return g_guest_ram;
}

/* Read one push buffer dword.
 *
 * The Xbox memory controller drives a 26-bit address bus, so every physical
 * address wraps modulo the installed RAM -- the same wrap the launcher's
 * mirror views model on the CPU side. Masking here rather than rejecting keeps
 * a legitimately wrapped pointer working instead of aborting the frame. */
static inline uint32_t pb_read32(uint32_t phys)
{
    return *(const uint32_t *)(g_guest_ram + (phys & (g_guest_ram_size - 1)));
}

/* Walk the channel's push buffer from GET to PUT, dispatching to pgraph.
 *
 * Command encoding is nv04's, unchanged on NV2A:
 *   bits 1:0 == 1        jump      (get = word & ~3)
 *   bits 1:0 == 2        call      (get = word & ~3, return address shadowed)
 *   word == 0x00020000   return
 *   (word & 0xe0000003) == 0x20000000   old-style jump, 29-bit target
 *   (word & 0xe0030003) == 0x00000000   increasing methods
 *   (word & 0xe0030003) == 0x40000000   non-increasing methods
 * with count in bits 28:18, subchannel in 15:13 and method in 12:2.
 *
 * DMA_PUT/DMA_GET are offsets within the channel's push buffer DMA object. On
 * Xbox that object is the whole of RAM with a zero base -- D3D allocates its
 * push buffer with MmAllocateContiguousMemory and hands the GPU the physical
 * address directly -- so the offsets are usable as physical addresses without
 * loading the DMA context out of RAMIN. A title that reprograms the context
 * would need nv_dma_load() here; none is known to.
 */
/* Bound the amount of synchronous GPU work performed by one guest MMIO
 * access. The guest commonly polls DMA_GET while the pusher catches up, so
 * each read is another safe opportunity to advance the same decoder state. */
#define PFIFO_METHOD_BUDGET 65536u
#define PFIFO_WORD_BUDGET   (PFIFO_METHOD_BUDGET * 2u + 64u)

static bool pfifo_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("XBOXRECOMP_PFIFO_TRACE") != NULL;
    return enabled != 0;
}

static bool pfifo_deep_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("XBOXRECOMP_PFIFO_TRACE_DEEP") != NULL;
    return enabled != 0;
}

static void pfifo_pull(NV2AState *d, uint32_t channel, const char *source)
{
    static uint32_t trace_lines;
    PFIFOChannelState *s = &d->user.decoder[channel];
    uint32_t get = pfifo_load(&d->user.dma_get[channel]);
    uint32_t put = pfifo_load(&d->user.dma_put[channel]);
    uint32_t start_get = get;
    uint32_t methods = 0;
    uint32_t words_this_call = 0;

    /* The first submission establishes where the stream starts.
     *
     * Hardware loads CACHE1_DMA_GET from the channel's RAMFC context when the
     * channel is scheduled, and the Xbox driver leaves it to that: it writes
     * CACHE1_DMA_GET (0x1244) = 0 during setup, programs DMA_INSTANCE and
     * enables DMA_PUSH, and then simply writes DMA_PUT. Its first PUT is the
     * push buffer base -- nothing has been appended yet -- so taking it as the
     * new GET costs no commands. Walking from 0 instead reads whatever is at
     * guest physical 0 (the KPCR) and decodes noise; that is exactly what the
     * "unrecognised command 0x00000007 at 0x00000004" abort was. */
    if (get == 0) {
        pfifo_store(&d->user.dma_get[channel], put);
        if (pfifo_trace_enabled() && trace_lines++ < 64)
            fprintf(stderr, "[PFIFO TRACE] %s init ch=%u put=%08X\n",
                    source, channel, put);
        return;
    }

    while (methods < PFIFO_METHOD_BUDGET &&
           words_this_call < PFIFO_WORD_BUDGET) {
        /* The producer may wrap or advance PUT while the async worker is
         * decoding. Hardware observes the live pointer; retaining the PUT
         * from entry can make GET chase a target that is no longer current. */
        put = pfifo_load(&d->user.dma_put[channel]);
        if (get == put)
            break;
        if (s->method_data_pending) {
            uint32_t param = pb_read32(get);
            get += 4;
            pgraph_method(d, s->subchannel,
                          s->increasing ? s->method + s->method_index * 4
                                        : s->method,
                          param);
            s->method_index++;
            s->method_count--;
            methods++;
            pfifo_store(&d->user.dma_get[channel], get);
            if (s->method_count == 0)
                s->method_data_pending = false;
            continue;
        }

        /* A malformed or mis-decoded stream can loop forever on a jump back to
         * itself. Preserve the guard across budget yields rather than resetting
         * it on every DMA_GET poll. */
        if (++s->words > (1u << 20)) {
            static int warned;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[NV2A] puller: runaway at get=0x%08X put=0x%08X, "
                        "abandoning frame\n", get, put);
                fflush(stderr);
            }
            get = put;
            s->method_data_pending = false;
            s->words = 0;
            break;
        }

        uint32_t word = pb_read32(get);
        get += 4;
        words_this_call++;

        if ((word & 0xe0000003) == 0x20000000) {          /* old jump */
            s->jmp_shadow = get;
            get = word & 0x1ffffffc;
        } else if ((word & 3) == 1) {                     /* jump */
            s->jmp_shadow = get;
            get = word & 0xfffffffc;
        } else if ((word & 3) == 2) {                     /* call */
            s->jmp_shadow = get;
            get = word & 0xfffffffc;
        } else if (word == 0x00020000) {                  /* return */
            get = s->jmp_shadow;
        } else if ((word & 0xe0030003) == 0x00000000 ||
                   (word & 0xe0030003) == 0x40000000) {
            s->increasing = (word & 0x40000000) == 0;
            s->method_count = (word >> 18) & 0x7ff;
            s->method_index = 0;
            s->method = word & 0x1ffc;
            s->subchannel = (word >> 13) & 7;
            s->method_data_pending = s->method_count != 0;
        } else {
            static int warned;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "[NV2A] puller: unrecognised command 0x%08X at "
                        "0x%08X ch=%u source=%s put=0x%08X jump=0x%08X "
                        "pending=%u method=0x%04X index=%u remaining=%u "
                        "subchannel=%u words=%u, abandoning frame\n",
                        word, get - 4, channel, source, put, s->jmp_shadow,
                        s->method_data_pending, s->method, s->method_index,
                        s->method_count, s->subchannel, s->words);
                fflush(stderr);
            }
            get = put;
            s->method_data_pending = false;
            s->words = 0;
            break;
        }

        pfifo_store(&d->user.dma_get[channel], get);
    }

    put = pfifo_load(&d->user.dma_put[channel]);
    pfifo_store(&d->user.dma_get[channel], get);
    if (get == put && !s->method_data_pending) {
        if (g_pfifo_async_enabled)
            memset(s, 0, sizeof(*s));
        else
            s->words = 0;
    }
    if (get != put && (methods >= PFIFO_METHOD_BUDGET ||
                       words_this_call >= PFIFO_WORD_BUDGET) &&
        pfifo_deep_trace_enabled()) {
        static uint32_t detail_lines;
        if (detail_lines < 8192) {
            detail_lines++;
            fprintf(stderr, "[PFIFO DETAIL] YIELD %s ch=%u start=%08X "
                    "end=%08X put=%08X methods=%u words=%u jump=%08X "
                    "pending=%u method=%04X index=%u remaining=%u\n",
                    source, channel, start_get, get, put, methods,
                    words_this_call, s->jmp_shadow, s->method_data_pending,
                    s->method, s->method_index, s->method_count);
            fflush(stderr);
        }
    }
    if (pfifo_trace_enabled() && trace_lines < 64 &&
        (trace_lines < 24 || get != put)) {
        trace_lines++;
        fprintf(stderr, "[PFIFO TRACE] %s ch=%u start=%08X put=%08X "
                "end=%08X methods=%u pending=%u remaining=%u\n",
                source, channel, start_get, put, get, methods,
                s->method_data_pending, s->method_count);
    }
}

/* Decode on a dedicated thread. PUT only publishes a pointer and signals the
 * event; GET polling observes published progress without taking the decoder
 * lock. The event remembers a kick that arrives while the worker is busy. */
static DWORD WINAPI pfifo_async_worker(LPVOID opaque)
{
    NV2AState *d = (NV2AState *)opaque;
    while (!d->exiting) {
        InterlockedExchange(&g_pfifo_async_stage, 0);
        DWORD result = WaitForSingleObject(d->pfifo.work_event, INFINITE);
        if (result != WAIT_OBJECT_0 || d->exiting) break;
        InterlockedIncrement(&g_pfifo_async_wakes);
        InterlockedExchange(&g_pfifo_async_stage, 1);

        for (;;) {
            bool did_work = false;
            qemu_mutex_lock(&d->pfifo.lock);
            for (uint32_t channel = 0; channel < NV2A_USER_NUM_CHANNELS;
                 channel++) {
                uint32_t get = pfifo_load(&d->user.dma_get[channel]);
                uint32_t put = pfifo_load(&d->user.dma_put[channel]);
                if (get == put) continue;
                did_work = true;
                InterlockedExchange(&g_pfifo_async_channel, (LONG)channel);
                InterlockedExchange(&g_pfifo_async_stage, 2);
                if (g_guest_ram) {
                    ULONGLONG decode_start = GetTickCount64();
                    pfifo_pull(d, channel, "WORKER");
                    __atomic_fetch_add(&g_pfifo_async_decode_ms,
                                       GetTickCount64() - decode_start,
                                       __ATOMIC_RELAXED);
                } else {
                    pfifo_store(&d->user.dma_get[channel], put);
                    memset(&d->user.decoder[channel], 0,
                           sizeof(d->user.decoder[channel]));
                }
                InterlockedIncrement(&g_pfifo_async_batches);
                InterlockedExchange(&g_pfifo_async_stage, 3);
            }
            qemu_mutex_unlock(&d->pfifo.lock);
            InterlockedExchange(&g_pfifo_async_stage, 4);
            if (!did_work) {
                InterlockedExchange(&g_pfifo_async_acked_puts,
                    InterlockedCompareExchange(&g_pfifo_async_puts, 0, 0));
                break;
            }
            Sleep(0);
        }
    }
    return 0;
}

static DWORD WINAPI pfifo_async_watchdog(LPVOID opaque)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t previous_wait_ms = 0;
    uint64_t previous_decode_ms = 0;
    while (!d->exiting) {
        Sleep(5000);
        uint64_t wait_ms = __atomic_load_n(&g_pfifo_async_wait_ms,
                                           __ATOMIC_RELAXED);
        uint64_t decode_ms = __atomic_load_n(&g_pfifo_async_decode_ms,
                                             __ATOMIC_RELAXED);
        fprintf(stderr, "[PFIFO ASYNC] puts=%ld acked=%ld throttles=%ld "
                "wakes=%ld batches=%ld "
                "wait_ms=%llu decode_ms=%llu max_wait_ms=%llu "
                "stage=%ld ch=%ld GET=%08X PUT=%08X\n",
                (long)InterlockedCompareExchange(&g_pfifo_async_puts, 0, 0),
                (long)InterlockedCompareExchange(&g_pfifo_async_acked_puts, 0, 0),
                (long)InterlockedCompareExchange(&g_pfifo_async_throttles, 0, 0),
                (long)InterlockedCompareExchange(&g_pfifo_async_wakes, 0, 0),
                (long)InterlockedCompareExchange(&g_pfifo_async_batches, 0, 0),
                (unsigned long long)(wait_ms - previous_wait_ms),
                (unsigned long long)(decode_ms - previous_decode_ms),
                (unsigned long long)__atomic_load_n(
                    &g_pfifo_async_wait_max_ms, __ATOMIC_RELAXED),
                (long)InterlockedCompareExchange(&g_pfifo_async_stage, 0, 0),
                (long)InterlockedCompareExchange(&g_pfifo_async_channel, 0, 0),
                pfifo_load(&d->user.dma_get[0]),
                pfifo_load(&d->user.dma_put[0]));
        fflush(stderr);
        previous_wait_ms = wait_ms;
        previous_decode_ms = decode_ms;
    }
    return 0;
}

uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t channel = (uint32_t)(addr / NV2A_USER_CHANNEL_SIZE);
    uint32_t reg = (uint32_t)(addr % NV2A_USER_CHANNEL_SIZE);
    uint64_t r = 0;

    if (channel < NV2A_USER_NUM_CHANNELS) {
        switch (reg) {
        case NV_USER_DMA_PUT:
            if (g_pfifo_async_enabled) {
                r = pfifo_load(&d->user.dma_put[channel]);
                break;
            }
            qemu_mutex_lock(&d->pfifo.lock);
            r = d->user.dma_put[channel];
            qemu_mutex_unlock(&d->pfifo.lock);
            break;
        case NV_USER_DMA_GET:
            if (g_pfifo_async_enabled) {
                r = pfifo_load(&d->user.dma_get[channel]);
                break;
            }
            qemu_mutex_lock(&d->pfifo.lock);
            if (g_guest_ram &&
                d->user.dma_get[channel] != d->user.dma_put[channel])
                pfifo_pull(d, channel, "GET");
            r = d->user.dma_get[channel];
            qemu_mutex_unlock(&d->pfifo.lock);
            break;
        case NV_USER_REF:     r = d->user.ref[channel];     break;
        default: break;
        }
    }

    nv2a_reg_log_read(NV_USER, addr, size, r);
    return r;
}

void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t channel = (uint32_t)(addr / NV2A_USER_CHANNEL_SIZE);
    uint32_t reg = (uint32_t)(addr % NV2A_USER_CHANNEL_SIZE);

    nv2a_reg_log_write(NV_USER, addr, size, val);

    if (channel >= NV2A_USER_NUM_CHANNELS)
        return;

    switch (reg) {
    case NV_USER_DMA_PUT:
        if (g_pfifo_async_enabled) {
            uint32_t new_put = (uint32_t)val;
            uint32_t old_put = pfifo_load(&d->user.dma_put[channel]);
            pfifo_store(&d->user.dma_put[channel], new_put);
            LONG put_seq = InterlockedIncrement(&g_pfifo_async_puts);
            if (pfifo_deep_trace_enabled() && new_put < old_put) {
                static LONG wraps_logged;
                if (InterlockedIncrement(&wraps_logged) <= 8192)
                    fprintf(stderr, "[PFIFO ASYNC] PUT wrap ch=%u get=%08X "
                            "old=%08X new=%08X\n", channel,
                            pfifo_load(&d->user.dma_get[channel]), old_put,
                            new_put);
            }
            SetEvent(d->pfifo.work_event);
            if (put_seq - InterlockedCompareExchange(
                    &g_pfifo_async_acked_puts, 0, 0) >=
                    g_pfifo_async_max_pending_puts) {
                InterlockedIncrement(&g_pfifo_async_throttles);
                ULONGLONG wait_start = GetTickCount64();
                while (!d->exiting &&
                       put_seq - InterlockedCompareExchange(
                           &g_pfifo_async_acked_puts, 0, 0) >=
                           g_pfifo_async_max_pending_puts)
                    Sleep(1);
                uint64_t elapsed = GetTickCount64() - wait_start;
                __atomic_fetch_add(&g_pfifo_async_wait_ms, elapsed,
                                   __ATOMIC_RELAXED);
                uint64_t old_max = __atomic_load_n(&g_pfifo_async_wait_max_ms,
                                                    __ATOMIC_RELAXED);
                while (elapsed > old_max &&
                       !__atomic_compare_exchange_n(
                           &g_pfifo_async_wait_max_ms, &old_max, elapsed,
                           false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            }
            break;
        }
        qemu_mutex_lock(&d->pfifo.lock);
        if (pfifo_deep_trace_enabled()) {
            static uint32_t detail_lines;
            PFIFOChannelState *s = &d->user.decoder[channel];
            uint32_t old_get = d->user.dma_get[channel];
            uint32_t old_put = d->user.dma_put[channel];
            uint32_t new_put = (uint32_t)val;
            if (detail_lines < 8192 &&
                (new_put < old_put || old_get > new_put ||
                 s->method_data_pending)) {
                detail_lines++;
                fprintf(stderr, "[PFIFO DETAIL] PUT ch=%u get=%08X "
                        "old_put=%08X new_put=%08X jump=%08X pending=%u "
                        "method=%04X index=%u remaining=%u words=%u\n",
                        channel, old_get, old_put, new_put, s->jmp_shadow,
                        s->method_data_pending, s->method, s->method_index,
                        s->method_count, s->words);
                fflush(stderr);
            }
        }
        /* Match the old per-submission local jmp_shadow lifetime while
         * retaining it across any budget yields within this submission. */
        if (d->user.dma_get[channel] == d->user.dma_put[channel] &&
            !d->user.decoder[channel].method_data_pending)
            memset(&d->user.decoder[channel], 0,
                   sizeof(d->user.decoder[channel]));
        pfifo_store(&d->user.dma_put[channel], (uint32_t)val);
        if (g_guest_ram)
            pfifo_pull(d, channel, "PUT");
        else {
            pfifo_store(&d->user.dma_get[channel], (uint32_t)val); /* ack only */
            memset(&d->user.decoder[channel], 0,
                   sizeof(d->user.decoder[channel]));
        }
        qemu_mutex_unlock(&d->pfifo.lock);
        break;
    case NV_USER_DMA_GET:
        qemu_mutex_lock(&d->pfifo.lock);
        if (pfifo_deep_trace_enabled()) {
            static uint32_t detail_lines;
            PFIFOChannelState *s = &d->user.decoder[channel];
            if (detail_lines < 8192) {
                detail_lines++;
                fprintf(stderr, "[PFIFO DETAIL] GET RESET ch=%u old=%08X "
                        "new=%08X put=%08X jump=%08X pending=%u method=%04X "
                        "index=%u remaining=%u words=%u\n",
                        channel, d->user.dma_get[channel], (uint32_t)val,
                        d->user.dma_put[channel], s->jmp_shadow,
                        s->method_data_pending, s->method, s->method_index,
                        s->method_count, s->words);
                fflush(stderr);
            }
        }
        if (pfifo_trace_enabled()) {
            static uint32_t reset_traces;
            PFIFOChannelState *s = &d->user.decoder[channel];
            if (reset_traces < 128 &&
                (s->jmp_shadow || s->method_data_pending || s->words)) {
                reset_traces++;
                fprintf(stderr, "[PFIFO TRACE] GET write ch=%u old=%08X "
                        "new=%08X put=%08X jump=%08X pending=%u "
                        "method=%04X index=%u remaining=%u words=%u\n",
                        channel, d->user.dma_get[channel], (uint32_t)val,
                        d->user.dma_put[channel], s->jmp_shadow,
                        s->method_data_pending, s->method, s->method_index,
                        s->method_count, s->words);
            }
        }
        pfifo_store(&d->user.dma_get[channel], (uint32_t)val);
        memset(&d->user.decoder[channel], 0,
               sizeof(d->user.decoder[channel]));
        qemu_mutex_unlock(&d->pfifo.lock);
        if (g_pfifo_async_enabled)
            SetEvent(d->pfifo.work_event);
        break;
    case NV_USER_REF:
        d->user.ref[channel] = (uint32_t)val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * Block dispatch table (from xemu nv2a.c)
 * ============================================================ */

#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                      \
    .offset = OFFSET,                                     \
    .size   = SIZE,                                       \
    .ops    = { .read = LNAME##_read, .write = LNAME##_write }, \
}
#define STUB_ENTRY(NAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                    \
    .offset = OFFSET,                                   \
    .size   = SIZE,                                     \
    .ops    = { .read = nv2a_stub_read, .write = nv2a_stub_write }, \
}

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    STUB_ENTRY(PFIFO_CACHE,   0x003000, 0x001000),
    STUB_ENTRY(PRMA,          0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    STUB_ENTRY(PCOUNTER,      0x00a000, 0x001000),
    STUB_ENTRY(PVPE,          0x00b000, 0x001000),
    STUB_ENTRY(PTV,           0x00d000, 0x001000),
    STUB_ENTRY(PRMFB,         0x0a0000, 0x020000),
    STUB_ENTRY(PRMVIO,        0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    STUB_ENTRY(PSTRAPS,       0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    STUB_ENTRY(PRMCIO,        0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    STUB_ENTRY(PRMDIO,        0x681000, 0x001000),
    /* NV_PRAMIN = 19 */
    { .name = NULL },
    /* NV_USER = 20 */
    ENTRY(USER,     user,     0x800000, 0x800000),
};

#undef ENTRY
#undef STUB_ENTRY

/* ============================================================
 * MMIO dispatch (for VEH handler integration)
 * ============================================================ */

/* PRAMIN is plain memory, not a register block.
 *
 * xemu maps it the same way -- memory_region_add_subregion(&d->mmio, 0x700000,
 * &d->ramin) -- which is why its blocktable entry is commented out there and
 * NULL here. Without this the window decodes to no block at all, so the DMA
 * objects the driver writes into RAMIN never reach d->ramin_ptr and every
 * nv_dma_load() reads back zeros. That is what dropped Burnout's semaphore
 * releases: "dma 0x00000008 offset 0x0 maps short (len 0)".
 */
static inline bool pramin_range(hwaddr addr, unsigned int size, hwaddr *off)
{
    if (addr < NV_PRAMIN_OFFSET || addr >= NV_PRAMIN_OFFSET + NV_PRAMIN_SIZE)
        return false;
    *off = addr - NV_PRAMIN_OFFSET;
    return *off + size <= NV_PRAMIN_SIZE;
}

uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size)
{
    hwaddr ramin_off;
    if (d->ramin_ptr && pramin_range(addr, size, &ramin_off)) {
        const uint8_t *p = d->ramin_ptr + ramin_off;
        switch (size) {
        case 1: return p[0];
        case 2: return (uint64_t)p[0] | ((uint64_t)p[1] << 8);
        case 4: return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                       ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24);
        default: return 0;
        }
    }

    /* Find which block handles this address */
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            return blocktable[i].ops.read(d, block_addr, size);
        }
    }
    NV2A_DPRINTF("MMIO read unmapped: addr=0x%llx\n", (unsigned long long)addr);
    return 0;
}

void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size)
{
    hwaddr ramin_off;
    if (d->ramin_ptr && pramin_range(addr, size, &ramin_off)) {
        uint8_t *p = d->ramin_ptr + ramin_off;
        switch (size) {
        case 4: p[3] = (uint8_t)(val >> 24); p[2] = (uint8_t)(val >> 16);
                /* fall through */
        case 2: p[1] = (uint8_t)(val >> 8);
                /* fall through */
        case 1: p[0] = (uint8_t)val;
                break;
        default: break;
        }
        return;
    }

    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            blocktable[i].ops.write(d, block_addr, val, size);
            return;
        }
    }
    NV2A_DPRINTF("MMIO write unmapped: addr=0x%llx val=0x%llx\n",
                 (unsigned long long)addr, (unsigned long long)val);
}

/* ============================================================
 * Vertical blank
 *
 * On real hardware the CRTC raises this ~60 times a second on its own. Here
 * nothing did, and that left the two halves of interrupt handling telling a
 * title different stories: the kernel's delivery thread invoked a registered
 * ISR as though hardware had fired, while these registers still said nothing
 * was pending. A correctly written ISR checks the hardware and returns "not
 * mine" -- Breakdown's does exactly that (sub_001C6890 tests PMC_INTR_EN_0
 * then PMC_INTR_0 & PCRTC), so its vblank -> KeInsertQueueDpc -> per-frame
 * work chain could never fire no matter how faithfully the ISR was called.
 *
 * Raising the pending bits here is the missing half. The ISR acknowledges by
 * writing INTR_0 (write-1-to-clear), which is why nothing needs to clear them
 * on this side. Whoever calls this owns the rate; the caller is simulating
 * the CRTC.
 * ============================================================ */

void nv2a_raise_vblank(void)
{
    NV2AState *d = g_nv2a;
    if (!d) return;

    /* Only the PCRTC-level bit is ours to set. nv2a_update_irq derives
     * PMC_INTR_0's PCRTC bit from (pcrtc.pending & pcrtc.enabled), so setting
     * it here as well would both duplicate that and bypass the enable gate --
     * a title that has not enabled vblank must not see one pending. */
    d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
    nv2a_update_irq(d);
}

/* ============================================================
 * Standalone initialization
 * ============================================================ */

NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size)
{
    if (g_nv2a) return g_nv2a;

    NV2AState *d = (NV2AState *)calloc(1, sizeof(NV2AState));
    if (!d) return NULL;

    /* Set up VRAM */
    g_vram_region.size = vram_size;
    d->vram = &g_vram_region;
    d->vram_ptr = vram_ptr;
    d->vram_pci.size = vram_size;

    /* Set up RAMIN */
    g_ramin_region.size = ramin_size;
    d->ramin.size = ramin_size;
    d->ramin_ptr = ramin_ptr;

    /* PCI config space: NV2A vendor/device */
    pci_set_long(d->parent_obj.config + PCI_VENDOR_ID, 0x02A010DE); /* NVIDIA NV2A */
    pci_set_long(d->parent_obj.config + PCI_CLASS_REVISION, 0x030000A1);

    /* Default PLL: 233 MHz core clock (Xbox default) */
    d->pramdac.core_clock_coeff = 0x00011C01; /* n=0x1C, m=1, p=0 */
    d->pramdac.core_clock_freq = NV2A_CRYSTAL_FREQ * 0x1C; /* ~233 MHz */

    /* Default timer divisors */
    d->ptimer.numerator = 1;
    d->ptimer.denominator = 1;

    /* Initialize PFIFO mutex */
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    const char *async_env = getenv("XBOXRECOMP_PFIFO_ASYNC");
    if (async_env && strcmp(async_env, "1") == 0) {
        const char *limit_env = getenv("XBOXRECOMP_PFIFO_MAX_PENDING_PUTS");
        if (limit_env && *limit_env) {
            char *end;
            long limit = strtol(limit_env, &end, 10);
            if (*end == '\0' && limit >= 1 && limit <= 1024)
                g_pfifo_async_max_pending_puts = (LONG)limit;
        }
        d->pfifo.work_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (d->pfifo.work_event) {
            g_pfifo_async_enabled = true;
            d->pfifo.thread.thread = CreateThread(NULL, 0,
                                                  pfifo_async_worker, d,
                                                  0, NULL);
            if (!d->pfifo.thread.thread) {
                g_pfifo_async_enabled = false;
                CloseHandle(d->pfifo.work_event);
                d->pfifo.work_event = NULL;
            } else {
                HANDLE watchdog = CreateThread(NULL, 0,
                                               pfifo_async_watchdog, d,
                                               0, NULL);
                if (watchdog) CloseHandle(watchdog);
            }
        }
        fprintf(stderr, "[PFIFO ASYNC] worker %s (max pending PUTs %ld)\n",
                g_pfifo_async_enabled ? "started" : "failed; using inline pull",
                (long)g_pfifo_async_max_pending_puts);
    }

    g_nv2a = d;

    fprintf(stderr, "[NV2A] Standalone GPU initialized: VRAM=%uMB RAMIN=%uKB\n",
            vram_size / (1024*1024), ramin_size / 1024);

    return d;
}
