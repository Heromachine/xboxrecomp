/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8→D3D11 rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D11 render target + viewport
 *   - Render state (blend, depth, cull) → D3D11 state objects
 *   - Begin/End draw + Inline vertex data → D3D11 DrawPrimitiveUP
 *   - Texture binding → D3D11 shader resource views
 *   - Clear commands → D3D11 ClearRenderTargetView
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D11_H
#define NV2A_PGRAPH_D3D11_H

#include <stdint.h>

/* Initialize the PGRAPH→D3D11 translator. Call after D3D11 device is created. */
void pgraph_d3d11_init(void);

/* Shut down and release resources. */
void pgraph_d3d11_shutdown(void);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Occlusion (ZPASS_PIXEL_CNT) counting for NV097 reports: enable/disable
 * counting, take the samples counted since the last collect (synchronously),
 * and drop any in-flight count. See nv2a_core.c GET_REPORT. */
void pgraph_d3d11_zpass_enable(int enable);
uint32_t pgraph_d3d11_zpass_collect(void);
void pgraph_d3d11_zpass_clear(void);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
    uint32_t vsh_draws;      /* draw_calls that went through the programmable
                              * NV2A vertex shader path (d3d8_vsh) rather
                              * than the fixed-function 2D fallback */
    uint32_t vsh_rebuilds;   /* d3d8_vsh_create_shader() calls -- should be
                              * far smaller than vsh_draws; see the comment
                              * above g_pg.vsh.program_dirty */
    uint32_t array_draws;    /* draws sourced from vertex-data arrays
                              * (ARRAY_ELEMENT16/32, DRAW_ARRAYS) */
    uint32_t array_gather_failures; /* array draws dropped because a vertex
                                     * span did not resolve to guest RAM */
} PgraphD3D11Stats;

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out);

#endif /* NV2A_PGRAPH_D3D11_H */
