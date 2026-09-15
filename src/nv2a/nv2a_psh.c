/*
 * NV2A register combiners -> HLSL pixel shader. See nv2a_psh.h.
 *
 * Structure and names follow xemu's pgraph/glsl/psh.c so the two can be read
 * side by side: parse the registers into stage/output descriptions, emit the
 * texture fetches, then each general combiner stage, then the final combiner
 * and alpha test.
 */
#include "nv2a_psh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* pgraph/psh_regs.h */
enum {
    PS_TEXTUREMODES_NONE = 0x00,
    PS_TEXTUREMODES_PROJECT2D = 0x01,
    PS_TEXTUREMODES_PROJECT3D = 0x02,
    PS_TEXTUREMODES_CUBEMAP = 0x03,
    PS_TEXTUREMODES_PASSTHRU = 0x04,
    PS_TEXTUREMODES_CLIPPLANE = 0x05,
    PS_TEXTUREMODES_BUMPENVMAP = 0x06,
    PS_TEXTUREMODES_BUMPENVMAP_LUM = 0x07,
    PS_TEXTUREMODES_BRDF = 0x08,
    PS_TEXTUREMODES_DOT_ST = 0x09,
    PS_TEXTUREMODES_DOT_ZW = 0x0a,
    PS_TEXTUREMODES_DOT_RFLCT_DIFF = 0x0b,
    PS_TEXTUREMODES_DOT_RFLCT_SPEC = 0x0c,
    PS_TEXTUREMODES_DOT_STR_3D = 0x0d,
    PS_TEXTUREMODES_DOT_STR_CUBE = 0x0e,
    PS_TEXTUREMODES_DPNDNT_AR = 0x0f,
    PS_TEXTUREMODES_DPNDNT_GB = 0x10,
    PS_TEXTUREMODES_DOTPRODUCT = 0x11,
    PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST = 0x12,
};

enum {
    PS_INPUTMAPPING_UNSIGNED_IDENTITY = 0x00,
    PS_INPUTMAPPING_UNSIGNED_INVERT = 0x20,
    PS_INPUTMAPPING_EXPAND_NORMAL = 0x40,
    PS_INPUTMAPPING_EXPAND_NEGATE = 0x60,
    PS_INPUTMAPPING_HALFBIAS_NORMAL = 0x80,
    PS_INPUTMAPPING_HALFBIAS_NEGATE = 0xa0,
    PS_INPUTMAPPING_SIGNED_IDENTITY = 0xc0,
    PS_INPUTMAPPING_SIGNED_NEGATE = 0xe0,
};

enum {
    PS_REGISTER_DISCARD = 0x00,
    PS_REGISTER_C0 = 0x01,
    PS_REGISTER_C1 = 0x02,
    PS_REGISTER_FOG = 0x03,
    PS_REGISTER_V0 = 0x04,
    PS_REGISTER_V1 = 0x05,
    PS_REGISTER_T0 = 0x08,
    PS_REGISTER_T1 = 0x09,
    PS_REGISTER_T2 = 0x0a,
    PS_REGISTER_T3 = 0x0b,
    PS_REGISTER_R0 = 0x0c,
    PS_REGISTER_R1 = 0x0d,
    PS_REGISTER_V1R0_SUM = 0x0e,
    PS_REGISTER_EF_PROD = 0x0f,
};

enum {
    PS_COMBINERCOUNT_MUX_MSB = 0x0001,
    PS_COMBINERCOUNT_UNIQUE_C0 = 0x0010,
    PS_COMBINERCOUNT_UNIQUE_C1 = 0x0100,
};

enum {
    PS_COMBINEROUTPUT_IDENTITY = 0x00,
    PS_COMBINEROUTPUT_BIAS = 0x08,
    PS_COMBINEROUTPUT_SHIFTLEFT_1 = 0x10,
    PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS = 0x18,
    PS_COMBINEROUTPUT_SHIFTLEFT_2 = 0x20,
    PS_COMBINEROUTPUT_SHIFTRIGHT_1 = 0x30,
    PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA = 0x80,
    PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA = 0x40,
    PS_COMBINEROUTPUT_AB_DOT_PRODUCT = 0x02,
    PS_COMBINEROUTPUT_CD_DOT_PRODUCT = 0x01,
    PS_COMBINEROUTPUT_AB_CD_MUX = 0x04,
};

enum { PS_CHANNEL_RGB = 0x00, PS_CHANNEL_ALPHA = 0x10 };

enum {
    PS_FINALCOMBINERSETTING_CLAMP_SUM = 0x80,
    PS_FINALCOMBINERSETTING_COMPLEMENT_V1 = 0x40,
    PS_FINALCOMBINERSETTING_COMPLEMENT_R0 = 0x20,
};

#define VAR_LEN 512

typedef struct { int reg, mod, chan; } InputInfo;
typedef struct { InputInfo a, b, c, d; } InputVarInfo;
typedef struct {
    InputInfo a, b, c, d, e, f, g;
    int clamp_sum, inv_v1, inv_r0, enabled;
} FCInputInfo;
typedef struct {
    int ab, cd, muxsum, flags, ab_op, cd_op, muxsum_op, mapping;
} OutputInfo;
typedef struct {
    InputVarInfo rgb_input, alpha_input;
    OutputInfo rgb_output, alpha_output;
} StageInfo;

typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
} Sb;

typedef struct {
    const NV2APshState *state;
    int num_stages, flags, cur_stage;
    StageInfo stage[8];
    FCInputInfo final_input;
    int tex_modes[4], input_tex[4], dot_map[4];
    int uses_r0, uses_r1;
    char var_e[VAR_LEN], var_f[VAR_LEN];
} Psh;

static void sb_printf(Sb *sb, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (sb->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->len) {
        sb->overflow = 1;
        return;
    }
    sb->len += (size_t)n;
}

int nv2a_psh_tex_is_linear(uint32_t color)
{
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x17:
    case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
    case 0x24: case 0x25:
    case 0x2E: case 0x2F: case 0x30: case 0x31:
    case 0x35: case 0x3F: case 0x40: case 0x41:
        return 1;
    default:
        return 0;
    }
}

float nv2a_psh_tex_depth_max(uint32_t color)
{
    switch (color) {
    case 0x2C: case 0x30: return 65535.0f;       /* Y16 fixed */
    case 0x31: return 511.9375f;                 /* Y16 float */
    case 0x2E: case 0x2F: return 16777215.0f;    /* X8_Y24 */
    default: return 0.0f;
    }
}

/* get_var: the HLSL name a register reads (or writes) as. An empty string
 * for a discarded destination. */
static void get_var(Psh *ps, int reg, int is_dest, char *out, size_t cap)
{
    switch (reg) {
    case PS_REGISTER_DISCARD:
        snprintf(out, cap, "%s", is_dest ? "" : "float4(0.0, 0.0, 0.0, 0.0)");
        break;
    case PS_REGISTER_C0:
        if ((ps->flags & PS_COMBINERCOUNT_UNIQUE_C0) || ps->cur_stage == 8)
            snprintf(out, cap, "consts[%d]", ps->cur_stage * 2);
        else
            snprintf(out, cap, "consts[0]");
        break;
    case PS_REGISTER_C1:
        if ((ps->flags & PS_COMBINERCOUNT_UNIQUE_C1) || ps->cur_stage == 8)
            snprintf(out, cap, "consts[%d]", ps->cur_stage * 2 + 1);
        else
            snprintf(out, cap, "consts[1]");
        break;
    case PS_REGISTER_FOG: snprintf(out, cap, "pFog"); break;
    case PS_REGISTER_V0: snprintf(out, cap, "v0"); break;
    case PS_REGISTER_V1: snprintf(out, cap, "v1"); break;
    case PS_REGISTER_T0: snprintf(out, cap, "t0"); break;
    case PS_REGISTER_T1: snprintf(out, cap, "t1"); break;
    case PS_REGISTER_T2: snprintf(out, cap, "t2"); break;
    case PS_REGISTER_T3: snprintf(out, cap, "t3"); break;
    case PS_REGISTER_R0: ps->uses_r0 = 1; snprintf(out, cap, "r0"); break;
    case PS_REGISTER_R1: ps->uses_r1 = 1; snprintf(out, cap, "r1"); break;
    case PS_REGISTER_V1R0_SUM:
        ps->uses_r0 = 1;
        snprintf(out, cap,
                 ps->final_input.clamp_sum
                     ? "saturate(float4(%s.rgb + %s.rgb, 0.0))"
                     : "float4(%s.rgb + %s.rgb, 0.0)",
                 ps->final_input.inv_v1 ? "(1.0 - v1)" : "v1",
                 ps->final_input.inv_r0 ? "(1.0 - r0)" : "r0");
        break;
    case PS_REGISTER_EF_PROD:
        snprintf(out, cap, "float4(%s * %s, 0.0)", ps->var_e, ps->var_f);
        break;
    default:
        snprintf(out, cap, "float4(0.0, 0.0, 0.0, 0.0)");
        break;
    }
}

static void get_input_var(Psh *ps, InputInfo in, int is_alpha, char *out,
                          size_t cap)
{
    char reg[VAR_LEN];
    size_t n;

    get_var(ps, in.reg, 0, reg, sizeof(reg));
    n = strlen(reg);
    if (!is_alpha)
        snprintf(reg + n, sizeof(reg) - n, "%s",
                 in.chan == PS_CHANNEL_ALPHA ? ".aaa" : ".rgb");
    else
        snprintf(reg + n, sizeof(reg) - n, "%s",
                 in.chan == PS_CHANNEL_ALPHA ? ".a" : ".b");

    switch (in.mod) {
    case PS_INPUTMAPPING_UNSIGNED_IDENTITY:
        snprintf(out, cap, "max(%s, 0.0)", reg); break;
    case PS_INPUTMAPPING_UNSIGNED_INVERT:
        snprintf(out, cap, "(1.0 - saturate(%s))", reg); break;
    case PS_INPUTMAPPING_EXPAND_NORMAL:
        snprintf(out, cap, "(2.0 * max(%s, 0.0) - 1.0)", reg); break;
    case PS_INPUTMAPPING_EXPAND_NEGATE:
        snprintf(out, cap, "(-2.0 * max(%s, 0.0) + 1.0)", reg); break;
    case PS_INPUTMAPPING_HALFBIAS_NORMAL:
        snprintf(out, cap, "(max(%s, 0.0) - 0.5)", reg); break;
    case PS_INPUTMAPPING_HALFBIAS_NEGATE:
        snprintf(out, cap, "(-max(%s, 0.0) + 0.5)", reg); break;
    case PS_INPUTMAPPING_SIGNED_IDENTITY:
        snprintf(out, cap, "%s", reg); break;
    case PS_INPUTMAPPING_SIGNED_NEGATE:
    default:
        snprintf(out, cap, "-%s", reg); break;
    }
}

static void get_output(const char *reg, int mapping, char *out, size_t cap)
{
    switch (mapping) {
    case PS_COMBINEROUTPUT_BIAS:
        snprintf(out, cap, "(%s - 0.5)", reg); break;
    case PS_COMBINEROUTPUT_SHIFTLEFT_1:
        snprintf(out, cap, "(%s * 2.0)", reg); break;
    case PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS:
        snprintf(out, cap, "((%s - 0.5) * 2.0)", reg); break;
    case PS_COMBINEROUTPUT_SHIFTLEFT_2:
        snprintf(out, cap, "(%s * 4.0)", reg); break;
    case PS_COMBINEROUTPUT_SHIFTRIGHT_1:
        snprintf(out, cap, "(%s / 2.0)", reg); break;
    case PS_COMBINEROUTPUT_IDENTITY:
    default:
        snprintf(out, cap, "%s", reg); break;
    }
}

/* add_stage_code: one channel (rgb or a) of one general combiner stage.
 * The intermediate ab/cd/mux_sum assignments go to `code`; the writes to the
 * destination registers are appended to `writes`, so the rgb and alpha halves
 * of a stage both read the registers as they were before the stage. */
static void add_stage_code(Psh *ps, Sb *code, Sb *writes, InputVarInfo input,
                           OutputInfo output, const char *mask, int is_alpha)
{
    char a[VAR_LEN], b[VAR_LEN], c[VAR_LEN], d[VAR_LEN];
    char ab[2 * VAR_LEN + 16], cd[2 * VAR_LEN + 16];
    char ab_map[2 * VAR_LEN + 32], cd_map[2 * VAR_LEN + 32];
    char muxsum[4 * VAR_LEN + 128], muxsum_map[4 * VAR_LEN + 160];
    char ab_dest[VAR_LEN], cd_dest[VAR_LEN], muxsum_dest[VAR_LEN];
    const char *caster = is_alpha ? "" : "(float3)";

    get_input_var(ps, input.a, is_alpha, a, sizeof(a));
    get_input_var(ps, input.b, is_alpha, b, sizeof(b));
    get_input_var(ps, input.c, is_alpha, c, sizeof(c));
    get_input_var(ps, input.d, is_alpha, d, sizeof(d));

    if (output.ab_op == PS_COMBINEROUTPUT_AB_DOT_PRODUCT)
        snprintf(ab, sizeof(ab), "dot(%s, %s)", a, b);
    else
        snprintf(ab, sizeof(ab), "(%s * %s)", a, b);
    if (output.cd_op == PS_COMBINEROUTPUT_CD_DOT_PRODUCT)
        snprintf(cd, sizeof(cd), "dot(%s, %s)", c, d);
    else
        snprintf(cd, sizeof(cd), "(%s * %s)", c, d);

    get_output(ab, output.mapping, ab_map, sizeof(ab_map));
    get_output(cd, output.mapping, cd_map, sizeof(cd_map));
    get_var(ps, output.ab, 1, ab_dest, sizeof(ab_dest));
    get_var(ps, output.cd, 1, cd_dest, sizeof(cd_dest));
    get_var(ps, output.muxsum, 1, muxsum_dest, sizeof(muxsum_dest));

    if (ab_dest[0])
        sb_printf(code, "ab.%s = clamp(%s(%s), -1.0, 1.0);\n", mask, caster, ab_map);
    if (cd_dest[0])
        sb_printf(code, "cd.%s = clamp(%s(%s), -1.0, 1.0);\n", mask, caster, cd_map);

    if (output.muxsum_op == PS_COMBINEROUTPUT_AB_CD_MUX)
        snprintf(muxsum, sizeof(muxsum), "((%s) ? %s(%s) : %s(%s))",
                 (ps->flags & PS_COMBINERCOUNT_MUX_MSB)
                     ? "r0.a >= 0.5" : "(((uint)(r0.a * 255.0)) & 1u) == 1u",
                 caster, cd, caster, ab);
    else
        snprintf(muxsum, sizeof(muxsum), "(%s + %s)", ab, cd);
    if (output.muxsum_op == PS_COMBINEROUTPUT_AB_CD_MUX)
        ps->uses_r0 = 1;
    get_output(muxsum, output.mapping, muxsum_map, sizeof(muxsum_map));
    if (muxsum_dest[0])
        sb_printf(code, "mux_sum.%s = clamp(%s(%s), -1.0, 1.0);\n", mask, caster,
                  muxsum_map);

    if (ab_dest[0]) {
        sb_printf(writes, "%s.%s = ab.%s;\n", ab_dest, mask, mask);
        if (!is_alpha && (output.flags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA))
            sb_printf(writes, "%s.a = ab.b;\n", ab_dest);
    }
    if (cd_dest[0]) {
        sb_printf(writes, "%s.%s = cd.%s;\n", cd_dest, mask, mask);
        if (!is_alpha && (output.flags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA))
            sb_printf(writes, "%s.a = cd.b;\n", cd_dest);
    }
    if (muxsum_dest[0])
        sb_printf(writes, "%s.%s = mux_sum.%s;\n", muxsum_dest, mask, mask);
}

static void add_final_stage_code(Psh *ps, Sb *code)
{
    char a[VAR_LEN], b[VAR_LEN], c[VAR_LEN], d[VAR_LEN], g[VAR_LEN];

    get_input_var(ps, ps->final_input.e, 0, ps->var_e, sizeof(ps->var_e));
    get_input_var(ps, ps->final_input.f, 0, ps->var_f, sizeof(ps->var_f));
    get_input_var(ps, ps->final_input.a, 0, a, sizeof(a));
    get_input_var(ps, ps->final_input.b, 0, b, sizeof(b));
    get_input_var(ps, ps->final_input.c, 0, c, sizeof(c));
    get_input_var(ps, ps->final_input.d, 0, d, sizeof(d));
    get_input_var(ps, ps->final_input.g, 1, g, sizeof(g));

    /* mix(c, b, a) */
    sb_printf(code, "fragColor.rgb = %s + lerp((float3)(%s), (float3)(%s), (float3)(%s));\n",
              d, c, b, a);
    sb_printf(code, "fragColor.a = %s;\n", g);
}

/* Sample coordinates for stage i, divided by the texture size for a linear
 * texture (xemu's norm%d). `coord` is a 2-component expression. */
static void tex_uv(const Psh *ps, int i, const char *coord, char *out, size_t cap)
{
    if (ps->state->rect_tex[i])
        snprintf(out, cap, "((%s) / texSize%d)", coord, i);
    else
        snprintf(out, cap, "(%s)", coord);
}

static const char *dotmap_func(int map)
{
    static const char *names[] = {
        "dotmap_zero_to_one", "dotmap_minus1_to_1_d3d", "dotmap_minus1_to_1_gl",
        "dotmap_minus1_to_1", "dotmap_zero_to_one", "dotmap_zero_to_one",
        "dotmap_zero_to_one", "dotmap_zero_to_one",
    };
    return names[map & 7];
}

static void append_shadowmap(const Psh *ps, Sb *vars, int i, int compare_z)
{
    static const char *cmp[] = { "<", "<", "==", "<=", ">", "!=", ">=", "<" };
    char uv[128];
    int func = ps->state->shadow_depth_func;

    if (func == 0) {
        sb_printf(vars, "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n", i);
        return;
    }
    if (func == 7) {
        sb_printf(vars, "float4 t%d = float4(1.0, 1.0, 1.0, 1.0);\n", i);
        return;
    }
    {
        int m = ps->state->dbg_shflip;
        const char *expr = m == 1 ? "float2(pT%d.x / pT%d.w, 1.0 - pT%d.y / pT%d.w)" :
                           m == 2 ? "float2(1.0 - pT%d.x / pT%d.w, pT%d.y / pT%d.w)" :
                           m == 3 ? "float2(1.0 - pT%d.x / pT%d.w, 1.0 - pT%d.y / pT%d.w)" :
                           "pT%d.xy / pT%d.w";
        char e2[160];
        snprintf(e2, sizeof(e2), expr, i, i, i, i);
        tex_uv(ps, i, e2, uv, sizeof(uv));
    }
    sb_printf(vars, "float t%d_depth = texSamp%d.Sample(samp%d, ", i, i, i);
    sb_printf(vars, uv, i, i);
    sb_printf(vars, ").r * depthMax[%d];\n", i);
    if (compare_z) {
        sb_printf(vars,
                  "float t%d_ref = clamp(pT%d.z / pT%d.w, 0.0, depthMax[%d]);\n"
                  "float4 t%d = (float4)(t%d_depth %s t%d_ref ? 1.0 : 0.0);\n",
                  i, i, i, i, i, i, cmp[func], i);
    } else {
        sb_printf(vars, "float4 t%d = (float4)(t%d_depth %s 0.0 ? 1.0 : 0.0);\n",
                  i, i, cmp[func]);
    }
}

static void emit_texture(Psh *ps, Sb *vars, int i)
{
    const NV2APshState *st = ps->state;
    char uv[256];
    int in = ps->input_tex[i];

    switch (ps->tex_modes[i]) {
    case PS_TEXTUREMODES_NONE:
        sb_printf(vars, "float4 t%d = float4(0.0, 0.0, 0.0, 1.0);\n", i);
        return;
    case PS_TEXTUREMODES_PROJECT2D:
        if (st->shadow_map[i]) {
            append_shadowmap(ps, vars, i, 0);
        } else {
            char c[32];
            snprintf(c, sizeof(c), "pT%d.xy / pT%d.w", i, i);
            tex_uv(ps, i, c, uv, sizeof(uv));
            sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, %s);\n", i, i, i, uv);
        }
        break;
    case PS_TEXTUREMODES_PROJECT3D:
        if (st->shadow_map[i]) {
            append_shadowmap(ps, vars, i, 1);
        } else {
            char c[32];
            snprintf(c, sizeof(c), "pT%d.xy / pT%d.w", i, i);
            tex_uv(ps, i, c, uv, sizeof(uv));
            sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, %s);\n", i, i, i, uv);
        }
        break;
    case PS_TEXTUREMODES_CUBEMAP:
        /* No cube textures are uploaded; xemu's remap onto a 2D texture. */
        sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, remapCubeTo2D(pT%d.xyz));\n",
                  i, i, i, i);
        break;
    case PS_TEXTUREMODES_PASSTHRU:
        sb_printf(vars, "float4 t%d = pT%d;\n", i, i);
        return;
    case PS_TEXTUREMODES_CLIPPLANE: {
        int j;
        sb_printf(vars, "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n", i);
        for (j = 0; j < 4; j++)
            sb_printf(vars, "if (pT%d.%c %s 0.0) { discard; }\n", i, "xyzw"[j],
                      ((st->clip_plane_mode >> (4 * i + j)) & 1) ? ">=" : "<");
        return;
    }
    case PS_TEXTUREMODES_BUMPENVMAP:
        if (i < 1)
            goto unported;
        sb_printf(vars,
                  "float2 dsdt%d = float2(sign3(t%d.b), sign3(t%d.g));\n"
                  "dsdt%d = float2(bumpMat[%d].x * dsdt%d.x + bumpMat[%d].z * dsdt%d.y,\n"
                  "                bumpMat[%d].y * dsdt%d.x + bumpMat[%d].w * dsdt%d.y);\n",
                  i, in, in, i, i, i, i, i, i, i, i, i);
        {
            char c[48];
            snprintf(c, sizeof(c), "pT%d.xy + dsdt%d", i, i);
            tex_uv(ps, i, c, uv, sizeof(uv));
        }
        sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, %s);\n", i, i, i, uv);
        break;
    case PS_TEXTUREMODES_BUMPENVMAP_LUM:
        if (i < 1)
            goto unported;
        sb_printf(vars,
                  "float3 dsdtl%d = float3(sign3(t%d.b), sign3(t%d.g), t%d.r);\n"
                  "dsdtl%d.xy = float2(bumpMat[%d].x * dsdtl%d.x + bumpMat[%d].z * dsdtl%d.y,\n"
                  "                    bumpMat[%d].y * dsdtl%d.x + bumpMat[%d].w * dsdtl%d.y);\n",
                  i, in, in, in, i, i, i, i, i, i, i, i, i);
        {
            char c[48];
            snprintf(c, sizeof(c), "pT%d.xy + dsdtl%d.xy", i, i);
            tex_uv(ps, i, c, uv, sizeof(uv));
        }
        sb_printf(vars,
                  "float4 t%d = texSamp%d.Sample(samp%d, %s);\n"
                  "t%d = t%d * (bumpScale[%d] * dsdtl%d.z + bumpOffset[%d]);\n",
                  i, i, i, uv, i, i, i, i, i);
        break;
    case PS_TEXTUREMODES_DOT_ST:
        if (i < 2)
            goto unported;
        sb_printf(vars,
                  "float dot%d = dot(pT%d.xyz, %s(t%d));\n"
                  "float2 dotST%d = float2(dot%d, dot%d);\n",
                  i, i, dotmap_func(ps->dot_map[i]), in, i, i - 1, i);
        {
            char c[16];
            snprintf(c, sizeof(c), "dotST%d", i);
            tex_uv(ps, i, c, uv, sizeof(uv));
        }
        sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, %s);\n", i, i, i, uv);
        break;
    case PS_TEXTUREMODES_DOT_ZW:
        if (i < 2)
            goto unported;
        sb_printf(vars, "float dot%d = dot(pT%d.xyz, %s(t%d));\n"
                        "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n",
                  i, i, dotmap_func(ps->dot_map[i]), in, i);
        return;
    case PS_TEXTUREMODES_DOT_STR_3D:
        if (i != 3)
            goto unported;
        sb_printf(vars,
                  "float dot%d = dot(pT%d.xyz, %s(t%d));\n"
                  "float3 dotSTR%d = float3(dot%d, dot%d, dot%d);\n",
                  i, i, dotmap_func(ps->dot_map[i]), in, i, i - 2, i - 1, i);
        {
            char c[24];
            snprintf(c, sizeof(c), "dotSTR%d.xy", i);
            tex_uv(ps, i, c, uv, sizeof(uv));
        }
        sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, %s);\n", i, i, i, uv);
        break;
    case PS_TEXTUREMODES_DPNDNT_AR:
    case PS_TEXTUREMODES_DPNDNT_GB:
        if (i < 1)
            goto unported;
        sb_printf(vars, "float4 t%d = texSamp%d.Sample(samp%d, t%d.%s);\n", i, i, i,
                  in, ps->tex_modes[i] == PS_TEXTUREMODES_DPNDNT_AR ? "ar" : "gb");
        break;
    case PS_TEXTUREMODES_DOTPRODUCT:
        if (i != 1 && i != 2)
            goto unported;
        sb_printf(vars, "float dot%d = dot(pT%d.xyz, %s(t%d));\n"
                        "float4 t%d = float4(0.0, 0.0, 0.0, 0.0);\n",
                  i, i, dotmap_func(ps->dot_map[i]), in, i);
        return;
    default:
    unported:
        sb_printf(vars, "float4 t%d = float4(0.0, 0.0, 0.0, 0.0); /* texture mode 0x%02X not ported */\n",
                  i, ps->tex_modes[i]);
        return;
    }

    /* A texture fetch happened: alpha kill applies. */
    if (st->alphakill[i])
        sb_printf(vars, "if (t%d.a == 0.0) { discard; }\n", i);
}

static void parse_input(InputInfo *var, int value)
{
    var->reg = value & 0xF;
    var->chan = value & 0x10;
    var->mod = value & 0xE0;
}

static void parse_combiner_inputs(uint32_t value, InputInfo *a, InputInfo *b,
                                  InputInfo *c, InputInfo *d)
{
    parse_input(d, value & 0xFF);
    parse_input(c, (value >> 8) & 0xFF);
    parse_input(b, (value >> 16) & 0xFF);
    parse_input(a, (value >> 24) & 0xFF);
}

static void parse_combiner_output(uint32_t value, OutputInfo *out)
{
    int flags = (int)(value >> 12);

    out->cd = value & 0xF;
    out->ab = (value >> 4) & 0xF;
    out->muxsum = (value >> 8) & 0xF;
    out->flags = flags;
    out->cd_op = flags & 1;
    out->ab_op = flags & 2;
    out->muxsum_op = flags & 4;
    out->mapping = flags & 0x38;
}

static const char k_preamble[] =
    "Texture2D texSamp0 : register(t0);\n"
    "Texture2D texSamp1 : register(t1);\n"
    "Texture2D texSamp2 : register(t2);\n"
    "Texture2D texSamp3 : register(t3);\n"
    "SamplerState samp0 : register(s0);\n"
    "SamplerState samp1 : register(s1);\n"
    "SamplerState samp2 : register(s2);\n"
    "SamplerState samp3 : register(s3);\n"
    "cbuffer NV2APsh : register(b0) {\n"
    "    float4 consts[18];\n"
    "    float4 fogColor;\n"
    "    float4 bumpMat[4];\n"
    "    float4 bumpScale;\n"
    "    float4 bumpOffset;\n"
    "    float4 depthMax;\n"
    "    float  alphaRef;\n"
    "    float  depthScale;\n"
    "    float  depthOffset;\n"
    "    float  depthFactor;\n"
    "};\n"
    /* The same order as d3d8_vsh.c's VS_OUT: D3D11 links stages by
     * position, so every field up to the last one read must be present. */
    "struct PS_IN {\n"
    "    float4 pos  : SV_POSITION;\n"
    "    float4 d0   : COLOR0;\n"
    "    float4 d1   : COLOR1;\n"
    "    float4 tc0  : TEXCOORD0;\n"
    "    float4 tc1  : TEXCOORD1;\n"
    "    float4 tc2  : TEXCOORD2;\n"
    "    float4 tc3  : TEXCOORD3;\n"
    "    float  fog  : FOG;\n"
    "    float  pts  : PSIZE;\n"
    "    float4 b0   : TEXCOORD4;\n"
    "    float4 b1   : TEXCOORD5;\n"
    "};\n"
    "float sign3(float x) {\n"
    "    x *= 255.0;\n"
    "    return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0;\n"
    "}\n"
    "float sign1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }\n"
    "float sign2(float x) {\n"
    "    x *= 255.0;\n"
    "    return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5;\n"
    "}\n"
    "float3 dotmap_zero_to_one(float4 c) { return c.rgb; }\n"
    "float3 dotmap_minus1_to_1_d3d(float4 c) { return float3(sign1(c.r), sign1(c.g), sign1(c.b)); }\n"
    "float3 dotmap_minus1_to_1_gl(float4 c) { return float3(sign2(c.r), sign2(c.g), sign2(c.b)); }\n"
    "float3 dotmap_minus1_to_1(float4 c) { return float3(sign3(c.r), sign3(c.g), sign3(c.b)); }\n"
    "float2 remapCubeTo2D(float3 t) {\n"
    "    float3 a = abs(t);\n"
    "    if (a.x > a.y && a.x > a.z) return (t.x > 0.0 ? float2(-t.z, t.y) : float2(t.z, t.y)) / a.x;\n"
    "    if (a.y > a.x && a.y > a.z) return (t.y > 0.0 ? float2(t.x, -t.z) : float2(t.x, t.z)) / a.y;\n"
    "    return (t.z > 0.0 ? float2(t.x, t.y) : float2(-t.x, t.y)) / a.z;\n"
    "}\n";

int nv2a_psh_generate(const NV2APshState *s, char *buf, size_t cap)
{
    static char vars_buf[16384], code_buf[32384], writes_buf[4096];
    Sb out = { buf, cap, 0, 0 };
    Sb vars = { vars_buf, sizeof(vars_buf), 0, 0 };
    Sb code = { code_buf, sizeof(code_buf), 0, 0 };
    Psh ps;
    int i;

    if (cap == 0)
        return -1;
    memset(&ps, 0, sizeof(ps));
    ps.state = s;
    ps.num_stages = s->combiner_control & 0xFF;
    if (ps.num_stages > 8)
        ps.num_stages = 8;
    ps.flags = (int)(s->combiner_control >> 8);
    for (i = 0; i < 4; i++)
        ps.tex_modes[i] = (s->shader_stage_program >> (i * 5)) & 0x1F;
    ps.dot_map[0] = 0;
    ps.dot_map[1] = (s->other_stage_input >> 0) & 0xF;
    ps.dot_map[2] = (s->other_stage_input >> 4) & 0xF;
    ps.dot_map[3] = (s->other_stage_input >> 8) & 0xF;
    ps.input_tex[0] = 0;
    ps.input_tex[1] = 0;
    ps.input_tex[2] = (s->other_stage_input >> 16) & 0xF;
    ps.input_tex[3] = (s->other_stage_input >> 20) & 0xF;
    for (i = 0; i < 4; i++)
        if (ps.input_tex[i] > 3)
            ps.input_tex[i] = 0;

    for (i = 0; i < ps.num_stages; i++) {
        parse_combiner_inputs(s->rgb_inputs[i], &ps.stage[i].rgb_input.a,
                              &ps.stage[i].rgb_input.b, &ps.stage[i].rgb_input.c,
                              &ps.stage[i].rgb_input.d);
        parse_combiner_inputs(s->alpha_inputs[i], &ps.stage[i].alpha_input.a,
                              &ps.stage[i].alpha_input.b, &ps.stage[i].alpha_input.c,
                              &ps.stage[i].alpha_input.d);
        parse_combiner_output(s->rgb_outputs[i], &ps.stage[i].rgb_output);
        parse_combiner_output(s->alpha_outputs[i], &ps.stage[i].alpha_output);
    }

    ps.final_input.enabled = s->final_inputs_0 || s->final_inputs_1;
    if (ps.final_input.enabled) {
        InputInfo blank;
        int flags = (int)(s->final_inputs_1 & 0xFF);
        parse_combiner_inputs(s->final_inputs_0, &ps.final_input.a,
                              &ps.final_input.b, &ps.final_input.c,
                              &ps.final_input.d);
        parse_combiner_inputs(s->final_inputs_1, &ps.final_input.e,
                              &ps.final_input.f, &ps.final_input.g, &blank);
        ps.final_input.clamp_sum = flags & PS_FINALCOMBINERSETTING_CLAMP_SUM;
        ps.final_input.inv_v1 = flags & PS_FINALCOMBINERSETTING_COMPLEMENT_V1;
        ps.final_input.inv_r0 = flags & PS_FINALCOMBINERSETTING_COMPLEMENT_R0;
    }

    sb_printf(&vars, "float4 pD0 = input.d0;\n");
    sb_printf(&vars, "float4 pD1 = input.d1;\n");
    sb_printf(&vars, "float4 pFog = float4(fogColor.rgb, saturate(input.fog));\n");
    sb_printf(&vars, "float4 pT0 = input.tc0;\n");
    sb_printf(&vars, "float4 pT1 = input.tc1;\n");
    sb_printf(&vars, "float4 pT2 = input.tc2;\n");
    sb_printf(&vars, "float4 pT3 = input.tc3;\n");
    sb_printf(&vars, "float4 v0 = pD0;\n");
    sb_printf(&vars, "float4 v1 = pD1;\n");
    sb_printf(&vars, "float4 ab = float4(0.0, 0.0, 0.0, 0.0);\n");
    sb_printf(&vars, "float4 cd = float4(0.0, 0.0, 0.0, 0.0);\n");
    sb_printf(&vars, "float4 mux_sum = float4(0.0, 0.0, 0.0, 0.0);\n");
    sb_printf(&vars, "float4 fragColor = float4(0.0, 0.0, 0.0, 0.0);\n");
    for (i = 0; i < 4; i++) {
        if (s->rect_tex[i])
            sb_printf(&vars, "float2 texSize%d; texSamp%d.GetDimensions(texSize%d.x, texSize%d.y);\n",
                      i, i, i, i);
    }

    for (i = 0; i < 4; i++)
        emit_texture(&ps, &vars, i);

    for (i = 0; i < ps.num_stages; i++) {
        Sb writes = { writes_buf, sizeof(writes_buf), 0, 0 };
        ps.cur_stage = i;
        sb_printf(&code, "// Stage %d\n", i);
        add_stage_code(&ps, &code, &writes, ps.stage[i].rgb_input,
                       ps.stage[i].rgb_output, "rgb", 0);
        add_stage_code(&ps, &code, &writes, ps.stage[i].alpha_input,
                       ps.stage[i].alpha_output, "a", 1);
        if (writes.overflow)
            code.overflow = 1;
        else
            sb_printf(&code, "%s", writes_buf);
    }

    if (ps.final_input.enabled) {
        ps.cur_stage = 8;
        sb_printf(&code, "// Final combiner\n");
        add_final_stage_code(&ps, &code);
    }

    if (s->alpha_test && s->alpha_func != 7) {
        static const char *ops[] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
        if (s->alpha_func == 0)
            sb_printf(&code, "discard;\n");
        else
            sb_printf(&code,
                      "if (!(round(fragColor.a * 255.0) %s alphaRef)) { discard; }\n",
                      ops[s->alpha_func & 7]);
    }

    /* xemu declares temporaries after the fact; r0's alpha starts as t0's. */
    if (ps.uses_r0)
        sb_printf(&vars, "float4 r0 = float4(0.0, 0.0, 0.0, %s);\n",
                  ps.tex_modes[0] != PS_TEXTUREMODES_NONE ? "t0.a" : "1.0");
    if (ps.uses_r1)
        sb_printf(&vars, "float4 r1 = float4(0.0, 0.0, 0.0, 0.0);\n");

    sb_printf(&out, "%s", k_preamble);
    if (s->z_perspective || s->poly_offset) {
        /* W-buffering: the stored depth is the pixel's eye w, not the
         * rasterizer's screen-space-linear z/w. xemu reconstructs w per
         * pixel from the triangle's vertices; D3D11 already hands the pixel
         * shader the perspective-correct clip w in SV_Position.w. Left to
         * z/w, large near triangles (a face in close-up) sorted wrongly and
         * the back of the head showed through.
         *
         * Polygon offset (decals flush with a wall) is xemu's: the bias is
         * added in depth units, the slope term only without w-buffering
         * (xemu leaves it unimplemented there). */
        sb_printf(&out, "float4 main(PS_IN input, out float oDepth : SV_Depth) : SV_TARGET {\n");
        if (s->z_perspective) {
            sb_printf(&out, "float zvalue = input.pos.w;\n");
            if (s->poly_offset)
                sb_printf(&out, "zvalue += depthOffset;\n");
        } else {
            sb_printf(&out,
                      "float zvalue = input.pos.z / depthScale;\n"
                      "zvalue += depthOffset + depthFactor * max(abs(ddx(zvalue)), abs(ddy(zvalue)));\n");
        }
        sb_printf(&out, "oDepth = saturate(zvalue * depthScale);\n");
    } else {
        sb_printf(&out, "float4 main(PS_IN input) : SV_TARGET {\n");
    }
    sb_printf(&out, "%s", vars_buf);
    sb_printf(&out, "%s", code_buf);
    switch (s->dbg_out) {
    case 1: sb_printf(&out, "fragColor.rgb = t0.rgb;\n"); break;
    case 2: sb_printf(&out, "fragColor.rgb = v0.rgb;\n"); break;
    case 3: if (ps.uses_r0) sb_printf(&out, "fragColor.rgb = r0.rgb;\n"); break;
    case 4: sb_printf(&out, "fragColor.rgb = t1.rgb; fragColor.a = 1.0;\n"); break;
    default: break;
    }
    sb_printf(&out, "return fragColor;\n}\n");

    if (out.overflow || vars.overflow || code.overflow)
        return -1;
    return (int)out.len;
}
