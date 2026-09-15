/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * Game code and data are linked to specific address ranges which vary
 * per game. Section addresses are parsed dynamically from the XBE header
 * at runtime, so this module works with ANY Xbox game.
 *
 * On Windows, we:
 * 1. Create a 64MB file mapping (CreateFileMapping)
 * 2. Map the base view + 28 mirror views at 64MB intervals
 * 3. Parse the XBE section table and copy sections to their Xbox VAs
 * 4. Set up simulated stack, heap, TIB, and kernel data area
 *
 * The mirror views ensure Xbox RAM wrapping works correctly: the Xbox
 * memory controller uses a 26-bit address bus, so ALL addresses wrap
 * modulo 64MB. File mapping views backed by the same section give us
 * true aliases where writes at one address are visible at all mirrors.
 */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory. 64 MB is the retail default; debug/beta builds ship for
 * 128 MB devkits and allocate accordingly (Halo's cachebeta pre-allocates ~57 MB
 * plus its debug arrays, which only fits on a devkit). Runtime-overridable via
 * xbox_SetTotalRam() before xbox_MemoryLayoutInit(); see g_xbox_total_ram. */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB (default) */
#define XBOX_DEVKIT_RAM         (128 * 1024 * 1024) /* 128 MB (debug kit) */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Actual mapped RAM for this run. Defaults to XBOX_TOTAL_RAM; a title with a
 * devkit build calls xbox_SetTotalRam(XBOX_DEVKIT_RAM) before init. Heap top and
 * mirror stride derive from this, not from the compile-time constant. */
extern size_t g_xbox_total_ram;
void xbox_SetTotalRam(size_t bytes);

/* NOTE: Section addresses (.text, .rdata, .data, etc.) are NOT hardcoded.
 * They are parsed from the XBE header at runtime in xbox_MemoryLayoutInit().
 * This allows the toolkit to work with ANY Xbox game without modification. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/**
 * Switch the NV2A register aperture from plain RAM to a trapping region, so
 * accesses fault into a VEH that can route them to real GPU emulation.
 *
 * By default the aperture is committed RAM plus a background thread that
 * hand-acknowledges a short list of busy/idle bits (see NV2A_ACK / NV2A_IDLE
 * in the .c). That models a title whose drawing is done by an HLE D3D8 layer.
 * A title running its own D3D8 code against real register emulation needs the
 * opposite: every access visible. Calling this stops the ack thread -- its
 * direct writes would otherwise fault into the same handler and fight the
 * register model -- and makes the aperture PAGE_NOACCESS.
 *
 * Calling it IS the opt-in; nothing else changes behaviour. Pair it with
 * nv2a_hook_init() and route EXCEPTION_ACCESS_VIOLATION in the aperture to
 * nv2a_hook_handle_mmio(), or the title will simply crash on its first GPU
 * access.
 *
 * @return TRUE if the aperture now traps.
 */
BOOL xbox_Nv2aEnableTrapping(void);

/**
 * Switch the APU register window (the first 512 KB of the MCPX aperture at
 * XBOX_MCPX_BASE) from plain RAM to a trapping region, so that reads and
 * writes route through apu_hook_handle_mmio() into the xemu-derived MCPX APU
 * model in src/apu/. The rest of the MCPX aperture -- AC97, USB, NIC -- is
 * untouched and stays plain RAM.
 *
 * Stops the NV2A ack thread if it is still running: it pokes MCPX_COUNTERS
 * offset 0x020010 directly, which is inside the window about to trap.
 *
 * Call before mcpx_apu_init_standalone(), so the VEH can service the very
 * first access, matching the order used for NV2A.
 *
 * @return TRUE if the window now traps.
 */
BOOL xbox_ApuEnableTrapping(void);

/**
 * Release the reserved Xbox memory layout.
 */
void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);
void xbox_ProtectMirrorsForDebug(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
#define XBOX_KERNEL_DATA_BASE   0x00740000
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
/* Object-type exports a title may compare against each other, so each needs a
 * distinct non-zero value rather than a shared placeholder. */
#define KDATA_MUTANT_OBJ_TYPE   0x090  /* ExMutantObjectType (4 bytes) */
#define KDATA_SEMAPHORE_OBJ_TYPE 0x0A0 /* ExSemaphoreObjectType (4 bytes) */
#define KDATA_TIMER_OBJ_TYPE    0x0B0  /* ExTimerObjectType (4 bytes) */
#define KDATA_FILE_OBJ_TYPE     0x0C0  /* IoFileObjectType (4 bytes) */
#define KDATA_TIME_INCREMENT    0x0D0  /* KeTimeIncrement (4 bytes) */
#define KDATA_BOOT_SMC_VIDEO    0x0E0  /* HalBootSMCVideoMode (4 bytes) */
#define KDATA_IDEX_CHANNEL      0x0F0  /* IdexChannelObject (opaque) */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */
/* HAL disk identity strings (ordinals 41/42). Each is an XBOX_ANSI_STRING
 * (Length, MaximumLength, Buffer VA) followed by the string bytes it points to,
 * because HalRandGather dereferences Buffer to read the text as entropy. */
#define KDATA_DISK_MODEL_STR    0x420  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_MODEL_BUF    0x430  /* model text (up to 48 bytes) */
#define KDATA_DISK_SERIAL_STR   0x460  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_SERIAL_BUF   0x470  /* serial text (up to 32 bytes) */
#define KDATA_DISK_CACHE_PARTS  0x4A0  /* HalDiskCachePartitionCount (4 bytes) */

/* ================================================================
 * Guest-visible thread objects
 * ================================================================
 *
 * ObReferenceObjectByHandle hands the guest a POINTER to a kernel object, and
 * a title is entitled to read fields out of it. That pointer used to be the
 * handle value itself (kernel_ob.c) or a plain 0 (the ordinal-246 bridge), so
 * every field read came from an address that was never a thread object.
 *
 * Breakdown's GetExitCodeThread (sub_001A837D) reads exactly two:
 *
 *   +0x004  byte   DISPATCHER_HEADER.SignalState -- a thread object signals
 *                  when the thread terminates, which is what makes waiting on
 *                  a thread handle work at all. Read as `cmp byte [ecx+4], 0`.
 *   +0x120  dword  ExitStatus, read only once the above is set.
 *
 * With no object behind them the poll read arbitrary guest memory, never saw a
 * finish, and the caller busy-waited forever -- 1.2 billion kernel calls in a
 * 185 s run (HeroLab task f6bd2dbc).
 *
 * These objects live in their own reserved slice of guest RAM rather than on
 * the guest heap: they must outlive the thread (a title can poll a handle
 * after the worker is gone) and they must be at a stable address, and the heap
 * is a bump allocator with no free. The region sits just above the kernel data
 * exports, in the gap between the last XBE section (~0x00460D58) and the stack
 * base, which nothing else claims.
 *
 * ONLY the two fields above are populated. The rest is deliberately left zero:
 * the real ETHREAD is not a Windows ETHREAD and nothing here has been ground-
 * truthed against xemu, so anything else would be invented. If a title turns
 * out to read a third field, measure it -- do not extrapolate a layout.
 */
#define XBOX_THREAD_OBJ_BASE   (XBOX_KERNEL_DATA_BASE + XBOX_KERNEL_DATA_SIZE)
#define XBOX_THREAD_OBJ_STRIDE 0x200   /* > 0x124, and keeps objects page-tidy */
#define XBOX_THREAD_OBJ_COUNT  128     /* 64 KB total */
#define XBOX_THREAD_OBJ_END    (XBOX_THREAD_OBJ_BASE + \
                                XBOX_THREAD_OBJ_STRIDE * XBOX_THREAD_OBJ_COUNT)

/* Field offsets inside one object. Kept as names so the two places that write
 * them and the test that checks them cannot drift apart. */
#define XBOX_THREAD_OBJ_SIGNALSTATE 0x004
#define XBOX_THREAD_OBJ_EXITSTATUS  0x120

/** Size of the simulated Xbox stack (8 MB).
 *  Increased from 1 MB because failed RECOMP_ICALL indirect calls
 *  can leak stdcall args onto the stack each frame. An 8 MB stack
 *  provides enough headroom for extended gameplay sessions. */

/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

/* SSE register storage, shared with the generated code. Defined in both this
 * header and templates/runtime/recomp_types.h -- a translation unit can end up
 * including both, so the guard keeps that from being a redefinition. Keep the
 * two identical: the generated code and the runtime have to agree on the
 * layout, and nothing else checks. */
#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

#define XBOX_STACK_SIZE     (8 * 1024 * 1024)

/** Base VA of the stack area (above last XBE section). */
#define XBOX_STACK_BASE     0x00780000

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

/* Fake TIB backing store for fs:-prefixed accesses (FS8/16/32 in
 * templates/runtime/recomp_types.h -- keep RECOMP_FAKE_TIB_SIZE identical
 * there, same reasoning as the RECOMP_XMM_DEFINED guard above). Genuinely
 * per-thread (RECOMP_TLS), and physically separate from the guest's linear
 * address space so a title's own use of a low linear address (Breakdown
 * does a plain `mov [4], eax` unrelated to threading) can never collide
 * with what the fake TIB stores at the same-looking fs: offset. */
#ifndef RECOMP_FAKE_TIB_SIZE
#define RECOMP_FAKE_TIB_SIZE 256
#endif
extern RECOMP_TLS uint8_t g_fake_tib[RECOMP_FAKE_TIB_SIZE];

/**
 * Populate this thread's fake TIB. Call once per thread -- the main thread
 * from xbox_MemoryLayoutInit(), and every SPAWN-mode worker thread from
 * bridge_thread_main() (kernel_bridge.c) -- since g_fake_tib is
 * RECOMP_TLS and therefore starts zeroed on every new thread, not just the
 * first one.
 */
void xbox_init_fake_tib(void);

/* ================================================================
 * Worker stack slices (host-tick-driven titles)
 * ================================================================
 *
 * A second way to drive a recompiled title, ported from the Burnout 3 fork as
 * the runtimes reunite (see docs/technical/burnout3-reunification.md).
 *
 * The default model (Halo, Crimson Skies) runs the game's entry routine inline
 * and it drives its own main loop. Some titles instead return from their entry
 * after spawning an init thread, and expect the *host* to drive the per-frame
 * tick -- Burnout 3 is tick-driven, not main-loop-driven. To call recompiled
 * code from the host's own message-loop thread, that thread needs a guest stack
 * (its g_esp starts at 0), which is what a worker slice provides.
 *
 * These slices are the one allocator for every guest stack but the main
 * thread's: a spawned game thread takes two adjacent slices
 * (xbox_AllocThreadStack), an interrupt-delivery thread or a host tick thread
 * takes one. They used to share the low end of this region with a separate
 * bump allocator for game threads, on the assumption that a title never needs
 * both kinds -- but interrupt delivery takes slices in every title, and the two
 * handed out the same memory.
 *
 * 24 slices is 6 MB, leaving the main thread the top 2 MB of the region, far
 * above any Xbox title's own main-thread stack.
 */
#define XBOX_WORKER_STACK_SIZE   (256 * 1024)
#define XBOX_WORKER_STACK_BASE   XBOX_STACK_BASE             /* 0x00780000 */
#define XBOX_WORKER_STACK_COUNT  24                          /* 6 MB total */
#define XBOX_WORKER_STACK_END    (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * XBOX_WORKER_STACK_COUNT)

/** Top (initial esp) of worker stack slice n, 16-byte aligned, growing down. */
#define XBOX_WORKER_STACK_TOP(n) (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * ((n) + 1) - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)  /* 0x00F80000 */

/** Exclusive top of the dynamic heap: the end of RAM for this run. Runtime,
 *  not a macro, because RAM size is now configurable (retail 64 MB vs devkit
 *  128 MB). The total mapped region (data + stack + heap) equals RAM so the
 *  engine's memory probing stops at the correct boundary. */
#define XBOX_HEAP_TOP       ((uint32_t)g_xbox_total_ram)   /* RAM maps from VA 0 */

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/**
 * Number of 64 MB mirror views to pre-map beyond the base region.
 *
 * Was 28 (1.75 GB), sized for the RenderWare/init-code wraparound this
 * comment block already described. That left a gap: Breakdown's XMV movie
 * codec computes an output address by OR-ing a real (low, in-RAM) offset
 * with 0xF0000000 -- `ecx = ecx | 0xF0000000u;` in sub_001C6280, a bit
 * pattern for tiled/aliased surface access. Every such address the OR can
 * ever produce lands in [0xF0000000, 0xF4000000) (the OR only sets the top
 * nibble; the low bits still span the 64 MB it aliases into), which sits
 * completely outside the 28-mirror range and faulted with no backing page.
 *
 * The MMX lifter work that made this codec's writes real (previously a
 * no-op, so nothing ever dereferenced the address) is what exposed this --
 * see HeroLab, Xbox Recompiler project, 2026-09-06. The gap itself is
 * general: real Xbox RAM mirrors modulo 64 MB across the *entire* 32-bit
 * space, not just the first 1.75 GB, so any title using a similar
 * high-bit-set alias would hit the same wall.
 *
 * 60 covers guest VA up to 0xF4000000 -- past the 0xF0000000 tiled range
 * with room to spare -- while stopping well short of the NV2A aperture at
 * 0xFD000000 and the MCPX apertures above it, which are mapped separately
 * at fixed addresses and must not overlap a mirror view.
 *
 * The mirror that would land on 0xF0000000 is no longer mapped: that window
 * is the write-combined alias of the CONTIGUOUS window (XBOX_CONTIG_WC_BASE in
 * kernel.h). The OR above is D3D locking a tiled surface, and the physical
 * address it ORs belongs to a contiguous allocation -- so low RAM was the
 * wrong backing, and Breakdown's movie frames overwrote heap data through it.
 */
#define XBOX_NUM_MIRRORS    60

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Thread-safe: no (single-threaded recompiled code).
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/**
 * Free a block from the Xbox heap. Currently a no-op (bump allocator).
 */
void xbox_HeapFree(uint32_t xbox_va);

/**
 * Allocate from the contiguous ("physical") window at XBOX_CONTIG_BASE, which
 * is where MmAllocateContiguousMemory(Ex) belongs. Returns an Xbox VA in the
 * 0x8xxxxxxx range -- MEM32() works on it exactly as on a heap VA -- or 0 when
 * the window is full. Grows DOWNWARD from the top, as the hardware does, so
 * the bottom of the window stays free for pinned-physical requests and the
 * fake kernel page. Alignment is rounded up to a page.
 *
 * Serving these from the general heap instead is what exhausted Breakdown's
 * 48.5 MB of heap and produced the CRT out-of-memory cascade; see the long
 * comment at the definition. Thread-safe: no.
 */
uint32_t xbox_ContigAlloc(uint32_t size, uint32_t alignment);

/** Release a block obtained from xbox_ContigAlloc. Ignores unknown addresses. */
void xbox_ContigFree(uint32_t xbox_va);

/** True when an Xbox VA lies inside the contiguous window. Lets a free path
 *  route a pointer to the allocator that actually handed it out. */
int xbox_IsContigAddress(uint32_t xbox_va);

/**
 * Size of a live block that xbox_ContigAlloc or xbox_HeapAlloc returned at
 * exactly this address: page-rounded for contiguous blocks (what the console's
 * MmQueryAllocationSize reports), exact for heap blocks. 0 for any other
 * address. Thread-safe: no.
 */
uint32_t xbox_AllocationSize(uint32_t xbox_va);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Used by the VEH handler to map additional mirror views on demand.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);

#ifdef __cplusplus
}
#endif


/* Carve a simulated stack for a spawned thread. Returns the Xbox VA of the
 * stack top, or 0 when the pool is exhausted. */
uint32_t xbox_AllocThreadStack(void);
void     xbox_FreeThreadStack(uint32_t stack_top);  /* on thread exit */

/* Worker stack slices (see XBOX_WORKER_STACK_*): every guest stack except the
 * main thread's comes from this one table. Thread-safe. */
int  xbox_worker_stack_alloc(void);   /* slice index, or -1 if none free */
int  xbox_worker_stack_alloc_span(int count); /* first of count adjacent slices, or -1 */
void xbox_worker_stack_free(int slot);
void   xbox_set_game_thread(void *h);  /* HANDLE, recorded for the host watchdog */
void  *xbox_thread_debug_handle(void); /* the game thread, or NULL under inline model */

/* PsCreateSystemThreadEx behaviour. Default INLINE runs the first call as the
 * game (Halo, Crimson Skies). SPAWN makes every call a real thread so a
 * host-tick-driven title's entry can return and let the host drive -- call
 * xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN) before the game starts. */
#define XBOX_THREAD_MODE_INLINE 0
#define XBOX_THREAD_MODE_SPAWN  1
void xbox_SetThreadMode(int mode);

#endif /* XBOX_MEMORY_LAYOUT_H */
