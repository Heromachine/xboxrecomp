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
#include "nv2a_state.h"  /* nv2a_get_guest_ram() -- see the texture upload
                          * section below for why a texture offset resolves
                          * through the exact same base/size the push
                          * buffer puller uses, not a second copy of it. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

/* D3D8 device — we include the full header for COM vtable access */
#include "../d3d/d3d8_xbox.h"
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);

/* d3d8_internal.h defines COBJMACROS before its own <d3d11.h> include, which
 * is what turns on the ID3D11Foo_Bar(...) call-style macros used throughout
 * this file's texture/pixel-shader section below. It must come before ANY
 * other header that pulls in <d3d11.h> -- d3d8_vsh.h does exactly that, so
 * this has to be included first or <d3d11.h>'s own include guard silently
 * keeps COBJMACROS from ever taking effect (this bit twice while writing
 * the section below: every ID3D11*_* call failed as an implicit-declaration
 * error until the order was fixed here, not there). */
#include "../d3d/d3d8_internal.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

/* NV2A programmable vertex shader translator (microcode -> HLSL). See the
 * "Programmable vertex shader" block below for why this file now feeds it:
 * every draw in this title uses SET_TRANSFORM_PROGRAM/_CONSTANT, never the
 * fixed-function pipeline, and this translator already existed but nothing
 * upstream of it ever called in from the raw NV2A pushbuffer path. */
#include "../d3d/d3d8_vsh.h"

/* d3d8_GetD3D11Device()/_Context() (declared in d3d8_internal.h, included
 * above) -- needed directly (not through the D3D8 vtable) to create
 * textures/samplers and bind a pixel shader for the VSH draw path. See the
 * "Texture upload + pixel shader" section below for why that path needs
 * its own PS at all: d3d8_shaders_prepare_draw() (the FFP fallback that
 * already handles real texture sampling) only runs when d3d8_vsh_prepare_
 * draw() FAILS, and d3d8_combiners_prepare_draw() only activates once
 * something has called SetPixelShader() -- Breakdown does neither, since
 * it drives raw NV2A pushbuffer methods for the pixel stage too, not the
 * D3D8 API. So for every one of our draws, no pixel shader has ever been
 * (re)bound at all; whatever the device happened to have left over from
 * setup is what has been producing the uniform white. */

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
     * not one of the enabled attributes. Defaults to opaque white.
     *
     * Real NV2A hardware defaults an unset vertex attribute to (0,0,0,1)
     * -- opaque black -- and this WAS suspected as the leading cause of
     * the whole-frame uniform white this title was producing once its
     * vertex program started running (see the VSH decoder/viewport-
     * epilogue work elsewhere in this file). It was tried, together with
     * the texture-upload path below, and measured directly: the frame
     * dump went from 307,200/307,200 white pixels to 307,200/307,200
     * BLACK pixels -- one uniform color swapped for another, not a fix.
     * That is because the texture-upload path (see resolve_texture_stage()
     * and the "Texture upload + pixel shader" section) is verified wired
     * correctly -- real D3D11 textures get created, the pixel shader
     * compiles and binds, and per-draw resolution picks a real cached
     * texture for the large majority of draws -- but every byte read back
     * from guest RAM at every texture offset observed, across a 100+
     * second run with live-vs-cached comparison ruling out a read-before-
     * write race, is 0x00. So the pixels this default currently controls
     * -- the untextured fallback draws -- are what's actually painting
     * the visible frame, and MODULATE against an all-zero-alpha texture
     * sample contributes nothing either way regardless of what this is
     * set to. Confirmed NOT called at all this session: NV097_SET_
     * DIFFUSE_COLOR4UB never fires, and NV2A_VERTEX_ATTR_DIFFUSE's
     * per-vertex count never goes non-zero either -- diffuse really does
     * come from nowhere but this static default for every draw observed.
     *
     * Left at white deliberately. Flip this back to black ONLY once real
     * (non-zero) texture bytes are confirmed reaching resolve_texture_
     * stage() -- either because the boot sequence has progressed further
     * (still sitting at namcologo.xmv as of this note) or because NV2A's
     * real texture DMA-object resolution (SET_CONTEXT_DMA_A/_B -> a RAMIN
     * descriptor lookup, xemu's nv_dma_map() in pgraph/texture.c) turns
     * out to matter here and gets implemented -- this recomp currently
     * has NO tracking of SET_CONTEXT_DMA_A/_B at all (they fall into
     * "truly unhandled"), unlike the push buffer object, which explicitly
     * documented ITS OWN "whole of RAM, zero base" shortcut as verified
     * for that one object specifically (see nv2a_core.c's pfifo_pull()
     * comment) -- that verification was never extended to textures, and
     * this is now a real, live candidate for why theirs read as blank. */
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

    /* Texture state per stage (4 stages). Addresses per nv2a_regs.h --
     * the old control0 comment here said 0x1B08, which is actually
     * SET_TEXTURE_ADDRESS; SET_TEXTURE_CONTROL0 is 0x1B0C. The CODE
     * already used the symbolic NV097_SET_TEXTURE_CONTROL0 constant, so
     * this was a stale comment, not a live bug -- fixed while adding the
     * two registers below it needed for real texture upload. */
    struct {
        uint32_t offset;     /* NV2A guest-RAM offset (SET_TEXTURE_OFFSET,
                               * 0x1B00) -- physical address, same "whole
                               * of RAM, zero base" DMA object convention
                               * the push buffer puller uses (see nv2a_
                               * core.c's pfifo_pull() comment). */
        uint32_t format;     /* Format register (SET_TEXTURE_FORMAT, 0x1B04) */
        uint32_t control0;   /* Control0 register (SET_TEXTURE_CONTROL0, 0x1B0C) */
        uint32_t control1;   /* Pitch, for LINEAR formats only (SET_TEXTURE_
                               * CONTROL1, 0x1B10) -- SWIZZLED/compressed
                               * formats are always tightly packed and
                               * don't use this. */
        uint32_t image_rect; /* Width/height, for LINEAR formats only
                               * (SET_TEXTURE_IMAGE_RECT, 0x1B1C) --
                               * SWIZZLED formats get their (power-of-two)
                               * dimensions from the FORMAT register's
                               * BASE_SIZE_U/V log2 fields instead. */
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
             * Deduped per-register (first value seen) rather than capped
             * at N total writes: a handful of "hot" registers (object
             * transforms) get rewritten every draw and would otherwise
             * crowd out the ones only ever set once (e.g. a viewport-style
             * scale/offset pair) long before this ever got to show them. */
            if (getenv("XBOXRECOMP_VSHDUMP")) {
                static int shown_reg[NV2A_VS_MAX_CONSTANTS];
                if (!shown_reg[api_reg]) {
                    fprintf(stderr, "[PGRAPH-D3D11] VSH const c[%d] (hw %d) = "
                            "(%.4f, %.4f, %.4f, %.4f)\n",
                            api_reg, g_pg.vsh.const_load, f4[0], f4[1], f4[2], f4[3]);
                    shown_reg[api_reg] = 1;
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
 * Texture upload + pixel shader (VSH draw path only)
 *
 * Format support is deliberately narrow, matched to what Breakdown was
 * measured (XBOXRECOMP_TEXDUMP=1) actually programming: every texture
 * seen is NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 (0x12, plain
 * 32bpp UI/icon textures) or _LC_IMAGE_CR8YB8CB8YA8 (0x24, a packed YUV
 * format -- almost certainly the namcologo.xmv movie decode texture,
 * given the boot sequence is still there). Both are LINEAR (the "LU_"/
 * "LC_" prefix in nv2a_regs.h, as opposed to "SZ_" for swizzled) --
 * nothing swizzled or DXT-compressed was ever observed, so no de-swizzle
 * or block-decompression is implemented here. A8R8G8B8 is handled below;
 * the YUV format is decoded (dimensions/pitch tracked) but not unpacked
 * to RGB -- see resolve_texture_stage()'s default case. If a future
 * title needs SWIZZLED or compressed formats, that is real additional
 * work (de-swizzle is a Z-order/Morton walk, not a memcpy) and should be
 * scoped separately rather than guessed at here.
 *
 * UV space: confirmed against xemu's hw/xbox/nv2a/pgraph/glsl/psh.c line
 * ~127 (`state->rect_tex[i] = f.linear`) -- LINEAR-format NV2A textures
 * are sampled with UNNORMALIZED (texel-space) coordinates on real
 * hardware, not the usual normalized [0,1] every D3D/GL sampler expects
 * by default. That is exactly why the measured UVs for this title span
 * 0.53-640.53 / 0.53-448.53 rather than 0-1: the NV2A vertex program's
 * oT0 output IS the real device-space value, same as oPos was before the
 * VPSCL/VPOFF epilogue fix. xemu's pixel shader generator normalizes by
 * dividing by the sampled texture's real size (psh.c ~line 1421); the
 * shader below does the same thing, in the same place (the pixel shader,
 * not the vertex shader) for the same reason -- only the pixel shader
 * knows which texture is actually bound to a given stage.
 * ══════════════════════════════════════════════════════════════════════ */

/* Only the two formats actually observed; anything else falls through to
 * "unsupported" (NULL SRV -> diffuse-only shading) rather than a guess. */
#define NV2A_TEX_COLOR_A8R8G8B8_LINEAR  0x12
#define NV2A_TEX_COLOR_YUV_CR8YB8CB8YA8 0x24
#define NV2A_TEX_COLOR_YUV_YB8CR8YA8CB8 0x25

#define TEX_CACHE_SIZE 16
typedef struct {
    uint32_t key;                 /* hash of offset/format/control1/image_rect */
    int      in_use;
    ID3D11Texture2D          *tex2d;
    ID3D11ShaderResourceView *srv;
    float    width, height;       /* real texel dimensions, for UV normalization */
} TexCacheEntry;

static TexCacheEntry g_tex_cache[TEX_CACHE_SIZE];
static ID3D11SamplerState *g_tex_sampler;   /* lazy-created, shared by all stages */
static ID3D11PixelShader  *g_tex_ps;        /* lazy-compiled, shared by all draws */
static ID3D11Buffer       *g_tex_ps_cb;     /* texSize0/hasTexture constant buffer */

/* Generic pixel shader for the VSH draw path. `hasTexture` is a uniform
 * (whole-draw) flag, not a per-pixel value, so the branch below compiles
 * to real dynamic branching rather than always sampling -- when no
 * supported texture is bound this never touches tex0/samp0, matching the
 * existing D3DFVF-path's SELECTARG1-vs-MODULATE choice, just implemented
 * as a real shader instead of a fixed-function texture-stage state (see
 * the file comment above this section for why the FFP/combiner paths
 * that would normally provide this are unreachable here). */
static const char *k_tex_ps_src =
    "Texture2D tex0 : register(t0);\n"
    "SamplerState samp0 : register(s0);\n"
    "cbuffer PSInfo : register(b0) {\n"
    "    float2 texSize0;\n"
    "    float  hasTexture;\n"
    "    float  pad0;\n"
    "};\n"
    "struct PS_IN {\n"
    "    float4 pos   : SV_POSITION;\n"
    "    float4 color : COLOR0;\n"
    "    float4 tex0  : TEXCOORD0;\n"
    "};\n"
    "float4 main(PS_IN input) : SV_TARGET {\n"
    "    float4 c = input.color;\n"
    "    if (hasTexture > 0.5) {\n"
    /* Undo NV2A's texel-space UV, same reasoning as oPos's VPSCL/VPOFF
     * epilogue in d3d8_vsh.c -- see this section's file comment. */
    "        float2 uv = input.tex0.xy / texSize0;\n"
    "        c = tex0.Sample(samp0, uv) * input.color;\n"
    "    }\n"
    "    return c;\n"
    "}\n";

static ID3D11PixelShader *get_tex_pixel_shader(void)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;

    if (g_tex_ps)
        return g_tex_ps;

    hr = D3DCompile(k_tex_ps_src, strlen(k_tex_ps_src), "nv2a_tex_ps",
                    NULL, NULL, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "[PGRAPH-D3D11] TEX: pixel shader compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    hr = ID3D11Device_CreatePixelShader(d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
        NULL, &g_tex_ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) {
        fprintf(stderr, "[PGRAPH-D3D11] TEX: CreatePixelShader failed: 0x%08lX\n", hr);
        return NULL;
    }

    /* Constant buffer for texSize0/hasTexture, updated per draw. */
    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = 16;  /* float2 + float + float, 16-byte aligned */
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_tex_ps_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "[PGRAPH-D3D11] TEX: CreateBuffer (PSInfo) failed: 0x%08lX\n", hr);
        return NULL;
    }

    fprintf(stderr, "[PGRAPH-D3D11] TEX: pixel shader compiled\n");
    return g_tex_ps;
}

static ID3D11SamplerState *get_tex_sampler(void)
{
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;

    if (g_tex_sampler)
        return g_tex_sampler;

    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    /* CLAMP, not WRAP: we divide by the real texture size to normalize a
     * texel-space UV (see the file comment above), so a UV that lands
     * exactly on the texture's edge should clamp there, not wrap -- WRAP
     * would only be correct if the source UV were already meant to tile,
     * which nothing here has established. */
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd, &g_tex_sampler);
    if (FAILED(hr)) {
        fprintf(stderr, "[PGRAPH-D3D11] TEX: CreateSamplerState failed: 0x%08lX\n", hr);
        return NULL;
    }
    return g_tex_sampler;
}

static uint32_t tex_cache_key(int stage)
{
    /* Everything that changes what bytes we'd read/how we'd interpret
     * them. Doesn't need to be cryptographic -- just distinct enough
     * that a title swapping textures across these fields gets a fresh
     * upload instead of stale cached bytes. */
    uint32_t h = 0x811c9dc5u;
    uint32_t vals[4] = { g_pg.tex[stage].offset, g_pg.tex[stage].format,
                          g_pg.tex[stage].control1, g_pg.tex[stage].image_rect };
    int i;
    for (i = 0; i < 4; i++) {
        h ^= vals[i];
        h *= 0x01000193u;
    }
    return h ? h : 1u;  /* reserve 0 for "empty slot" */
}

/* Resolve (uploading/caching as needed) the texture bound to `stage`.
 * Returns NULL if the stage is disabled, or programmed with a format
 * this translator doesn't decode (see the file comment above) -- either
 * way the caller falls back to diffuse-only shading, never a guess at
 * texture content. */
static TexCacheEntry *resolve_texture_stage(int stage)
{
    uint32_t key, color;
    int i, free_slot;
    uint32_t width, height, pitch;
    const uint8_t *guest_ram;
    uint32_t guest_ram_size;
    const uint8_t *src;
    TexCacheEntry *entry;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
    HRESULT hr;

    if (stage < 0 || stage >= 4 || !g_pg.tex[stage].enabled)
        return NULL;

    color = (g_pg.tex[stage].format & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    if (color != NV2A_TEX_COLOR_A8R8G8B8_LINEAR) {
        /* YUV movie texture and anything else: not decoded yet (see file
         * comment). Warn once per stage so this is visible without being
         * a flood -- 40k+ draws would otherwise repeat it every frame. */
        static int warned[4];
        if (!warned[stage]) {
            fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: color format 0x%02X not "
                    "decoded (only A8R8G8B8/0x12 is) -- falling back to "
                    "diffuse-only for this stage\n", stage, color);
            warned[stage] = 1;

            /* Diagnostic-only peek at the raw bytes (no texture created):
             * tells apart "this format just isn't decoded yet" from "the
             * memory behind it isn't real data anyway" -- same question
             * being asked of the A8R8G8B8 path below, useful to know
             * before deciding whether YUV decode is worth the added
             * scope. Not gated on XBOXRECOMP_TEXDUMP separately since
             * it's a one-shot per stage either way. */
            if (getenv("XBOXRECOMP_TEXDUMP")) {
                uint32_t sz;
                const uint8_t *ram = nv2a_get_guest_ram(&sz);
                if (ram && sz && (sz & (sz - 1)) == 0) {
                    const uint8_t *p = ram + (g_pg.tex[stage].offset & (sz - 1));
                    fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: (YUV, undecoded) raw "
                            "bytes at offset=0x%08X: %02X%02X%02X%02X %02X%02X%02X%02X\n",
                            stage, g_pg.tex[stage].offset,
                            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                }
            }
        }
        return NULL;
    }

    width  = (g_pg.tex[stage].image_rect & NV097_SET_TEXTURE_IMAGE_RECT_WIDTH) >> 16;
    height = (g_pg.tex[stage].image_rect & NV097_SET_TEXTURE_IMAGE_RECT_HEIGHT);
    pitch  = (g_pg.tex[stage].control1 & NV097_SET_TEXTURE_CONTROL1_IMAGE_PITCH) >> 16;
    if (width == 0 || height == 0)
        return NULL;  /* IMAGE_RECT not programmed yet */
    if (pitch == 0)
        pitch = width * 4;  /* A8R8G8B8 is 4 bytes/texel; tightly packed if unset */

    key = tex_cache_key(stage);

    /* Cache lookup */
    free_slot = -1;
    for (i = 0; i < TEX_CACHE_SIZE; i++) {
        if (g_tex_cache[i].in_use && g_tex_cache[i].key == key) {
            /* TEMPORARY: peek the CURRENT live bytes (bypassing the
             * cached upload) and compare against what got cached, to
             * tell apart "this memory is permanently blank" from "we
             * cached it before the game finished writing it" -- a race
             * the cache-once design would otherwise hide forever. Only
             * reads 4 bytes and only under XBOXRECOMP_TEXDUMP; strip
             * once the guest-RAM timing question is settled. */
            if (getenv("XBOXRECOMP_TEXDUMP")) {
                static uint32_t last_live[4];
                uint32_t sz2;
                const uint8_t *ram2 = nv2a_get_guest_ram(&sz2);
                if (ram2 && sz2 && (sz2 & (sz2 - 1)) == 0) {
                    const uint8_t *p2 = ram2 + (g_pg.tex[stage].offset & (sz2 - 1));
                    uint32_t live;
                    memcpy(&live, p2, 4);
                    if (live != last_live[stage]) {
                        fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: live bytes at "
                                "offset=0x%08X changed: 0x%08X -> 0x%08X "
                                "(cached upload was from an earlier read)\n",
                                stage, g_pg.tex[stage].offset, last_live[stage], live);
                        last_live[stage] = live;
                    }
                }
            }
            return &g_tex_cache[i];
        }
        if (free_slot < 0 && !g_tex_cache[i].in_use)
            free_slot = i;
    }

    /* Cache miss: resolve guest RAM and upload. SET_TEXTURE_OFFSET is a
     * physical address under the same "whole of RAM, zero base" DMA
     * object convention nv2a_core.c's push buffer puller uses for its
     * own reads -- nv2a_get_guest_ram() reads back the exact base/size
     * that convention resolved to, rather than this file keeping a
     * second copy of it (see nv2a_state.h's comment on that accessor). */
    guest_ram = nv2a_get_guest_ram(&guest_ram_size);
    if (!guest_ram) {
        static int warned_ram;
        if (!warned_ram) {
            fprintf(stderr, "[PGRAPH-D3D11] TEX: guest RAM not armed yet "
                    "(nv2a_set_guest_ram not called) -- skipping upload\n");
            warned_ram = 1;
        }
        return NULL;
    }
    if (guest_ram_size == 0 || (guest_ram_size & (guest_ram_size - 1)) != 0) {
        /* pb_read32()-style masking below assumes a power-of-two size,
         * same as nv2a_core.c's own pb_read32(); guard rather than
         * silently reading the wrong bytes if that assumption ever
         * stops holding. */
        return NULL;
    }
    src = guest_ram + (g_pg.tex[stage].offset & (guest_ram_size - 1));

    if (free_slot < 0) {
        /* Evict slot 0. Simple and rare in practice: Breakdown's own
         * texture set (icons + one movie frame texture, per XBOXRECOMP_
         * TEXDUMP) is well under TEX_CACHE_SIZE, so eviction under normal
         * play would mean something is churning through many distinct
         * textures -- worth another look if that count climbs, but not
         * a correctness issue either way. */
        free_slot = 0;
        if (g_tex_cache[0].srv)   ID3D11ShaderResourceView_Release(g_tex_cache[0].srv);
        if (g_tex_cache[0].tex2d) ID3D11Texture2D_Release(g_tex_cache[0].tex2d);
        memset(&g_tex_cache[0], 0, sizeof(g_tex_cache[0]));
    }
    entry = &g_tex_cache[free_slot];

    memset(&td, 0, sizeof(td));
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    /* A8R8G8B8 in NV2A's naming is byte order B,G,R,A in memory (same as
     * D3DFMT_A8R8G8B8) -- DXGI_FORMAT_B8G8R8A8_UNORM is the exact same
     * memory layout, so this is a straight copy, no channel repacking. */
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = src;
    sd.SysMemPitch = pitch;

    hr = ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &td, &sd, &entry->tex2d);
    if (FAILED(hr)) {
        fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: CreateTexture2D failed: 0x%08lX "
                "(%ux%u, pitch=%u, offset=0x%08X)\n",
                stage, hr, width, height, pitch, g_pg.tex[stage].offset);
        memset(entry, 0, sizeof(*entry));
        return NULL;
    }

    memset(&srvd, 0, sizeof(srvd));
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    hr = ID3D11Device_CreateShaderResourceView(d3d8_GetD3D11Device(),
        (ID3D11Resource *)entry->tex2d, &srvd, &entry->srv);
    if (FAILED(hr)) {
        fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: CreateShaderResourceView failed: "
                "0x%08lX\n", stage, hr);
        ID3D11Texture2D_Release(entry->tex2d);
        memset(entry, 0, sizeof(*entry));
        return NULL;
    }

    entry->key = key;
    entry->in_use = 1;
    entry->width = (float)width;
    entry->height = (float)height;

    fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: uploaded %ux%u A8R8G8B8 from "
            "offset=0x%08X pitch=%u (cache slot %d)\n",
            stage, width, height, g_pg.tex[stage].offset, pitch, free_slot);

    if (getenv("XBOXRECOMP_TEXDUMP")) {
        /* Raw bytes actually read, so "the upload path is wired but the
         * source bytes are genuinely blank/uninitialized" can be told
         * apart from "the offset/pitch math is reading the wrong place."
         * A handful of texels from the first row plus one from mid-image
         * (pitch bytes in) is enough to tell solid-color from real data
         * without dumping the whole texture. */
        fprintf(stderr, "[PGRAPH-D3D11] TEX[%d]: first row: "
                "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                "... mid-image (row %u): %02X%02X%02X%02X\n",
                stage,
                src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
                src[8], src[9], src[10], src[11], src[12], src[13], src[14], src[15],
                height / 2,
                src[(size_t)pitch * (height / 2) + 0], src[(size_t)pitch * (height / 2) + 1],
                src[(size_t)pitch * (height / 2) + 2], src[(size_t)pitch * (height / 2) + 3]);
    }

    return entry;
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

    if (use_vsh) {
        /* The SetTexture()/SetTextureStageState() calls just above are
         * D3D8 API state, but nothing reads it for this path -- neither
         * d3d8_shaders_prepare_draw() nor d3d8_combiners_prepare_draw()
         * ever runs here (see the "Texture upload + pixel shader" file
         * comment above for why). Bind a real texture and our own pixel
         * shader directly instead, so they're authoritative for VSH
         * draws regardless of what that dead D3D8 state says. */
        ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
        ID3D11PixelShader *ps = get_tex_pixel_shader();
        TexCacheEntry *te = resolve_texture_stage(0);

        if (ctx && ps && g_tex_ps_cb) {
            D3D11_MAPPED_SUBRESOURCE mapped;

            ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_tex_ps_cb,
                    0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                float *f = (float *)mapped.pData;
                f[0] = te ? te->width  : 1.0f;  /* texSize0.x */
                f[1] = te ? te->height : 1.0f;  /* texSize0.y */
                f[2] = te ? 1.0f : 0.0f;         /* hasTexture */
                f[3] = 0.0f;                      /* pad */
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_tex_ps_cb, 0);
            }
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_tex_ps_cb);

            if (te) {
                ID3D11SamplerState *samp = get_tex_sampler();
                ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &te->srv);
                if (samp)
                    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &samp);
            } else {
                /* Unbind stage 0 explicitly rather than leaving whatever
                 * a previous draw's SRV was -- resolve_texture_stage()
                 * returning NULL (disabled stage, or an unsupported
                 * format like the YUV movie texture) must mean "no
                 * texture," not "reuse the last one". */
                ID3D11ShaderResourceView *null_srv = NULL;
                ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
            }
        }

        /* Opt-in, same env var as the rest of this file's texture
         * diagnostics: how many VSH draws actually had a real texture
         * bound vs fell back to diffuse-only, so "textures upload fine
         * but the screen doesn't change" can be told apart from "stage 0
         * just isn't enabled for whichever draws end up visible." */
        if (getenv("XBOXRECOMP_TEXDUMP")) {
            static uint32_t textured_draws, untextured_draws;
            if (te) textured_draws++; else untextured_draws++;
            if ((textured_draws + untextured_draws) <= 10 ||
                (textured_draws + untextured_draws) % 2000 == 0) {
                fprintf(stderr, "[PGRAPH-D3D11] TEX draw stats: textured=%u "
                        "untextured=%u (this draw: stage0 enabled=%d te=%p)\n",
                        textured_draws, untextured_draws,
                        g_pg.tex[0].enabled, (void *)te);
            }
        }
    }

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
 * Texture state diagnostic (temporary, opt-in)
 *
 * Real texture upload (format decode, swizzle, mip handling) is not
 * implemented yet -- this dumps decoded texture-stage state, once per
 * distinct value seen per stage, so what Breakdown actually programs
 * (swizzled vs linear, which COLOR code, dimensions, pitch) can be read
 * directly instead of guessed at before committing to an implementation.
 * Same pattern as XBOXRECOMP_VSHDUMP: env-gated, off by default, safe to
 * leave in. Field masks/shifts are NV097_SET_TEXTURE_* from nv2a_regs.h,
 * not restated from memory.
 * ══════════════════════════════════════════════════════════════════════ */
static void texdump_stage(int stage)
{
    static uint32_t last_offset[4], last_format[4], last_control0[4],
                     last_control1[4], last_image_rect[4];
    static int have_last[4];

    if (!getenv("XBOXRECOMP_TEXDUMP"))
        return;
    if (stage < 0 || stage >= 4)
        return;

    if (have_last[stage] &&
        last_offset[stage]     == g_pg.tex[stage].offset &&
        last_format[stage]     == g_pg.tex[stage].format &&
        last_control0[stage]   == g_pg.tex[stage].control0 &&
        last_control1[stage]   == g_pg.tex[stage].control1 &&
        last_image_rect[stage] == g_pg.tex[stage].image_rect)
        return;  /* Nothing changed since the last dump for this stage */

    last_offset[stage]     = g_pg.tex[stage].offset;
    last_format[stage]     = g_pg.tex[stage].format;
    last_control0[stage]   = g_pg.tex[stage].control0;
    last_control1[stage]   = g_pg.tex[stage].control1;
    last_image_rect[stage] = g_pg.tex[stage].image_rect;
    have_last[stage] = 1;

    uint32_t fmt = g_pg.tex[stage].format;
    uint32_t color = (fmt & NV097_SET_TEXTURE_FORMAT_COLOR) >> 8;
    uint32_t dim   = (fmt & NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY) >> 4;
    uint32_t mips  = (fmt & NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS) >> 16;
    uint32_t szu   = (fmt & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U) >> 20;
    uint32_t szv   = (fmt & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V) >> 24;
    uint32_t szp   = (fmt & NV097_SET_TEXTURE_FORMAT_BASE_SIZE_P) >> 28;
    uint32_t cube  = (fmt & NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE) ? 1u : 0u;
    uint32_t c0    = g_pg.tex[stage].control0;
    uint32_t c1    = g_pg.tex[stage].control1;
    uint32_t rect  = g_pg.tex[stage].image_rect;
    uint32_t pitch = (c1 & NV097_SET_TEXTURE_CONTROL1_IMAGE_PITCH) >> 16;
    uint32_t rect_w = (rect & NV097_SET_TEXTURE_IMAGE_RECT_WIDTH) >> 16;
    uint32_t rect_h = (rect & NV097_SET_TEXTURE_IMAGE_RECT_HEIGHT);

    fprintf(stderr, "[PGRAPH-D3D11] TEX[%d] offset=0x%08X format=0x%08X "
            "(color=0x%02X dim=%u mips=%u log2_u=%u log2_v=%u log2_p=%u cube=%u) "
            "control0=0x%08X (enable=%u) control1=0x%08X (pitch=%u) "
            "image_rect=0x%08X (w=%u h=%u)\n",
            stage, g_pg.tex[stage].offset, fmt, color, dim, mips, szu, szv, szp,
            cube, c0, (c0 >> 30) & 1, c1, pitch, rect, rect_w, rect_h);
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

    /* ── Viewport ──
     * On real hardware these two methods don't just update pgraph state --
     * they write DIRECTLY into fixed slots of the SAME constant register
     * file a vertex program reads via c[] (see xemu pgraph.c's own
     * SET_VIEWPORT_OFFSET/_SCALE handlers: `pg->vsh_constants[NV_IGRAPH_
     * XF_XFCTX_VPOFF/_VPSCL][slot] = parameter`), bypassing the generic
     * SET_TRANSFORM_CONSTANT load-pointer path entirely. Breakdown's own
     * compiled vertex program reads c[NV_IGRAPH_XF_XFCTX_VPSCL] (=c[58])
     * and c[NV_IGRAPH_XF_XFCTX_VPOFF] (=c[59]) directly to do exactly the
     * `screen = ndc*scale + offset` transform a title doing this in
     * fixed-function would get automatically -- confirmed via
     * XBOXRECOMP_VSHDUMP: those two registers are never touched by any
     * SET_TRANSFORM_CONSTANT write in this title at all, so without this
     * mirror they silently stay at their zero-init default and every
     * vertex collapses to (0,0,0,1) even though the shader itself is
     * decoded and translated correctly. Mirroring into d3d8_vsh's
     * constant array here reproduces that hardware behavior. */
    case NV097_SET_VIEWPORT_OFFSET:
    case NV097_SET_VIEWPORT_OFFSET + 4:
    case NV097_SET_VIEWPORT_OFFSET + 8:
    case NV097_SET_VIEWPORT_OFFSET + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
        g_pg.vp_offset[idx] = u2f(param);
        d3d8_vsh_set_constant(NV_IGRAPH_XF_XFCTX_VPOFF, g_pg.vp_offset, 1);
        if (idx == 3 && getenv("XBOXRECOMP_VSHDUMP")) {
            fprintf(stderr, "[PGRAPH-D3D11] VPOFF -> c[%d] = (%.4f, %.4f, %.4f, %.4f)\n",
                    NV_IGRAPH_XF_XFCTX_VPOFF, g_pg.vp_offset[0], g_pg.vp_offset[1],
                    g_pg.vp_offset[2], g_pg.vp_offset[3]);
        }
        return 1;
    }

    case NV097_SET_VIEWPORT_SCALE:
    case NV097_SET_VIEWPORT_SCALE + 4:
    case NV097_SET_VIEWPORT_SCALE + 8:
    case NV097_SET_VIEWPORT_SCALE + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_SCALE) / 4;
        g_pg.vp_scale[idx] = u2f(param);
        d3d8_vsh_set_constant(NV_IGRAPH_XF_XFCTX_VPSCL, g_pg.vp_scale, 1);
        if (idx == 3 && getenv("XBOXRECOMP_VSHDUMP")) {
            fprintf(stderr, "[PGRAPH-D3D11] VPSCL -> c[%d] = (%.4f, %.4f, %.4f, %.4f)\n",
                    NV_IGRAPH_XF_XFCTX_VPSCL, g_pg.vp_scale[0], g_pg.vp_scale[1],
                    g_pg.vp_scale[2], g_pg.vp_scale[3]);
        }
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
        texdump_stage(stage);
        return 1;
    }
    case NV097_SET_TEXTURE_FORMAT:
    case NV097_SET_TEXTURE_FORMAT + 0x40:
    case NV097_SET_TEXTURE_FORMAT + 0x80:
    case NV097_SET_TEXTURE_FORMAT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FORMAT) / 0x40;
        g_pg.tex[stage].format = param;
        texdump_stage(stage);
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
        texdump_stage(stage);
        return 1;
    }

    /* CONTROL1 (pitch) and IMAGE_RECT (width/height) only matter for
     * LINEAR formats -- SWIZZLED/compressed ones are tightly packed and
     * get their (power-of-two) dimensions from FORMAT's BASE_SIZE_U/V log2
     * fields instead (see the g_pg.tex[] struct comment above). Both used
     * to fall into the 0x1B00-0x1C00 ignore range; tracked now because
     * real texture upload needs them for whichever formats turn out to be
     * LINEAR -- see texdump_stage() below for how that's being determined
     * before committing to an upload implementation. */
    case NV097_SET_TEXTURE_CONTROL1:
    case NV097_SET_TEXTURE_CONTROL1 + 0x40:
    case NV097_SET_TEXTURE_CONTROL1 + 0x80:
    case NV097_SET_TEXTURE_CONTROL1 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL1) / 0x40;
        g_pg.tex[stage].control1 = param;
        texdump_stage(stage);
        return 1;
    }

    case NV097_SET_TEXTURE_IMAGE_RECT:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x40:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0x80:
    case NV097_SET_TEXTURE_IMAGE_RECT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_IMAGE_RECT) / 0x40;
        g_pg.tex[stage].image_rect = param;
        texdump_stage(stage);
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
        /* TEMPORARY diagnostic (XBOXRECOMP_TEXDUMP): is per-vertex diffuse
         * ever actually part of the stream for any draw, independent of
         * both texturing and the current_diffuse default question? */
        if (getenv("XBOXRECOMP_TEXDUMP") && slot == NV2A_VERTEX_ATTR_DIFFUSE &&
            cnt != g_pg.vattr[slot].count) {
            fprintf(stderr, "[PGRAPH-D3D11] vattr[DIFFUSE] count: %u -> %u\n",
                    g_pg.vattr[slot].count, cnt);
        }
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
        /* TEMPORARY diagnostic (XBOXRECOMP_TEXDUMP): does this title ever
         * actually call this at all, or does every draw run on the static
         * default the whole session? Deduped by value so it doesn't flood
         * if the title does call it frequently with the same color. */
        if (getenv("XBOXRECOMP_TEXDUMP") && param != g_pg.current_diffuse) {
            fprintf(stderr, "[PGRAPH-D3D11] SET_DIFFUSE_COLOR4UB: 0x%08X -> 0x%08X\n",
                    g_pg.current_diffuse, param);
        }
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
