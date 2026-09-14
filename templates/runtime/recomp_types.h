/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes the real guest return address (the VA of
 *   the instruction after the call); ret discards it with esp += 4.
 *   The value is never used to transfer control -- control flow is C
 *   call/return -- but it must be correct because guest code reads it
 *   (__SEH_prolog's scope table, _alloca probes, "mov eax, [esp]").
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
/* math.h is load-bearing, and its absence was invisible.
 *
 * The lifter emits sqrt() and fabs() for fsqrt/fabs -- 146 of them in one of
 * Halo's nine chunks alone -- and now sin/cos/tan/atan2/log2/exp2/fmod for the
 * x87 transcendentals. With no declaration in scope, C89 implicit declaration
 * makes every one of them return `int`: the caller reads EAX instead of XMM0 and
 * gets garbage, then converts that garbage to double. Vector normalisation is
 * 1/sqrt(x), so this corrupts every matrix the title builds.
 *
 * Nothing reported it because generated code is compiled with /w (see the game
 * CMakeLists) -- MSVC's C4013 was emitted and discarded. Same failure as the
 * missing stdlib.h in kernel_bridge.c, in a hotter path. */
#include <math.h>

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* Guest INT 3.
 *
 * A guest int3 is a DEBUG BREAK, not a fatal fault, and modelling it as one
 * was wrong on every host. Retail Xbox code uses int3 as "stop here if a
 * debugger is attached, otherwise carry on", and titles rely on that. The
 * NV2A FIFO error handler in Breakdown is the clear case (guest 0x001C7150):
 *
 *     push 0x1cd110 ; call 0x1ca140   ; print Type/Source/Class/Method/Data
 *     add  esp, 0x20                  ; clean up its arguments
 *     int3                            ; break IF a debugger is attached
 *     mov  [esi+0x2100], 0x1000       ; ...then ACKNOWLEDGE and continue
 *
 * Execution deliberately continues in the same function. On hardware with no
 * debugger attached this is an ordinary assert-and-continue.
 *
 * __builtin_trap() emits ud2 and kills the process; MSVC's intrinsic raises a
 * breakpoint that terminates just the same with nothing attached. Either way
 * an ordinary guest assertion became a hard crash -- and under Wine it became
 * a SILENT HANG, because an unhandled fault parks the process at a debugger
 * prompt with every counter frozen. That cost this project a whole
 * investigation (HeroLab task b7d706a3).
 *
 * So: report the site once and return. This name is deliberately hijacked
 * even on MSVC -- recomp_types.h is included only by translated guest code,
 * so the override is scoped to exactly the construct it is about.
 *
 * int3 ALSO appears as ordinary inter-function padding, where reaching it IS
 * a genuine bug. Logging keeps that case visible rather than silent: the run
 * continues into whatever follows the padding, and the harness's [FATAL]
 * handler names the resulting crash instead of hanging.
 */
#ifdef __debugbreak
#undef __debugbreak
#endif
void recomp_debug_break(const char *file, int line);
#define __debugbreak() recomp_debug_break(__FILE__, __LINE__)

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
/* Per-thread register state.
 *
 * These started as plain globals, which works exactly as long as one thread
 * runs recompiled code. Halo is the first title to create real workers (its
 * cache/file loader), and the runtime papered over that by running every worker
 * synchronously inside PsCreateSystemThreadEx -- so a worker that blocks
 * waiting for work never returns and startup deadlocks.
 *
 * On hardware each thread has its own register set, so model it that way.
 * Thread-local costs an indirection per access; a deadlock costs the title. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* x87 stack. Per-thread for the same reason the integer registers are:
 * arguments are passed in st(0)/st(1) across call boundaries. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
extern RECOMP_TLS uint32_t g_seh_ebp;
extern RECOMP_TLS uint32_t g_ebp;

/* x87 control and status. Thread-local for the same reason the x87 stack
   above is: one guest routine can lift to several C functions, so a compare
   and the FNSTSW that reads it can land in different bodies, and the control
   word has to survive a call. (g_fp_stack/g_fp_top are declared above.) */
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
/* Direction flag. `std; rep movsd; cld` is how the CRT's memmove copies an
 * overlapping block whose destination is above its source: backward, from the
 * last element down. Dropping std copied forward from the END pointers --
 * ecx*4 bytes past the destination, pointers moving the wrong way -- and on
 * Breakdown that smeared text over live objects the first time a menu shuffled
 * a list. Thread-local like the other flags, and clear at thread start as the
 * ABI requires. */
extern RECOMP_TLS int g_df;
#define RECOMP_DF_STEP(n)     (g_df ? (uint32_t)0 - (uint32_t)(n) : (uint32_t)(n))

#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/** Per-VA ICALL watch (see g_kernel_watch_va for the kernel-bridge
 * equivalent). Zero = off. Set to a guest VA to count every RECOMP_ICALL /
 * RECOMP_ICALL_SAFE dispatch to it, resolved or not, in g_icall_watch_count
 * -- durable across a long run, unlike the 16-entry ring buffer above. */
extern volatile uint32_t g_icall_watch_va;
extern volatile uint64_t g_icall_watch_count;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);

/* Indirect-branch target feedback. The ring buffer above is crash forensics --
 * 16 entries, overwritten constantly. This is a durable, deduplicated record of
 * every target the title ever reached, for feeding back into the next codegen
 * run (tools/recomp/icall_feedback.py).
 *
 * The header is pulled in only when the feature is on, so a default build needs
 * neither the file nor src/kernel on its include path. Disabled,
 * RECOMP_ICALL_OBSERVE discards its arguments without expanding them, so the
 * RECOMP_ICALL_SEEN_* constants need not exist either. */
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#endif

/**
 * Function entry trace, emitted only for addresses passed to
 * tools.recomp --trace-functions. Bring-up is largely "which of these
 * init calls does it not come back from", and answering that by
 * overriding a function loses the body you were trying to observe.
 */
void recomp_trace_enter(const char *name, uint32_t va);
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
void recomp_trace_exit(const char *name, uint32_t va);
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
void recomp_trace_esp(const char *name, const char *tag);
#define RECOMP_TRACE_ESP(name, tag) recomp_trace_esp((name), (tag))


/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))
/* 64-bit: MMX movq/qword-operand instructions (movq mm, [addr]; pand mm,
 * qword ptr [addr]; ...). _mem_accessor() in the lifter maps mem_size 8
 * here -- before that mapping existed it silently fell back to MEM32,
 * which would have read only half of a qword operand. */
#define MEM64(addr)  (*(volatile uint64_t *)XBOX_PTR(addr))

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/**
 * fs:[offset] access -- the Xbox TIB/KPCR, a per-thread structure.
 *
 * On real hardware fs:[N] and linear guest address N are two entirely
 * separate pieces of memory. Earlier generated code folded an fs:-prefixed
 * operand into an ordinary MEM32(N)-style access, as if the segment prefix
 * were just another displacement -- so a title's own unrelated use of low
 * linear addresses (e.g. Breakdown does a plain `mov [4], eax` for something
 * that has nothing to do with threading) silently clobbered whatever the
 * fake TIB had stored at that same guest address, corrupting a later,
 * unrelated read of fs:[4] elsewhere in the title. FS8/16/32 read this
 * separate, RECOMP_TLS (genuinely per-thread) backing store instead, so the
 * two can never collide. Host code populates it via xbox_init_fake_tib()
 * (xbox_memory_layout.c) once per thread, main and every spawned worker
 * alike -- see kernel_bridge.c's bridge_thread_main().
 *
 * HeroLab: Xbox Recompiler project, Breakdown sub_001AAC76 investigation
 * (task tracked 2026-09-04). Offsets actually read across Breakdown's own
 * binary: 0, 4, 0x20, 0x24, 0x28, 0x58 -- RECOMP_FAKE_TIB_SIZE below is sized
 * with headroom past that for other titles.
 */
#define RECOMP_FAKE_TIB_SIZE 256
extern RECOMP_TLS uint8_t g_fake_tib[RECOMP_FAKE_TIB_SIZE];

#define FS8(off)   (*(volatile uint8_t  *)(g_fake_tib + (off)))
#define FS16(off)  (*(volatile uint16_t *)(g_fake_tib + (off)))
#define FS32(off)  (*(volatile uint32_t *)(g_fake_tib + (off)))

/* ================================================================
 * SSE / XMM register state
 *
 * XMM is 128 bits of architectural state, not a scalar float. Modelling
 * it as a `float` made movaps/movups transfer 4 of 16 bytes and silently
 * drop the upper three lanes, and left the packed arithmetic with no
 * representation at all.
 *
 * The registers are global for the same reason the volatile GPRs are:
 * one guest routine can lift to several C functions, so a value produced
 * in one body and read in the next has to outlive the body that wrote it.
 * A function-local declaration would also shadow these, and the local
 * starts zeroed -- a returned float would silently read as 0.0.
 *
 * The helpers are lane-wise C rather than host intrinsics: the guest
 * semantics stay explicit (MINPS returning src on unordered, CMPNEQPS
 * being the unordered form) and the header stays portable.
 * ================================================================ */

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

extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* -- construction -- */

static inline RecompXmm XMM_ZERO(void) {
    RecompXmm r; r.q[0] = 0; r.q[1] = 0; return r;
}

/** movss from memory: lane 0 set, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR(float v) {
    RecompXmm r = XMM_ZERO(); r.f[0] = v; return r;
}

/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) {
    RecompXmm r = XMM_ZERO(); r.d[0] = v; return r;
}

/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) {
    RecompXmm r = XMM_ZERO(); r.u[0] = bits; return r;
}

/* -- guest memory --
 * Addresses are guest VAs, so they go through MEM32 like every other
 * access. Done lane-wise, which is also unaligned-safe for movups. */

static inline RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
}

static inline void XMM_STORE(uint32_t addr, RecompXmm v) {
    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
}

/* movlps/movhps move 8 bytes into or out of one half, leaving the
 * other half alone. */
#define XMM_LOAD_LOW(dst, addr)   recomp_xmm_load_half(&(dst), (addr), 0)
#define XMM_LOAD_HIGH(dst, addr)  recomp_xmm_load_half(&(dst), (addr), 1)
#define XMM_STORE_LOW(addr, src)  recomp_xmm_store_half((addr), (src), 0)
#define XMM_STORE_HIGH(addr, src) recomp_xmm_store_half((addr), (src), 1)

static inline void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,
                                        int high) {
    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
}

static inline void recomp_xmm_store_half(uint32_t addr, RecompXmm src,
                                         int high) {
    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
}

/** movlhps: dst high = src low. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r;
}

/** movhlps: dst low = src high. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = b.q[1]; r.q[1] = a.q[1]; return r;
}

/* -- packed arithmetic -- */

#define RECOMP_XMM_LANEWISE(name, expr)                                   \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.f[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when the lanes are unordered or
 * equal -- that is the hardware's tie-break, not a C fmin/fmax. */
RECOMP_XMM_LANEWISE(XMM_MIN, (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])

#define RECOMP_XMM_BITWISE(name, expr)                                    \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.u[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_BITWISE(XMM_AND,  a.u[i] & b.u[i])
RECOMP_XMM_BITWISE(XMM_OR,   a.u[i] | b.u[i])
RECOMP_XMM_BITWISE(XMM_XOR,  a.u[i] ^ b.u[i])
/* ANDNPS is ~dst & src, not dst & ~src. */
RECOMP_XMM_BITWISE(XMM_ANDN, (~a.u[i]) & b.u[i])

/* Compares produce an all-ones or all-zero mask per lane. EQ/LT/LE are
 * the ordered forms (false when either lane is NaN); NEQ is the
 * unordered form, so it is true when a lane is NaN. */
RECOMP_XMM_BITWISE(XMM_CMP_EQ,  (a.f[i] == b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LT,  (a.f[i] <  b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LE,  (a.f[i] <= b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_NEQ, (a.f[i] == b.f[i]) ? 0u : 0xFFFFFFFFu)

/** movmskps: the four lane sign bits, packed into the low nibble. */
static inline uint32_t XMM_MOVEMASK(RecompXmm a) {
    return ((a.u[0] >> 31) & 1u) | (((a.u[1] >> 31) & 1u) << 1)
         | (((a.u[2] >> 31) & 1u) << 2) | (((a.u[3] >> 31) & 1u) << 3);
}

/** shufps: lanes 0-1 selected out of `a`, lanes 2-3 out of `b`. */
static inline RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

/** unpcklps / unpckhps: interleave the low or high halves. */
static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    return r;
}

/** pcmpgtd on XMM operands (integer dword compare -- distinct from the
 * float compares above). Not exercised by any title yet, added for
 * parity with the MMX form below, which real code does use. */
static inline RecompXmm XMM_CMP_GT_D(RecompXmm a, RecompXmm b) {
    RecompXmm r; int i;
    for (i = 0; i < 4; ++i) r.u[i] = (a.i[i] > b.i[i]) ? 0xFFFFFFFFu : 0u;
    return r;
}

/* ================================================================
 * MMX register state
 *
 * Real MMX aliases the x87 FPU register stack: each 64-bit MMX register
 * is the low 64 bits of one x87 data register, and EMMS resets the tag
 * word so the FPU can be used again. This runtime models x87 separately
 * as g_fp_stack[8]/g_fp_top, so routing MMX through it would need EMMS
 * to actually retag those stack slots, and every FPU op to know when it
 * is looking at retired MMX bits rather than a float.
 *
 * That complexity is only needed if a title interleaves x87 float code
 * with MMX in the same live range. Breakdown's XMV movie codec (IDCT,
 * motion compensation, YUV->RGB -- the only MMX user in this title as
 * of this writing) does not: every one of its functions that touches
 * mm0-mm7 was checked for real x87 mnemonics (fld, fst, fadd, ...)
 * anywhere in the same body, and none were found; several call emms
 * exactly once, at the end, as well-formed MMX code should. So mm0-mm7
 * are modelled here as eight independent 64-bit integer registers, not
 * as indices into g_fp_stack. This is an empirical finding about this
 * title's code, not a general MMX truth -- a title that does interleave
 * x87 and MMX inside one function would need the aliased model instead,
 * and should not assume this comment still applies to it.
 *
 * The registers are global RECOMP_TLS state for the same reason the XMM
 * registers above are (see that comment): one guest routine can lift to
 * several C functions, so a value produced in one body and read in the
 * next has to outlive the body that wrote it. mm0-mm7 were previously
 * declared as per-function locals in the generated code (see
 * FunctionTranslator._find_used_xmm / the mmx_regs declaration this
 * replaced) -- exactly the shadowing bug the XMM comment warns about,
 * just not yet observed in practice because nothing exercised real MMX
 * arithmetic across a body split before this change existed.
 *
 * Plain uint64_t (not a lane union like RecompXmm) is deliberate: movd
 * already lifts as a direct scalar read/write of mm0-mm7 (`mm0 =
 * MEM32(ecx);` zero-extends exactly like real MOVD-to-MMX), and
 * register-to-register movq/pand/por/pxor lift the same way (`mm0 =
 * mm2;`, `mm0 &= mm2;`). A union type would make that scalar assignment
 * a type error. Instructions that need a lane view (paddw, punpcklbw,
 * psraw, ...) get one through the MMX_* helpers below, which
 * reinterpret the uint64_t internally via RecompMmxLanes and hand back
 * a uint64_t -- the generated call site never sees a union.
 * ================================================================ */

extern RECOMP_TLS uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS uint64_t g_mm4, g_mm5, g_mm6, g_mm7;

/** movq mm, [addr] / movq [addr], mm / movntq [addr], mm -- qword guest
 * memory access. (movntq's "non-temporal" cache hint has no meaning for
 * this runtime; it is otherwise a plain store.) */
#define MMX_MEM(addr)        MEM64(addr)
#define MMX_STORE(addr, v)   (MEM64(addr) = (uint64_t)(v))

typedef union RecompMmxLanes {
    uint64_t q;
    int64_t  sq;
    uint32_t d[2];
    int32_t  sd[2];
    uint16_t w[4];
    int16_t  sw[4];
    uint8_t  b[8];
    int8_t   sb[8];
} RecompMmxLanes;

/* -- lane-wise arithmetic / compare --
 * Each MMX_* helper takes the two source registers as plain uint64_t
 * (already read out of mm0-mm7 or memory by the caller) and returns the
 * new 64-bit value to assign back, e.g. `mm4 = MMX_PADDW(mm4, mm2);`.
 * Non-saturating adds/subs/multiplies rely on normal C unsigned
 * wraparound to supply the hardware's wraparound-on-overflow -- the
 * result bit pattern is identical whether the lane is read as signed or
 * unsigned, so these do not need a signed/unsigned variant. */

#define RECOMP_MMX_LANEWISE(name, field, count, expr)                    \
    static inline uint64_t name(uint64_t av, uint64_t bv) {              \
        RecompMmxLanes a, b, r; int i;                                   \
        a.q = av; b.q = bv;                                              \
        for (i = 0; i < (count); ++i) { r.field[i] = (expr); }           \
        return r.q;                                                      \
    }

RECOMP_MMX_LANEWISE(MMX_PADDB, b, 8, (uint8_t)(a.b[i] + b.b[i]))
RECOMP_MMX_LANEWISE(MMX_PADDW, w, 4, (uint16_t)(a.w[i] + b.w[i]))
RECOMP_MMX_LANEWISE(MMX_PADDD, d, 2, (uint32_t)(a.d[i] + b.d[i]))
RECOMP_MMX_LANEWISE(MMX_PSUBB, b, 8, (uint8_t)(a.b[i] - b.b[i]))
RECOMP_MMX_LANEWISE(MMX_PSUBW, w, 4, (uint16_t)(a.w[i] - b.w[i]))
RECOMP_MMX_LANEWISE(MMX_PSUBD, d, 2, (uint32_t)(a.d[i] - b.d[i]))
/* PMULLW keeps the low 16 bits of each 16x16 product, which is the same
 * bit pattern whether the inputs are taken as signed or unsigned. */
RECOMP_MMX_LANEWISE(MMX_PMULLW, w, 4,
                     (uint16_t)((uint32_t)a.w[i] * (uint32_t)b.w[i]))
/* PAVGB: unsigned, round-to-nearest average -- (a + b + 1) >> 1. */
RECOMP_MMX_LANEWISE(MMX_PAVGB, b, 8,
                     (uint8_t)(((unsigned)a.b[i] + (unsigned)b.b[i] + 1u) >> 1))
RECOMP_MMX_LANEWISE(MMX_PCMPEQB, b, 8, (a.b[i] == b.b[i]) ? 0xFFu : 0u)
RECOMP_MMX_LANEWISE(MMX_PCMPEQW, w, 4, (a.w[i] == b.w[i]) ? 0xFFFFu : 0u)
RECOMP_MMX_LANEWISE(MMX_PCMPGTB, sb, 8, (a.sb[i] > b.sb[i]) ? 0xFFu : 0u)
RECOMP_MMX_LANEWISE(MMX_PCMPGTW, sw, 4, (a.sw[i] > b.sw[i]) ? 0xFFFFu : 0u)
/* pcmpgtd shares its mnemonic with an SSE2 128-bit form (see
 * _lift_sse's pand/pandn/por/pxor/pcmpgtd handling); this is the MMX
 * (64-bit, two dwords) side of it. */
RECOMP_MMX_LANEWISE(MMX_PCMPGTD, sd, 2, (a.sd[i] > b.sd[i]) ? 0xFFFFFFFFu : 0u)

/** pmaddwd: multiply four signed word lanes pairwise, add adjacent
 * products into two signed dwords. */
static inline uint64_t MMX_PMADDWD(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r;
    a.q = av; b.q = bv;
    r.sd[0] = (int32_t)a.sw[0] * (int32_t)b.sw[0]
            + (int32_t)a.sw[1] * (int32_t)b.sw[1];
    r.sd[1] = (int32_t)a.sw[2] * (int32_t)b.sw[2]
            + (int32_t)a.sw[3] * (int32_t)b.sw[3];
    return r.q;
}

/* -- pack (narrow with saturation) -- */

static inline int16_t recomp_ssat16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}
static inline int8_t recomp_ssat8(int32_t v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}
static inline uint8_t recomp_usat8(int32_t v) {
    if (v > 255) return 255;
    if (v < 0) return 0;
    return (uint8_t)v;
}

/** packssdw dst, src: dst's two signed dwords -> the low two words of
 * the result (signed-saturated), src's two dwords -> the high two. */
static inline uint64_t MMX_PACKSSDW(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r;
    a.q = av; b.q = bv;
    r.sw[0] = recomp_ssat16(a.sd[0]); r.sw[1] = recomp_ssat16(a.sd[1]);
    r.sw[2] = recomp_ssat16(b.sd[0]); r.sw[3] = recomp_ssat16(b.sd[1]);
    return r.q;
}

/** packsswb dst, src: dst's four signed words -> the low four bytes
 * (signed-saturated), src's four words -> the high four. */
static inline uint64_t MMX_PACKSSWB(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r; int i;
    a.q = av; b.q = bv;
    for (i = 0; i < 4; ++i) {
        r.sb[i]     = recomp_ssat8(a.sw[i]);
        r.sb[i + 4] = recomp_ssat8(b.sw[i]);
    }
    return r.q;
}

/** packuswb dst, src: dst's four signed words -> the low four bytes
 * (unsigned-saturated), src's four words -> the high four. */
static inline uint64_t MMX_PACKUSWB(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r; int i;
    a.q = av; b.q = bv;
    for (i = 0; i < 4; ++i) {
        r.b[i]     = recomp_usat8(a.sw[i]);
        r.b[i + 4] = recomp_usat8(b.sw[i]);
    }
    return r.q;
}

/* -- unpack (interleave low or high half) -- */

static inline uint64_t MMX_PUNPCKLBW(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r; int i;
    a.q = av; b.q = bv;
    for (i = 0; i < 4; ++i) { r.b[2*i] = a.b[i]; r.b[2*i+1] = b.b[i]; }
    return r.q;
}
static inline uint64_t MMX_PUNPCKHBW(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r; int i;
    a.q = av; b.q = bv;
    for (i = 0; i < 4; ++i) { r.b[2*i] = a.b[i+4]; r.b[2*i+1] = b.b[i+4]; }
    return r.q;
}
static inline uint64_t MMX_PUNPCKLWD(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r;
    a.q = av; b.q = bv;
    r.w[0] = a.w[0]; r.w[1] = b.w[0]; r.w[2] = a.w[1]; r.w[3] = b.w[1];
    return r.q;
}
static inline uint64_t MMX_PUNPCKHWD(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r;
    a.q = av; b.q = bv;
    r.w[0] = a.w[2]; r.w[1] = b.w[2]; r.w[2] = a.w[3]; r.w[3] = b.w[3];
    return r.q;
}
static inline uint64_t MMX_PUNPCKLDQ(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r;
    a.q = av; b.q = bv;
    r.d[0] = a.d[0]; r.d[1] = b.d[0];
    return r.q;
}
static inline uint64_t MMX_PUNPCKHDQ(uint64_t av, uint64_t bv) {
    RecompMmxLanes a, b, r;
    a.q = av; b.q = bv;
    r.d[0] = a.d[1]; r.d[1] = b.d[1];
    return r.q;
}

/* -- shifts --
 * Real hardware: a shift count greater than the element width zeroes a
 * logical shift's whole destination, or fills an arithmetic shift's
 * destination with each lane's own sign bit. `count` is a runtime
 * uint64_t -- the full width of an mm-register or m64 count operand,
 * or a small immediate the caller widens -- so the "too large" check
 * has to happen before it ever reaches a C shift operator. */

static inline uint64_t MMX_PSLLW(uint64_t av, uint64_t count) {
    RecompMmxLanes a, r; int i;
    if (count > 15) return 0;
    a.q = av;
    for (i = 0; i < 4; ++i) r.w[i] = (uint16_t)(a.w[i] << count);
    return r.q;
}
static inline uint64_t MMX_PSLLD(uint64_t av, uint64_t count) {
    RecompMmxLanes a, r; int i;
    if (count > 31) return 0;
    a.q = av;
    for (i = 0; i < 2; ++i) r.d[i] = a.d[i] << count;
    return r.q;
}
static inline uint64_t MMX_PSLLQ(uint64_t av, uint64_t count) {
    if (count > 63) return 0;
    return av << count;
}
static inline uint64_t MMX_PSRLW(uint64_t av, uint64_t count) {
    RecompMmxLanes a, r; int i;
    if (count > 15) return 0;
    a.q = av;
    for (i = 0; i < 4; ++i) r.w[i] = (uint16_t)(a.w[i] >> count);
    return r.q;
}
static inline uint64_t MMX_PSRLD(uint64_t av, uint64_t count) {
    RecompMmxLanes a, r; int i;
    if (count > 31) return 0;
    a.q = av;
    for (i = 0; i < 2; ++i) r.d[i] = a.d[i] >> count;
    return r.q;
}
static inline uint64_t MMX_PSRLQ(uint64_t av, uint64_t count) {
    if (count > 63) return 0;
    return av >> count;
}
/** psraw: count clamped to 15 rather than branching to an explicit
 * "fill with sign" path -- a 16-bit lane promotes to a 32-bit int for
 * the shift, so its top 16 bits are already sign-extension, and
 * shifting right by 15 (one less than the lane width) spreads the
 * sign bit through every bit the lane will keep after truncation.
 * Clamping instead of using the raw (possibly huge) count also keeps
 * this a well-defined C shift. */
static inline uint64_t MMX_PSRAW(uint64_t av, uint64_t count) {
    RecompMmxLanes a, r; int i;
    int c = (count > 15) ? 15 : (int)count;
    a.q = av;
    for (i = 0; i < 4; ++i) r.sw[i] = (int16_t)(a.sw[i] >> c);
    return r.q;
}
/** psrad: same idea, but a 32-bit lane has no wider promotion to lean
 * on, so the clamp itself (to 31, one less than the width) is what
 * keeps the shift legal while still producing all-sign-bits. */
static inline uint64_t MMX_PSRAD(uint64_t av, uint64_t count) {
    RecompMmxLanes a, r; int i;
    int c = (count > 31) ? 31 : (int)count;
    a.q = av;
    for (i = 0; i < 2; ++i) r.sd[i] = (int32_t)(a.sd[i] >> c);
    return r.q;
}

/** pshufw dst, src, imm8: each 2-bit field of imm8 selects which of
 * src's four words lands in the corresponding destination word. */
static inline uint64_t MMX_PSHUFW(uint64_t av, uint32_t imm) {
    RecompMmxLanes a, r;
    a.q = av;
    r.w[0] = a.w[(imm >> 0) & 3u];
    r.w[1] = a.w[(imm >> 2) & 3u];
    r.w[2] = a.w[(imm >> 4) & 3u];
    r.w[3] = a.w[(imm >> 6) & 3u];
    return r.q;
}

/** cvtps2pi: an xmm source's low two floats -> an mm destination's two
 * signed 32-bit ints, round-to-nearest-even (the default FP rounding
 * mode almost every title runs with; this runtime does not model
 * MXCSR/FPU control-word rounding control). */
static inline uint64_t MMX_CVTPS2PI(RecompXmm src) {
    RecompMmxLanes r;
    r.sd[0] = (int32_t)lrintf(src.f[0]);
    r.sd[1] = (int32_t)lrintf(src.f[1]);
    return r.q;
}

/** cvtpi2ps dst, src: an mm source's two signed 32-bit ints -> the low
 * two lanes of xmm dst; the upper two lanes keep whatever dst already
 * held, exactly like real hardware, so this takes and returns the
 * whole xmm value. */
static inline RecompXmm MMX_CVTPI2PS(RecompXmm dst, uint64_t src) {
    RecompMmxLanes s;
    s.q = src;
    dst.f[0] = (float)s.sd[0];
    dst.f[1] = (float)s.sd[1];
    return dst;
}

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
/* x86 evaluates the signed conditions at the operand width, not at 32 bits.
   The generated code passes LO8/HI8/LO16 sub-register reads straight in, and
   those zero-extend, so recover the width and sign-extend before comparing. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* less */
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* >= */
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* <= */
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* > */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                                     RECOMP_FLAG_WIDTH(a, b)) < 0)  /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return (val << n) | (val >> (32 - n));
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return (val >> n) | (val << (32 - n));
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/**
 * x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits (that is what PF means). Used by the x87 float-compare idiom
 * `fnstsw ax; test ah, mask; jp/jnp`, which is how all pre-SSE code branches on
 * a float comparison. Without a real parity here the branch was hardcoded and
 * every such comparison went one fixed direction.
 */
static inline int recomp_parity8(uint32_t x) {
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* Guest RDTSC runs at the Xbox CPU frequency, not the host CPU frequency.
 * Titles can use a fixed 733,333,333 Hz constant (Breakdown's XMV code does),
 * so returning a raw host TSC makes elapsed time depend on the host machine.
 * The kernel runtime derives this shared clock from monotonic host QPC. */
uint64_t recomp_rdtsc64(void);
#define RECOMP_RDTSC64() recomp_rdtsc64()

/* ================================================================
 * Port I/O (IN / OUT)
 *
 * Same failure shape as rdtsc above: with no lifter case, IN fell
 * through to the "unhandled instruction" path and emitted nothing,
 * so `in al, dx` left AL holding whatever the surrounding code had
 * last put in eax -- a garbage port value that changed with
 * unrelated edits -- and OUT vanished entirely.
 *
 * The model lives in the kernel HAL rather than here because port
 * space is hardware state, not translation: reads must answer with
 * what the Xbox southbridge would return, and some of them change
 * on every read.
 *
 * width is the access width in bytes (1, 2 or 4).
 * ================================================================ */

uint32_t recomp_port_in(uint16_t port, unsigned width);
void     recomp_port_out(uint16_t port, uint32_t value, unsigned width);

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Build the flat, directly-indexed dispatch table.
 *
 * Turns recomp_lookup from a binary search over every translated function into
 * a bounds check and one indexed load -- the C form of Microsoft's
 * `jmp [table + guest_eip*8]`. Call it once at startup, before any recompiled
 * code runs.
 *
 * Entirely optional: if it is never called, or returns 0 because the allocation
 * failed, recomp_lookup keeps using the binary search and everything still
 * works. Costs 8 bytes per byte of guest code span (see
 * recomp_dispatch_flat_bytes), allocated with calloc so the untouched middle
 * stays uncommitted.
 *
 * Returns 1 if the flat table is in use, 0 if the search is.
 */
int recomp_dispatch_init(void);

/** Bytes held by the flat table, or 0 if it was never built. */
size_t recomp_dispatch_flat_bytes(void);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 the guest return address before this macro.
 * If not found, pops it back off to keep the stack balanced.
 *
 * The range check (0x00400000 to 0xFE000000) skips garbage VAs that
 * come from uninitialized vtable pointers. Adjust this range based
 * on your game's .text section boundaries. Kernel thunks at
 * 0xFE000000+ must NOT be blocked.
 *
 * CUSTOMIZE: Change the VA range check to match your game's code range.
 * Your .text section typically spans 0x00010000 to ~0x003XXXXX.
 * Any VA outside .text and below 0xFE000000 is likely garbage.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (_va == g_icall_watch_va) g_icall_watch_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); _fn(); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 */
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (_va == g_icall_watch_va) g_icall_watch_count++; \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); _fn(); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    if (_va == g_icall_watch_va) g_icall_watch_count++; \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); _fn(); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#endif /* RECOMP_TYPES_H */
