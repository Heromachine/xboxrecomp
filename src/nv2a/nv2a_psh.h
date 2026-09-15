/*
 * NV2A register combiners -> HLSL pixel shader.
 *
 * A port of xemu's GLSL generator (hw/xbox/nv2a/pgraph/glsl/psh.c) to HLSL,
 * driven by the raw combiner registers a title writes through the NV097
 * methods (SET_COMBINER_*, SET_SHADER_STAGE_PROGRAM, ...). Kept free of D3D11
 * so the generated text can be checked on the host.
 *
 * What is not ported: window clip regions, colour keys, convolution filters,
 * texture borders, point sprites, the BRDF and reflection texture modes, and
 * xemu's per-pixel depth reconstruction (the rasterizer's depth is used). A
 * texture mode that is not ported samples as zero and is named in a comment.
 */
#ifndef NV2A_PSH_H
#define NV2A_PSH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t combiner_control;     /* SET_COMBINER_CONTROL */
    uint32_t shader_stage_program; /* SET_SHADER_STAGE_PROGRAM */
    uint32_t other_stage_input;    /* SET_SHADER_OTHER_STAGE_INPUT | DOT_RGBMAPPING */
    uint32_t final_inputs_0;       /* SET_COMBINER_SPECULAR_FOG_CW0 */
    uint32_t final_inputs_1;       /* SET_COMBINER_SPECULAR_FOG_CW1 */
    uint32_t rgb_inputs[8], rgb_outputs[8];
    uint32_t alpha_inputs[8], alpha_outputs[8];
    uint32_t clip_plane_mode;      /* SET_SHADER_CLIP_PLANE_MODE */

    /* Per texture stage, from SET_TEXTURE_FORMAT/CONTROL0. */
    uint8_t enabled[4];
    uint8_t rect_tex[4];           /* linear format: texel-space coordinates */
    uint8_t shadow_map[4];         /* depth format: compare, don't sample */
    uint8_t alphakill[4];
    uint8_t dim_tex[4];            /* 2 or 3 */

    uint8_t shadow_depth_func;     /* SET_SHADOW_DEPTH_FUNC, 0..7 */
    uint8_t alpha_test;
    uint8_t alpha_func;            /* GL enum minus 0x200: 0 NEVER .. 7 ALWAYS */
    uint8_t z_perspective;         /* CONTROL0 Z_PERSPECTIVE_ENABLE: w-buffer */
    uint8_t poly_offset;           /* SET_POLY_OFFSET_FILL_ENABLE on a triangle draw */

    /* Debug (XBOXRECOMP_DBG_*, set by the translator; 0 = off).
     * dbg_shflip: 1 flip v, 2 flip u, 3 both, for shadow-map lookups.
     * dbg_out: replace the output rgb -- 1 t0, 2 v0, 3 r0, 4 t1. */
    uint8_t dbg_shflip;
    uint8_t dbg_out;
} NV2APshState;

/* Pixel shader constant buffer at register(b0). 16-byte aligned; matches the
 * cbuffer the generator emits. */
typedef struct {
    float consts[18][4];   /* c0/c1 of stages 0-7, then of the final combiner */
    float fog_color[4];
    float bump_mat[4][4];  /* m00 m01 m10 m11 per stage */
    float bump_scale[4];
    float bump_offset[4];
    float depth_max[4];    /* shadow map depth range per stage */
    float alpha_ref;       /* 0..255 */
    float depth_scale;     /* 1 / depth range top, for w-buffered depth */
    float depth_offset;    /* SET_POLYGON_OFFSET_BIAS, depth units */
    float depth_factor;    /* SET_POLYGON_OFFSET_SCALE_FACTOR */
} NV2APshConstants;

/* NV097 texture colour codes that address texels by pitch (xemu's
 * kelvin_color_format_info_map "linear"). */
int nv2a_psh_tex_is_linear(uint32_t color);

/* NV097 texture colour codes that hold depth, and the depth each one's
 * integer range tops out at (0 for a non-depth format). */
float nv2a_psh_tex_depth_max(uint32_t color);

/* Write the HLSL for `s` into buf. Returns the length, or -1 if cap is too
 * small. */
int nv2a_psh_generate(const NV2APshState *s, char *buf, size_t cap);

#endif
