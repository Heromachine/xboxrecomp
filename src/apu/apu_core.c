/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Optional cross-thread progress probe for a silent APU stall. */
static bool g_apu_diag_enabled;
static bool g_apu_state_trace;
static uint64_t g_apu_transition_last_ms;

void apu_trace_internal_fectl(uint32_t old, uint32_t value, int voice)
{
    if (!g_apu_state_trace ||
        ((old ^ value) & NV_PAPU_FECTL_FEMETHMODE) == 0)
        return;
    uint64_t now = GetTickCount64();
    uint64_t last = __atomic_load_n(&g_apu_transition_last_ms,
                                    __ATOMIC_RELAXED);
    if (now - last >= 1000 &&
        __atomic_compare_exchange_n(&g_apu_transition_last_ms, &last, now,
                                    false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
        fprintf(stderr, "[APU TRANSITION] ms=%llu idle_voice=%d FECTL "
                "%08X -> %08X\n", (unsigned long long)now,
                voice, old, value);
}
static volatile LONG g_apu_diag_stage;
static volatile LONG g_apu_diag_voice = -1;
static volatile LONG g_apu_diag_frames;
static volatile LONG g_apu_diag_stop;
static HANDLE g_apu_diag_thread;

static void apu_diag_stage(LONG stage)
{
    if (g_apu_diag_enabled)
        InterlockedExchange(&g_apu_diag_stage, stage);
}

void apu_diag_set_voice(int voice)
{
    if (g_apu_diag_enabled)
        InterlockedExchange(&g_apu_diag_voice, voice);
}

static DWORD WINAPI apu_diag_watchdog(LPVOID arg)
{
    LONG prior = -1;
    unsigned still_seconds = 0;
    (void)arg;
    while (!InterlockedCompareExchange(&g_apu_diag_stop, 0, 0)) {
        Sleep(2000);
        if (InterlockedCompareExchange(&g_apu_diag_stop, 0, 0)) break;
        LONG frames = InterlockedCompareExchange(&g_apu_diag_frames, 0, 0);
        if (frames == prior) still_seconds += 2;
        else still_seconds = 0;
        prior = frames;
        if (still_seconds >= 4) {
            fprintf(stderr, "[APU DIAG] stalled %u s frames=%ld stage=%ld voice=%ld\n",
                    still_seconds, (long)frames,
                    (long)InterlockedCompareExchange(&g_apu_diag_stage, 0, 0),
                    (long)InterlockedCompareExchange(&g_apu_diag_voice, 0, 0));
            fflush(stderr);
        }
    }
    return 0;
}

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        uint32_t old_ists = qatomic_or(&d->regs[NV_PAPU_ISTS],
                                       NV_PAPU_ISTS_GINTSTS);
        /* APU traps can arrive much faster than the standalone kernel's
         * periodic ISR poll. Wake the registered handler on a rising edge;
         * otherwise each trap can hold VP/DSP for a full poll interval. */
        void (*callback)(void *opaque) =
            __atomic_load_n(&d->irq_callback, __ATOMIC_ACQUIRE);
        if (!(old_ists & NV_PAPU_ISTS_GINTSTS) && callback)
            callback(__atomic_load_n(&d->irq_opaque, __ATOMIC_RELAXED));
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

void mcpx_apu_set_irq_callback(MCPXAPUState *d,
                               void (*callback)(void *opaque), void *opaque)
{
    if (!d) return;
    __atomic_store_n(&d->irq_opaque, opaque, __ATOMIC_RELAXED);
    __atomic_store_n(&d->irq_callback, callback, __ATOMIC_RELEASE);
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        if (g_apu_state_trace) {
            uint32_t old = qatomic_read(&d->regs[addr]);
            uint32_t mask = addr == NV_PAPU_FECTL ?
                NV_PAPU_FECTL_FEMETHMODE : NV_PAPU_SECTL_XCNTMODE;
            if (((old ^ (uint32_t)val) & mask) != 0) {
                uint64_t now = GetTickCount64();
                uint64_t last = __atomic_load_n(&g_apu_transition_last_ms,
                                                __ATOMIC_RELAXED);
                if (now - last >= 1000 &&
                    __atomic_compare_exchange_n(&g_apu_transition_last_ms,
                                                &last, now, false,
                                                __ATOMIC_RELAXED,
                                                __ATOMIC_RELAXED))
                    fprintf(stderr, "[APU TRANSITION] ms=%llu guest %s "
                            "%08X -> %08X\n", (unsigned long long)now,
                            addr == NV_PAPU_FECTL ? "FECTL" : "SECTL",
                            old, (uint32_t)val);
            }
        }
        qatomic_set(&d->regs[addr], (uint32_t)val);
        qemu_cond_broadcast(&d->cond);
        if (d->resume_event) SetEvent(d->resume_event);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* XAudio2 path. frame_buf already holds this cycle's 256 samples of the
     * title's own audio: mcpx_apu_dsp_frame() wrote the VP mix into it one
     * 32-sample slice per sub-frame (the frame thread zeroes it when the
     * VP pipeline isn't running). Mix the test tone and software voices on
     * top and submit exactly those samples, as xemu's monitor frame does.
     * This used to clear frame_buf first and submit 1024 samples of tone and
     * software mixer only, so nothing the game played ever reached the
     * speakers. */
    if (xa2_is_active()) {
        int chunk = MIXER_FRAME_SAMPLES;

        if (g_test_tone.active && !g_audio_muted) {
            for (int i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        if (g_audio_muted)
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        else
            mixer_render(d->monitor.frame_buf, chunk);

        {
            /* Level meter for logs: peak over ~5 s. */
            static int peak, cycles;
            for (int i = 0; i < chunk; i++) {
                int l = abs(d->monitor.frame_buf[i][0]), r = abs(d->monitor.frame_buf[i][1]);
                if (l > peak) peak = l;
                if (r > peak) peak = r;
            }
            if (++cycles >= 937) {
                fprintf(stderr, "[APU] output peak %d over last ~5 s\n", peak);
                peak = 0;
                cycles = 0;
            }
        }

        xa2_submit_samples((const int16_t *)d->monitor.frame_buf, chunk);
        return;
    }

    if (!g_waveout.initialized) return;

    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    /* Wait if this buffer is still playing (with timeout) */
    int wait_loops = 0;
    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) break;
    }

    /* Fill the large waveOut buffer by rendering multiple 256-sample frames */
    int16_t *out = (int16_t *)g_waveout.bufs[idx];
    int remaining = WAVEOUT_BUF_SAMPLES;
    int out_offset = 0;

    while (remaining > 0) {
        int chunk = (remaining < MIXER_FRAME_SAMPLES) ? remaining : MIXER_FRAME_SAMPLES;

        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

        /* Test tone (skip if muted) */
        if (g_test_tone.active && !g_audio_muted) {
            for (int i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }

        /* Mix software voices (skip if muted) */
        if (!g_audio_muted)
            mixer_render(d->monitor.frame_buf, chunk);

        /* Copy to waveOut buffer */
        memcpy(out + out_offset * 2, d->monitor.frame_buf, chunk * 2 * sizeof(int16_t));
        out_offset += chunk;
        remaining -= chunk;
    }

    /* Submit to waveOut */
    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

static void throttle(MCPXAPUState *d)
{
    if (d->ep_frame_div % 8) {
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    apu_diag_stage(3);
    mcpx_apu_dsp_frame(d, mixbins);
    apu_diag_stage(4);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        apu_diag_stage(0);
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        {
            /* Which branch the frame thread takes, ~every 5 s. The EP frame is
             * 8 sub-frames of 32 samples; the VP must run for each of them or
             * the monitor submits 256 samples of which only some are fresh. */
            static unsigned iters, full, light;
            static unsigned xcnt_off, fe_halted, fe_trapped, fe_other;
            static ULONGLONG last;
            ULONGLONG now = GetTickCount64();
            iters++;
            if (apu_active && !g_test_tone.active) full++; else light++;
            if (!apu_active && g_apu_state_trace) {
                uint32_t mode = fectl & NV_PAPU_FECTL_FEMETHMODE;
                if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF) xcnt_off++;
                else if (mode == NV_PAPU_FECTL_FEMETHMODE_HALTED) fe_halted++;
                else if (mode == NV_PAPU_FECTL_FEMETHMODE_TRAPPED) fe_trapped++;
                else fe_other++;
            }
            if (!last) last = now;
            if (now - last >= 5000) {
                fprintf(stderr, "[APU] frame thread: %u iterations, %u full "
                        "pipeline, %u monitor-only (~5 s)\n", iters, full, light);
                if (g_apu_state_trace)
                    fprintf(stderr, "[APU STATE] SECTL=%08X XCNT=%d FECTL=%08X "
                            "IEN=%08X ISTS=%08X FETFORCE1=%08X "
                            "off=%u halted=%u trapped=%u other=%u mute=%d\n",
                            qatomic_read(&d->regs[NV_PAPU_SECTL]), xcntmode,
                            fectl, qatomic_read(&d->regs[NV_PAPU_IEN]),
                            qatomic_read(&d->regs[NV_PAPU_ISTS]),
                            qatomic_read(&d->regs[NV_PAPU_FETFORCE1]),
                            xcnt_off, fe_halted, fe_trapped, fe_other,
                            g_audio_muted);
                fflush(stderr);
                iters = full = light = 0;
                xcnt_off = fe_halted = fe_trapped = fe_other = 0;
                last = now;
            }
        }

        /* Match xemu's APU clock when the guest stops the VP/DSP engine.
         * Advancing ep_frame_div and submitting silence during a halted or
         * trapped interval makes the output clock run while the guest's
         * voice engine is stopped. The software mixer and test tone still
         * need continuous delivery, so retain the monitor-only path for
         * those host-side sources. */
        if (!apu_active && !g_test_tone.active && !g_mixer_active_count) {
            d->set_irq = true;
            update_irq(d);
            d->set_irq = false;
            /* Wake as soon as the guest resumes FECTL/SECTL. A fixed 5 ms
             * sleep here limits a completion trap to ~200 VP frames/s even
             * when the guest ISR now runs promptly. Keep 5 ms as a fallback
             * for missed signals or a halted guest. */
            qemu_mutex_unlock(&d->lock);
            if (d->resume_event)
                WaitForSingleObject(d->resume_event, 5);
            else
                Sleep(5);
            qemu_mutex_lock(&d->lock);
            continue;
        }

        throttle(d);
        apu_diag_stage(1);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            apu_diag_stage(2);
            se_frame(d);
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer).
             * No VP output this cycle, so start from silence. */
            if (((d->ep_frame_div + 1) % 8) == 0)
                memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
            apu_diag_stage(4);
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }

        /*
         * Raise the APU interrupt. Ported from xemu apu.c:271-286, which this
         * frame thread otherwise mirrors; the port had kept the three terms
         * above but dropped the interrupt half of what they are for, so
         * update_irq() ended up with a single call site -- the guest's own
         * ISTS acknowledge -- and could therefore never raise anything the
         * guest had not already been told about.
         *
         * The consequence was an inert ISR. Breakdown registers an APU
         * interrupt routine and the kernel delivers it at 60 Hz; it reads
         * NV_PAPU_ISTS, finds GINTSTS clear, and returns, every time. That is
         * what left the intro movie unable to advance (HeroLab task 31bb489a).
         *
         * set_notify_status() in apu_vp.c already sets d->set_irq when a voice
         * completes a notify -- the voice-completion path was fully present,
         * with nothing anywhere to consume the flag. This is that consumer.
         *
         * !apu_active is exactly xemu's condition (XCNTMODE_OFF, or FECTL
         * TRAPPED, or FECTL HALTED), reusing the terms already computed above.
         *
         * No bql_lock()/unlock() pair around update_irq() as xemu has: that is
         * QEMU's big lock, which does not exist here. update_irq() touches
         * only d->regs through qatomic ops and pci_irq_assert(), a no-op stub
         * in qemu_shim.h, so calling it under d->lock is safe. The real
         * delivery is not a PCI IRQ line at all -- it is the kernel's existing
         * 60 Hz ISR dispatch reading an ISTS that is finally truthful.
         */
        apu_diag_stage(5);
        if (!apu_active) {
            d->set_irq = true;
        }
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        if (g_apu_diag_enabled)
            InterlockedIncrement(&g_apu_diag_frames);

        /*
         * NOTE: this loop used to end with an unconditional
         * unlock/Sleep(N)/lock here, to give a guest thread blocked in
         * voice_lock() (apu_vp.c) a window to acquire d->lock before this
         * thread re-took it. That was a mitigation, not a fix: re-measured
         * over several clean-rebuild runs, Sleep(10) still let Wine's
         * "RtlpWaitForCriticalSection ... wait timed out (60 sec)" fire on
         * roughly 2 of 3 runs (HeroLab task 0470e51b), and unconditionally
         * sleeping every iteration also throttled this thread to roughly
         * 1/15th of its intended ~1500 Hz cadence regardless of whether the
         * guest was even contending.
         *
         * voice_lock() no longer takes d->lock at all -- it updates
         * d->vp.voice_locked[] atomically instead (see the comment there).
         * That removes the guest's only path to this lock entirely, so
         * there is nothing here for a per-iteration yield to protect
         * against; removing it restores this thread to its intended
         * cadence.
         */
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_apu_diag_enabled = GetEnvironmentVariableA("XBOXRECOMP_APU_TRACE_DEEP", NULL, 0) > 0;
    g_apu_state_trace = GetEnvironmentVariableA("XBOXRECOMP_APU_STATE_TRACE", NULL, 0) > 0;
    g_apu_diag_stop = 0;
    g_apu_diag_frames = 0;
    g_apu_diag_stage = 0;
    g_apu_diag_voice = -1;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;
    d->resume_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);

    /* Dedicate the last logical core to this thread. The main/PFIFO thread
     * is known to be extremely CPU-hungry (task ed053492 -- millions of
     * PGRAPH methods per few seconds) and main.c excludes this same core
     * from its own affinity mask, so this is a genuine reservation, not
     * just a scheduling hint: nothing else the process starts should ever
     * land here. Investigating whether CPU starvation of this thread
     * contributes to the audio corruption in task 2bb0e89d -- this alone
     * is not a fix for that bug, just removes one variable. Single-core
     * machines get no affinity call (GetSystemInfo already reflects them). */
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (si.dwNumberOfProcessors > 1) {
            DWORD_PTR mask = (DWORD_PTR)1 << (si.dwNumberOfProcessors - 1);
            if (SetThreadAffinityMask(d->apu_thread.thread, mask)) {
                fprintf(stderr, "[APU] frame thread pinned to logical core %lu of %lu\n",
                        (unsigned long)(si.dwNumberOfProcessors - 1),
                        (unsigned long)si.dwNumberOfProcessors);
            } else {
                fprintf(stderr, "[APU] SetThreadAffinityMask failed: %lu\n",
                        (unsigned long)GetLastError());
            }
        }
    }

    mcpx_apu_wait_for_idle(d);

    /*
     * Let the frame thread actually run.
     *
     * d->pause_requested starts true and wait_for_idle() above re-asserts it,
     * so at this point the thread is parked in qemu_cond_wait(). In xemu
     * something always takes it back out: mcpx_apu_reset_hold() does
     * wait_for_idle -> reset -> resume, and mcpx_apu_vm_state_change() resumes
     * when the VM starts running. A standalone build has neither a device
     * reset nor a VM state machine, and this port kept only ONE resume call --
     * inside mcpx_apu_play_test_tone(), a debug helper. So unless someone
     * played the test tone, the APU frame thread stayed parked from init for
     * the life of the process: no voice processing, no notify completions, no
     * interrupt sources, no audio.
     *
     * This is the standalone equivalent of "the VM is now running", and it
     * mirrors the wait_for_idle -> resume order xemu's reset path uses.
     * HeroLab task 31bb489a.
     */
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);
    if (g_apu_diag_enabled)
        g_apu_diag_thread = CreateThread(NULL, 0, apu_diag_watchdog, NULL, 0, NULL);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    if (g_apu_diag_thread) {
        InterlockedExchange(&g_apu_diag_stop, 1);
        WaitForSingleObject(g_apu_diag_thread, 3000);
        CloseHandle(g_apu_diag_thread);
        g_apu_diag_thread = NULL;
    }

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    if (d->resume_event) SetEvent(d->resume_event);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);
    if (d->resume_event) CloseHandle(d->resume_event);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    /* GP (0x30000) and EP (0x50000) regions ignored for now */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    g_mixer_voices[slot].active = 0;
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || v->pcm_bytes == 0) return;
    v->looping = looping;
    v->play_offset = 0;
    v->active = 1;
    InterlockedIncrement((volatile LONG *)&g_mixer_active_count);

    /* Wake up APU thread if it was paused */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * play_offset is stored as a 16.16 fixed-point source frame position. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint32_t inc = (uint32_t)(((uint64_t)voice->sample_rate << 16) / 48000);
        uint32_t pos = voice->play_offset; /* 16.16 fixed-point */
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos = 0;
                    src_frame = 0;
                } else {
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = 0;
            } else if (voice->active) {
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
}
