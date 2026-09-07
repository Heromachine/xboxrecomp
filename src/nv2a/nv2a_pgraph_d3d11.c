/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Translates NV2A push buffer methods into D3D8→D3D11 rendering calls.
 * Designed for Xbox static recompilation (xboxrecomp toolkit).
 *
 * INLINE_ARRAY vertex layout is DECODED, not assumed. NV2A has 16 vertex
 * attribute slots (position, weight, normal, diffuse, specular, fog,
 * point size, back-diffuse, back-specular, texture0-3, three reserved --
 * see NV2A_VERTEX_ATTR_* in nv2a_regs.h); the title enables whichever it
 * needs via NV097_SET_VERTEX_DATA_ARRAY_FORMAT (method 0x1760 + slot*4),
 * and for an INLINE_ARRAY draw the per-vertex byte layout is the enabled
 * slots packed back-to-back in slot order, each sized by its own format
 * (float=4B, UB=1B, S1/S32K=2B) times its component count -- exactly the
 * algorithm xemu's pgraph_gl_bind_inline_array() uses (see
 * xemu-fork/hw/xbox/nv2a/pgraph/gl/vertex.c). A prior version of this file
 * hardcoded a 5-dword (X,Y,U,V,Color) layout as a guess; it happened to
 * match the total dword count for the simplest observed draw (3 floats
 * position + 2 floats texcoord0 = 5 dwords) but attributed the dwords to
 * the wrong fields (no per-vertex diffuse is actually present -- see
 * NV2A_VERTEX_ATTR_DIFFUSE handling below), and broke completely on any
 * draw using more attributes (a 4-texture-stage quad was 11 dwords/vertex,
 * read as 8 vertices at stride 5 instead of the real 4 at stride 11).
 */

#include "nv2a_pgraph_d3d11.h"
#include "nv2a_regs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

/* D3D8 device — we include the full header for COM vtable access */
#include "../d3d/d3d8_xbox.h"
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);

/* NV2A programmable vertex shader translator (microcode -> HLSL). See the
 * "Programmable vertex shader" block below for why this file now feeds it:
 * every draw in this title uses SET_TRANSFORM_PROGRAM/_CONSTANT, never the
 * fixed-function pipeline, and this translator already existed but nothing
 * upstream of it ever called in from the raw NV2A pushbuffer path. */
#include "../d3d/d3d8_vsh.h"

/* Global.txd texture lookup */
/* Game-specific texture lookup - only available when GAME_HAS_FONT_ATLAS is defined */
#ifdef GAME_HAS_FONT_ATLAS
typedef struct { char name[24]; IDirect3DTexture8 *texture; uint32_t width, height, format; } TXD_Entry;
typedef struct { TXD_Entry entries[512]; int count; } TXD_Dict;
extern TXD_Dict g_global_txd;
extern int g_textures_loaded;
extern IDirect3DTexture8 *txd_find(const TXD_Dict *dict, const char *name);
#else
static int g_textures_loaded = 0;
#endif

/* Font atlas DXT5 data - game-specific, only available in burnout3 */
#ifdef GAME_HAS_FONT_ATLAS
#include "font_atlas_data.h"
#endif

/* Create a D3D8 texture from raw DXT5 data */
static IDirect3DTexture8 *create_dxt5_texture(IDirect3DDevice8 *dev,
    uint32_t width, uint32_t height, const void *dxt5_data, uint32_t data_size)
{
    IDirect3DTexture8 *tex = NULL;
    /* D3DFMT_DXT5 = 0x35545844 ('DXT5') on Xbox, mapped to DXGI_FORMAT_BC3 in our layer.
     * Our d3d8 layer uses format code 0x0F for DXT5. */
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, width, height, 1,
        0 /*Usage*/, 0x0F /*DXT5*/, 0 /*D3DPOOL_DEFAULT*/, &tex);
    if (hr != 0 || !tex) {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to create font atlas texture: hr=0x%08X\n", hr);
        return NULL;
    }

    /* Lock and fill with DXT5 data */
    D3DLOCKED_RECT lr = {0};
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (hr == 0 && lr.pBits) {
        memcpy(lr.pBits, dxt5_data, data_size);
        tex->lpVtbl->UnlockRect(tex, 0);
        fprintf(stderr, "[PGRAPH-D3D11] Created font atlas: %ux%u DXT5 (%u bytes)\n",
                width, height, data_size);
    } else {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to lock font atlas: hr=0x%08X\n", hr);
    }
    return tex;
}

/* ══════════════════════════════════════════════════════════════════════
 * NV2A method constants
 *
 * These come from nv2a_regs.h, included above. A local block used to redefine
 * a 21-name subset of them here, and eight of the twenty-one were wrong --
 * CLEAR_SURFACE and the whole clear group were written 0x1BC4 low (0x01D0
 * rather than 0x1D94), so nothing ever cleared the target and the real methods
 * fell into an ignore range labelled "Combiners". DEPTH_TEST_ENABLE,
 * CULL_FACE_ENABLE, SHADE_MODE and TEXTURE_CONTROL0 were wrong too. The
 * redefinitions shadowed the correct values silently. Take them from the
 * header; do not restate them.
 * ══════════════════════════════════════════════════════════════════════ */

/* NV2A draw modes → D3D primitive types */
static int nv2a_draw_mode_to_d3d(uint32_t mode) {
    switch (mode) {
        case 1:  return D3DPT_POINTLIST;
        case 2:  return D3DPT_LINELIST;
        case 3:  return D3DPT_LINESTRIP;  /* LINE_LOOP → LINE_STRIP */
        case 4:  return D3DPT_LINESTRIP;
        case 5:  return D3DPT_TRIANGLELIST;
        case 6:  return D3DPT_TRIANGLESTRIP;
        case 7:  return D3DPT_TRIANGLEFAN;
        case 8:  return D3DPT_TRIANGLELIST; /* QUADS → TRI_LIST (needs conversion) */
        default: return D3DPT_TRIANGLELIST;
    }
}

/* NV2A blend factors → D3D blend */
static uint32_t nv2a_blend_to_d3d(uint32_t nv) {
    switch (nv) {
        case 0x0000: return D3DBLEND_ZERO;
        case 0x0001: return D3DBLEND_ONE;
        case 0x0302: return D3DBLEND_SRCALPHA;
        case 0x0303: return D3DBLEND_INVSRCALPHA;
        default:     return D3DBLEND_ONE;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Translator State
 * ══════════════════════════════════════════════════════════════════════ */

/* Inline vertex accumulator capacity, in dwords. The largest observed draw
 * so far uses 11 dwords/vertex (position xyz + 4 texture stages), well
 * under this; sized generously since actual per-vertex stride now varies
 * with whatever the title enables, not a fixed guess. */
#define MAX_INLINE_DWORDS (16384 * 4)

/* RwIm2DVertex-compatible output vertex (28 bytes) */
typedef struct {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} OutputVertex;

/* Per-slot state mirrored from NV097_SET_VERTEX_DATA_ARRAY_FORMAT, one
 * entry per of the 16 NV2A vertex attribute slots (NV2A_VERTEX_ATTR_* in
 * nv2a_regs.h). count == 0 means the title has not enabled this slot, so
 * it contributes nothing to the INLINE_ARRAY per-vertex byte layout. */
typedef struct {
    uint32_t format;  /* NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_* */
    uint32_t count;   /* component count (the FORMAT register's SIZE field) */
    uint32_t size;     /* bytes per component, derived from format */
} VertexAttrFmt;

static struct {
    /* Draw state */
    int in_draw;           /* Between BEGIN and END */
    uint32_t draw_mode;    /* NV2A draw mode (0=end, 6=tristrip, etc.) */
    int d3d_prim_type;     /* Translated D3D prim type */

    /* Inline vertex accumulator */
    uint32_t inline_data[MAX_INLINE_DWORDS];
    uint32_t inline_count; /* Number of dwords accumulated */

    /* Real vertex attribute layout, decoded from the title's own
     * NV097_SET_VERTEX_DATA_ARRAY_FORMAT writes -- see the file header. */
    VertexAttrFmt vattr[16];

    /* "Current" diffuse color, for draws where NV2A_VERTEX_ATTR_DIFFUSE is
     * not one of the enabled attributes (observed to be the common case --
     * these titles set color once via NV097_SET_DIFFUSE_COLOR4UB rather
     * than per vertex). Defaults to opaque white. Real NV2A hardware
     * defaults an unset vertex attribute to (0,0,0,1) -- opaque BLACK --
     * which through this translator's MODULATE texture stage would zero
     * out every textured draw; if colors still look wrong after this fix,
     * check whether the title actually calls SET_DIFFUSE_COLOR4UB for
     * these draws before assuming this default is the right one. */
    uint32_t current_diffuse;

    /* Clear state */
    uint32_t clear_color;
    uint32_t clear_rect_h;  /* (width << 16) | x */
    uint32_t clear_rect_v;  /* (height << 16) | y */

    /* Render state cache */
    int depth_test;
    int blend_enable;
    uint32_t blend_sfactor;
    uint32_t blend_dfactor;
    int cull_enable;
    int alpha_test;
    uint32_t color_mask;

    /* Viewport */
    float vp_offset[4];
    float vp_scale[4];
    uint32_t surface_clip_h;
    uint32_t surface_clip_v;

    /* ── Programmable vertex shader (NV2A "PROGRAM" transform mode) ──
     *
     * Breakdown never uses the fixed-function pipeline -- every draw runs
     * SET_TRANSFORM_EXECUTION_MODE (0x1E94) MODE=2 (see xemu hw/xbox/nv2a/
     * pgraph/glsl/vsh.c: MODE==2 is vertex program, MODE==0 is fixed
     * function) and uploads its own microcode/constants via SET_TRANSFORM_
     * PROGRAM (0x0B00) / SET_TRANSFORM_CONSTANT (0x0B80). Both are 32-dword
     * (8 instruction / 8 constant) load-pointer windows -- mirrors xemu's
     * pgraph.c DEF_METHOD_INC handling for these two methods exactly:
     * `slot % 4` gives the dword's position within the current 128-bit
     * instruction or float4 constant, and a persistent load-pointer
     * register (set by SET_TRANSFORM_PROGRAM_LOAD / _CONSTANT_LOAD, not
     * derived from the method address) tracks the absolute index and
     * auto-increments only after the 4th dword lands -- see
     * vsh_program_write()/vsh_constant_write() below. */
    struct {
        uint32_t program_data[NV2A_VS_MAX_INSTRUCTIONS][4]; /* raw microcode */
        int      program_load;    /* current instruction slot (persists
                                    * across pushbuffer commands) */
        int      program_length;  /* highest instruction slot touched + 1;
                                    * upper bound passed to d3d8_vsh_create_
                                    * shader() (its own parser stops at the
                                    * real final-instruction flag, so a
                                    * generous bound here is harmless) */
        int      program_dirty;   /* at least one instruction dword landed
                                    * since program_shadow was last taken --
                                    * a cheap pre-check so most draws skip
                                    * even looking at the microcode; see
                                    * program_shadow below for the real
                                    * "did it actually change" test */
        uint32_t program_shadow[NV2A_VS_MAX_INSTRUCTIONS][4]; /* microcode as
                                    * of the last shader_handle (re)build.
                                    * Measured in practice: this title
                                    * reissues byte-identical microcode
                                    * before most draws (same shader, same
                                    * object type), so program_dirty alone
                                    * rebuilds ~1 draw in 3 for nothing --
                                    * memcmp against this before actually
                                    * deleting/recreating the handle turns
                                    * that into a rare event instead of a
                                    * steady-state cost. The D3DCompile
                                    * itself is separately hash-cached
                                    * inside d3d8_vsh.c regardless, so this
                                    * is about avoiding needless slot churn/
                                    * parsing, not avoiding recompiles. */
        int      program_shadow_length;
        uint32_t shader_handle;    /* current d3d8_vsh handle, 0 = none yet */
        uint16_t inputs_read;      /* v# bitmask of shader_handle's program,
                                     * from d3d8_vsh_parse(); drives how the
                                     * draw-time vertex buffer is packed */

        uint32_t const_accum[4];  /* in-progress float4 (raw bit patterns) */
        int      const_load;      /* hardware constant slot, set directly by
                                    * SET_TRANSFORM_CONSTANT_LOAD. Hardware
                                    * slot 96 == D3D8 API register c0 (this
                                    * title writes CONSTANT_LOAD=96) --
                                    * d3d8_vsh_set_constant() is API-relative
                                    * (indexes c[0..191] directly), so the
                                    * -96 bias is applied once, here. */

        int transform_mode;        /* last SET_TRANSFORM_EXECUTION_MODE MODE
                                     * field (2=program, 0=fixed). Read at
                                     * actual draw time in submit_draw(),
                                     * never cached earlier -- the raw
                                     * register write alternates 2/0 through
                                     * an upload sequence, so sampling it at
                                     * write time (rather than at the BEGIN/
                                     * END that follows) misclassifies draws. */
    } vsh;

    /* Texture state per stage (4 stages) */
    struct {
        uint32_t offset;     /* NV2A VRAM offset (method 0x1B00) */
        uint32_t format;     /* Format register (method 0x1B04) */
        uint32_t control0;   /* Control0 register (method 0x1B08) */
        int enabled;         /* Decoded from control0 bit 30 */
    } tex[4];

    /* Cached texture pointers */
    void *menu_texture;           /* IDirect3DTexture8* from Global.txd */
    IDirect3DTexture8 *font_atlas; /* Created from captured DXT5 data */
    int texture_lookup_done;

    /* Stats */
    PgraphD3D11Stats stats;

    /* Chyron scroll */
    float chyron_scroll_offset;  /* Pixels to shift X for chyron text */

    /* Init flag */
    int initialized;
} g_pg;

/* ══════════════════════════════════════════════════════════════════════
 * Float/uint32 conversion
 * ══════════════════════════════════════════════════════════════════════ */
static float u2f(uint32_t u) {
    union { float f; uint32_t i; } x;
    x.i = u;
    return x.f;
}

/* ══════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════ */

void pgraph_d3d11_init(void)
{
    memset(&g_pg, 0, sizeof(g_pg));
    g_pg.current_diffuse = 0xFFFFFFFF;  /* opaque white; see struct comment */
    g_pg.clear_color = 0xFF000000;
    g_pg.color_mask = 0x01010101;
    g_pg.initialized = 1;

    fprintf(stderr, "[PGRAPH-D3D11] Translator initialized\n");
}

void pgraph_d3d11_shutdown(void)
{
    g_pg.initialized = 0;
    fprintf(stderr, "[PGRAPH-D3D11] Translator shut down (draws=%u, verts=%u)\n",
            g_pg.stats.draw_calls, g_pg.stats.vertices_submitted);
}

/* ══════════════════════════════════════════════════════════════════════
 * Vertex attribute decode
 *
 * Mirrors xemu's pgraph_gl_bind_inline_array() / pgraph_gl_bind_vertex_
 * attributes() (hw/xbox/nv2a/pgraph/gl/vertex.c): walk the 16 attribute
 * slots in order, and for every one the title has enabled (count != 0),
 * pack it into the per-vertex byte layout at its own size/alignment. The
 * INLINE_ARRAY stream IS that packed layout -- there is no separate
 * "stride register" for it, unlike a VRAM-sourced vertex array.
 * ══════════════════════════════════════════════════════════════════════ */

#define ROUND_UP_TO(x, y) ((((x) + (y) - 1) / (y)) * (y))

/* Returns total per-vertex stride in bytes; fills byte_offset[slot] for
 * every enabled slot (undefined for disabled slots -- callers must check
 * vattr[slot].count first). */
static uint32_t compute_vertex_layout(uint32_t byte_offset[16])
{
    uint32_t offset = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t count = g_pg.vattr[i].count;
        if (count == 0)
            continue;
        uint32_t size = g_pg.vattr[i].size ? g_pg.vattr[i].size : 4;
        offset = ROUND_UP_TO(offset, size);
        byte_offset[i] = offset;
        offset += size * count;
        offset = ROUND_UP_TO(offset, size);
    }
    return offset;
}

/* Read one component at byte pointer p, per NV097_SET_VERTEX_DATA_ARRAY_
 * FORMAT_TYPE_*. UB formats are normalized unsigned byte (0..255 -> 0..1);
 * S1 is normalized signed 16-bit; S32K is raw (unnormalized) signed 16-bit,
 * used for things like skin weight indices rather than colors/positions --
 * not expected on position/texcoord/diffuse but decoded correctly if seen.
 * CMP (packed 11/11/10 normal) is not unpacked here: nothing this 2D
 * translator reads (position, texcoord0, diffuse) can legally use it per
 * the array-format assertions in xemu's own SET_VERTEX_DATA_ARRAY_FORMAT
 * handler, so a real occurrence would mean this translator has grown a
 * new consumer, not that this is silently wrong. */
static float read_component_as_float(uint32_t format, const uint8_t *p)
{
    uint32_t u;
    int16_t s16;
    switch (format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        memcpy(&u, p, 4);
        return u2f(u);
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        memcpy(&s16, p, 2);
        return s16 / 32767.0f;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        memcpy(&s16, p, 2);
        return (float)s16;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        return p[0] / 255.0f;
    default:
        return 0.0f;
    }
}

/* Decode up to 4 components of attribute `slot` for the vertex starting at
 * vbase, using this draw's byte_offset[] (from compute_vertex_layout()) to
 * find where `slot` actually lands within the vertex -- every enabled
 * attribute before it in slot order pushes it further into the vertex, so
 * this must NOT simply read from vbase for every slot. Missing components
 * default to (0,0,0,1), the same default NV2A uses for an under-specified
 * attribute (e.g. a 3-component position gets an implicit w=1). Does
 * nothing (leaves out[] at caller's defaults) if the slot is disabled. */
static void decode_attr_floats(const uint8_t *vbase, const uint32_t byte_offset[16],
                                int slot, float out[4])
{
    uint32_t count = g_pg.vattr[slot].count;
    if (count == 0)
        return;
    uint32_t format = g_pg.vattr[slot].format;
    uint32_t size = g_pg.vattr[slot].size ? g_pg.vattr[slot].size : 4;
    const uint8_t *p = vbase + byte_offset[slot];
    for (uint32_t c = 0; c < 4; c++) {
        out[c] = (c < count) ? read_component_as_float(format, p + c * size)
                              : (c == 3 ? 1.0f : 0.0f);
    }
}

/* Decode the DIFFUSE attribute as a packed D3DCOLOR (0xAARRGGBB) dword. */
static uint32_t decode_diffuse_color(const uint8_t *vbase, const uint32_t byte_offset[16], int slot)
{
    uint32_t format = g_pg.vattr[slot].format;
    uint32_t count = g_pg.vattr[slot].count;
    const uint8_t *p = vbase + byte_offset[slot];
    if (format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D && count == 4) {
        /* UB_D3D's byte order on NV2A IS D3DCOLOR's byte order (xemu binds
         * it as GL_BGRA for exactly this reason -- see gl/vertex.c). A
         * native little-endian read of the 4 bytes reconstructs the
         * 0xAARRGGBB value directly; no repacking needed. */
        uint32_t c;
        memcpy(&c, p, 4);
        return c;
    }
    /* UB_OGL is (R,G,B,A) byte order -- the reverse of D3DCOLOR -- and any
     * float-typed diffuse stream needs component->byte packing regardless.
     * Decode generically and repack. */
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    decode_attr_floats(vbase, byte_offset, slot, rgba);
    uint32_t r = (uint32_t)(rgba[0] * 255.0f + 0.5f) & 0xFF;
    uint32_t g = (uint32_t)(rgba[1] * 255.0f + 0.5f) & 0xFF;
    uint32_t b = (uint32_t)(rgba[2] * 255.0f + 0.5f) & 0xFF;
    uint32_t a = (uint32_t)(rgba[3] * 255.0f + 0.5f) & 0xFF;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ══════════════════════════════════════════════════════════════════════
 * Programmable vertex shader: microcode/constant accumulation, and the
 * vertex-buffer packer that matches d3d8_vsh's generic input layout (see
 * the g_pg.vsh struct comment above for the load-pointer semantics).
 * ══════════════════════════════════════════════════════════════════════ */

/* SET_TRANSFORM_PROGRAM (0x0B00): accumulate raw 128-bit microcode.
 * `method` only ever contributes `slot % 4` (this dword's position within
 * the current instruction); the absolute instruction index comes from the
 * persistent g_pg.vsh.program_load, which a single pushbuffer command can
 * carry across many 32-dword windows -- see the struct comment. */
static void vsh_program_write(uint32_t method, uint32_t param)
{
    int slot = (int)((method - NV097_SET_TRANSFORM_PROGRAM) / 4);
    int word = slot % 4;

    if (g_pg.vsh.program_load < 0 ||
        g_pg.vsh.program_load >= NV2A_VS_MAX_INSTRUCTIONS) {
        /* Would corrupt an adjacent array rather than just this title's
         * shader -- drop and say so once, instead of silently either. */
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "[PGRAPH-D3D11] VSH program_load %d out of range "
                    "(max %d); dropping upload -- bad SET_TRANSFORM_PROGRAM_"
                    "LOAD (0x1E9C)?\n",
                    g_pg.vsh.program_load, NV2A_VS_MAX_INSTRUCTIONS - 1);
            warned = 1;
        }
        return;
    }

    g_pg.vsh.program_data[g_pg.vsh.program_load][word] = param;
    if (word == 3) {
        g_pg.vsh.program_load++;
        if (g_pg.vsh.program_load > g_pg.vsh.program_length)
            g_pg.vsh.program_length = g_pg.vsh.program_load;
        g_pg.vsh.program_dirty = 1;
    }
}

/* SET_TRANSFORM_CONSTANT (0x0B80): same load-pointer pattern, for float4
 * constants. Written straight through to d3d8_vsh_set_constant() as each
 * float4 completes, rather than mirrored into a second local array --
 * that function's own dirty flag means the actual GPU constant buffer
 * upload still happens at most once per draw (in d3d8_vsh_prepare_draw(),
 * called from dev_DrawPrimitiveUP()), no matter how many individual
 * constant writes land here in between. */
static void vsh_constant_write(uint32_t method, uint32_t param)
{
    int slot = (int)((method - NV097_SET_TRANSFORM_CONSTANT) / 4);
    int word = slot % 4;

    g_pg.vsh.const_accum[word] = param;
    if (word == 3) {
        int api_reg = g_pg.vsh.const_load - 96;  /* Xbox c0 == hw slot 96 */
        if (api_reg >= 0 && api_reg < NV2A_VS_MAX_CONSTANTS) {
            float f4[4];
            f4[0] = u2f(g_pg.vsh.const_accum[0]);
            f4[1] = u2f(g_pg.vsh.const_accum[1]);
            f4[2] = u2f(g_pg.vsh.const_accum[2]);
            f4[3] = u2f(g_pg.vsh.const_accum[3]);
            d3d8_vsh_set_constant(api_reg, f4, 1);

            /* Opt-in instrument, same env var as d3d8_vsh.c's HLSL dump --
             * lets the two be read side by side (which c[] registers a
             * shader reads, and what values actually landed there) instead
             * of guessing at a wrong transform from pixel output alone.
             * Capped so it does not become the same 336k-line flood the
             * bug report measured. */
            if (getenv("XBOXRECOMP_VSHDUMP")) {
                static int shown = 0;
                if (shown < 64) {
                    fprintf(stderr, "[PGRAPH-D3D11] VSH const c[%d] (hw %d) = "
                            "(%.4f, %.4f, %.4f, %.4f)\n",
                            api_reg, g_pg.vsh.const_load, f4[0], f4[1], f4[2], f4[3]);
                    shown++;
                }
            }
        } else {
            static int warned = 0;
            if (!warned) {
                fprintf(stderr, "[PGRAPH-D3D11] VSH const hw slot %d (api %d, "
                        "bias -96) outside 0-%d; ignoring\n",
                        g_pg.vsh.const_load, api_reg, NV2A_VS_MAX_CONSTANTS - 1);
                warned = 1;
            }
        }
        g_pg.vsh.const_load++;
    }
}

/* Compute the tightly-packed per-vertex byte layout d3d8_vsh's generic
 * input-layout builder (create_vsh_input_layout()/d3d8_vsh_default_input_
 * format() in src/d3d/d3d8_vsh.c) will bind for a given input-register
 * bitmask -- same algorithm, so the vertex buffer assembled below lands on
 * the bytes the compiled shader actually reads. */
static uint32_t compute_vsh_vertex_layout(uint16_t inputs_read, uint32_t dst_offset[16])
{
    uint32_t offset = 0;
    for (int i = 0; i < 16; i++) {
        if (!(inputs_read & (1u << i)))
            continue;
        dst_offset[i] = offset;
        offset += d3d8_vsh_input_format_size(d3d8_vsh_default_input_format(i));
    }
    return offset;
}

/* Convert one source vertex -- decoded per the title's REAL declared
 * SET_VERTEX_DATA_ARRAY_FORMAT layout, via the same decode_attr_floats()/
 * decode_diffuse_color() the fixed-function path uses -- into the active
 * shader's expected input layout at `dst`. Only the destination packing
 * differs from the legacy 2D path; the source decode is unchanged. */
static void convert_vsh_vertex(const uint8_t *src_vb, const uint32_t src_offset[16],
                                uint16_t inputs_read, uint8_t *dst)
{
    uint32_t dst_off = 0;
    for (int i = 0; i < 16; i++) {
        if (!(inputs_read & (1u << i)))
            continue;
        DXGI_FORMAT fmt = d3d8_vsh_default_input_format(i);
        UINT size = d3d8_vsh_input_format_size(fmt);

        if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
            /* Packed-color slot (diffuse/specular/back-specular). NV2A
             * UB_D3D is already D3DCOLOR byte order (see decode_diffuse_
             * color()'s comment) -- no repacking needed beyond what that
             * function does. current_diffuse is only a meaningful fallback
             * for the DIFFUSE slot; an unused specular-family slot defaults
             * to 0, matching NV2A's (0,0,0,0) default for an attribute the
             * title never declared. */
            uint32_t packed = 0;
            if (g_pg.vattr[i].count) {
                packed = decode_diffuse_color(src_vb, src_offset, i);
            } else if (i == NV2A_VERTEX_ATTR_DIFFUSE) {
                packed = g_pg.current_diffuse;
            }
            memcpy(dst + dst_off, &packed, 4);
        } else {
            float f4[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            decode_attr_floats(src_vb, src_offset, i, f4);
            memcpy(dst + dst_off, f4, size);
        }
        dst_off += size;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Draw Submission
 * ══════════════════════════════════════════════════════════════════════ */

static void submit_draw(void)
{
    if (g_pg.inline_count == 0)
        return;

    /* Decode the real per-vertex layout from what the title actually
     * programmed via SET_VERTEX_DATA_ARRAY_FORMAT -- see the block comment
     * above compute_vertex_layout(). This replaces a former hardcoded
     * 5-dword (X,Y,U,V,Color) assumption that only accidentally matched
     * the simplest draw's dword count and was wrong for everything else. */
    uint32_t byte_offset[16];
    uint32_t stride_bytes = compute_vertex_layout(byte_offset);
    if (stride_bytes == 0)
        return;  /* Title has not enabled any attribute -- nothing to draw */

    uint32_t num_verts = (g_pg.inline_count * 4) / stride_bytes;
    if (num_verts < 3)
        return;

    const uint8_t *base = (const uint8_t *)g_pg.inline_data;
    int actual_prim_type = g_pg.d3d_prim_type;
    uint32_t out_vert_count = num_verts;

    /* Handle QUADS (mode 8): convert to triangle list (6 verts per quad) */
    int is_quads = (g_pg.draw_mode == 8);
    uint32_t num_quads = is_quads ? (num_verts / 4) : 0;
    if (is_quads) {
        out_vert_count = num_quads * 6;  /* 2 triangles per quad */
        actual_prim_type = D3DPT_TRIANGLELIST;
    }

    /* Calculate primitive count */
    uint32_t prim_count = 0;
    switch (actual_prim_type) {
        case D3DPT_TRIANGLELIST:  prim_count = out_vert_count / 3; break;
        case D3DPT_TRIANGLESTRIP: prim_count = out_vert_count - 2; break;
        case D3DPT_TRIANGLEFAN:   prim_count = out_vert_count - 2; break;
        case D3DPT_LINELIST:      prim_count = out_vert_count / 2; break;
        case D3DPT_LINESTRIP:     prim_count = out_vert_count - 1; break;
        default: prim_count = out_vert_count / 3; break;
    }
    if (prim_count == 0)
        return;

    /* ── Choose the vertex pipeline for this draw ──
     *
     * Breakdown's own draws are always programmable (see the g_pg.vsh
     * struct comment), but transform_mode is checked fresh every draw
     * rather than assumed, so a title mixing fixed-function (menu/HUD)
     * and programmable (world geometry) draws is handled correctly, and a
     * failed shader (re)build degrades to the fixed-function 2D fallback
     * instead of drawing nothing. */
    int use_vsh = (g_pg.vsh.transform_mode == 2 && g_pg.vsh.program_length > 0);

    if (use_vsh) {
        /* Two-stage gate before touching d3d8_vsh at all: program_dirty is
         * a cheap "did any instruction dword land since the last check"
         * flag (set in vsh_program_write()); only if that's true do we pay
         * for the memcmp against program_shadow to ask whether the
         * microcode actually differs from what shader_handle already is.
         * In practice the title reissues the same microcode far more often
         * than it changes it, so this turns ~1-in-3 draws worth of
         * delete_shader/create_shader/parse churn into a rare event --
         * see program_shadow's comment. d3d8_vsh_create_shader() itself
         * just copies bytes into a slot either way (cheap), and the actual
         * D3DCompile is separately hash-cached inside d3d8_vsh.c
         * regardless, so this is a hygiene/CPU-cost improvement, not a
         * correctness one: it was already impossible for this to cause a
         * spurious recompile. */
        int need_rebuild = (g_pg.vsh.shader_handle == 0);
        if (g_pg.vsh.program_dirty) {
            g_pg.vsh.program_dirty = 0;
            if (g_pg.vsh.program_length != g_pg.vsh.program_shadow_length ||
                memcmp(g_pg.vsh.program_data, g_pg.vsh.program_shadow,
                       (size_t)g_pg.vsh.program_length * sizeof(g_pg.vsh.program_data[0])) != 0) {
                need_rebuild = 1;
            }
        }

        if (need_rebuild) {
            DWORD new_handle = 0;
            if (SUCCEEDED(d3d8_vsh_create_shader((const DWORD *)g_pg.vsh.program_data,
                                                  g_pg.vsh.program_length, &new_handle))) {
                /* Parse independently (cheap vs. the D3DCompile this also
                 * triggers lazily on first use) just to learn which v#
                 * registers the program reads, so the vertex buffer below
                 * is packed to match -- d3d8_vsh.h exposes the program
                 * struct/parser publicly for exactly this. */
                NV2AVshProgram parsed;
                d3d8_vsh_parse((const DWORD *)g_pg.vsh.program_data,
                                g_pg.vsh.program_length, &parsed);
                memcpy(g_pg.vsh.program_shadow, g_pg.vsh.program_data,
                       sizeof(g_pg.vsh.program_data));
                g_pg.vsh.program_shadow_length = g_pg.vsh.program_length;
                g_pg.stats.vsh_rebuilds++;
                fprintf(stderr, "[PGRAPH-D3D11] VSH: rebuilt shader handle=0x%lX "
                        "(%d insns, inputs=0x%04X)\n",
                        (unsigned long)new_handle, g_pg.vsh.program_length,
                        (unsigned)parsed.inputs_read);
                /* Delete the OLD handle only now that the new one exists --
                 * keeps steady-state slot usage at 1 out of the 128 d3d8_vsh
                 * provides. Without this, a title whose microcode really
                 * does change every few draws would still churn through
                 * slots one-for-one even with the shadow-compare above. */
                if (g_pg.vsh.shader_handle)
                    d3d8_vsh_delete_shader(g_pg.vsh.shader_handle);
                g_pg.vsh.shader_handle = new_handle;
                g_pg.vsh.inputs_read = parsed.inputs_read;
            } else {
                fprintf(stderr, "[PGRAPH-D3D11] VSH: d3d8_vsh_create_shader failed "
                        "(%d insns); falling back to fixed-function this draw\n",
                        g_pg.vsh.program_length);
            }
        }
        use_vsh = (g_pg.vsh.shader_handle != 0);
    }

    uint32_t vsh_dst_offset[16];
    uint32_t vsh_stride = 0;
    if (use_vsh) {
        vsh_stride = compute_vsh_vertex_layout(g_pg.vsh.inputs_read, vsh_dst_offset);
        if (vsh_stride == 0)
            use_vsh = 0;  /* Program declares no inputs we can supply -- fall back */
    }

    OutputVertex *out = NULL;
    uint8_t *vsh_verts = NULL;

    if (use_vsh) {
        /* Programmable path: pack each vertex into whatever layout the
         * ACTIVE PROGRAM's inputs_read calls for (see convert_vsh_vertex()
         * above), not the fixed 28-byte OutputVertex the 2D fallback uses.
         * Source values still come from the title's real, decoded
         * SET_VERTEX_DATA_ARRAY_FORMAT layout (base/byte_offset/stride_
         * bytes) computed above -- unchanged from the fixed-function path. */
        vsh_verts = (uint8_t *)_alloca((size_t)out_vert_count * vsh_stride);
        #define CONVERT_VSH_VERT(dst_idx, src_idx) \
            convert_vsh_vertex(base + (size_t)(src_idx) * stride_bytes, byte_offset, \
                                g_pg.vsh.inputs_read, \
                                vsh_verts + (size_t)(dst_idx) * vsh_stride)
        if (is_quads) {
            uint32_t out_idx = 0;
            for (uint32_t q = 0; q < num_quads; q++) {
                uint32_t qi = q * 4;
                CONVERT_VSH_VERT(out_idx + 0, qi + 0);
                CONVERT_VSH_VERT(out_idx + 1, qi + 1);
                CONVERT_VSH_VERT(out_idx + 2, qi + 2);
                CONVERT_VSH_VERT(out_idx + 3, qi + 0);
                CONVERT_VSH_VERT(out_idx + 4, qi + 2);
                CONVERT_VSH_VERT(out_idx + 5, qi + 3);
                out_idx += 6;
            }
        } else {
            for (uint32_t i = 0; i < num_verts; i++) {
                CONVERT_VSH_VERT(i, i);
            }
        }
        #undef CONVERT_VSH_VERT
    } else {
        /* Fixed-function 2D fallback (unchanged from before this task --
         * pre-transformed XYZRHW position, packed diffuse, one texcoord). */
        out = (OutputVertex *)_alloca(out_vert_count * sizeof(OutputVertex));

        /* Helper to convert one inline vertex. Reads whichever attributes the
         * title actually enabled (see compute_vertex_layout()); position and
         * texcoord0 default to (0,0,0,1)/(0,0) if somehow disabled, and
         * diffuse falls back to the tracked "current" constant color when it
         * is not part of the per-vertex stream (the common case observed so
         * far -- see g_pg.current_diffuse). */
        #define CONVERT_VERT(dst_idx, src_idx) do { \
            const uint8_t *_vb = base + (size_t)(src_idx) * stride_bytes; \
            float _pos[4] = {0.0f, 0.0f, 0.0f, 1.0f}; \
            float _uv[4]  = {0.0f, 0.0f, 0.0f, 1.0f}; \
            decode_attr_floats(_vb, byte_offset, NV2A_VERTEX_ATTR_POSITION, _pos); \
            decode_attr_floats(_vb, byte_offset, NV2A_VERTEX_ATTR_TEXTURE0, _uv); \
            out[dst_idx].x     = _pos[0]; \
            out[dst_idx].y     = _pos[1]; \
            out[dst_idx].z     = _pos[2]; \
            out[dst_idx].rhw   = _pos[3]; \
            out[dst_idx].u     = _uv[0]; \
            out[dst_idx].v     = _uv[1]; \
            out[dst_idx].color = g_pg.vattr[NV2A_VERTEX_ATTR_DIFFUSE].count ? \
                decode_diffuse_color(_vb, byte_offset, NV2A_VERTEX_ATTR_DIFFUSE) : \
                g_pg.current_diffuse; \
        } while(0)

        if (is_quads) {
            /* Convert quads (v0,v1,v2,v3) → two triangles (v0,v1,v2), (v0,v2,v3) */
            uint32_t out_idx = 0;
            for (uint32_t q = 0; q < num_quads; q++) {
                uint32_t qi = q * 4;
                CONVERT_VERT(out_idx + 0, qi + 0);  /* tri 1: v0 */
                CONVERT_VERT(out_idx + 1, qi + 1);  /* tri 1: v1 */
                CONVERT_VERT(out_idx + 2, qi + 2);  /* tri 1: v2 */
                CONVERT_VERT(out_idx + 3, qi + 0);  /* tri 2: v0 */
                CONVERT_VERT(out_idx + 4, qi + 2);  /* tri 2: v2 */
                CONVERT_VERT(out_idx + 5, qi + 3);  /* tri 2: v3 */
                out_idx += 6;
            }
        } else {
            for (uint32_t i = 0; i < num_verts; i++) {
                CONVERT_VERT(i, i);
            }
        }
        #undef CONVERT_VERT

        /* Chyron scroll: shift X for vertices in the chyron Y band (366-382).
         * Simple continuous scroll — no per-vertex wrapping to avoid artifacts
         * from split triangle-strip quads spanning the screen. Only meaningful
         * for the pre-transformed 2D path -- chyron text is a fixed-function
         * HUD overlay, never a programmable-VS draw. */
        if (g_pg.chyron_scroll_offset != 0.0f && out_vert_count >= 6) {
            /* Check if this draw is in the chyron band */
            int is_chyron = 1;
            for (uint32_t i = 0; i < (out_vert_count < 8 ? out_vert_count : 8); i++) {
                if (out[i].y < 360.0f || out[i].y > 390.0f) {
                    is_chyron = 0;
                    break;
                }
            }
            if (is_chyron) {
                /* Find the total text width */
                float min_x = 9999.0f, max_x = -9999.0f;
                for (uint32_t i = 0; i < out_vert_count; i++) {
                    if (out[i].x < min_x) min_x = out[i].x;
                    if (out[i].x > max_x) max_x = out[i].x;
                }
                float text_width = max_x - min_x;

                /* Scroll loops: text slides left, then resets to start position.
                 * Total cycle = text scrolls fully off-left + re-enters from right. */
                float cycle = text_width + 640.0f;
                float scroll = fmodf(g_pg.chyron_scroll_offset, cycle);

                /* Apply uniform shift to ALL vertices (no per-vertex wrap) */
                for (uint32_t i = 0; i < out_vert_count; i++) {
                    out[i].x -= scroll;
                }
            }
        }
    }

    /* Log first few draws: the decoded attribute layout plus the resulting
     * per-vertex values, as evidence that INLINE_ARRAY is now being read
     * according to what the title actually programmed rather than a fixed
     * guess. */
    if (g_pg.stats.draw_calls < 3 && num_verts >= 3) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw (mode=%u, %u in -> %u out) "
                "stride=%uB pos.cnt=%u tex0.cnt=%u diffuse.cnt=%u (0=not in stream, uses current=0x%08X):\n",
                g_pg.draw_mode, num_verts, out_vert_count, stride_bytes,
                g_pg.vattr[NV2A_VERTEX_ATTR_POSITION].count,
                g_pg.vattr[NV2A_VERTEX_ATTR_TEXTURE0].count,
                g_pg.vattr[NV2A_VERTEX_ATTR_DIFFUSE].count, g_pg.current_diffuse);
        uint32_t show = num_verts < 8 ? num_verts : 8;
        for (uint32_t i = 0; i < show; i++) {
            const uint8_t *vb = base + (size_t)i * stride_bytes;
            float pos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            float uv[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
            decode_attr_floats(vb, byte_offset, NV2A_VERTEX_ATTR_POSITION, pos);
            decode_attr_floats(vb, byte_offset, NV2A_VERTEX_ATTR_TEXTURE0, uv);
            uint32_t color = g_pg.vattr[NV2A_VERTEX_ATTR_DIFFUSE].count ?
                decode_diffuse_color(vb, byte_offset, NV2A_VERTEX_ATTR_DIFFUSE) : g_pg.current_diffuse;
            fprintf(stderr, "  [%u] pos=(%.3f, %.3f, %.3f) uv=(%.3f, %.3f) color=0x%08X\n",
                    i, pos[0], pos[1], pos[2], uv[0], uv[1], color);
        }
    }

    /* Get D3D8 device */
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    if (!dev) return;

    /* Set up 2D render state — always enable alpha for menu transparency */
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    if (use_vsh) {
        /* Programmable path: position is a shader input now, not a
         * pretransformed screen coordinate -- do NOT declare an FVF here,
         * that would force the fixed-function pipeline and the vertex
         * program would never run (this was the actual cause of the black
         * screen: INLINE_ARRAY position data are NV2A vertex-shader
         * inputs, and feeding them to D3DFVF_XYZRHW draws them as already-
         * transformed screen pixels instead). SetVertexShader() with our
         * d3d8_vsh handle sets g_device_state.vertex_shader, which is what
         * makes dev_DrawPrimitiveUP()'s own d3d8_vsh_prepare_draw() call
         * (src/d3d/d3d8_device.c) bind this program -- see the comment
         * above where shader_handle is (re)built. */
        dev->lpVtbl->SetVertexShader(dev, g_pg.vsh.shader_handle);
    } else {
        /* Fixed-function fallback: pre-transformed 2D with texture. */
        dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    }

    /* Bind texture based on NV2A VRAM offset.
     * Game-specific texture mapping is handled via GAME_HAS_FONT_ATLAS
     * compile flag. Generic path uses vertex color only. */
#ifdef GAME_HAS_FONT_ATLAS
    if (g_textures_loaded) {
        if (!g_pg.texture_lookup_done) {
            g_pg.texture_lookup_done = 1;
            fprintf(stderr, "[PGRAPH-D3D11] Texture lookup init (global_txd has %d textures)\n",
                    g_global_txd.count);
            for (int ti = 0; ti < g_global_txd.count; ti++) {
                fprintf(stderr, "    [%3d] %-24s %3ux%-3u fmt=0x%X\n",
                        ti, g_global_txd.entries[ti].name,
                        g_global_txd.entries[ti].width,
                        g_global_txd.entries[ti].height,
                        g_global_txd.entries[ti].format);
            }
        }

        IDirect3DTexture8 *tex = NULL;
        uint32_t vram_off = g_pg.tex[0].offset;
        switch (vram_off) {
            case 0x03C1ED00: tex = txd_find(&g_global_txd, "B3Logo"); break;
            case 0x03C24700: tex = txd_find(&g_global_txd, "bg"); break;
            case 0x03C24B80: tex = txd_find(&g_global_txd, "big_curve"); break;
            case 0x03C7BE00: tex = txd_find(&g_global_txd, "Buttons"); break;
            case 0x03C95700: tex = txd_find(&g_global_txd, "dpad"); break;
            case 0x03C95980: tex = txd_find(&g_global_txd, "FE"); break;
            case 0x03CA1A80: tex = txd_find(&g_global_txd, "small_curve"); break;
            case 0x03D57000: tex = txd_find(&g_global_txd, "box_curve"); break;
            case 0x03CB9200: tex = txd_find(&g_global_txd, "grid"); break;
            case 0x02EC0400:
                dev->lpVtbl->EndScene(dev);
                g_pg.inline_count = 0;
                return;
            case 0x021C4100:
                if (!g_pg.font_atlas) {
                    g_pg.font_atlas = create_dxt5_texture(dev,
                        FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT,
                        font_atlas_dxt5, FONT_ATLAS_SIZE);
                }
                tex = g_pg.font_atlas;
                break;
            case 0: tex = NULL; break;
            default: tex = NULL; break;
        }

        if (tex) {
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 13 /*ADDRESSU*/, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 14 /*ADDRESSV*/, 3 /*CLAMP*/);
        } else {
            /* No texture — use vertex color only */
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 0 /*DIFFUSE*/);
        }
    } else {
        dev->lpVtbl->SetTexture(dev, 0, NULL);
    }
#else
    /* Generic path: no game-specific texture lookup, use vertex color only */
    {
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2, 0 /*DIFFUSE*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5, 0 /*DIFFUSE*/);
    }
#endif

    /* Begin scene if needed */
    dev->lpVtbl->BeginScene(dev);

    /* Draw */
    if (use_vsh) {
        dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)actual_prim_type,
                                      prim_count, vsh_verts, vsh_stride);
        g_pg.stats.vsh_draws++;
    } else {
        dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)actual_prim_type,
                                      prim_count, out, sizeof(OutputVertex));
    }

    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += num_verts;

    if (g_pg.stats.draw_calls <= 5 || (g_pg.stats.draw_calls % 1000) == 0) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw #%u: %u verts, prim=%d, prims=%u, "
                "vsh=%s (stride=%u)\n",
                g_pg.stats.draw_calls, num_verts, actual_prim_type, prim_count,
                use_vsh ? "yes" : "no", use_vsh ? vsh_stride : (unsigned)sizeof(OutputVertex));
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Method Handler
 * ══════════════════════════════════════════════════════════════════════ */

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    if (!g_pg.initialized)
        return 0;

    g_pg.stats.methods_handled++;

    switch (method) {

    /* ── Draw Begin/End ── */
    case NV097_SET_BEGIN_END:
        if (param == 0) {
            /* END: submit accumulated vertices */
            if (g_pg.in_draw) {
                submit_draw();
                g_pg.in_draw = 0;
            }
        } else {
            /* BEGIN: start new draw */
            g_pg.in_draw = 1;
            g_pg.draw_mode = param;
            g_pg.d3d_prim_type = nv2a_draw_mode_to_d3d(param);
            g_pg.inline_count = 0;
        }
        return 1;

    /* ── Inline Vertex Data ── */
    case NV097_INLINE_ARRAY:
        if (g_pg.in_draw && g_pg.inline_count < MAX_INLINE_DWORDS) {
            g_pg.inline_data[g_pg.inline_count++] = param;
        }
        return 1;

    /* ── Clear ── */
    case NV097_SET_COLOR_CLEAR_VALUE:
        g_pg.clear_color = param;
        return 1;

    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        g_pg.clear_rect_h = param;
        return 1;

    case NV097_SET_CLEAR_RECT_VERTICAL:
        g_pg.clear_rect_v = param;
        return 1;

    case NV097_CLEAR_SURFACE:
    {
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();
        if (dev) {
            uint32_t flags = 0;
            if (param & 0xF0) flags |= 1;  /* D3DCLEAR_TARGET */
            if (param & 0x01) flags |= 2;  /* D3DCLEAR_ZBUFFER */
            if (param & 0x02) flags |= 4;  /* D3DCLEAR_STENCIL */
            dev->lpVtbl->Clear(dev, 0, NULL, flags, g_pg.clear_color, 1.0f, 0);
        }
        g_pg.stats.clears++;
        return 1;
    }

    /* ── Render State ── */
    case NV097_SET_DEPTH_TEST_ENABLE:
        g_pg.depth_test = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_ENABLE:
        g_pg.blend_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_FUNC_SFACTOR:
        g_pg.blend_sfactor = param;
        return 1;

    case NV097_SET_BLEND_FUNC_DFACTOR:
        g_pg.blend_dfactor = param;
        return 1;

    case NV097_SET_CULL_FACE_ENABLE:
        g_pg.cull_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_ALPHA_TEST_ENABLE:
        g_pg.alpha_test = param ? 1 : 0;
        return 1;

    case NV097_SET_COLOR_MASK:
        g_pg.color_mask = param;
        return 1;

    case NV097_SET_SHADE_MODE:
        /* 1=flat, 2=gouraud — we always use gouraud */
        return 1;

    /* ── Viewport ── */
    case NV097_SET_VIEWPORT_OFFSET:
    case NV097_SET_VIEWPORT_OFFSET + 4:
    case NV097_SET_VIEWPORT_OFFSET + 8:
    case NV097_SET_VIEWPORT_OFFSET + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
        g_pg.vp_offset[idx] = u2f(param);
        return 1;
    }

    case NV097_SET_VIEWPORT_SCALE:
    case NV097_SET_VIEWPORT_SCALE + 4:
    case NV097_SET_VIEWPORT_SCALE + 8:
    case NV097_SET_VIEWPORT_SCALE + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_SCALE) / 4;
        g_pg.vp_scale[idx] = u2f(param);
        return 1;
    }

    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        g_pg.surface_clip_h = param;
        return 1;

    case NV097_SET_SURFACE_CLIP_VERTICAL:
        g_pg.surface_clip_v = param;
        return 1;

    /* ── Texture state tracking (4 stages, 0x40 stride) ── */
    case NV097_SET_TEXTURE_OFFSET:
    case NV097_SET_TEXTURE_OFFSET + 0x40:
    case NV097_SET_TEXTURE_OFFSET + 0x80:
    case NV097_SET_TEXTURE_OFFSET + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_OFFSET) / 0x40;
        g_pg.tex[stage].offset = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FORMAT:
    case NV097_SET_TEXTURE_FORMAT + 0x40:
    case NV097_SET_TEXTURE_FORMAT + 0x80:
    case NV097_SET_TEXTURE_FORMAT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FORMAT) / 0x40;
        g_pg.tex[stage].format = param;
        return 1;
    }
    case NV097_SET_TEXTURE_CONTROL0:
    case NV097_SET_TEXTURE_CONTROL0 + 0x40:
    case NV097_SET_TEXTURE_CONTROL0 + 0x80:
    case NV097_SET_TEXTURE_CONTROL0 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL0) / 0x40;
        g_pg.tex[stage].control0 = param;
        g_pg.tex[stage].enabled = (param >> 30) & 1;
        return 1;
    }

    /* ── Vertex attribute format (drives INLINE_ARRAY decode) ──
     * This register lives at 0x1760 + slot*4, 16 slots; it used to fall
     * inside the 0x1680-0x1780 "ignore, not needed yet" range, which is
     * exactly the bug this lead was about -- see the file header comment
     * and compute_vertex_layout()/decode_attr_floats() above. */
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x04:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x08:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x0C:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x10:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x14:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x18:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x1C:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x20:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x24:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x28:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x2C:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x30:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x34:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x38:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x3C:
    {
        int slot = (method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4;
        /* TYPE is bits 0-3, SIZE (component count) is bits 4-7 -- see the
         * NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE/_SIZE masks in
         * nv2a_regs.h. STRIDE (bits 8-31) is for a VRAM-sourced vertex
         * array and does not apply to INLINE_ARRAY, whose packing is
         * always tight (compute_vertex_layout() derives it), so it is
         * intentionally not read here. */
        uint32_t fmt = param & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE;
        uint32_t cnt = (param & NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE) >> 4;
        g_pg.vattr[slot].format = fmt;
        g_pg.vattr[slot].count = cnt;
        switch (fmt) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            g_pg.vattr[slot].size = 1;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            g_pg.vattr[slot].size = 2;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        default:
            g_pg.vattr[slot].size = 4;
            break;
        }
        return 1;
    }

    /* Constant ("current") diffuse color for vertices where DIFFUSE is not
     * one of the enabled per-vertex attributes -- see g_pg.current_diffuse.
     * Address 0x156C was previously outside every known range in this
     * function (neither a case nor inside the ignore list), so it was
     * counted as "truly unhandled" and dropped. */
    case NV097_SET_DIFFUSE_COLOR4UB:
        g_pg.current_diffuse = param;
        return 1;

    /* ── Programmable vertex shader control registers ──
     * SET_TRANSFORM_PROGRAM (0x0B00) and SET_TRANSFORM_CONSTANT (0x0B80)
     * themselves are handled as ranges in the default: block below (each
     * is a 32-dword load-pointer window, same pattern as the ignore-range
     * checks already there) -- see vsh_program_write()/vsh_constant_write()
     * and the g_pg.vsh struct comment. These five are single registers. */
    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        /* MODE field (bits 0-1): 2 = vertex program, 0 = fixed function --
         * see xemu hw/xbox/nv2a/pgraph/glsl/vsh.c. Recorded here for
         * submit_draw() to read at actual draw time; the raw register
         * write alternates 2/0 through an upload sequence, so treating a
         * write-time sample as the draw's mode (rather than whatever is
         * current when the draw itself happens) would misclassify draws --
         * this is what a prior lead's draw-time sampling was checking for. */
        g_pg.vsh.transform_mode = (int)(param & NV097_SET_TRANSFORM_EXECUTION_MODE_MODE);
        return 1;

    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        g_pg.vsh.program_load = (int)param;
        return 1;

    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        g_pg.vsh.const_load = (int)param;
        return 1;

    case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN:
    case NV097_SET_TRANSFORM_PROGRAM_START:
        /* CXT_WRITE_EN gates context-switch microcode patching; START picks
         * a non-zero entry point for it. d3d8_vsh compiles and runs the
         * whole accumulated program from instruction 0 always, which is
         * correct for a single straight-line vertex program (what this
         * title uses) but would be wrong for a title that jumps into a
         * subroutine mid-microcode. Acknowledged, not acted on -- flag if a
         * future title is ever seen writing a non-zero START. */
        return 1;

    default:
        /* SET_TRANSFORM_PROGRAM / SET_TRANSFORM_CONSTANT: 32-dword
         * (8-instruction / 8-constant) load-pointer windows, checked as
         * ranges here rather than enumerated as 32 case labels each, same
         * style as the ignore-ranges below. This is also the fix for what
         * used to be a real bug, not just a mislabel: 0x0B80-0x0C00 was in
         * the ignore list below captioned "Transform program", but per
         * nv2a_regs.h 0x0B80 is SET_TRANSFORM_CONSTANT and 0x0B00 (never in
         * any range, so falling all the way through to "truly unhandled"
         * below) is SET_TRANSFORM_PROGRAM -- both were being discarded
         * (503,893 and 336,108 writes respectively, measured over ~90s),
         * which is the actual reason Breakdown's geometry never reached the
         * screen: the vertex program and its constants never reached
         * d3d8_vsh at all. See vsh_program_write()/vsh_constant_write(). */
        if (method >= NV097_SET_TRANSFORM_PROGRAM &&
            method <  NV097_SET_TRANSFORM_PROGRAM + 0x80) {
            vsh_program_write(method, param);
            return 1;
        }
        if (method >= NV097_SET_TRANSFORM_CONSTANT &&
            method <  NV097_SET_TRANSFORM_CONSTANT + 0x80) {
            vsh_constant_write(method, param);
            return 1;
        }

        /* Known ranges we can safely ignore.
         *
         * Names here were checked against nv2a_regs.h; several were previously
         * wrong, and one range hid a bug rather than merely mislabelling it:
         * 0x1D60-0x1EA0 was called "Combiners" but the combiner registers are
         * at 0x0260 and 0x1E40. What it actually spans is the clear group
         * (0x1D8C-0x1D9C), which this function has cases for -- they just
         * never matched, because the local constants were 0x1BC4 low. With the
         * constants taken from the header those cases win before this default,
         * and the range stops short of them. */
        if ((method >= 0x0E00 && method < 0x1000) ||  /* No NV097 method actually
                                                        * lives here (checked
                                                        * against nv2a_regs.h);
                                                        * measured zero writes.
                                                        * Left in place since it
                                                        * catches nothing either
                                                        * way, but the previous
                                                        * "Transform constants"
                                                        * label was wrong -- that
                                                        * register is 0x0B80,
                                                        * handled above now. */
            (method >= 0x1680 && method < 0x1780) ||  /* Vertex array format/offset */
            (method >= 0x1B00 && method < 0x1C00) ||  /* Texture registers */
            (method >= 0x1D60 && method < 0x1D8C) ||  /* Semaphore, ZMIN/MAX, AA */
            (method >= 0x1DA0 && method < 0x1EA0) ||  /* Fog, combiner OCW, shader */
            method == 0x0100 ||                        /* NO_OPERATION */
            method == 0x0110 ||                        /* WAIT_FOR_IDLE */
            method == 0x0180 ||                        /* SET_CONTEXT_DMA_NOTIFIES */
            method == 0x0394 ||                        /* SET_CLIP_MIN */
            method == 0x0398 ||                        /* SET_CLIP_MAX */
            method == 0x039C ||                        /* SET_CULL_FACE (winding) */
            /* The flip group, previously written 0x18 low (0x0108-0x0118): it
             * ignored three methods the title never sends and let the real
             * ones fall through as unhandled. 0x0130 FLIP_STALL is absent on
             * purpose -- pgraph_method presents on it before asking here. */
            method == 0x0120 || method == 0x0124 ||    /* SET_FLIP_READ/WRITE */
            method == 0x0128 || method == 0x012C)      /* FLIP_MODULO/INCREMENT */
        {
            return 1;  /* Silently handled (ignored but acknowledged) */
        }

        g_pg.stats.methods_ignored++;
        return 0;  /* Truly unhandled */
    }
}

void pgraph_d3d11_flush(void)
{
    if (g_pg.in_draw) {
        submit_draw();
        g_pg.in_draw = 0;
    }
    g_pg.stats.frames++;
}

void pgraph_d3d11_set_chyron_scroll(uint32_t pixels)
{
    g_pg.chyron_scroll_offset = (float)pixels;
}

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out)
{
    if (out) *out = g_pg.stats;
}
