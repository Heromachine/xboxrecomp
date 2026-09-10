/*
 * kernel_hal.c - Hardware Abstraction Layer
 *
 * Implements IRQL simulation, performance counters, system time,
 * processor stalls, bug checks, floating point state, and hardware stubs.
 *
 * The Xbox HAL provides low-level hardware access that doesn't exist on
 * a standard Windows PC. Most of these functions are either:
 *   - Directly mappable (perf counters, system time)
 *   - Simulated (IRQL tracking via TLS)
 *   - Stubbed (PCI access, SMC, interrupts)
 */

#include "kernel.h"
#include <stdio.h>
#if defined(_WIN32)
#include <intrin.h>
#endif
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#endif

/* ============================================================================
 * IRQL Simulation
 *
 * Xbox uses IRQL (Interrupt Request Level) for synchronization:
 *   PASSIVE_LEVEL (0) - normal thread execution
 *   APC_LEVEL (1) - APC delivery
 *   DISPATCH_LEVEL (2) - scheduler/DPC level, no page faults allowed
 *
 * On Windows, we simulate IRQL with a thread-local variable. Raising to
 * DISPATCH_LEVEL doesn't actually prevent preemption, but the tracking
 * allows code that checks IRQL to function correctly.
 * ============================================================================ */

static XBOX_THREAD_LOCAL KIRQL g_current_irql = PASSIVE_LEVEL;

/*
 * KfRaiseIrql - Raises IRQL to the specified level.
 * Returns the previous IRQL. Uses __fastcall (ECX = NewIrql).
 */
KIRQL __fastcall xbox_KfRaiseIrql(KIRQL NewIrql)
{
    KIRQL old = g_current_irql;

    if (NewIrql < old) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "KfRaiseIrql: attempt to lower IRQL from %d to %d (use KfLowerIrql)",
            old, NewIrql);
    }

    g_current_irql = NewIrql;
    return old;
}

/*
 * KfLowerIrql - Lowers IRQL to the specified level.
 * Uses __fastcall (ECX = NewIrql).
 */
VOID __fastcall xbox_KfLowerIrql(KIRQL NewIrql)
{
    if (NewIrql > g_current_irql) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "KfLowerIrql: attempt to raise IRQL from %d to %d (use KfRaiseIrql)",
            g_current_irql, NewIrql);
    }

    g_current_irql = NewIrql;
}

/*
 * KeRaiseIrqlToDpcLevel - Convenience function to raise to DISPATCH_LEVEL.
 */
KIRQL __stdcall xbox_KeRaiseIrqlToDpcLevel(void)
{
    KIRQL old = g_current_irql;
    g_current_irql = DISPATCH_LEVEL;
    return old;
}

/* ============================================================================
 * KeTickCount
 *
 * Exported as a data pointer, not a function. The Xbox kernel increments
 * this every ~1ms (approximating the Xbox tick interval).
 * Updated lazily when read, using GetTickCount.
 * ============================================================================ */

volatile ULONG xbox_KeTickCount = 0;

/* Call this periodically or on-demand to update KeTickCount */
static void xbox_update_tick_count(void)
{
    xbox_KeTickCount = GetTickCount();
}

/* ============================================================================
 * Performance Counters
 *
 * Direct 1:1 mapping to Win32 QueryPerformanceCounter/Frequency.
 * Both Xbox and Windows return LARGE_INTEGER.
 * ============================================================================ */

/* Guest RDTSC has a fixed frequency even when the host CPU differs.
 * QueryPerformanceCounter is monotonic on Windows; the POSIX compatibility
 * implementation uses CLOCK_MONOTONIC. Keep the separate KeQueryPerformance*
 * APIs in their matching host units.
 *
 * Split whole seconds from the fractional remainder before multiplying: an
 * absolute counter multiplied by the guest frequency would overflow quickly.
 * Host QPC frequencies used here (Windows clocks or POSIX nanoseconds) keep
 * the fractional product within uint64_t. Querying the frequency also avoids
 * shared lazy-initialization state across guest threads. */
uint64_t recomp_rdtsc64(void)
{
    LARGE_INTEGER counter, frequency;
    const uint64_t guest_hz = 733333333ULL;
    uint64_t ticks, hz;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return 0;
    QueryPerformanceCounter(&counter);
    ticks = (uint64_t)counter.QuadPart;
    hz = (uint64_t)frequency.QuadPart;
    return (ticks / hz) * guest_hz + ((ticks % hz) * guest_hz) / hz;
}

LARGE_INTEGER __stdcall xbox_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter;
}

LARGE_INTEGER __stdcall xbox_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return freq;
}

/* ============================================================================
 * System Time
 *
 * KeQuerySystemTime returns the current time as a FILETIME (100ns since
 * January 1, 1601). Direct Win32 mapping.
 * ============================================================================ */

VOID __stdcall xbox_KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    if (CurrentTime)
        GetSystemTimeAsFileTime((LPFILETIME)CurrentTime);
}

/* ============================================================================
 * Processor Stall
 *
 * KeStallExecutionProcessor performs a busy-wait for the given number
 * of microseconds. Used for hardware timing (e.g., waiting for GPU).
 * ============================================================================ */

VOID __stdcall xbox_KeStallExecutionProcessor(ULONG MicroSeconds)
{
    LARGE_INTEGER freq, start, now;

    if (MicroSeconds == 0)
        return;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    LONGLONG target_counts = (freq.QuadPart * MicroSeconds) / 1000000;

    do {
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) < target_counts);
}

/* ============================================================================
 * Floating Point State
 *
 * Xbox kernel requires saving/restoring FP state when kernel code uses
 * floating point. On Windows user-mode this is handled automatically by
 * the OS, so these are no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeSaveFloatingPointState(PVOID FloatingPointState)
{
    (void)FloatingPointState;
    /* No-op: Windows user-mode preserves FP state across context switches */
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_KeRestoreFloatingPointState(PVOID FloatingPointState)
{
    (void)FloatingPointState;
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Bug Check (Blue Screen of Death)
 *
 * KeBugCheck/KeBugCheckEx are the Xbox equivalent of BSOD. In our
 * recompilation, we log the error and terminate the process.
 * ============================================================================ */

VOID __stdcall xbox_KeBugCheck(ULONG BugCheckCode)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_HAL,
        "*** KeBugCheck: code=0x%08X ***", BugCheckCode);

#ifdef _DEBUG
    DebugBreak();
#endif

    ExitProcess(BugCheckCode);
}

VOID __stdcall xbox_KeBugCheckEx(
    ULONG BugCheckCode,
    ULONG_PTR Param1,
    ULONG_PTR Param2,
    ULONG_PTR Param3,
    ULONG_PTR Param4)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_HAL,
        "*** KeBugCheckEx: code=0x%08X, params=(0x%p, 0x%p, 0x%p, 0x%p) ***",
        BugCheckCode, (void*)Param1, (void*)Param2, (void*)Param3, (void*)Param4);

#ifdef _DEBUG
    DebugBreak();
#endif

    ExitProcess(BugCheckCode);
}

/* ============================================================================
 * HAL PCI Access
 *
 * HalReadWritePCISpace reads/writes PCI configuration space. The Xbox uses
 * this for GPU and southbridge setup. Not needed on Windows - stub it.
 * ============================================================================ */

/* NV2A config space, bus 1 / device 0 / function 0.
 *
 * Only the GPU is modelled. A zeroed buffer (what this returned before) reads
 * as vendor ID 0x0000, so a title probing for the GPU concludes the slot is
 * empty: Breakdown allocates its framebuffers, probes here, finds nothing, and
 * tears the vblank interrupt back down instead of calling AvSetDisplayMode.
 *
 * BAR0 is the register aperture at 0xFD000000 -- the same range
 * xbox_memory_layout.c currently backs as plain RAM, so a title that trusts
 * this and starts driving registers will read zeros rather than GPU state.
 * That is the next gap, not something this function can close. */
static uint32_t nv2a_pci_config_dword(ULONG reg)
{
    switch (reg & 0xFCu) {
    case 0x00: return 0x02A010DEu;  /* device 0x02A0 (NV2A), vendor 0x10DE */
    case 0x04: return 0x02B00007u;  /* status; command: io+mem+bus master */
    case 0x08: return 0x030000A1u;  /* class 03:00:00 VGA, revision A1 */
    case 0x10: return 0xFD000000u;  /* BAR0: MMIO registers */
    case 0x14: return 0xF0000000u;  /* BAR1: framebuffer aperture */
    case 0x2C: return 0x02A010DEu;  /* subsystem id/vendor */
    case 0x3C: return 0x00000103u;  /* interrupt pin A, line 3 */
    default:   return 0x00000000u;
    }
}

VOID __stdcall xbox_HalReadWritePCISpace(
    ULONG BusNumber,
    ULONG SlotNumber,
    ULONG RegisterNumber,
    PVOID Buffer,
    ULONG Length,
    BOOLEAN WritePCISpace)
{
    /* NT packs the slot as device in bits 0-4, function in bits 5-7. */
    ULONG device   = SlotNumber & 0x1Fu;
    ULONG function = (SlotNumber >> 5) & 0x07u;
    int   is_nv2a  = (BusNumber == 1 && device == 0 && function == 0);
    uint32_t dword;
    uint8_t bytes[4];
    ULONG i;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "HalReadWritePCISpace: bus=%u dev=%u fn=%u reg=0x%X len=%u %s%s",
        BusNumber, device, function, RegisterNumber, Length,
        WritePCISpace ? "WRITE" : "READ", is_nv2a ? " [NV2A]" : "");

    if (WritePCISpace || !Buffer || Length == 0)
        return;

    /* All-ones is what real PCI returns for an unpopulated slot. */
    dword = is_nv2a ? nv2a_pci_config_dword(RegisterNumber) : 0xFFFFFFFFu;

    bytes[0] = (uint8_t)(dword);
    bytes[1] = (uint8_t)(dword >> 8);
    bytes[2] = (uint8_t)(dword >> 16);
    bytes[3] = (uint8_t)(dword >> 24);

    for (i = 0; i < Length; i++)
        ((uint8_t *)Buffer)[i] = bytes[(RegisterNumber + i) & 3u];
}

/* ============================================================================
 * Port I/O
 *
 * Backs the IN/OUT instructions the lifter now emits calls for. Before that
 * they hit the lifter's generic unhandled-instruction path, which emits a
 * comment and nothing else -- so `in al, dx` left AL holding whatever the
 * previous instruction had put in eax, and the title acted on a garbage port
 * value that shifted with unrelated code changes.
 *
 * Ground truth for the model is xemu (hw/xbox/acpi_xbox.c), the project's
 * reference emulator: the Xbox PM/ACPI I/O block sits at 0x8000 and its GPIO
 * sub-block at offset 0xC0, so GPIO register 0 is port 0x80C0.
 * ============================================================================ */

#define XBOX_PM_IO_BASE   0x8000u
#define XBOX_PM_GPIO0     (XBOX_PM_IO_BASE + 0xC0u)  /* 0x80C0 */

/* Bit 5 of GPIO 0 is the TV encoder's field pin -- which field of an
 * interlaced frame is being scanned out. xemu alternates it on every read
 * rather than tying it to a clock, and that is what the reference emulator
 * runs titles against, so match it: a caller polling for a field change makes
 * progress instead of spinning. Breakdown's D3D layer reads it, inverts bit 5
 * and stores the boolean (sub_001C68F4 / sub_001C6AF1).
 *
 * Unsynchronised on purpose: concurrent reads can only disagree about which
 * field is current, which is exactly what racing the real pin would do. */
static unsigned g_tv_field_pin;

uint32_t recomp_port_in(uint16_t port, unsigned width)
{
    uint32_t value = 0;

    if (port == XBOX_PM_GPIO0) {
        g_tv_field_pin = (g_tv_field_pin + 1u) & 1u;
        value = g_tv_field_pin << 5;
    } else {
        static int unmodelled_warnings;
        if (unmodelled_warnings < 8) {
            unmodelled_warnings++;
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
                "recomp_port_in: unmodelled port 0x%04X (width %u) -> 0",
                port, width);
        }
    }

    if (width == 1) return value & 0xFFu;
    if (width == 2) return value & 0xFFFFu;
    return value;
}

void recomp_port_out(uint16_t port, uint32_t value, unsigned width)
{
    /* Writing GPIO 0 latches output pins nothing here models; xemu ignores it
     * too, acting only on the aspect-ratio register at GPIO 0x16 (port
     * 0x80D6), which is worth revisiting once there is a display to size. */
    if (port == XBOX_PM_GPIO0)
        return;

    {
        static int unmodelled_warnings;
        if (unmodelled_warnings < 8) {
            unmodelled_warnings++;
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
                "recomp_port_out: unmodelled port 0x%04X = 0x%X (width %u)",
                port, value, width);
        }
    }
}

/* ============================================================================
 * Guest INT 3
 *
 * Backs the __debugbreak() the lifter emits for a guest 0xCC. See the comment
 * on that macro in templates/runtime/recomp_types.h for why this reports and
 * returns rather than trapping.
 *
 * Deduplicated per call site so a break inside a hot loop cannot flood the
 * log. File pointers compare by identity, which is exact here: every site in
 * one translation unit shares the same __FILE__ literal.
 * ============================================================================ */

void recomp_debug_break(const char *file, int line)
{
    enum { MAX_SITES = 64 };
    static const char *seen_file[MAX_SITES];
    static int seen_line[MAX_SITES];
    static int seen_count;
    int i;

    for (i = 0; i < seen_count; i++) {
        if (seen_line[i] == line && seen_file[i] == file)
            return;
    }

    if (seen_count < MAX_SITES) {
        seen_file[seen_count] = file;
        seen_line[seen_count] = line;
        seen_count++;
    }

    fprintf(stderr, "  [GUEST] int3 debug break at %s:%d -- continuing "
            "(hardware ignores it with no debugger attached)\n",
            file ? file : "?", line);
    fflush(stderr);
}

/* ============================================================================
 * HAL Firmware & Shutdown
 *
 * HalReturnToFirmware returns to the Xbox dashboard. For us, this means
 * exit the game cleanly.
 * ============================================================================ */

VOID __stdcall xbox_HalReturnToFirmware(ULONG Routine)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "HalReturnToFirmware: routine=%u (exiting)", Routine);
    /* ExitProcess below never returns, so it also skips atexit -- dump here
     * or the run's ICALL feedback (see recomp_icall_feedback.h) is lost.
     * A title that exits this way instead of crashing is still exactly the
     * kind of run worth capturing: it reached furthest, so it is the run
     * with the most feedback to give. */
#ifdef RECOMP_ICALL_FEEDBACK
    recomp_icall_feedback_dump(RECOMP_ICALL_FEEDBACK_PATH);
#endif
    ExitProcess(0);
}

VOID __stdcall xbox_HalInitiateShutdown(void)
{
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL, "HalInitiateShutdown (exiting)");
    ExitProcess(0);
}

BOOLEAN __stdcall xbox_HalIsResetOrShutdownPending(void)
{
    return FALSE;
}

/* ============================================================================
 * SMC (System Management Controller)
 *
 * HalReadSMCTrayState reads the DVD tray state. No disc tray on PC.
 * ============================================================================ */

ULONG __stdcall xbox_HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount)
{
    /* Tray state: 0x10 = media detected (disc present) */
    if (TrayState)
        *TrayState = 0x10;
    if (TrayStateChangeCount)
        *TrayStateChangeCount = 0;
    return 0; /* Success */
}

/* ============================================================================
 * Software Interrupts
 *
 * Used for APC/DPC delivery on Xbox. Stubbed since we don't have real
 * interrupt-driven DPC delivery.
 * ============================================================================ */

VOID __stdcall xbox_HalClearSoftwareInterrupt(KIRQL RequestIrql)
{
    (void)RequestIrql;
}

VOID __stdcall xbox_HalRequestSoftwareInterrupt(KIRQL RequestIrql)
{
    (void)RequestIrql;
}

VOID __stdcall xbox_HalDisableSystemInterrupt(ULONG BusInterruptLevel, KIRQL Irql)
{
    (void)BusInterruptLevel;
    (void)Irql;
}

ULONG __stdcall xbox_HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql)
{
    (void)BusInterruptLevel;
    if (Irql)
        *Irql = PASSIVE_LEVEL;
    return 0;
}

/* ============================================================================
 * Interrupt Objects
 *
 * Used by DSOUND and other drivers for hardware interrupt handling.
 * Since we replace the audio/graphics subsystems entirely, these are stubs.
 * ============================================================================ */

VOID __stdcall xbox_KeInitializeInterrupt(
    PXBOX_KINTERRUPT Interrupt,
    PVOID ServiceRoutine,
    PVOID ServiceContext,
    ULONG Vector,
    KIRQL Irql,
    ULONG InterruptMode,
    BOOLEAN ShareVector)
{
    (void)Vector;
    (void)InterruptMode;
    (void)ShareVector;

    if (!Interrupt)
        return;

    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->Irql = Irql;
    Interrupt->Connected = FALSE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeInitializeInterrupt: interrupt=%p, routine=%p, vector=%u",
        Interrupt, ServiceRoutine, Vector);
}

BOOLEAN __stdcall xbox_KeConnectInterrupt(PXBOX_KINTERRUPT Interrupt)
{
    if (!Interrupt)
        return FALSE;

    Interrupt->Connected = TRUE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeConnectInterrupt: interrupt=%p (stubbed - no real HW interrupts)",
        Interrupt);

    return TRUE;
}

BOOLEAN __stdcall xbox_KeDisconnectInterrupt(PXBOX_KINTERRUPT Interrupt)
{
    BOOLEAN was_connected;

    if (!Interrupt)
        return FALSE;

    /* Returns the PREVIOUS connected state, not success. */
    was_connected = Interrupt->Connected;
    Interrupt->Connected = FALSE;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "KeDisconnectInterrupt: interrupt=%p was_connected=%d",
        Interrupt, (int)was_connected);

    return was_connected;
}

/* ============================================================================
 * Miscellaneous Port I/O Stubs
 * ============================================================================ */

VOID __stdcall xbox_WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count)
{
    (void)Port;
    (void)Buffer;
    (void)Count;
}

VOID __stdcall xbox_WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    (void)Port;
    (void)Buffer;
    (void)Count;
}

/* ============================================================================
 * System Time (Set)
 *
 * NtSetSystemTime - we don't actually change the system clock, just log it.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtSetSystemTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER PreviousTime)
{
    if (PreviousTime)
        GetSystemTimeAsFileTime((LPFILETIME)PreviousTime);

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
        "NtSetSystemTime: ignored (not setting system clock)");

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Display / AV
 *
 * These are declared in kernel.h for the thunk table but will be fully
 * implemented by the D3D replacement layer. We provide realistic AV pack
 * detection so games can query display capabilities (480p, 720p, widescreen).
 * ============================================================================ */

static ULONG g_av_saved_data_address = 0;
static ULONG g_av_display_mode = 0;

ULONG __stdcall xbox_AvGetSavedDataAddress(void)
{
    return g_av_saved_data_address;
}

VOID __stdcall xbox_AvSendTVEncoderOption(
    PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)
{
    (void)RegisterBase;
    (void)Param;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "AvSendTVEncoderOption: option=0x%02X param=0x%X", Option, Param);

    if (!Result)
        return;

    switch (Option) {
    case AV_OPTION_QUERY_AVPACK:
        /* Report HDTV/Component pack - allows games to offer 480p/720p.
         *
         * Neither extra bit OR'd in below is invented -- both are what
         * Breakdown's OWN embedded video-mode table (a game-data array
         * of {key, width<<16|height, ...} entries at guest 0x1CC860,
         * walked via guest-side tracing at HeroLab task f542e7ac)
         * requires to ever match, confirmed by instrumenting the
         * title's own mode-search (sub_001C6574 -> sub_001C65ED in
         * this title's disassembly) and watching it reject every
         * candidate until the right bits were present:
         *
         *   Bit 8 (0x100): every one of 30 sampled table entries,
         *   spanning pack types 3/4/5 (SCART/HDTV/VGA) and resolutions
         *   from 640x240 to 1920x1080, carries this bit in the same
         *   position. Without it, sub_001C6574's lookup never matches
         *   any real row -- only the 0xFFFFFFFF sentinel past the end
         *   of the table (confirmed: this is what happened before the
         *   fix below existed at all).
         *
         *   Bit 22 (0x400000): needed for a SECOND, deeper check in
         *   sub_001C65ED (`entry_key & eax` where `eax` is derived from
         *   this Result value) that runs even after a table row matches
         *   on width/height and three other bit-tests -- confirmed by
         *   tracing entry index 19 (640x480, HDTV) all the way through
         *   every earlier check successfully, only to fail on exactly
         *   this one because bit 22 was 0. Every HDTV/SCART/VGA table
         *   entry's own key has this bit set too (byte 2 of every
         *   sampled key is >= 0x40), so it reads as a shared "supports
         *   this whole class of modes" flag rather than anything
         *   specific to HDTV -- but that is inference from the guest
         *   data, not confirmed against hardware documentation.
         *
         * Without BOTH bits, the title's mode-search always returns
         * E_FAIL (0x80004005), which is the actual branch that skips
         * display initialization -- AvSetDisplayMode is never reached
         * because of this, not because of the AV pack type itself.
         * Neither bit's real-hardware meaning is confirmed against SDK
         * documentation (none available in this sandbox); both are
         * guest-observed ground truth, not values invented from memory.
         * If display init still doesn't complete with both set, trace
         * sub_001C65ED again rather than assume these two are wrong --
         * the first fix (bit 8 alone) measurably changed the table
         * lookup's outcome without yet being sufficient on its own, and
         * the same could be true here. */
        *Result = AV_PACK_HDTV | 0x100 | 0x400000;
        break;

    case AV_OPTION_QUERY_MODE:
        /* Return current display mode */
        *Result = g_av_display_mode;
        break;

    case AV_OPTION_QUERY_AV_CAPABILITIES:
        /* Report support for 480i, 480p, 720p, and widescreen */
        *Result = AV_FLAGS_HDTV_480i | AV_FLAGS_HDTV_480p
                | AV_FLAGS_HDTV_720p | AV_FLAGS_WIDESCREEN
                | AV_FLAGS_60Hz;
        break;

    case AV_OPTION_QUERY_ENCODER_TYPE:
        /* Conexant CX25871 (common in retail Xboxes) */
        *Result = 4;
        break;

    case AV_OPTION_QUERY_MODE_CAPS:
        /* Same as capabilities for our purposes */
        *Result = AV_FLAGS_HDTV_480i | AV_FLAGS_HDTV_480p
                | AV_FLAGS_HDTV_720p | AV_FLAGS_WIDESCREEN
                | AV_FLAGS_60Hz;
        break;

    case AV_OPTION_SET_MODE:
        g_av_display_mode = Param;
        *Result = 0;
        break;

    case AV_OPTION_BLANK_SCREEN:
    case AV_OPTION_MACROVISION_MODE:
    case AV_OPTION_FLICKER_FILTER:
    case AV_OPTION_ZERO_MODE:
        *Result = 0;
        break;

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL,
            "AvSendTVEncoderOption: unknown option 0x%02X", Option);
        *Result = 0;
        break;
    }
}

VOID __stdcall xbox_AvSetSavedDataAddress(ULONG Address)
{
    g_av_saved_data_address = Address;
}

VOID __stdcall xbox_AvSetDisplayMode(
    PVOID RegisterBase, ULONG Step, ULONG Mode,
    ULONG Format, ULONG Pitch, ULONG FrameBuffer)
{
    (void)RegisterBase;
    (void)Step;
    (void)Format;
    (void)Pitch;
    (void)FrameBuffer;

    g_av_display_mode = Mode;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_HAL,
        "AvSetDisplayMode: step=%u mode=0x%X format=0x%X pitch=%u fb=0x%X",
        Step, Mode, Format, Pitch, FrameBuffer);
}

/* ============================================================================
 * SMBus - HalReadSMBusValue / HalWriteSMBusValue
 *
 * The Xbox SMBus connects the CPU to the System Management Controller (SMC),
 * EEPROM, temperature sensor, and TV encoder. Games use these to detect
 * AV pack type, read EEPROM settings, and check hardware state.
 *
 * We simulate responses for the most commonly queried devices:
 *   - SMC (0x20): firmware version, tray state, AV pack, temperatures
 *   - EEPROM (0xA8): handled separately via ExQueryNonVolatileSetting
 *   - Temperature sensor (0x98): CPU/board temperatures
 * ============================================================================ */

NTSTATUS __stdcall xbox_HalReadSMBusValue(
    UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN ReadWordValue, PULONG DataValue)
{
    if (!DataValue)
        return STATUS_INVALID_PARAMETER;

    *DataValue = 0;

    switch (SlaveAddress) {
    case SMC_SLAVE_ADDRESS:  /* 0x20 - System Management Controller */
        switch (CommandCode) {
        case SMC_CMD_FIRMWARE_VER:
            /* "P01" = production SMC, return 'P' for first byte.
             * Games read version byte-by-byte: P(0x50), 0(0x30), 1(0x31) */
            *DataValue = 0x50; /* 'P' */
            break;
        case SMC_CMD_TRAY_STATE:
            /* 0x60 = media present, tray closed */
            *DataValue = 0x60;
            break;
        case SMC_CMD_AV_PACK:
            /* HDTV/Component pack */
            *DataValue = AV_PACK_HDTV;
            break;
        case SMC_CMD_CPU_TEMP:
            *DataValue = 40; /* 40 degrees C */
            break;
        case SMC_CMD_MB_TEMP:
            *DataValue = 35; /* 35 degrees C */
            break;
        case SMC_CMD_FAN_SPEED:
            *DataValue = 50; /* ~50% fan speed */
            break;
        case SMC_CMD_INTERRUPT_REASON:
            *DataValue = 0;  /* No pending interrupt */
            break;
        case SMC_CMD_ERROR_CODE:
            *DataValue = 0;  /* No error */
            break;
        default:
            xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
                "HalReadSMBusValue: SMC unknown cmd=0x%02X", CommandCode);
            break;
        }
        break;

    case TEMP_SLAVE_ADDRESS:  /* 0x98 - ADM1032 temperature sensor */
        /* CommandCode 0x00 = local temp, 0x01 = remote temp */
        if (CommandCode == 0x00)
            *DataValue = 35;  /* Board: 35C */
        else if (CommandCode == 0x01)
            *DataValue = 40;  /* CPU: 40C */
        else
            *DataValue = 30;
        break;

    case ENCODER_SLAVE_ADDRESS:  /* 0xD4 - TV encoder */
        /* Return 0 for most encoder register reads */
        *DataValue = 0;
        break;

    default:
        xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
            "HalReadSMBusValue: unknown slave=0x%02X cmd=0x%02X",
            SlaveAddress, CommandCode);
        break;
    }

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalReadSMBusValue: slave=0x%02X cmd=0x%02X word=%d -> 0x%X",
        SlaveAddress, CommandCode, ReadWordValue, *DataValue);

    (void)ReadWordValue;
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_HalWriteSMBusValue(
    UCHAR SlaveAddress, UCHAR CommandCode, BOOLEAN WriteWordValue, ULONG DataValue)
{
    (void)WriteWordValue;

    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_HAL,
        "HalWriteSMBusValue: slave=0x%02X cmd=0x%02X word=%d val=0x%X (ignored)",
        SlaveAddress, CommandCode, WriteWordValue, DataValue);

    /* Writes to SMC (LED control, fan speed, etc.) are silently accepted */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * HAL Data Exports
 *
 * Ordinals 40, 41 and 42 are variables, not functions. Games read them
 * directly through the thunk table, so the thunk must hand back the address
 * of real storage -- pointing these at a function is what produced garbage
 * disk metadata before.
 *
 * The strings are counted (Length/MaximumLength), not NUL-terminated, matching
 * the kernel's STRING type. Values describe the virtual disk we present; no
 * real hardware is queried.
 * ============================================================================ */

ULONG xbox_HalDiskCachePartitionCount = 3;

static char g_disk_model[]  = "XBOXRECOMP VIRTUAL HDD";
static char g_disk_serial[] = "XR0000000000";

XBOX_ANSI_STRING xbox_HalDiskModelNumber = {
    sizeof(g_disk_model) - 1,
    sizeof(g_disk_model) - 1,
    g_disk_model
};

XBOX_ANSI_STRING xbox_HalDiskSerialNumber = {
    sizeof(g_disk_serial) - 1,
    sizeof(g_disk_serial) - 1,
    g_disk_serial
};

/*
 * Video mode the SMC reported at boot. 0 lets title code fall back to querying
 * the AV pack, which we answer properly in AvGetSavedDataAddress/SMBus.
 */
ULONG xbox_HalBootSMCVideoMode = 0;

/*
 * IDE channel object. Real kernels export a device object for the ATA channel;
 * drivers only ever pass it back to us, so identity is all that is required.
 */
static ULONG g_idex_channel_data = 0x49444558; /* 'IDEX' */
PVOID xbox_IdexChannelObject = &g_idex_channel_data;

/* ============================================================================
 * Shutdown Notification
 * ============================================================================ */

VOID __stdcall xbox_HalRegisterShutdownNotification(
    PVOID ShutdownRegistration,
    BOOLEAN Register)
{
    /*
     * Registers a callback to run on reboot/shutdown. We never initiate an
     * Xbox-style shutdown -- HalReturnToFirmware terminates the process -- so
     * the callback would never fire. Recorded in the log so a title relying on
     * shutdown cleanup is visible rather than silently ignored.
     */
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_HAL,
        "HalRegisterShutdownNotification: %s registration=%p (never invoked)",
        Register ? "register" : "unregister", ShutdownRegistration);
}

/* ============================================================================
 * Unknown Ordinal Stubs
 * ============================================================================ */

VOID __stdcall xbox_Unknown_8(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 8 called (stubbed)");
}

VOID __stdcall xbox_Unknown_23(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 23 called (stubbed)");
}

VOID __stdcall xbox_Unknown_42(void)
{
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "Unknown ordinal 42 called (stubbed)");
}

/* ============================================================================
 * Debug / Timing
 * ============================================================================ */

VOID __stdcall xbox_DbgBreakPoint(void)
{
    /*
     * Titles call this from assertion paths. Under a debugger this should
     * break; without one, raising a breakpoint exception would terminate the
     * process on a condition the title may well survive. Log loudly and
     * continue, and only actually break when a debugger is attached to catch it.
     */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_HAL, "DbgBreakPoint called by title");

    if (IsDebuggerPresent())
        DebugBreak();
}

ULONGLONG __stdcall xbox_KeQueryInterruptTime(void)
{
    /*
     * Time since boot in NT 100ns units. GetTickCount64 is milliseconds, so
     * scale by 10,000. Resolution is coarser than the real kernel's, but it is
     * monotonic, which is the property callers actually depend on.
     */
    return (ULONGLONG)GetTickCount64() * 10000ULL;
}
