/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>

/* Access to recompiled code registers. Per-thread: RECOMP_TLS comes from
 * xbox_memory_layout.h and must match the definitions there -- a plain extern
 * here binds to the TLS template rather than the calling thread's copy, which
 * reads as every register being zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    /* LaunchDataPage (ordinal 164) - NULL (no launch data) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - SMC video mode word from boot. 0 is
     * "no video mode reported", which titles treat as auto-detect. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    /* IdexChannelObject (ordinal 357) - IDE channel object. Opaque; only ever
     * passed back to Io* routines we stub, so a recognisable non-null is enough. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL) = XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
        };
        for (int k = 0; k < 2; k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
static int g_kernel_call_count = 0;

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* ── Guest-visible thread objects ─────────────────────────────────────────
 *
 * ObReferenceObjectByHandle gives the guest a pointer to a kernel object and
 * the guest reads fields out of it. There was no object: the ordinal-246
 * bridge wrote a plain 0, so Breakdown's GetExitCodeThread polled
 * `byte [0 + 4]` -- arbitrary guest memory the title itself writes to -- and
 * never saw a worker finish. See XBOX_THREAD_OBJ_* in xbox_memory_layout.h
 * for the layout, the two fields that are real, and why nothing else is.
 *
 * One slot per live thread. A slot is NOT recycled when the thread exits: a
 * title may poll a handle after its worker is gone, and a reused slot would
 * answer for the wrong thread. It is released by NtClose, which is the point
 * at which the guest gives up its reference to the handle.
 */
struct bridge_thread_obj {
    volatile LONG in_use;    /* slot claimed (interlocked) */
    HANDLE        handle;    /* native thread handle; NULL for an inline run */
    uint32_t      token;     /* 32-bit handle token the guest holds */
};
static struct bridge_thread_obj s_thread_objs[XBOX_THREAD_OBJ_COUNT];

/* Slot 0 is reserved: it is the object handed back for a thread handle this
 * bridge did not create, and it is never signalled. A title that polls such a
 * handle gets a stable STILL_ACTIVE rather than whatever byte happened to be
 * at the old bogus address -- which on this title was genuinely live memory,
 * since Breakdown does a plain `mov [4], eax` of its own. */
#define BRIDGE_THREAD_OBJ_UNKNOWN 0

static uint32_t bridge_thread_obj_va(int slot)
{
    return XBOX_THREAD_OBJ_BASE + (uint32_t)slot * XBOX_THREAD_OBJ_STRIDE;
}

/* Claim a slot for a thread and present it as running. Returns -1 when the
 * table is full; the caller carries on without an object, which is the old
 * behaviour (a poll that never finishes) rather than a failure. */
static int bridge_thread_obj_alloc(HANDLE h, uint32_t token)
{
    int i;

    for (i = BRIDGE_THREAD_OBJ_UNKNOWN + 1; i < XBOX_THREAD_OBJ_COUNT; i++) {
        if (InterlockedCompareExchange(&s_thread_objs[i].in_use, 1, 0) != 0)
            continue;
        s_thread_objs[i].handle = h;
        s_thread_objs[i].token  = token;
        memset(XBOX_TO_NATIVE(bridge_thread_obj_va(i)), 0,
               XBOX_THREAD_OBJ_STRIDE);
        return i;
    }
    fprintf(stderr, "  [KERNEL] thread-object table full (%d); GetExitCodeThread "
            "cannot report this thread finishing\n", XBOX_THREAD_OBJ_COUNT);
    fflush(stderr);
    return -1;
}

/* Signal the object: the thread is finished and this was its exit status.
 *
 * The status is written BEFORE the signal byte, because the guest reads them
 * in that order -- `cmp byte [ecx+4], 0` and only then `mov eax, [ecx+0x120]`
 * -- and the poller runs on a different thread from the one exiting. The
 * barrier between them is what keeps a poller that has just seen the signal
 * from reading a status that has not landed yet. */
static void bridge_thread_obj_signal(int slot, uint32_t exit_status)
{
    uint32_t va;

    if (slot < 0 || slot >= XBOX_THREAD_OBJ_COUNT)
        return;
    va = bridge_thread_obj_va(slot);
    BRIDGE_MEM32(va + XBOX_THREAD_OBJ_EXITSTATUS) = exit_status;
    MemoryBarrier();
    BRIDGE_MEM8(va + XBOX_THREAD_OBJ_SIGNALSTATE) = 1;
}

/* The object for a handle token the guest passed back, or 0 if that token was
 * never a thread this bridge created. */
static uint32_t bridge_thread_obj_for_token(uint32_t token)
{
    int i;

    if (!token)
        return 0;
    for (i = BRIDGE_THREAD_OBJ_UNKNOWN + 1; i < XBOX_THREAD_OBJ_COUNT; i++)
        if (s_thread_objs[i].in_use && s_thread_objs[i].token == token)
            return bridge_thread_obj_va(i);
    return 0;
}

/* Reverse of the above: the native thread handle behind an object pointer.
 *
 * Needed because the guest does not keep the handle once it has the object.
 * Breakdown's SetThreadPriority (sub_001A8277) hands the object straight to
 * KeSetBasePriorityThread -- its Object* out-parameter aliases the handle
 * argument slot (`lea eax, [ebp+8]`), so the handle is overwritten by the
 * object before that call is made. */
static HANDLE bridge_thread_handle_for_object(uint32_t object_va)
{
    uint32_t off;
    int slot;

    if (object_va < XBOX_THREAD_OBJ_BASE || object_va >= XBOX_THREAD_OBJ_END)
        return NULL;
    off = object_va - XBOX_THREAD_OBJ_BASE;
    if (off % XBOX_THREAD_OBJ_STRIDE)
        return NULL;
    slot = (int)(off / XBOX_THREAD_OBJ_STRIDE);
    return s_thread_objs[slot].in_use ? s_thread_objs[slot].handle : NULL;
}

/* Give a slot back. Only ever called once the guest has dropped the handle
 * (NtClose) or when the thread it was claimed for never started. */
static void bridge_thread_obj_release(int slot)
{
    if (slot <= BRIDGE_THREAD_OBJ_UNKNOWN || slot >= XBOX_THREAD_OBJ_COUNT)
        return;
    s_thread_objs[slot].handle = NULL;
    s_thread_objs[slot].token  = 0;
    InterlockedExchange(&s_thread_objs[slot].in_use, 0);
}

/* Release the slot behind a handle token (NtClose). */
static void bridge_thread_obj_release_token(uint32_t token)
{
    int i;

    if (!token)
        return;
    for (i = BRIDGE_THREAD_OBJ_UNKNOWN + 1; i < XBOX_THREAD_OBJ_COUNT; i++) {
        if (s_thread_objs[i].in_use && s_thread_objs[i].token == token) {
            bridge_thread_obj_release(i);
            return;
        }
    }
}

/* Clear the whole object region and reserve slot 0. Guest RAM is already
 * zeroed when it is mapped, but this region is read by the title as a kernel
 * structure, so it is spelled out here rather than assumed -- and it has to be
 * re-cleared if a run ever re-initialises the bridge. */
static void bridge_thread_objs_init(void)
{
    int i;

    memset(XBOX_TO_NATIVE(XBOX_THREAD_OBJ_BASE), 0,
           XBOX_THREAD_OBJ_END - XBOX_THREAD_OBJ_BASE);
    for (i = 0; i < XBOX_THREAD_OBJ_COUNT; i++) {
        s_thread_objs[i].handle = NULL;
        s_thread_objs[i].token  = 0;
        s_thread_objs[i].in_use = (i == BRIDGE_THREAD_OBJ_UNKNOWN);
    }
}

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's own object slot, so PsTerminateSystemThread -- which never
 * returns and so cannot report back to bridge_thread_main -- can signal it.
 * -1 on the main thread and on any thread with no object. */
static RECOMP_TLS int g_thread_obj_slot = -1;

/* Top of the stack slices a spawned thread runs on, 0 elsewhere. Given back
 * when the thread exits by either route. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

static void bridge_release_thread_stack(void)
{
    if (g_thread_stack_top) {
        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
    }
}

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top;
    int obj_slot;
};

static void bridge_write_handle(uint32_t handle_va, HANDLE h);

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    fn();
    g_esp += 12;
}

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    /* Own register set (RECOMP_TLS), own simulated stack, own fake TIB.
     * g_fake_tib is RECOMP_TLS too and starts zeroed on every new OS
     * thread, so each genuinely new thread needs this exactly once here --
     * NOT in bridge_run_thread_inline() below, which this function calls
     * into but which is also reused for the inline-fallback path on an
     * ALREADY-running, already-initialized thread when the stack-slot pool
     * is exhausted; re-running init there would reset that thread's live
     * fake-TIB state mid-execution. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    g_thread_obj_slot = s->obj_slot;
    xbox_init_fake_tib();
    free(s);

    bridge_run_thread_inline(fn, ctx1, ctx2);

    /* Returning off the end of the start routine is an exit too -- a worker
     * that finishes this way never reaches PsTerminateSystemThread, and a
     * title polling its handle would wait for a thread that is already gone. */
    bridge_thread_obj_signal(g_thread_obj_slot, g_eax);

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    bridge_release_thread_stack();
    return 0;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  int obj_slot)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;
    s->obj_slot = obj_slot;

    th = CreateThread(NULL, 0, bridge_thread_main, s, 0, NULL);
    if (!th) free(s);
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    else xbox_set_game_thread(th);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                uint32_t stack_top = xbox_AllocThreadStack();
                /* Claimed before the thread starts so the thread can carry its
                 * own slot; the handle and token are filled in below, once
                 * CreateThread has produced them. A thread that finishes in
                 * between signals its object by slot, which needs neither. */
                int obj_slot = bridge_thread_obj_alloc(NULL, 0);

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    {
                        /* The routine runs on the CALLING thread, so for the
                         * duration it is the thread this slot describes --
                         * otherwise a PsTerminateSystemThread inside it would
                         * signal (and report the exit status of) whichever
                         * thread happens to be hosting it. Saved and restored
                         * because an inline run can nest. */
                        int outer = g_thread_obj_slot;
                        g_thread_obj_slot = obj_slot;
                        bridge_run_thread_inline(fn, start_context1,
                                                 start_context2);
                        g_thread_obj_slot = outer;
                    }
                    /* It has already finished, so its object is signalled at
                     * once. It still needs a handle the guest can hand back to
                     * GetExitCodeThread: there is no native thread to name, so
                     * the token is synthetic and distinct from the 0xBEEF0001
                     * written above (which belongs to the main thread and is
                     * never polled). Without this, a title that polls a worker
                     * the stack pool could not accommodate waits forever for a
                     * routine that has in fact already run. */
                    if (obj_slot >= 0) {
                        uint32_t token = 0xBEEF0100u | (uint32_t)obj_slot;
                        s_thread_objs[obj_slot].token = token;
                        bridge_thread_obj_signal(obj_slot, g_eax);
                        if (xbox_handle_ptr)
                            BRIDGE_MEM32(xbox_handle_ptr) = token;
                    }
                } else {
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top,
                                                    obj_slot);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
                            start_routine, start_context1, stack_top);
                    fflush(stderr);
                    if (xbox_handle_ptr && th) {
                        bridge_write_handle(xbox_handle_ptr, th);
                        if (obj_slot >= 0) {
                            s_thread_objs[obj_slot].handle = th;
                            s_thread_objs[obj_slot].token =
                                BRIDGE_MEM32(xbox_handle_ptr);
                        }
                    } else if (!th && obj_slot >= 0) {
                        /* The thread never started, so nothing will ever name
                         * or signal this slot. (A thread that DID start keeps
                         * its slot even with no handle out-parameter -- it is
                         * still going to signal it by slot when it exits.) */
                        bridge_thread_obj_release(obj_slot);
                    }
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (getenv("XBOXRECOMP_SAVE_TRACE")) {
        fprintf(stderr, "  [SAVE] NtClose token=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    /* Giving up the handle is giving up the reference to the object behind it,
     * so this is where a thread object's slot goes back -- not when the thread
     * exits, because a title is entitled to read the exit status afterwards
     * (that is the whole point of GetExitCodeThread). No-op for every handle
     * that is not a thread's. */
    bridge_thread_obj_release_token(raw_handle);

    /* Close real handles but skip fake/synthetic ones */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* From the contiguous window, not the general heap: MEM32() works on both,
     * but the heap is the title's own arena space and these buffers are large.
     * See xbox_ContigAlloc. */
    uint32_t xbox_va = xbox_ContigAlloc(size, 4096);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. */
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        if (g_kernel_call_count <= 100) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    xbox_va = xbox_ContigAlloc(size, align);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);

    /* Route by address rather than assuming: the pinned-physical path above
     * hands back window addresses it never registered with either allocator,
     * and older builds of this bridge handed back heap addresses. Both must
     * survive being freed. */
    if (xbox_IsContigAddress(addr)) {
        xbox_ContigFree(addr);
    } else {
        xbox_HeapFree(addr);
    }
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (g_kernel_call_count <= 200) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT) */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    xbox_HalReturnToFirmware(routine);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ──────
 * Both __fastcall: the new IRQL arrives in cl, and the arg-size entries are 0
 * accordingly. Reading STACK_ARG(0) instead took whatever the caller last left
 * on its stack as the level. Breakdown's DSOUND brackets its allocator with
 * `mov cl, 2 / call KfRaiseIrql` ... `mov cl, [saved] / call KfLowerIrql`. */
static void bridge_KfRaiseIrql(void)
{
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)g_ecx);
}

static void bridge_KfLowerIrql(void)
{
    xbox_KfLowerIrql((UCHAR)g_ecx);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);

    g_eax = (uint32_t)xbox_KeSetEvent(XBOX_TO_NATIVE(event_ptr), increment, (BOOLEAN)wait);
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);

    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        XBOX_TO_NATIVE(object), wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

static HANDLE bridge_resolve_handle(uint32_t token);

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    if (handle) PulseEvent(handle);
    g_eax = 0;
}

/* ── NtCreateSemaphore (ordinal 193, 4 args) ─────────────── */
/*
 * Unbridged, this was worse than a no-op: the default path returns 0, and 0 as
 * an NTSTATUS is STATUS_SUCCESS. So the guest was told its semaphore had been
 * created successfully while the out-handle was never written, leaving it
 * holding a NULL handle it believed was valid. The visible symptom is a
 * permanent block one call later:
 *     NtWaitForSingleObjectEx: token=0x00000000 handle=0x0 timeout=INFINITE
 * Breakdown creates it at guest 0x001AAE38 and releases at 0x001AAE74, a
 * create/release pair in one routine. Same class the NtSetEvent comment above
 * records for Halo's map-copy worker; a semaphore is just a counting version
 * of the same handoff.
 *
 * xbox_NtCreateSemaphore() ignores ObjectAttributes (unnamed semaphores only),
 * which is why the pointer is still translated but nothing depends on its
 * contents. Local HANDLE then bridge_write_handle(), as NtCreateEvent does, so
 * an 8-byte host handle never gets written through a 4-byte guest slot.
 */
static void bridge_NtCreateSemaphore(void)
{
    uint32_t handle_ptr   = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    LONG     initial      = (LONG)STACK_ARG(2);
    LONG     maximum      = (LONG)STACK_ARG(3);

    HANDLE   local_handle = NULL;
    NTSTATUS status = xbox_NtCreateSemaphore(
        &local_handle, XBOX_TO_NATIVE(obj_attr_ptr), initial, maximum);

    if (status >= 0 && handle_ptr)
        bridge_write_handle(handle_ptr, local_handle);

    g_eax = (uint32_t)status;
}

/* ── NtReleaseSemaphore (ordinal 222, 3 args) ────────────── */
/*
 * The other half of the pair. Unbridged it silently dropped every release, so
 * even once creation works nothing would ever raise the count and a waiter
 * would still sleep forever.
 *
 * PreviousCount is an optional out-parameter; XBOX_TO_NATIVE maps a NULL guest
 * pointer to NULL, and xbox_NtReleaseSemaphore passes it straight to
 * ReleaseSemaphore, which accepts NULL. Guest LONG and host LONG are both 4
 * bytes, so this one needs no local-and-copy dance.
 */
static void bridge_NtReleaseSemaphore(void)
{
    HANDLE   handle   = bridge_resolve_handle(STACK_ARG(0));
    LONG     release  = (LONG)STACK_ARG(1);
    uint32_t prev_ptr = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtReleaseSemaphore(
        handle, release, (PLONG)XBOX_TO_NATIVE(prev_ptr));
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */
/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);
    PLARGE_INTEGER interval = (PLARGE_INTEGER)XBOX_TO_NATIVE(interval_ptr);

    /* DIAGNOSTIC (2026-09-06): the title sometimes passes an Interval that is
     * not a mapped guest address (~0xFFFD8F00 observed). xbox_nt_timeout_to_ms
     * guards NULL but not validity, so it dereferences it and takes the whole
     * process down -- about 2 runs in 5. Report the value and the guest caller
     * so the ORIGIN can be found; the stdcall arg size for this ordinal is
     * correct (12 bytes, 3 args), so this is a genuinely bad pointer rather
     * than the esp-walking class of bug that bit ordinal 47.
     *
     * Substituting a ZERO interval, not NULL: NULL means "wait forever" in
     * this API, so passing it would trade a crash for a parked thread. Zero
     * means "do not wait", which cannot hang the caller. This is a stopgap to
     * collect data, NOT a decision about what a bad Interval should mean. */
    if (interval_ptr && !xbox_IsXboxAddress(interval_ptr)) {
        static LARGE_INTEGER no_wait;   /* QuadPart == 0 -> 0 ms */
        static int warned;

        if (warned < 8) {
            warned++;
            fprintf(stderr, "  [KERNEL] KeDelayExecutionThread: Interval "
                    "0x%08X is not a mapped guest address -- skipping the wait "
                    "(guest caller 0x%08X, wait_mode=%u alertable=%u)\n",
                    interval_ptr, g_esp ? BRIDGE_MEM32(g_esp - 4) : 0,
                    wait_mode, alertable);
            fflush(stderr);
        }
        interval = &no_wait;
    }

    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable, interval);
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die. Unbridged this returned 0 and execution
 * carried on into whatever the bug check was there to prevent, so the real
 * failure surfaced later somewhere unrelated. Report the code and stop
 * pretending the call succeeded.
 *
 * These must NOT return. Real KeBugCheck halts the machine, so a compiler
 * treats everything after the call site as unreachable and freely places
 * inter-function `int3` padding there. Returning walks the guest straight
 * into that padding, which surfaces as a bare "wine: Unhandled illegal
 * instruction" in a function with no relationship to the actual fault --
 * exactly the false trail that cost HeroLab task 1c4b9a06 a session.
 */
static void bridge_bugcheck_halt(void)
{
    fprintf(stderr, "  [KERNEL] bug check raised at guest VA 0x%08X -- halting "
            "(real KeBugCheck never returns)\n",
            g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);

    /* Walk the GUEST stack for plausible code addresses.
     *
     * The immediate caller alone is not enough here: this title's bug check
     * comes from sub_001B0071, which is the CRT's doexit() -- so the title is
     * deliberately EXITING, and the question is always "what decided to?".
     * That answer is further up the chain. Filtering to the executable VA
     * range turns the raw stack into a usable approximation of it.
     *
     * Approximate on purpose: guest stack slots holding data that happens to
     * look like a code address will appear too. Read the list as candidates,
     * not as a call stack. */
    if (g_esp) {
        int printed = 0;
        uint32_t i;

        fprintf(stderr, "  Guest stack, code-shaped values (candidates, "
                        "innermost first):\n");
        for (i = 0; i < 128 && printed < 12; i++) {
            uint32_t v = BRIDGE_MEM32(g_esp + i * 4);
            if (v >= 0x00011000u && v < 0x00210000u) {
                fprintf(stderr, "    [%3u] 0x%08X\n", i, v);
                printed++;
            }
        }
        if (!printed)
            fprintf(stderr, "    (none)\n");
    }

    fflush(stderr);
    ExitProcess(1);
}

static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    bridge_bugcheck_halt();
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    bridge_bugcheck_halt();
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB.
     * Just return the Xbox VA as-is. Don't call xbox_MmGetPhysicalAddress
     * which would return a native pointer. */
    g_eax = addr;
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* Kept diagnostic (HeroLab task 28b5168a): per prior
     * measurement this is bridged and available but never actually called.
     * Unconditional log so if that ever changes -- with the AvPack ordinals
     * re-registered -- it is impossible to miss. */
    fprintf(stderr, "  [AVPACK] AvSetDisplayMode(step=%u mode=0x%X format=0x%X "
            "pitch=%u fb=0x%X) called from guest VA 0x%08X\n",
            step, mode, format, pitch, fb, g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);
    fflush(stderr);

    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    /* Publish the exit before the thread goes away. This is the ordinary way a
     * worker finishes, so it is the path that has to make GetExitCodeThread
     * stop polling; bridge_thread_main covers the routine that just returns
     * instead. Harmless on the main thread, which has no object slot. */
    bridge_thread_obj_signal(g_thread_obj_slot, exit_status);

    /*
     * This does not return on hardware. Returning was survivable while every
     * thread ran on the host's main thread, but a spawned worker that returns
     * here falls off the end of its start routine and into whatever bytes
     * follow -- Halo's input worker landed on an int 3, and the resulting
     * breakpoint took down the whole process while the main thread was still
     * inside input_initialize.
     *
     * The main thread still returns: it is the host's thread and unwinding
     * back to main() is how the process shuts down cleanly.
     */
    if (g_is_spawned_thread) {
        /* Nothing runs on the guest stack past this point. */
        bridge_release_thread_stack();
        ExitThread(exit_status);
    }
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── KeInsertQueueDpc (ordinal 119) ───────────────────────
 * BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2)
 *
 * Real hardware queues the DPC to run later at DISPATCH_LEVEL. This used to
 * be completely unbridged (silent "no bridge for ordinal 119" no-op) -- any
 * title whose ISR depends on its DPC actually running got nothing past the
 * ISR itself (see the interrupt-delivery thread above, and HeroLab task
 * 1c4b9a06 for how this was found: a title's ISR called this every simulated
 * vblank and the DeferredRoutine that was supposed to drive per-frame work
 * never ran).
 *
 * Runs the DeferredRoutine synchronously, immediately, on the calling
 * thread -- matches this bridge's existing preference for running a
 * callback rather than modeling real scheduling (see
 * bridge_PsCreateSystemThreadEx's own "must run synchronously" comment).
 * The calling thread already has a valid guest stack (the interrupt-delivery
 * thread above, or whatever thread is already running recompiled code if a
 * title queues its own DPC outside interrupt context), so this nests an
 * ordinary guest call rather than needing a stack of its own.
 */
static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc_va   = STACK_ARG(0);
    uint32_t sysarg1  = STACK_ARG(1);
    uint32_t sysarg2  = STACK_ARG(2);
    uint32_t routine  = BRIDGE_MEM32(dpc_va + 12);
    uint32_t context  = BRIDGE_MEM32(dpc_va + 16);
    recomp_func_t fn;

    fn = recomp_lookup_manual(routine);
    if (!fn) fn = recomp_lookup(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeInsertQueueDpc: DPC 0x%08X routine 0x%08X "
                "not resolvable\n", dpc_va, routine);
        g_eax = 0;
        return;
    }

    /* VOID DeferredRoutine(PKDPC Dpc, PVOID DeferredContext,
     *                       PVOID SystemArgument1, PVOID SystemArgument2);
     * stdcall, so the callee's own "ret 16" restores g_esp fully. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = sysarg2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = sysarg1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* dummy return address */
    fn();

    g_eax = 1;  /* TRUE: approximation -- we don't track already-queued state */
}

/* ── KeSynchronizeExecution (ordinal 153) ─────────────────
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                 PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                 PVOID SynchronizeContext)
 *
 * Same shape as KeInsertQueueDpc above before ITS fix: entirely unbridged, so
 * every call fell through to the generic "no bridge for ordinal" path,
 * silently returned FALSE, and SynchronizeRoutine never ran. Found live
 * (2026-09-07) while tracing why Breakdown's namcologo.xmv stream stops after
 * exactly two prefetched chunks: the streaming-buffer service loop
 * (sub_001D4CC3 / sub_001DAA3C, guest addresses) calls this in a loop
 * checking a per-buffer busy flag afterward, and the routine that is
 * supposed to synchronize with the delivering interrupt and update that flag
 * was never being invoked at all -- confirmed via the one-line-per-slot
 * "[KERNEL] WARNING: no bridge for ordinal 153 ... called from guest VA
 * 0x001D4CF7" this bridge already prints for any unwired ordinal.
 *
 * xbox_KeSynchronizeExecution() in kernel_sync.c is NOT used here: it casts
 * SynchronizeRoutine straight to a host function pointer and calls it
 * natively, which cannot work in this recompiler -- guest code is translated
 * into differently-addressed host C functions (sub_XXXXXXXX, looked up by
 * recomp_lookup/recomp_lookup_manual), not left callable at its literal
 * 32-bit address. That guest memory happens to be identity-mapped in this
 * build (xbox_memory_layout.c's "ideal case", offset 0) means the call reads
 * as valid code bytes without a page fault, but they are the ORIGINAL x86
 * instructions being executed as x86-64 host code with none of the expected
 * register/stack state -- undefined, not a working invocation. Left
 * un-deleted since removing it is a separate cleanup, not required for this
 * fix.
 *
 * Runs SynchronizeRoutine synchronously on the calling thread, exactly like
 * bridge_KeInsertQueueDpc's DeferredRoutine: this bridge already prefers
 * running callbacks immediately over modeling real IRQL-based preemption,
 * and "raise to the interrupt's IRQL and call the routine" on real hardware
 * already happens synchronously on the caller's own thread. */
static void bridge_KeSynchronizeExecution(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    recomp_func_t fn;
    (void)interrupt_va;  /* not modeled -- opaque token only, same as elsewhere */

    fn = recomp_lookup_manual(routine);
    if (!fn) fn = recomp_lookup(routine);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine 0x%08X "
                "not resolvable\n", routine);
        g_eax = 0;
        return;
    }

    /* BOOLEAN SynchronizeRoutine(PVOID SynchronizeContext);
     * stdcall, one argument, so the callee's own "ret 4" restores g_esp. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* dummy return address */
    fn();
    /* g_eax is now whatever SynchronizeRoutine returned -- pass it through
     * unmodified, matching how a real call would leave eax for the caller. */
}

/* ── NV2A interrupt plumbing (ordinals 44, 98, 109) ───────
 *
 * The D3D8 library linked into a title installs an ISR for the GPU's vblank /
 * command-completion interrupt. There is no NV2A here, so nothing raises
 * that interrupt on its own -- KeConnectInterrupt below starts a dedicated
 * delivery thread that actually invokes the registered ServiceRoutine
 * periodically instead, simulating hardware raising it (see the delivery
 * thread further down).
 *
 * KeConnectInterrupt reports success unconditionally: reporting failure
 * sends Halo's rasterizer down an error path during preinitialize, and the
 * goal is to get past setup either way.
 *
 * HISTORY: this used to be a pure no-op ("no interrupt is ever delivered"),
 * which is correct for a title that only polls its status registers but
 * left any title whose per-frame work is re-armed exclusively through this
 * ISR -> KeInsertQueueDpc chain with NOTHING ever re-driving it after the
 * first frame. Confirmed on one such title (HeroLab project "Xbox
 * Recompiler", Breakdown task 1c4b9a06) via live caller-capture
 * instrumentation, not guessed: KeConnectInterrupt was reached and reported
 * success, the registered ISR's own KeInsertQueueDpc call was itself a
 * silent no-op (see bridge_KeInsertQueueDpc below, previously unbridged),
 * and nothing downstream of frame 1 ever ran again.
 */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    if (irql_va) {
        /* IRQL is conventionally the vector for device interrupts. */
        BRIDGE_MEM8(irql_va) = (uint8_t)level;
    }
    g_eax = level;
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    /* Xbox KINTERRUPT is 44 bytes. */
    memset(XBOX_TO_NATIVE(interrupt_va), 0, 44);
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector;
    g_eax = 0;
}

/* Delivery rate for the simulated interrupt below. 60 Hz is the NTSC vblank
 * rate KeConnectInterrupt's real hardware counterpart would be tied to;
 * measured ground truth on one title (xemu, real Xbox) put its actual
 * observed dispatch rate at ~76/sec, so this is a reasonable approximation,
 * not a verified-correct rate for every title -- revisit per-title if a
 * title turns out to be timing-sensitive to the exact vblank rate. */
#define INTERRUPT_DELIVERY_HZ 60

/* A title can connect several interrupts (measured on one title: GPU vblank,
 * plus separate audio/network-shaped ones at other vectors) -- one delivery
 * thread per connected interrupt, up to this many. Not a general PIC (no
 * priority, no masking, no real IRQL) -- just "make every registered
 * ServiceRoutine actually run periodically," which is the piece that was
 * missing entirely. */
#define MAX_DELIVERED_INTERRUPTS 8
static uint32_t g_connected_interrupts[MAX_DELIVERED_INTERRUPTS];
static volatile LONG g_connected_interrupt_count = 0;

static DWORD WINAPI interrupt_delivery_thread(LPVOID param)
{
    int slot;
    uint32_t stack_top, routine, context, interrupt_va;

    interrupt_va = (uint32_t)(uintptr_t)param;

    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        fprintf(stderr, "  [KERNEL] interrupt delivery: no worker stack slice "
                "available for interrupt 0x%08X, not started\n", interrupt_va);
        return 0;
    }
    stack_top = XBOX_WORKER_STACK_TOP(slot);
    /* Genuinely new native thread: RECOMP_TLS (g_esp etc.) starts zeroed and
     * the fake TIB is per-thread too. Same one-time setup bridge_thread_main
     * does for a spawned game thread. */
    xbox_init_fake_tib();

    routine = BRIDGE_MEM32(interrupt_va + 0);
    context = BRIDGE_MEM32(interrupt_va + 4);

    fprintf(stderr, "  [KERNEL] interrupt delivery: armed on interrupt 0x%08X, "
            "routine=0x%08X context=0x%08X, %d Hz\n",
            interrupt_va, routine, context, INTERRUPT_DELIVERY_HZ);
    fflush(stderr);

    for (;;) {
        recomp_func_t fn;

        Sleep(1000 / INTERRUPT_DELIVERY_HZ);

        fn = recomp_lookup_manual(routine);
        if (!fn) fn = recomp_lookup(routine);
        if (!fn) continue;  /* not (yet) resolvable -- try again next tick */

        /* BOOLEAN ServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext);
         * stdcall, so the callee's own "ret 8" restores g_esp fully -- no
         * cleanup needed here after fn() returns. */
        g_esp = stack_top;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = interrupt_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* dummy return address */
        g_seh_ebp = g_esp;
        fn();
    }
}

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt) */
static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine = BRIDGE_MEM32(interrupt_va + 0);
    recomp_func_t fn = recomp_lookup_manual(routine);
    if (!fn) fn = recomp_lookup(routine);

    /* Only deliver interrupts whose ServiceRoutine resolves to real,
     * lifted code. One title measured connects several interrupts and at
     * least one had a routine address that resolved to nothing (one past
     * the end of an unrelated function, i.e. not a real entry point) --
     * calling into that blind is how this crashed the first time this was
     * tried. Reporting connected either way matches real hardware always
     * connecting successfully; we just don't simulate hardware raising an
     * interrupt whose handler we can't identify. */
    if (fn) {
        LONG idx = InterlockedIncrement(&g_connected_interrupt_count) - 1;
        if (idx < MAX_DELIVERED_INTERRUPTS) {
            HANDLE h;
            g_connected_interrupts[idx] = interrupt_va;
            h = CreateThread(NULL, 0, interrupt_delivery_thread,
                              (LPVOID)(uintptr_t)interrupt_va, 0, NULL);
            if (h) {
                CloseHandle(h);
            } else {
                fprintf(stderr, "  [KERNEL] KeConnectInterrupt: failed to start "
                        "delivery thread for interrupt 0x%08X\n", interrupt_va);
            }
        } else {
            fprintf(stderr, "  [KERNEL] KeConnectInterrupt: interrupt 0x%08X exceeds "
                    "MAX_DELIVERED_INTERRUPTS (%d), not delivered\n",
                    interrupt_va, MAX_DELIVERED_INTERRUPTS);
        }
    } else {
        fprintf(stderr, "  [KERNEL] KeConnectInterrupt: interrupt 0x%08X routine "
                "0x%08X does not resolve to a function, not delivered\n",
                interrupt_va, routine);
    }

    g_eax = 1;  /* connected */
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN)
 * Records a callback for console shutdown. Nothing here ever shuts down that
 * way, so registration is accepted and dropped. */
static void bridge_HalRegisterShutdownNotification(void)
{
    g_eax = 0;
}

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 *
 * Initializes a timer object. Xbox KTIMER is 40 bytes.
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type = STACK_ARG(1);

    /* Zero the structure (40 bytes) */
    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Set Type (0x08 = TimerNotificationObject, 0x09 = TimerSynchronizationObject) */
    BRIDGE_MEM16(timer_va + 0) = (uint16_t)(0x08 + (type & 1));
    g_eax = 0;
}

/* ── KeSetTimer / KeSetTimerEx (ordinal 149/150) ──────────
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 *
 * Sets a timer. We don't actually start timers - just record the state.
 * Returns FALSE (timer was not already set).
 */
static void bridge_KeSetTimer(void)
{
    /* Timer functionality is not needed for basic execution.
     * Return FALSE = timer was not previously set. */
    g_eax = 0;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    uint32_t block = STACK_ARG(0);
    /* Return a reasonable default size. Actual pool blocks are managed
     * by the kernel; for recompilation, returning 0 might be OK since
     * code usually uses this for debugging/stats. */
    g_eax = 0;
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    static RECOMP_TLS char path[MAX_PATH];
    uint32_t ansi_str_va, buf_va;
    uint16_t len;

    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;

    /* An ANSI_STRING carries an explicit Length and its buffer is not required
     * to be NUL-terminated at that length -- a title can point two strings at
     * one buffer and vary Length to mean different things. Breakdown does
     * exactly that: it builds "\Device\Harddisk0\PartitionN\" once, then
     * decrements Length by one to address the raw partition device
     * ("...PartitionN", no trailing separator) rather than the filesystem root.
     * Reading the buffer as a C string collapses the two and the raw device
     * open lands on the filesystem directory instead. Length 0 keeps the old
     * NUL-terminated reading -- some callers leave it unset. */
    len = BRIDGE_MEM16(ansi_str_va + 0);
    if (len == 0 || len >= MAX_PATH)
        return (const char*)XBOX_TO_NATIVE(buf_va);

    memcpy(path, (const char*)XBOX_TO_NATIVE(buf_va), len);
    path[len] = '\0';

    /* A path built from corrupted memory arrives here as non-printable bytes.
     * Dump the ANSI_STRING that produced it -- Length, MaximumLength, the
     * buffer VA and its first bytes -- so "is the Length wrong or is the
     * buffer contents wrong" is answerable without guessing. Bounded, and it
     * only fires on a path that is already broken. */
    {
        uint16_t i;
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)path[i];
            if (c < 0x20 || c > 0x7E) {
                static int dumped;
                if (dumped++ < 6) {
                    uint16_t maxlen = BRIDGE_MEM16(ansi_str_va + 2);
                    const unsigned char *raw =
                        (const unsigned char *)XBOX_TO_NATIVE(buf_va);
                    uint16_t n = len < 32 ? len : 32, k;
                    fprintf(stderr,
                            "  [PATH] CORRUPT: ANSI_STRING@0x%08X Length=%u "
                            "MaximumLength=%u Buffer=0x%08X bad byte at %u\n"
                            "         raw:",
                            ansi_str_va, len, maxlen, buf_va, i);
                    for (k = 0; k < n; k++) fprintf(stderr, " %02X", raw[k]);
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
                break;
            }
        }
    }
    return path;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

/* Xbox path each open handle was created with, so a later open can resolve
 * an OBJECT_ATTRIBUTES.RootDirectory-relative name against it (see
 * bridge_build_oa below). Parallel to s_handle_table, indexed the same way;
 * cleared alongside it in bridge_take_handle. Populated only for handles
 * opened through bridge_create_file_impl -- good enough since RootDirectory
 * is only ever a directory handle a title itself opened earlier. */
static char s_handle_path[BRIDGE_HANDLE_MAX][MAX_PATH];

static void bridge_set_handle_path(uint32_t token, const char* path)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX && path) {
            strncpy(s_handle_path[i], path, MAX_PATH - 1);
            s_handle_path[i][MAX_PATH - 1] = '\0';
        }
    }
}

static const char* bridge_get_handle_path(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX && s_handle_path[i][0])
            return s_handle_path[i];
    }
    return NULL;
}

static uint32_t bridge_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            s_handle_path[i][0] = '\0';
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path.
 *
 * Handles a RootDirectory-relative open: OBJECT_ATTRIBUTES.RootDirectory
 * (offset 0) names an already-open directory handle, and ObjectName is
 * then a name relative to it rather than an absolute device path. This
 * used to be ignored entirely (RootDirectory hardcoded to NULL below, only
 * ObjectName ever read) -- any title that opens a file relative to a
 * directory handle instead of building one absolute path string got a
 * NULL ObjectName->Buffer here and failed with STATUS_OBJECT_PATH_NOT_FOUND
 * before ever reaching real path translation. See
 * Breakdown-Launcher HeroLab task 1b0f5bf7-5d54-4fb6-a89f-1ba04ad8969a
 * (sub_001ABDD9's NtOpenFile, kernel call #124 in that trace). */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    static RECOMP_TLS char joined[MAX_PATH];
    uint32_t    root_token = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 0) : 0;
    const char* root_path  = root_token ? bridge_get_handle_path(root_token) : NULL;
    const char* rel_path   = bridge_get_xbox_path(obj_attrs_va);
    const char* path;

    if (root_path) {
        if (rel_path && rel_path[0]) {
            /* rel_path may or may not carry its own leading separator. */
            size_t root_len = strlen(root_path);
            BOOL   root_has_sep = root_len && (root_path[root_len - 1] == '\\');
            BOOL   rel_has_sep  = rel_path[0] == '\\';
            snprintf(joined, MAX_PATH - 1, "%s%s%s", root_path,
                      (root_has_sep || rel_has_sep) ? "" : "\\",
                      (root_has_sep && rel_has_sep) ? rel_path + 1 : rel_path);
        } else {
            /* No relative name: the open targets the root directory itself. */
            snprintf(joined, MAX_PATH - 1, "%s", root_path);
        }
        joined[MAX_PATH - 1] = '\0';
        path = joined;
    } else {
        /* No RootDirectory (or it didn't resolve to a tracked path) --
         * unchanged behavior: ObjectName must be the absolute path. */
        path = rel_path;
    }

    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        /* TEMP TRACE (2026-09-03): dump the raw guest OBJECT_ATTRIBUTES so
         * a NULL name isn't a black box -- see HeroLab task
         * 1b0f5bf7-5d54-4fb6-a89f-1ba04ad8969a. */
        uint32_t root_tok    = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 0) : 0;
        uint32_t objname_va  = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 4) : 0;
        uint32_t buf_va      = objname_va ? BRIDGE_MEM32(objname_va + 4) : 0;
        uint16_t len         = objname_va ? BRIDGE_MEM16(objname_va + 0) : 0;
        fprintf(stderr, "  [TRACE bridge_create_file_impl] NULL name: obj_attrs_va=0x%08X "
                "RootDirectory_token=0x%08X ObjectName_va=0x%08X Buffer_va=0x%08X Length=%u "
                "root_path_resolved=%s\n",
                obj_attrs_va, root_tok, objname_va, buf_va, (unsigned)len,
                bridge_get_handle_path(root_tok) ? bridge_get_handle_path(root_tok) : "(none)");
        fflush(stderr);
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st)) {
        uint32_t token = bridge_handle_token(h);
        if (handle_va) BRIDGE_MEM32(handle_va) = token;
        /* name.Buffer is the resolved (possibly RootDirectory-joined) path
         * built by bridge_build_oa -- record it so a later open can use
         * *this* handle as ITS RootDirectory. */
        bridge_set_handle_path(token, name.Buffer);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    fprintf(stderr, "  [FILE] NtCreateFile handle_va=0x%08X oa=0x%08X ios=0x%08X\n",
            handle_va, obj_attrs, iostatus);
    fflush(stderr);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);
}

/*
 * Completion for a file request that carried an Event or an APC routine.
 *
 * Both bridges below do the I/O synchronously, and used to drop args 1-3
 * (Event, ApcRoutine, ApcContext) on the floor. A title that issues an async
 * request and waits alertably for the completion then waits forever: Halo's
 * cache-partition setup does exactly that, gives up after its 5-second SleepEx,
 * and asserts "setup for new cache file failed (#0)".
 *
 * ponytail: the APC runs inline here rather than at the next alertable wait.
 * The data really is ready by then, so the observable result matches; a title
 * that depends on the APC *not* having run yet would notice. A per-thread
 * deferred queue drained at alertable waits was tried for Halo's map streamer
 * and made no difference (it still issues one 14 KB batch and stops), so it was
 * dropped rather than risk changing this shared path for the other titles.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{
    /* The APC can be game code or a kernel export. Halo's XAPI passes the
     * latter -- 0xFE0000FC, one of our own synthetic thunk VAs -- so the recomp
     * dispatch correctly fails to find it and the kernel fallback is the one
     * that matters. Checking only recomp_lookup left it undelivered. */
    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {
        /* VOID ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG) */
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;   /* dummy return address */
        fn();
        g_esp += 12;
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

/* Per-thread pending-APC ring. An APC is delivered on the thread that issued
 * the request, which is also the thread that waits, so thread-local is right. */
static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (apc_routine) {
        deliver_one_apc(apc_routine, apc_context, iostatus);
    }
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);

    if (getenv("XBOXRECOMP_SAVE_TRACE")) {
        fprintf(stderr, "  [SAVE] NtReadFile token=0x%08X len=%u off=%lld "
                "status=0x%08X info=%u ret=0x%08X event=0x%08X apc=0x%08X\n",
                STACK_ARG(0), length, poff ? (long long)off.QuadPart : -1LL,
                (unsigned)ios.Status, (unsigned)ios.Information,
                (unsigned)g_eax, STACK_ARG(1), STACK_ARG(2));
        fflush(stderr);
    }

    /* Reads, ~every 5 s, plus every failed or short one. Breakdown streams
     * BGM, sound effects and speech out of D:\stream\*.stw, and sound that
     * fades in and out with speech missing entirely looks like streaming
     * rather than mixing. A short read means the title got less audio than it
     * asked for; a failure means it got none. */
    {
        static unsigned reads, shorts, failures;
        static uint64_t bytes;
        static ULONGLONG last_ms;
        ULONGLONG now = GetTickCount64();
        int bad = (int)ios.Status < 0 || (uint32_t)ios.Information < length;
        reads++;
        bytes += (uint32_t)ios.Information;
        if ((int)ios.Status < 0) failures++;
        else if ((uint32_t)ios.Information < length) shorts++;
        if (bad && (failures + shorts) <= 20)
            fprintf(stderr, "  [FILE] read %u bytes at %lld -> status 0x%08X, %u bytes\n",
                    length, poff ? (long long)off.QuadPart : -1LL,
                    (unsigned)ios.Status, (unsigned)ios.Information);
        if (!last_ms) last_ms = now;
        if (now - last_ms >= 5000) {
            fprintf(stderr, "  [FILE] %u reads, %llu KB, %u short, %u failed (~5 s)\n",
                    reads, (unsigned long long)(bytes / 1024), shorts, failures);
            fflush(stderr);
            reads = shorts = failures = 0;
            bytes = 0;
            last_ms = now;
        }
    }

    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    if (getenv("XBOXRECOMP_SAVE_TRACE")) {
        fprintf(stderr, "  [SAVE] NtWriteFile token=0x%08X len=%u off=%lld "
                "status=0x%08X info=%u ret=0x%08X event=0x%08X apc=0x%08X\n",
                STACK_ARG(0), length, poff ? (long long)off.QuadPart : -1LL,
                (unsigned)ios.Status, (unsigned)ios.Information,
                (unsigned)g_eax, STACK_ARG(1), STACK_ARG(2));
        fflush(stderr);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    if (getenv("XBOXRECOMP_SAVE_TRACE")) {
        fprintf(stderr, "  [SAVE] NtFlushBuffersFile token=0x%08X status=0x%08X "
                "info=%u ret=0x%08X\n", STACK_ARG(0), (unsigned)ios.Status,
                (unsigned)ios.Information, (unsigned)g_eax);
        fflush(stderr);
    }
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 10 args = 40 bytes) ─
 *
 * The Xbox export takes a FILE_INFORMATION_CLASS between Length and FileName,
 * exactly as NT's does (NT additionally has ReturnSingleEntry, which the Xbox
 * kernel drops because it is always single-entry). This bridge was written for
 * 9 arguments with no class slot, so every argument from index 7 on was read
 * one slot early: FileName came back as the class constant 1, and RestartScan
 * as the FileName pointer. Reading a "descriptor" at guest VA 1 produced a
 * 16-bit Length of whatever happens to sit at guest address 0, which is how
 * this call reached kernel_file.c's unbounded pattern_wide[Length] write.
 * On top of that the stdcall cleanup popped 36 bytes where the callee owed 40,
 * leaving g_esp 4 low after every enumeration call.
 *
 * Ground truth is the title's own code: all three call sites in Breakdown
 * (0x001ABA08, 0x001ABA6F, 0x001AE4B2 in tools/disasm/output/asm/text.asm)
 * push exactly ten arguments, with the constant 1 (FileDirectoryInformation)
 * at index 7 and the ANSI_STRING at index 8. See HeroLab task 17f002cc.
 */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t info_class  = STACK_ARG(7);  /* FILE_INFORMATION_CLASS */
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    /* The Xbox kernel only implements FileDirectoryInformation (1) here, and
     * that is the only layout xbox_NtQueryDirectoryFile fills in. Warn once
     * rather than fail, so a title using something else is visible instead of
     * silently mis-served. */
    if (info_class != 1) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "  [KERNEL] NtQueryDirectoryFile: unsupported "
                    "FileInformationClass %u (only FileDirectoryInformation=1 "
                    "is implemented); serving it as class 1\n", info_class);
            fflush(stderr);
        }
    }
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (target_va) {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);
        if (buf_va && len < max_len) {
            memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
            BRIDGE_MEM16(target_va) = len;
        }
    }
    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* ── NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) */
static void bridge_NtDeviceIoControlFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(4);
    uint32_t ioctl     = STACK_ARG(5);
    uint32_t in_va     = STACK_ARG(6);
    uint32_t in_len    = STACK_ARG(7);
    uint32_t out_va    = STACK_ARG(8);
    uint32_t out_len   = STACK_ARG(9);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtDeviceIoControlFile(handle, NULL, NULL, NULL, &ios,
                ioctl,
                in_va  ? XBOX_TO_NATIVE(in_va)  : NULL, in_len,
                out_va ? XBOX_TO_NATIVE(out_va) : NULL, out_len);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── IoCreateSymbolicLink (ordinal 63) ───────────────────── */
static void bridge_IoCreateSymbolicLink(void)
{
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── ObReferenceObjectByHandle (ordinal 246) ───────────────
 *
 * Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType,
 *                                          PVOID* Object)
 * 3 args, not the 6 of Windows NT.
 *
 * This wrote a plain 0 for every type, which is not a pointer to anything. A
 * title that only passes the result back to another kernel call never noticed;
 * one that READS the object did, silently. Breakdown's GetExitCodeThread reads
 * a thread object's signal state and exit status, got neither, and busy-waited
 * forever -- 1.2 billion kernel calls in 185 s (HeroLab task f6bd2dbc).
 *
 * Thread handles now resolve to a real guest-visible object (see
 * XBOX_THREAD_OBJ_* in xbox_memory_layout.h). Every other type still gets 0:
 * no title here has been observed reading one, and inventing a layout for an
 * object nothing has measured is what produced this bug in the first place.
 *
 * NOTE for callers downstream: the out-parameter commonly ALIASES the handle
 * argument (Breakdown does `lea eax, [ebp+8]`), so after this returns the
 * guest may no longer hold the handle at all -- only the object. Anything that
 * then takes "a thread" from the guest must accept an object pointer too; see
 * bridge_KeSetBasePriorityThread.
 */
static void bridge_ObReferenceObjectByHandle(void)
{
    uint32_t handle     = STACK_ARG(0);
    uint32_t obj_type   = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    uint32_t object     = 0;

    if (obj_type == XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE) {
        object = bridge_thread_obj_for_token(handle);
        if (!object) {
            /* A thread handle this bridge did not create -- the inline main
             * thread, or a handle the title made up. Answer with the reserved
             * never-signalled object so the poll is at least consistent. */
            object = bridge_thread_obj_va(BRIDGE_THREAD_OBJ_UNKNOWN);
        }
    }

    if (object_ptr) BRIDGE_MEM32(object_ptr) = object;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    /* Kept diagnostic (HeroLab task 28b5168a, "what gates display
     * init"): called only twice in a whole run per prior measurement, so
     * logging every call unconditionally is cheap. The guest return address
     * sits at [esp-4] at this point -- kernel_thunk_dispatch has already
     * popped the dummy/return slot before invoking the bridge (see its own
     * comment); this is the same expression bridge_HalReadWritePCISpace
     * already uses successfully for the same purpose. */
    fprintf(stderr, "  [AVPACK] AvGetSavedDataAddress called from guest VA 0x%08X\n",
            g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);
    fflush(stderr);
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
    fprintf(stderr, "  [AVPACK] AvGetSavedDataAddress -> 0x%08X\n", g_eax);
    fflush(stderr);
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    /* Same rationale as AvGetSavedDataAddress above. Also log the Option
     * argument and the Result actually written, so the branch the title
     * takes afterward can be reasoned about from the SAME value it saw,
     * not from what xbox_AvSendTVEncoderOption() is currently coded to
     * return (already known: AV_OPTION_QUERY_AVPACK -> AV_PACK_HDTV). */
    uint32_t option = STACK_ARG(1);
    PULONG result_ptr = (PULONG)XBOX_TO_NATIVE(STACK_ARG(3));
    fprintf(stderr, "  [AVPACK] AvSendTVEncoderOption(option=0x%02X, param=0x%X) "
            "called from guest VA 0x%08X\n",
            option, STACK_ARG(2), g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);
    fflush(stderr);
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               result_ptr);
    fprintf(stderr, "  [AVPACK] AvSendTVEncoderOption(option=0x%02X) -> *Result=0x%08X\n",
            option, result_ptr ? *result_ptr : 0xDEADBEEF);
    fflush(stderr);
    g_eax = 0;
}

/* ── HalReadWritePCISpace (ordinal 46, 6 args)
 * Writes bytes at a caller-supplied address only, so the marshalling is the
 * safe kind: Buffer is the one pointer and XBOX_TO_NATIVE maps it. */
static void bridge_HalReadWritePCISpace(void)
{
    fprintf(stderr, "  [KERNEL] HalReadWritePCISpace bus=%u slot=%u reg=0x%X len=%u %s"
            " -- called from guest VA 0x%08X\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2), STACK_ARG(4),
            STACK_ARG(5) ? "WRITE" : "READ",
            g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);
    fflush(stderr);
    xbox_HalReadWritePCISpace(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
                              XBOX_TO_NATIVE(STACK_ARG(3)),
                              STACK_ARG(4), (BOOLEAN)STACK_ARG(5));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees.
 *
 * Do NOT route this through xbox_ExFreePool: that calls
 * HeapFree(GetProcessHeap(), P), and guest pool memory is not on the host
 * heap, so P is a pointer the host allocator has never seen (see the long
 * memory-model note by the disabled-wrapper list below). The matching
 * allocators here -- bridge_ExAllocatePool and bridge_ExAllocatePoolWithTag
 * -- both come from xbox_HeapAlloc, i.e. the GUEST heap, so the free has to
 * go back to the guest heap too, with the guest VA passed through unchanged
 * rather than converted.
 *
 * UNEXERCISED, and honestly so: this was written while chasing Breakdown's
 * heap exhaustion on the theory that pool blocks were leaking. They were not.
 * Breakdown never calls ordinal 17 at all -- its heap filled up because the
 * contiguous-memory bridges were serving ~39 MB of arenas out of the general
 * heap, which is fixed separately (xbox_ContigAlloc). So nothing here has ever
 * run. It is kept because the previous implementation was wrong in a way that
 * would corrupt the host heap the first time a title did call it, and a
 * correct-but-untested bridge beats a wrong one sitting behind a comment.
 * Treat it as unverified until some title exercises it.
 * HeroLab: Xbox Recompiler, task 999ce5ca (2026-09-04). */
static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoCreateDevice (ordinal 65, 6 args) */
/*
 * Deliberately no longer forwards to xbox_IoCreateDevice() in kernel_io.c.
 * That routing could not have worked, which is presumably why it stayed
 * commented out, and it fails in two independent ways:
 *
 *  1. (PVOID*)XBOX_TO_NATIVE(STACK_ARG(5)) writes an 8-byte HOST pointer into
 *     a 4-byte guest slot -- both the wrong value and four bytes of collateral.
 *     bridge_NtCreateEvent's own comment warns about exactly this.
 *  2. xbox_IoCreateDevice returns a HeapAlloc() block on the HOST heap and puts
 *     DeviceExtension at offset 8 of its XBOX_FAKE_DEVICE (64-bit build). The
 *     guest dereferences the result through MEM32 and reads DeviceExtension at
 *     +0x18. Either mismatch alone yields a wild pointer.
 *
 * Unbridged it was worse than absent, the same class as NtCreateSemaphore
 * above but with a destructive ending. Breakdown's Memory Unit driver -- the
 * "MU__" tag at guest 0x00203F00 -- does:
 *
 *     call IoCreateDevice(..., ext_size=0x170, ..., &dev)
 *     jl   fail                 ; the default 0 return reads as STATUS_SUCCESS
 *     mov  eax, [ebp-4]         ; dev, NEVER WRITTEN -> stale stack slot
 *     mov  edx, [eax+0x18]      ; DeviceExtension, dereferenced from garbage
 *     rep  stosd (0x5C dwords)  ; 368-byte memset THROUGH THAT GARBAGE POINTER
 *
 * So every boot scribbled 368 zero bytes at an arbitrary guest address, and
 * nothing pointed at it because the call had "succeeded".
 *
 * LAYOUT NOTE, read before changing the header size. The true Xbox
 * DEVICE_OBJECT layout is not available here -- not in this tree, and not in
 * xemu, which has no reason to model a kernel object. The ONE offset with
 * evidence behind it is DeviceExtension at +0x18, read straight out of the
 * guest code above. The header is therefore sized generously and zeroed rather
 * than sized to a remembered number: an unknown field answering zero is
 * survivable, and inventing a struct would be the constant-from-memory mistake
 * this project keeps paying for. Revisit only with a real layout in hand.
 */
#define XBOX_DEVICE_OBJECT_HEADER 0x80u  /* >= any offset the evidence supports */

static void bridge_IoCreateDevice(void)
{
    uint32_t ext_size = STACK_ARG(1);
    uint32_t out_ptr  = STACK_ARG(5);
    uint32_t total, va, ext_va;

    if (!out_ptr) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }

    total = XBOX_DEVICE_OBJECT_HEADER + ext_size;
    va = xbox_HeapAlloc(total, 16);
    if (!va) {
        *(uint32_t *)XBOX_TO_NATIVE(out_ptr) = 0;
        g_eax = (uint32_t)STATUS_INSUFFICIENT_RESOURCES;
        return;
    }

    /* xbox_HeapAlloc is a bump allocator and promises nothing about contents,
     * and the caller reads fields this bridge does not set. */
    memset(XBOX_TO_NATIVE(va), 0, total);

    ext_va = ext_size ? (va + XBOX_DEVICE_OBJECT_HEADER) : 0;
    *(uint32_t *)((uintptr_t)XBOX_TO_NATIVE(va) + 0x18) = ext_va;
    *(uint32_t *)XBOX_TO_NATIVE(out_ptr) = va;

    fprintf(stderr, "  [BRIDGE] IoCreateDevice: ext_size=%u -> device VA 0x%08X, "
            "extension VA 0x%08X\n", ext_size, va, ext_va);
    fflush(stderr);

    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── KeCancelTimer (ordinal 97, 1 arg) */
static void bridge_KeCancelTimer(void)
{
    g_eax = (uint32_t)xbox_KeCancelTimer(
        (PXBOX_KTIMER)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── KeDisconnectInterrupt (ordinal 100, 1 arg) */
static void bridge_KeDisconnectInterrupt(void)
{
    g_eax = (uint32_t)xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── KeSetBasePriorityThread (ordinal 143, 2 args)
 *
 * The "thread" argument is whatever the title has in hand, and that is usually
 * NOT a handle: Breakdown's SetThreadPriority (sub_001A8277) calls
 * ObReferenceObjectByHandle first, whose out-parameter overwrites the handle
 * slot with the object, and passes the object here. Resolve it back to the
 * native thread before falling through to the handle-token reading that any
 * other caller needs. */
static void bridge_KeSetBasePriorityThread(void)
{
    uint32_t thread = STACK_ARG(0);
    HANDLE   h      = bridge_thread_handle_for_object(thread);

    if (!h) h = bridge_resolve_handle(thread);

    g_eax = (uint32_t)xbox_KeSetBasePriorityThread(h, (LONG)STACK_ARG(1));
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg) */
static void bridge_MmQueryAllocationSize(void)
{
    uint32_t va = STACK_ARG(0);
    /* Ask the allocator that handed the block out. VirtualQuery cannot answer
     * for those: the heap and the contiguous window are each one big host
     * mapping, so RegionSize runs from the block to the end of the whole
     * window. Breakdown's audio allocator zeroes whatever this returns right
     * after allocating (guest 0x001D3D46); with RegionSize that wiped every
     * contiguous block above the new one, including the free-list headers the
     * previous allocation had just written. Addresses neither allocator knows
     * (the pinned-physical path) keep the old answer. */
    uint32_t size = xbox_AllocationSize(va);
    g_eax = size ? size
                 : (uint32_t)xbox_MmQueryAllocationSize(XBOX_TO_NATIVE(va));
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL,
                             (BOOLEAN)STACK_ARG(2));
    if (st >= 0 && handle_va) bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtResumeThread (ordinal 224, 2 args) */
/* ── NtSuspendThread (ordinal 231, 2 args) ───────────────── */
/*
 * The exact sibling of bridge_NtResumeThread below, and it was the only half
 * of the pair missing. Unbridged it returned 0 -- STATUS_SUCCESS -- without
 * suspending anything, so a thread that asked to park itself carried straight
 * on and asked again. That shows up as a three-call cycle running flat out:
 * ordinal 231 61,966 times, NtReleaseSemaphore 61,964, NtWaitForSingleObjectEx
 * 61,915 in one 120 s run -- a worker handing off and then failing to sleep.
 *
 * This became reachable only once the semaphore bridges above were added; it
 * is the next thing the same code path does.
 */
static void bridge_NtSuspendThread(void)
{
    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0). Xbox uses __fastcall here, so the argument never reaches
 * the stack and the arg-size entry is 0. Reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfDereferenceObject(void)
{
    xbox_ObfDereferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  4;  /* HalClearSoftwareInterrupt (1) */
    case  39: return  8;  /* HalDisableSystemInterrupt (2) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt (1) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  8;  /* IoStartNextPacket (2) */
    case  82: return 12;  /* IoStartNextPacketByKey (3) */
    case  83: return 16;  /* IoStartPacket (4) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 40;  /* NtQueryDirectoryFile (10) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 217: return 16;  /* NtQueryVirtualMemory (4) */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 20;  /* NtWaitForMultipleObjectsEx (5) */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */

    /* ── Crypto ── */

    default:  return  0;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimer;  /* KeSetTimerEx */

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 113: return bridge_KeInitializeTimerEx;
    case 119: return bridge_KeInsertQueueDpc;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 153: return bridge_KeSynchronizeExecution;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;


    /* BISECT-OFF case 338: bridge_XcRC4Key */
    /* BISECT-OFF case 339: bridge_XcRC4Crypt */


    /* NOT ROUTED, deliberately. The wrappers above exist and compile, and each
     * has a working xbox_* behind it, but routing them made Halo 2276 crash
     * EARLIER than leaving them stubbed -- twice, with two different faults.
     * Bisected to a memory-model mismatch, not to the wrappers' arithmetic:
     *
     *   xbox_IoCreateDevice HeapAllocs from GetProcessHeap() and writes that
     *   NATIVE pointer through its out-parameter. The bridge hands it the
     *   native address of a 4-BYTE GUEST slot, so a 64-bit pointer is written
     *   into 4 bytes: it clobbers the adjacent guest dword and leaves the title
     *   a truncated pointer it then dereferences. Crash was a write to
     *   0x90909090.
     *
     *   xbox_ExFreePool calls HeapFree(GetProcessHeap(), P). Guest pool memory
     *   is not on the host heap, so P is a pointer HeapFree has never seen.
     *
     * These xbox_* functions were written for a NATIVE caller, where pointers
     * are host pointers and allocations are host allocations. The bridge is a
     * different world: pointers are guest VAs and memory lives in the mapped
     * guest space. XBOX_TO_NATIVE converts an address; it cannot convert an
     * allocator.
     *
     * So 'an xbox_* exists, therefore the wrapper is mechanical' is false, and
     * tools.kernel_audit.coverage no longer says it. Each of these needs its
     * memory model checked one at a time: which side owns the allocation, and
     * whether an out-pointer must carry a guest VA. Ones that only read or
     * write bytes at a caller-supplied address (the Xc* crypto group,
     * RtlTimeToTimeFields) should be fine; ones that allocate, free, or hand
     * back a pointer are not.
     *
     * Left in place rather than deleted: the wrappers are correct as argument
     * marshalling, and re-deriving them is the easy half of the work.
     */
    /* Av 1/2 clear the memory-model bar the note above sets. AvSetSavedDataAddress
     * (4, already bridged) stores a guest VA and AvGetSavedDataAddress hands the
     * same value back, so nothing crosses the guest/host pointer boundary; the
     * title was storing an address and reading back 0. AvSendTVEncoderOption only
     * writes through the caller's Result pointer, which XBOX_TO_NATIVE maps.
     *
     * Unbridged, option 0x0A (QUERY_AVPACK) never wrote Result at all, so the
     * title read "no AV pack connected" and skipped display setup entirely --
     * AvSetDisplayMode (3) is bridged but was never once called in a 200s run. */
    /* DELIBERATELY UNREGISTERED FOR NOW -- these two are correct, and that is
     * exactly the problem. Bridged, AvSendTVEncoderOption answers the
     * QUERY_AVPACK probe truthfully, the title stops believing no display is
     * connected, and it walks into a display-init path this engine cannot yet
     * finish: nothing executes the push buffer, there is no window/swapchain
     * anywhere in the build, and the XMV codec is ~1,558 stubbed MMX
     * instructions. Measured 2026-09-06 -- bridged, boot stops after
     * dsstdfx.bin with 0 dispatch calls; unbridged, the title skips video and
     * reaches D:\romdata\movie\namcologo.xmv with 9.4M dispatch calls in 60s.
     *
     * Re-register both as soon as the display path can actually complete;
     * leaving the kernel lying about the AV pack is a stopgap, not the
     * intended end state. The bridge functions above are written and correct.
     * HeroLab: Xbox Recompiler project, tasks b7d706a3 / a7c305d1. */
    /* REGISTERED 2026-09-07. Everything above this line is history now, and
     * every blocker it names was removed the same day: the push buffer
     * executes, there is a window and a swap chain, and the MMX stub count is
     * down from ~1,558 to ~45. Re-measured with all three in place -- bridged,
     * boot is completely healthy: 35 file opens reaching namcologo.xmv, no bug
     * checks, no faults, frames unchanged. Neither the 2026-09-06 stall after
     * dsstdfx.bin nor the garbage path string reproduces at all.
     *
     * The garbage-string "memory corruption" lead directly above was therefore
     * a symptom of the half-finished display path, not a bug of its own. Do
     * not chase it.
     *
     * Nor was the AV pack TYPE ever the gate. What actually gated display init
     * was two undocumented bits missing from the QUERY_AVPACK result; see
     * xbox_AvSendTVEncoderOption in kernel_hal.c. With those added the title
     * calls AvSetDisplayMode for the first time.
     *
     * The lesson, since this note misled a later reader once already: a
     * comment describing a blocker is a snapshot, not a standing fact. When
     * one says "re-register as soon as X can complete", check whether X
     * completes now before trusting its conclusion.
     * HeroLab tasks e305a8f6, 28b5168a. */
    case   1: return bridge_AvGetSavedDataAddress;
    case   2: return bridge_AvSendTVEncoderOption;
    case  17: return bridge_ExFreePool;   /* guest-heap free -- see note above */
    case  46: return bridge_HalReadWritePCISpace;  /* GPU discovery -- see kernel_hal.c */
    /* IoCreateDevice (65): the last unbridged ordinal. Body rewritten to
     * allocate in GUEST memory with DeviceExtension at +0x18; see it for the
     * 368-byte wild memset the unbridged path caused on every boot. */
    case  65: return bridge_IoCreateDevice;
    /* case  97: bridge_KeCancelTimer */
    /* case 100: bridge_KeDisconnectInterrupt */
    /* KeSetBasePriorityThread (143): bridge and its 8-byte arg entry both
     * already existed. Two stack args, no out-parameter, nothing wider than 4
     * bytes crossing the boundary. */
    case 143: return bridge_KeSetBasePriorityThread;
    /* KeStallExecutionProcessor (151): the bridge and its arg-size entry
     * (line ~2792) both already existed; only this registration was missing.
     * Found 2026-09-07 because an MCPX/APU register wait in DSOUND
     * (guest ~0x001D4632) calls it in a bounded busy-retry that never sees
     * its expected value, so it burns the full retry budget on every
     * attempt -- unbridged, each call is a near-free "no bridge" fallback,
     * so the retry loop span at native speed: 1.5B+ calls and 150+ MB of
     * throttled-sample log per run. Bridging makes the 1us stall real
     * (xbox_KeStallExecutionProcessor busy-waits for real via
     * QueryPerformanceCounter), which paces each retry honestly and cuts
     * the call rate by several orders of magnitude. This does NOT resolve
     * the wait itself -- 0x1DBA8C still never takes the expected value, so
     * the outer loop still retries forever -- it only stops that retry from
     * being free. */
    case 151: return bridge_KeStallExecutionProcessor;
    /* MmLockUnlockBufferPages (175) and MmQueryAllocationSize (180): both
     * bridges, both arg-size entries (12 and 4) and both xbox_Mm*
     * implementations already existed; only these two registrations were
     * missing, the same shape as ordinal 151 above. Breakdown calls both --
     * 175 from guest 0x00204A93, 180 from guest 0x001D3D37, which is inside
     * the audio code.
     *
     * 180 is the one that changes behaviour: the unbridged fallback silently
     * returned 0, and a buffer-management path told its buffer is zero bytes
     * long does not do anything useful with it. (The bridge now asks the
     * allocator for the size; VirtualQuery's RegionSize, its first answer,
     * overstated contiguous blocks by the rest of the window.)
     *
     * 175 is a deliberate no-op inside (page locking is meaningless in user
     * mode) and is registered for honesty rather than effect: an unbridged
     * call is indistinguishable in the log from a missing one. */
    case 175: return bridge_MmLockUnlockBufferPages;
    case 180: return bridge_MmQueryAllocationSize;
    /* case 192: bridge_NtCreateMutant */
    /* NtCreateSemaphore (193) / NtReleaseSemaphore (222): bridges written this
     * session -- see their definitions for why an unbridged NtCreateSemaphore
     * is actively harmful rather than merely absent (it reports SUCCESS and
     * hands back a NULL handle). NtQuerySymbolicLinkObject (215) already had a
     * bridge and only wanted registering. */
    case 193: return bridge_NtCreateSemaphore;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 222: return bridge_NtReleaseSemaphore;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;
    /* ObfDereferenceObject (250): fastcall, argument in ecx, arg-size entry
     * correctly 0 -- the bridge reads g_ecx rather than the stack, so routing
     * it does not disturb esp. */
    case 250: return bridge_ObfDereferenceObject;
    /* case 252: bridge_PhyGetLinkState */
    /* case 253: bridge_PhyInitialize */
    /* case 305: bridge_RtlTimeToTimeFields */
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    /* case 340: bridge_XcHMAC */
    /* case 346: bridge_XcDESKeyParity */

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Current dispatching slot.
 *
 * MUST be per-thread. Written by recomp_lookup_kernel() (resolve) and read a
 * few instructions later by kernel_thunk_dispatch() (dispatch) -- previously
 * a plain `static int`, the only piece of dispatch state in this file that
 * was not RECOMP_TLS like the rest of the register set (g_esp, g_eax, ...).
 * Two threads racing through resolve-then-dispatch at the same time (this
 * runtime delivers four ISR threads at 60 Hz each, plus the boot/game
 * thread, so this is not rare) let thread B's resolve overwrite thread A's
 * slot before A reads it: A then runs B's bridge and, worse, cleans
 * g_slot_arg_bytes[B] bytes off its OWN g_esp. Root-caused live via
 * kernel_bridge.c__slot-race-check.patch and gs_siblings.py /
 * gs_2c84_bisect.py in Breakdown-Launcher/debug-instruments -- HeroLab task
 * e1b79c22 (Xbox Recompiler project). The critical-section pair the /GS
 * functions call constantly (ordinals 277/294) takes 4 argument bytes; what
 * the four ISR threads hammer at 60 Hz (KeInsertQueueDpc 119, KeSetEvent
 * 145, KeDelayExecutionThread 99, KeInitializeDpc 107) all take 12 -- clean
 * 12 where 4 was owed and esp is left exactly +8, which is where the /GS
 * stack-cookie epilogue then reads the wrong slot and aborts. */
static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;

    /* TEMP DIAGNOSTIC (2026-09-04, HeroLab task f096f240): is the steady-state
     * RtlEnterCriticalSection traffic on thread #1 (stack ~0x007FFFxx, the
     * boot thread) still-varying call sites (real forward progress through
     * new code) or the same handful of return addresses forever (a genuine
     * spin)? The <=200 cap below only samples the very start of the run, so
     * also dump a burst of detail every 20000 calls to see the picture deep
     * into the run without flooding the log. */
    if (g_kernel_call_count <= 200 ||
        (g_kernel_call_count % 20000) < 20) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses. */
        fprintf(stderr,
                "  [KERNEL] #%d: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %d total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    uint32_t _watch_before = 0;
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
    }

    if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), "
                    "returning 0 -- called from guest VA 0x%08X\n",
                    ordinal, slot, g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();
    bridge_thread_objs_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries still carrying the ordinal marker are touched, so scanning
     * back over unrelated .rdata is harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000)) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            g_slot_arg_bytes[slot] = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
