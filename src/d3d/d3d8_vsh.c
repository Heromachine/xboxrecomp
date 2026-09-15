/**
 * NV2A Vertex Shader Microcode to HLSL Translator - Implementation
 *
 * Translates NV2A 128-bit vertex shader microcode instructions into
 * HLSL vertex shader source, compiles them, and caches the results.
 *
 * The translation pipeline is:
 *   1. Parse: 128-bit instruction words -> NV2AVshInstruction structs
 *   2. Analyze: determine which input registers (v0-v15) are read
 *   3. Generate HLSL: emit HLSL code mapping NV2A ops to HLSL intrinsics
 *   4. Compile: D3DCompile -> ID3D11VertexShader
 *   5. Cache: hash microcode -> reuse compiled shader on subsequent draws
 *
 * The generated HLSL uses:
 *   - cbuffer at b1: 192 float4 constants (c0-c191)
 *   - Input semantics: ATTR0-ATTR15 mapped to v0-v15
 *   - Output semantics: SV_POSITION, COLOR0/1, TEXCOORD0-3, FOG, PSIZE
 */

#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * NV2A Instruction Bit Field Extraction
 *
 * Ported field-for-field from xemu's reference decoder (subprojects/
 * nv2a_vsh_cpu/src/nv2a_vsh_disassembler.c) -- see the field-by-field
 * documentation in d3d8_vsh.h above NV2AVshDstRegType for the full table
 * and why the old flat-128-bit model here was wrong in two independent
 * ways (word[0] unused, and operand fields scattered non-contiguously
 * across words 1-3), not just a single off-by-N offset. Do not restate
 * these positions from memory -- if a field is ever in doubt, re-open
 * that file instead.
 *
 * `EXTRACT(insn, word, start, size)` mirrors xemu's own macro exactly:
 * word `word` of the 4-DWORD instruction, bits [start, start+size).
 * ================================================================ */

#define VSH_EXTRACT(insn, word, start, size) \
    (((insn)[(word)] >> (start)) & ((uint32_t)(1u << (size)) - 1u))

/* Word 1 */
static inline uint32_t vsh_a_swizzle_w(const DWORD *t) { return VSH_EXTRACT(t, 1, 0, 2); }
static inline uint32_t vsh_a_swizzle_z(const DWORD *t) { return VSH_EXTRACT(t, 1, 2, 2); }
static inline uint32_t vsh_a_swizzle_y(const DWORD *t) { return VSH_EXTRACT(t, 1, 4, 2); }
static inline uint32_t vsh_a_swizzle_x(const DWORD *t) { return VSH_EXTRACT(t, 1, 6, 2); }
static inline uint32_t vsh_a_negate(const DWORD *t)    { return VSH_EXTRACT(t, 1, 8, 1); }
static inline uint32_t vsh_input_reg(const DWORD *t)   { return VSH_EXTRACT(t, 1, 9, 4); }
static inline uint32_t vsh_const_reg(const DWORD *t)   { return VSH_EXTRACT(t, 1, 13, 8); }
static inline uint32_t vsh_mac_opcode(const DWORD *t)  { return VSH_EXTRACT(t, 1, 21, 4); }
static inline uint32_t vsh_ilu_opcode(const DWORD *t)  { return VSH_EXTRACT(t, 1, 25, 3); }

/* Word 2 */
static inline uint32_t vsh_c_temp_hi(const DWORD *t)   { return VSH_EXTRACT(t, 2, 0, 2); }
static inline uint32_t vsh_c_swizzle_w(const DWORD *t) { return VSH_EXTRACT(t, 2, 2, 2); }
static inline uint32_t vsh_c_swizzle_z(const DWORD *t) { return VSH_EXTRACT(t, 2, 4, 2); }
static inline uint32_t vsh_c_swizzle_y(const DWORD *t) { return VSH_EXTRACT(t, 2, 6, 2); }
static inline uint32_t vsh_c_swizzle_x(const DWORD *t) { return VSH_EXTRACT(t, 2, 8, 2); }
static inline uint32_t vsh_c_negate(const DWORD *t)    { return VSH_EXTRACT(t, 2, 10, 1); }
static inline uint32_t vsh_b_type(const DWORD *t)      { return VSH_EXTRACT(t, 2, 11, 2); }
static inline uint32_t vsh_b_temp(const DWORD *t)      { return VSH_EXTRACT(t, 2, 13, 4); }
static inline uint32_t vsh_b_swizzle_w(const DWORD *t) { return VSH_EXTRACT(t, 2, 17, 2); }
static inline uint32_t vsh_b_swizzle_z(const DWORD *t) { return VSH_EXTRACT(t, 2, 19, 2); }
static inline uint32_t vsh_b_swizzle_y(const DWORD *t) { return VSH_EXTRACT(t, 2, 21, 2); }
static inline uint32_t vsh_b_swizzle_x(const DWORD *t) { return VSH_EXTRACT(t, 2, 23, 2); }
static inline uint32_t vsh_b_negate(const DWORD *t)    { return VSH_EXTRACT(t, 2, 25, 1); }
static inline uint32_t vsh_a_type(const DWORD *t)      { return VSH_EXTRACT(t, 2, 26, 2); }
static inline uint32_t vsh_a_temp(const DWORD *t)      { return VSH_EXTRACT(t, 2, 28, 4); }

/* Word 3 */
static inline uint32_t vsh_final(const DWORD *t)            { return VSH_EXTRACT(t, 3, 0, 1); }
static inline uint32_t vsh_rel_addr(const DWORD *t)          { return VSH_EXTRACT(t, 3, 1, 1); }
static inline uint32_t vsh_out_is_ilu(const DWORD *t)        { return VSH_EXTRACT(t, 3, 2, 1); }
static inline uint32_t vsh_out_index(const DWORD *t)         { return VSH_EXTRACT(t, 3, 3, 8); }
static inline uint32_t vsh_out_is_output(const DWORD *t)     { return VSH_EXTRACT(t, 3, 11, 1); }
static inline uint32_t vsh_out_writemask(const DWORD *t)     { return VSH_EXTRACT(t, 3, 12, 4); }
static inline uint32_t vsh_ilu_temp_writemask(const DWORD *t) { return VSH_EXTRACT(t, 3, 16, 4); }
static inline uint32_t vsh_out_temp_reg(const DWORD *t)      { return VSH_EXTRACT(t, 3, 20, 4); }
static inline uint32_t vsh_mac_temp_writemask(const DWORD *t) { return VSH_EXTRACT(t, 3, 24, 4); }
static inline uint32_t vsh_c_type(const DWORD *t)            { return VSH_EXTRACT(t, 3, 28, 2); }
static inline uint32_t vsh_c_temp_lo(const DWORD *t)         { return VSH_EXTRACT(t, 3, 30, 2); }

static inline uint32_t vsh_c_temp_reg(const DWORD *t)
{
    /* Genuinely split across two words -- see the header comment. */
    return ((vsh_c_temp_hi(t) & 3u) << 2) | (vsh_c_temp_lo(t) & 3u);
}

/* ================================================================
 * Module State
 * ================================================================ */

/* Stored shader programs */
static NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];
static int g_vsh_slot_count = 0;

/* Constant registers (192 float4) */
static NV2AVSConstants g_vsh_constants;
static BOOL g_vsh_constants_dirty = TRUE;

/* D3D11 constant buffer for VS constants */
static ID3D11Buffer *g_vsh_cb = NULL;

/* Shader cache: maps microcode hash to compiled shader + input layout */
typedef struct {
    uint32_t            hash;
    int                 in_use;
    ID3D11VertexShader *vs;
    ID3DBlob           *vs_blob;      /* Bytecode for input layout creation */
    ID3D11InputLayout  *layouts[16];  /* Cached layouts per input mask subset */
    uint16_t            layout_masks[16];
    int                 layout_count;
    uint16_t            inputs_read;  /* Which v registers are read */
} VshCacheEntry;

static VshCacheEntry g_vsh_cache[NV2A_VS_CACHE_SIZE];

/* ================================================================
 * Hash Function (FNV-1a)
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193;
    }
    return h;
}

/* ================================================================
 * Microcode Parser
 * ================================================================ */

static NV2AVshOutputReg decode_output_mux(uint32_t out_index)
{
    /* Cross-checked directly against out_reg_name[] in xemu's
     * hw/xbox/nv2a/pgraph/glsl/vsh-prog.c -- indices 1,2,13,14 are
     * reserved ("???" there) and 15 is "A0.x" (an alternate a0 write path
     * this translator does not model); only the low nibble of the 8-bit
     * field is ever meaningful. This table was already correct before
     * this rewrite -- the bug was in how the surrounding bits were
     * located, not in this mapping. */
    switch (out_index & 0xF) {
    case 0:  return NV2A_VSH_OUT_POS;
    case 3:  return NV2A_VSH_OUT_D0;
    case 4:  return NV2A_VSH_OUT_D1;
    case 5:  return NV2A_VSH_OUT_FOG;
    case 6:  return NV2A_VSH_OUT_PTS;
    case 7:  return NV2A_VSH_OUT_B0;
    case 8:  return NV2A_VSH_OUT_B1;
    case 9:  return NV2A_VSH_OUT_T0;
    case 10: return NV2A_VSH_OUT_T1;
    case 11: return NV2A_VSH_OUT_T2;
    case 12: return NV2A_VSH_OUT_T3;
    default: return NV2A_VSH_OUT_NONE;
    }
}

/* Decode operands A, B, C into `raw[0..2]`, unconditionally -- every
 * field is read regardless of what either opcode will end up using.
 * Ported directly from nv2a_vsh_disassembler.c's parse_inputs()/
 * process_input(). Deliberately NOT yet filtered by opcode: that
 * filtering happens in the caller (mirroring nv2a_vsh_parse_step()'s own
 * two-phase structure) so it can be shared between MAC's up-to-3-operand
 * selection and ILU's fixed "always C" rule without duplicating the
 * per-operand decode itself. */
static void parse_raw_operands(const DWORD *insn, int input_index, int const_index,
                                NV2AVshSrcOperand raw[3])
{
    uint32_t rel = vsh_rel_addr(insn);  /* word 3 bit 1 -- shared a0.x flag */

    /* Operand A: type/temp-reg in word 2, swizzle/negate in word 1. */
    raw[0].reg_type = (NV2AVshRegType)vsh_a_type(insn);
    raw[0].negate    = (int)vsh_a_negate(insn);
    raw[0].swizzle.x = (uint8_t)vsh_a_swizzle_x(insn);
    raw[0].swizzle.y = (uint8_t)vsh_a_swizzle_y(insn);
    raw[0].swizzle.z = (uint8_t)vsh_a_swizzle_z(insn);
    raw[0].swizzle.w = (uint8_t)vsh_a_swizzle_w(insn);

    /* Operand B: type/temp-reg and swizzle/negate both in word 2. */
    raw[1].reg_type = (NV2AVshRegType)vsh_b_type(insn);
    raw[1].negate    = (int)vsh_b_negate(insn);
    raw[1].swizzle.x = (uint8_t)vsh_b_swizzle_x(insn);
    raw[1].swizzle.y = (uint8_t)vsh_b_swizzle_y(insn);
    raw[1].swizzle.z = (uint8_t)vsh_b_swizzle_z(insn);
    raw[1].swizzle.w = (uint8_t)vsh_b_swizzle_w(insn);

    /* Operand C: type in word 3, temp-reg split across words 2 and 3,
     * swizzle/negate in word 2. */
    raw[2].reg_type = (NV2AVshRegType)vsh_c_type(insn);
    raw[2].negate    = (int)vsh_c_negate(insn);
    raw[2].swizzle.x = (uint8_t)vsh_c_swizzle_x(insn);
    raw[2].swizzle.y = (uint8_t)vsh_c_swizzle_y(insn);
    raw[2].swizzle.z = (uint8_t)vsh_c_swizzle_z(insn);
    raw[2].swizzle.w = (uint8_t)vsh_c_swizzle_w(insn);

    {
        int s;
        uint32_t temp_regs[3] = { vsh_a_temp(insn), vsh_b_temp(insn), vsh_c_temp_reg(insn) };
        for (s = 0; s < 3; s++) {
            raw[s].rel_addr = 0;
            switch (raw[s].reg_type) {
            case NV2A_VSH_REG_TEMP:
                raw[s].reg_index = (int)temp_regs[s];
                break;
            case NV2A_VSH_REG_INPUT:
                raw[s].reg_index = input_index;  /* shared v# for the instruction */
                break;
            case NV2A_VSH_REG_CONST:
                raw[s].reg_index = const_index;  /* shared c#/context# */
                raw[s].rel_addr  = (int)rel;
                break;
            case NV2A_VSH_REG_NONE:
            default:
                raw[s].reg_index = 0;
                break;
            }
        }
    }
}

/* Select which of the 3 raw operands each unit's opcode actually reads,
 * mirroring nv2a_vsh_parse_step()'s two switches in
 * nv2a_vsh_disassembler.c exactly -- including the real-hardware quirk
 * that MAC_ADD reads A and C (raw[2]), not A and B. Leaving an unused
 * input slot at NV2A_VSH_REG_NONE (the memset in d3d8_vsh_parse() already
 * did this) is what keeps inputs_read from picking up phantom v#
 * registers whenever an operand slot an opcode doesn't use happens to
 * decode with type INPUT. */
static void select_mac_inputs(NV2AVshInstruction *inst, const NV2AVshSrcOperand raw[3])
{
    if (inst->mac.opcode == NV2A_VSH_MAC_NOP)
        return;

    inst->mac.inputs[0] = raw[0];
    switch (inst->mac.opcode) {
    case NV2A_VSH_MAC_MOV:
    case NV2A_VSH_MAC_ARL:
        break;  /* A only */
    case NV2A_VSH_MAC_MUL:
    case NV2A_VSH_MAC_DP3:
    case NV2A_VSH_MAC_DP4:
    case NV2A_VSH_MAC_DPH:
    case NV2A_VSH_MAC_DST:
    case NV2A_VSH_MAC_MIN:
    case NV2A_VSH_MAC_MAX:
    case NV2A_VSH_MAC_SGE:
    case NV2A_VSH_MAC_SLT:
        inst->mac.inputs[1] = raw[1];
        break;
    case NV2A_VSH_MAC_MAD:
        inst->mac.inputs[1] = raw[1];
        inst->mac.inputs[2] = raw[2];
        break;
    case NV2A_VSH_MAC_ADD:
        inst->mac.inputs[1] = raw[2];  /* A + C, not A + B -- see comment above */
        break;
    default:
        break;
    }
}

static void select_ilu_input(NV2AVshInstruction *inst, const NV2AVshSrcOperand raw[3])
{
    /* Real hardware only ever gives ILU operand C, regardless of opcode
     * (the RCP/RCC/RSQ/EXP/LOG scalar-replicate behavior is applied at
     * HLSL emission time by emit_scalar_swizzle(), not here). */
    if (inst->ilu.opcode != NV2A_VSH_ILU_NOP)
        inst->ilu.inputs[0] = raw[2];
}

/* Destination decode: a single shared temp-register write-target and a
 * single shared "real output or constant/context write-back" target,
 * distributed between MAC and ILU by the bits below -- NOT two
 * independent per-unit destination-mux fields. Ported directly from
 * nv2a_vsh_disassembler.c's parse_outputs(), including the paired-ILU-
 * forced-to-R1 hardware quirk. */
static void parse_destinations(NV2AVshInstruction *inst, const DWORD *insn)
{
    uint32_t out_temp_reg       = vsh_out_temp_reg(insn);        /* word3[20:23] */
    uint32_t temp_writemask_mac = vsh_mac_temp_writemask(insn);  /* word3[24:27] */
    uint32_t temp_writemask_ilu = vsh_ilu_temp_writemask(insn);  /* word3[16:19] */
    uint32_t out_writemask      = vsh_out_writemask(insn);       /* word3[12:15] */

    if (temp_writemask_mac) {
        inst->mac.outputs[0].dst_type   = NV2A_VSH_DST_TEMP;
        inst->mac.outputs[0].reg_index  = (int)out_temp_reg;
        inst->mac.outputs[0].write_mask = (uint8_t)temp_writemask_mac;
    }

    if (temp_writemask_ilu) {
        inst->ilu.outputs[0].dst_type = NV2A_VSH_DST_TEMP;
        inst->ilu.outputs[0].reg_index = (inst->mac.opcode != NV2A_VSH_MAC_NOP)
            ? 1                    /* paired with an active MAC op -> forced to R1 */
            : (int)out_temp_reg;   /* solo ILU temp write -> real decoded index */
        inst->ilu.outputs[0].write_mask = (uint8_t)temp_writemask_ilu;
    }

    if (out_writemask) {
        NV2AVshDstOperand *dst;
        int is_ilu = (int)vsh_out_is_ilu(insn);       /* word3[2] */
        int is_output = (int)vsh_out_is_output(insn); /* word3[11] */
        uint32_t out_index = vsh_out_index(insn);     /* word3[3:10] */

        dst = is_ilu ? &inst->ilu.outputs[0] : &inst->mac.outputs[0];
        if (dst->dst_type != NV2A_VSH_DST_NONE)
            dst++;  /* slot 0 already claimed by the temp write above */

        if (is_output) {
            dst->dst_type  = NV2A_VSH_DST_OUTPUT;
            dst->reg_index = (int)decode_output_mux(out_index);
        } else {
            /* Constant/context write-back -- decoded, not modeled. See
             * emit_one_dest()'s NV2A_VSH_DST_CONST case. */
            dst->dst_type  = NV2A_VSH_DST_CONST;
            dst->reg_index = (int)out_index;
        }
        dst->write_mask = (uint8_t)out_writemask;
    }

    if (inst->mac.opcode == NV2A_VSH_MAC_ARL) {
        /* ARL's destination is the address register, emitted specially by
         * emit_mac_op() without going through emit_one_dest() -- recorded
         * here mainly for consistency with real hardware (which treats
         * this as a conflict if outputs[0] was already claimed above; we
         * don't detect that rare conflict, we just let ARL win, since
         * nothing currently observed relies on it). */
        inst->mac.outputs[0].dst_type   = NV2A_VSH_DST_ADDRESS;
        inst->mac.outputs[0].reg_index  = 0;
        inst->mac.outputs[0].write_mask = 0;
    }
}

void d3d8_vsh_parse(const DWORD *microcode, int num_insns,
                     NV2AVshProgram *program)
{
    int i;
    memset(program, 0, sizeof(*program));

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    for (i = 0; i < num_insns; i++) {
        const DWORD *insn = &microcode[i * 4];
        NV2AVshInstruction *inst = &program->insns[i];
        NV2AVshSrcOperand raw[3];
        int const_index, input_index;

        memset(inst, 0, sizeof(*inst));

        /* Opcodes -- word 1, NOT word 0 (see the header comment: this was
         * the single bug that made every instruction decode as a NOP).
         * ILU is 3 bits, not 4. */
        inst->mac.opcode = (NV2AVshMacOp)vsh_mac_opcode(insn);
        inst->ilu.opcode = (NV2AVshIluOp)vsh_ilu_opcode(insn);

        /* Shared constant and input register indices (word 1). */
        const_index = (int)vsh_const_reg(insn);
        input_index = (int)vsh_input_reg(insn);
        if (const_index >= NV2A_VS_MAX_CONSTANTS)
            const_index = 0;
        if (input_index >= NV2A_VS_MAX_INPUTS)
            input_index = 0;

        parse_raw_operands(insn, input_index, const_index, raw);
        select_mac_inputs(inst, raw);
        select_ilu_input(inst, raw);
        parse_destinations(inst, insn);

        inst->is_final = (int)vsh_final(insn);

        /* Track v# registers actually read. Safe to trust every populated
         * slot here (unlike blindly decoding all 3 raw operands): select_
         * mac_inputs()/select_ilu_input() only ever populate the slots the
         * real opcode reads, leaving the rest at NV2A_VSH_REG_NONE. */
        {
            int s;
            for (s = 0; s < 3; s++) {
                if (inst->mac.inputs[s].reg_type == NV2A_VSH_REG_INPUT)
                    program->inputs_read |= (1u << inst->mac.inputs[s].reg_index);
                if (inst->mac.inputs[s].reg_type == NV2A_VSH_REG_CONST &&
                    (inst->mac.inputs[s].reg_index == NV2A_VS_VPSCL_REG ||
                     inst->mac.inputs[s].reg_index == NV2A_VS_VPOFF_REG))
                    program->uses_viewport_ctx = 1;
            }
            if (inst->ilu.inputs[0].reg_type == NV2A_VSH_REG_INPUT)
                program->inputs_read |= (1u << inst->ilu.inputs[0].reg_index);
            if (inst->ilu.inputs[0].reg_type == NV2A_VSH_REG_CONST &&
                (inst->ilu.inputs[0].reg_index == NV2A_VS_VPSCL_REG ||
                 inst->ilu.inputs[0].reg_index == NV2A_VS_VPOFF_REG))
                program->uses_viewport_ctx = 1;
        }

        program->length = i + 1;

        /* Stop at final instruction -- matches real execution (see
         * nv2a_vsh_emulator.c, which halts on step->is_final too). */
        if (inst->is_final)
            break;
    }
}

/* ================================================================
 * HLSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    if (size > 0) buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    if (sb->pos >= sb->size - 1) return;
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, remaining, fmt, ap);
    va_end(ap);
    if (n > 0 && n < remaining)
        sb->pos += n;
    else if (n >= remaining)
        sb->pos = sb->size - 1;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        if (src->reg_index == 12)
            sb_append(sb, "R12"); /* oPos alias */
        else
            sb_append(sb, "R%d", src->reg_index);
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr)
            sb_append(sb, "c[a0 + %d]", src->reg_index);
        else
            sb_append(sb, "c[%d]", src->reg_index);
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an HLSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit one destination write (a single outputs[] slot).
 *
 * Real hardware has no independent "temp dest" vs "output dest" fields --
 * outputs[0]/outputs[1] are just "up to two writes this op makes," each
 * independently typed (see NV2AVshDstRegType and parse_destinations() in
 * the parser above). Called once per populated slot, each with its own
 * write mask -- unlike the old single-shared-mask model, MAC/ILU can (and
 * do) write a temp register and a real output with different masks in
 * the same instruction.
 */
static void emit_one_dest(StrBuf *sb, const NV2AVshDstOperand *dst,
                           const char *rhs)
{
    if (dst->write_mask == 0)
        return;

    switch (dst->dst_type) {
    case NV2A_VSH_DST_TEMP:
        if (dst->reg_index == 12)
            sb_append(sb, "    R12");
        else
            sb_append(sb, "    R%d", dst->reg_index);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = (%s)", rhs);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
        return;

    case NV2A_VSH_DST_OUTPUT: {
        const char *name = output_reg_name((NV2AVshOutputReg)dst->reg_index);
        uint8_t mask = dst->write_mask;
        if ((NV2AVshOutputReg)dst->reg_index == NV2A_VSH_OUT_FOG) {
            /* A write to oFog lands its most significant masked component
             * in x (xemu vsh-prog.c fog_mask_str): a program that writes
             * fog as oFog.w still sets the fog distance. */
            static const uint8_t fog_mask[16] = {
                0x0, 0x8, 0x8, 0xC, 0x8, 0xC, 0xC, 0xE,
                0x8, 0xC, 0xC, 0xE, 0xC, 0xE, 0xE, 0xF,
            };
            mask = fog_mask[mask & 0xF];
        }
        if (name) {
            sb_append(sb, "    %s", name);
            emit_write_mask(sb, mask);
            sb_append(sb, " = (%s)", rhs);
            emit_write_mask(sb, mask);
            sb_append(sb, ";\n");
        }
        return;
    }

    case NV2A_VSH_DST_CONST:
        /* Real hardware can write a computed value back into a constant/
         * context register (used by context-switch microcode -- see
         * NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN). Not modeled: this
         * translator runs each vertex independently and stateless, and
         * the constant buffer is read-only from the shader's own
         * perspective -- there is nowhere a mid-shader write could go
         * that would affect anything else, so it is decoded (for
         * correctness of everything else in the instruction) but silently
         * dropped here rather than guessed at. Flag if a title is ever
         * seen actually depending on it. */
        return;

    case NV2A_VSH_DST_ADDRESS:
        /* ARL's a0 write is emitted directly by emit_mac_op()'s ARL case,
         * not through here -- see parse_destinations()'s comment. */
        return;

    case NV2A_VSH_DST_NONE:
    default:
        return;
    }
}

/* Emit both possible destination writes for one MAC or ILU operation. */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand outputs[2],
                              const char *rhs)
{
    emit_one_dest(sb, &outputs[0], rhs);
    emit_one_dest(sb, &outputs[1], rhs);
}

/**
 * Emit the HLSL for one MAC operation.
 */
static void emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->mac.opcode) {
    case NV2A_VSH_MAC_NOP:
        return;

    case NV2A_VSH_MAC_MOV:
        /* dst = A */
        emit_source(&expr, &inst->mac.inputs[0], 0);
        break;

    case NV2A_VSH_MAC_MUL:
        /* dst = A * B */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, " * ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ADD:
        /* dst = A + C (select_mac_inputs() already put C into inputs[1]
         * for ADD -- see that function's comment). */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAD:
        /* dst = A * B + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, " * ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac.inputs[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ".xyz, ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ".xyz).xxxx");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "dot(float4(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ".xyz, 1.0), ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w) */
        sb_append(&expr, "float4(1.0, ");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ".y * ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ".y, ");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ".z, ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ".w)");
        break;

    case NV2A_VSH_MAC_MIN:
        sb_append(&expr, "min(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAX:
        sb_append(&expr, "max(");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_SLT:
        /* dst = (A < B) ? 1.0 : 0.0
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a)
         * Equivalent to: step(a, b) where a < b yields 1
         * Using explicit form for clarity: */
        sb_append(&expr, "(1.0 - step(");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_SGE:
        /* dst = (A >= B) ? 1.0 : 0.0
         * step(edge, x) returns 1 if x >= edge, 0 otherwise */
        sb_append(&expr, "step(");
        emit_source(&expr, &inst->mac.inputs[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac.inputs[0], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ARL:
        /* a0 = floor(A.x + 0.001) - writes the address register, not a
         * float register. The bias is xemu's (vsh-prog.c _ARL): an index
         * stored as a byte arrives as 17/255, and scaled back up it is
         * 16.99..., which a bare floor turns into the neighbouring bone --
         * skinned vertices then tore holes in close-up faces. */
        sb_append(sb, "    a0 = (int)floor(");
        emit_source(sb, &inst->mac.inputs[0], 0);
        sb_append(sb, ".x + 0.001);\n");
        return; /* No destination register write */

    default:
        return;
    }

    emit_dest_assign(sb, inst->mac.outputs, expr_buf);
}

/**
 * Emit the HLSL for one ILU operation.
 */
static void emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->ilu.opcode) {
    case NV2A_VSH_ILU_NOP:
        return;

    case NV2A_VSH_ILU_MOV:
        /* dst = C */
        emit_source(&expr, &inst->ilu.inputs[0], 0);
        break;

    case NV2A_VSH_ILU_RCP:
        /* dst = (1.0 / C.x).xxxx */
        sb_append(&expr, "(1.0 / ");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RCC:
        /* dst = 1/C.x clamped AWAY from zero into [2^-64, 2^64], keeping its
         * sign (xemu clampAwayZeroInf). A positive-only clamp collapsed every
         * vertex behind the camera (w < 0) onto the viewport offset. */
        sb_append(&expr, "float((1.0 / ");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, ") >= 0.0 ? clamp(1.0 / ");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, ", 5.421011e-20, 1.8446744e19) : clamp(1.0 / ");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, ", -1.8446744e19, -5.421011e-20)).xxxx");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = (1.0 / sqrt(abs(C.x))).xxxx */
        sb_append(&expr, "rsqrt(abs(");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, ")).xxxx");
        break;

    case NV2A_VSH_ILU_EXP:
        /* dst = exp2(C.x).xxxx */
        sb_append(&expr, "exp2(");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_LOG: {
        /* dst = log2(abs(C.x)).xxxx
         * Guard against log2(0) which is -inf on NV2A -> clamp to large negative */
        sb_append(&expr, "log2(max(abs(");
        emit_source(&expr, &inst->ilu.inputs[0], 1);
        sb_append(&expr, "), 1.175494e-38)).xxxx");
        break;
    }

    case NV2A_VSH_ILU_LIT: {
        /* NV2A LIT instruction:
         *   dst.x = 1.0
         *   dst.y = max(src.x, 0.0)
         *   dst.z = (src.x > 0) ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0
         *   dst.w = 1.0
         *
         * We emit a helper call. The lit() HLSL intrinsic has similar but
         * not identical semantics, so we use an inline expansion. */
        sb_append(&expr, "float4(1.0, max(");
        emit_source(&expr, &inst->ilu.inputs[0], 0);
        sb_append(&expr, ".x, 0.0), (");
        emit_source(&expr, &inst->ilu.inputs[0], 0);
        sb_append(&expr, ".x > 0.0) ? exp2(clamp(");
        emit_source(&expr, &inst->ilu.inputs[0], 0);
        sb_append(&expr, ".w, -128.0, 128.0) * log2(max(");
        emit_source(&expr, &inst->ilu.inputs[0], 0);
        sb_append(&expr, ".y, 0.0) + 1e-30)) : 0.0, 1.0)");
        break;
    }

    default:
        return;
    }

    emit_dest_assign(sb, inst->ilu.outputs, expr_buf);
}

/**
 * Map NV2A input register index to a D3D11 input semantic.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position
 *   v1  = Blend weight
 *   v2  = Normal
 *   v3  = Diffuse color
 *   v4  = Specular color
 *   v5  = Fog coordinate
 *   v6  = Point size / back diffuse
 *   v7  = Back specular
 *   v8  = Texture coord 0
 *   v9  = Texture coord 1
 *   v10 = Texture coord 2
 *   v11 = Texture coord 3
 *   v12-v15 = Additional attributes
 *
 * We use generic ATTR semantics so the input layout can match any
 * vertex buffer format at bind time.
 */
static const char *input_semantic_name(int reg_index)
{
    /* All inputs use the generic TEXCOORD semantic with unique indices
     * to avoid mismatches. The input layout will map them correctly. */
    (void)reg_index;
    return "ATTR";
}

int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    uint16_t inputs = program->inputs_read;

    sb_init(&sb, buf, bufsize);

    /* Constant buffer: 192 float4 constants */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "cbuffer VSH_Constants : register(b1) {\n"
        "    float4 c[%d];\n"
        "    float4 nv2aSurface;\n"
        "    float4 nv2aFog;\n"
        "};\n"
        "\n", NV2A_VS_MAX_CONSTANTS);

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d : ATTR%d;\n", i, i);
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : FOG;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD4;\n"
        "    float4 oB1  : TEXCOORD5;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i <= 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Alias input registers for readability */
    sb_append(&sb, "    /* Input register aliases */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d = input.v%d;\n", i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables */
    sb_append(&sb,
        /* (0,0,0,1) for every output, as xemu's vsh.c. A texture
         * coordinate the program never writes a w for must still divide by
         * 1: the pixel stage samples projectively (xy / w). */
        "    /* Output registers */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(0,0,0,1);\n"
        "    float4 oPts = float4(0,0,0,1);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];

        sb_append(&sb, "\n    /* Instruction %d */\n", i);

        /* MAC operation */
        if (inst->mac.opcode != NV2A_VSH_MAC_NOP)
            emit_mac_op(&sb, inst);

        /* ILU operation (executes in parallel with MAC on hardware;
         * in HLSL they are sequential but semantically equivalent
         * because ILU reads source C, not MAC destinations) */
        if (inst->ilu.opcode != NV2A_VSH_ILU_NOP)
            emit_ilu_op(&sb, inst);
    }

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Undo NV2A's device-space bake-in (see NV2A_VS_VPSCL_REG/_VPOFF_REG's
     * comment in d3d8_vsh.h): a program following the standard compiled-
     * vertex-shader epilogue convention leaves oPos in pixel/device space,
     * not the pre-divide clip space D3D11's rasterizer requires from
     * SV_POSITION. Reversing it with the exact same c[VPSCL]/c[VPOFF] the
     * program itself used, rather than hardcoding a viewport size, keeps
     * this correct regardless of what resolution/viewport the title
     * programmed. Gated on uses_viewport_ctx (set during parsing) so a
     * program that never actually reads those two registers -- and so
     * must be producing clip-space output some other way -- doesn't get
     * a transform applied that was never part of its own instructions.
     * Guarded against a zero VPSCL (no SET_VIEWPORT_SCALE seen yet) so an
     * early draw divides by nothing rather than by zero. */
    if (program->uses_viewport_ctx) {
        sb_append(&sb,
            "    /* NV2A pixel space -> D3D11 clip space. With the render target's\n"
            "     * size known this is xemu's mapping, (2*xy - size) / size with y\n"
            "     * pointing down; a viewport smaller than the target (a bloom pass\n"
            "     * drawing 128x64 into a larger surface) then lands where the title\n"
            "     * put it. Without it, undo c[VPSCL]/c[VPOFF] as before. */\n"
            "    if (nv2aSurface.x > 0.0 && nv2aSurface.y > 0.0) {\n"
            "        oPos.xy = float2((2.0 * oPos.x - nv2aSurface.x) / nv2aSurface.x,\n"
            "                         (nv2aSurface.y - 2.0 * oPos.y) / nv2aSurface.y);\n"
            "    } else if (c[%d].x != 0.0 && c[%d].y != 0.0) {\n"
            "        oPos.xy = (oPos.xy - c[%d].xy) / c[%d].xy;\n"
            "    }\n"
            "    if (nv2aSurface.z > 0.0) {\n"
            "        oPos.z = oPos.z / nv2aSurface.z;\n"
            "    } else if (c[%d].z != 0.0) {\n"
            "        oPos.z = oPos.z / c[%d].z;\n"
            "    }\n"
            /* Undo the program's divide by w and keep w, as xemu's vsh-prog.c
             * does, so the rasterizer clips homogeneously. Forcing w = 1 left
             * triangles that cross the camera plane with no way to clip. A 2D
             * program's w is already 1, so its output is unchanged. */
            "    oPos.w = (oPos.w >= 0.0) ? clamp(oPos.w, 5.421011e-20, 1.8446744e19)\n"
            "                             : clamp(oPos.w, -1.8446744e19, -5.421011e-20);\n"
            "    oPos.xyz *= oPos.w;\n\n",
            NV2A_VS_VPSCL_REG, NV2A_VS_VPSCL_REG, NV2A_VS_VPOFF_REG, NV2A_VS_VPSCL_REG,
            NV2A_VS_VPSCL_REG, NV2A_VS_VPSCL_REG);
    }

    /* Fog factor, as xemu's vsh.c computes it after the program. Without
     * this, a program that never writes oFog left the fog register at 0 --
     * full fog -- and any combiner that mixes by fog drew the fog colour. */
    sb_append(&sb,
        "    if (nv2aFog.x == 0.0) {\n"
        "        oFog = float4(1.0, 1.0, 1.0, 1.0);\n"
        "    } else {\n"
        "        float fogDistance = oFog.x;\n"
        "        int fogMode = (int)nv2aFog.y;\n"
        "        float fogFactor;\n"
        "        if (fogMode == 0 || fogMode == 4) {\n"
        "            fogFactor = nv2aFog.z + fogDistance * nv2aFog.w - 1.0;\n"
        "        } else if (fogMode == 1 || fogMode == 5) {\n"
        "            fogFactor = nv2aFog.z + pow(2.0, fogDistance * nv2aFog.w * 16.0) - 1.5;\n"
        "        } else {\n"
        "            fogFactor = nv2aFog.z + pow(2.0, -fogDistance * fogDistance * nv2aFog.w * nv2aFog.w * 32.0) - 1.5;\n"
        "        }\n"
        "        if (fogMode >= 4) fogFactor = abs(fogFactor);\n"
        /* XBOXRECOMP_DBG_FOGF=<f>: force the fog factor (debug) */
        "        if (nv2aSurface.w > 0.0) fogFactor = nv2aSurface.w;\n"
        "        oFog = (float4)fogFactor;\n"
        "    }\n\n");

    /* Populate output structure */
    sb_append(&sb,
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    o.oPos = oPos;\n"
        "    o.oD0  = saturate(oD0);\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(oD1);\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n"
        "    o.oFog = oFog.x;\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(oB0);\n"
        "    o.oB1  = saturate(oB1);\n"
        "    return o;\n"
        "}\n");

    return sb.pos;
}

/* ================================================================
 * Input Layout Management
 *
 * When using a programmable VS, we need an input layout that matches
 * the shader's declared inputs. The layout maps vertex buffer elements
 * to the ATTR# semantics declared in the generated HLSL.
 *
 * The mapping from NV2A v# registers to vertex data depends on the
 * game's vertex stream setup. We use a simple mapping, matching the real
 * NV2A vertex attribute slot numbering exactly (NV2A_VERTEX_ATTR_* in
 * nv2a_regs.h -- this file has no dependency on that header, since it
 * predates and is decoupled from the pgraph method-address layer, but the
 * numeric slot values below must still agree with it):
 *
 *   v0  -> ATTR0  -> POSITION (float3)
 *   v1  -> ATTR1  -> BLENDWEIGHT (float4)
 *   v2  -> ATTR2  -> NORMAL (float3)
 *   v3  -> ATTR3  -> DIFFUSE (D3DCOLOR)
 *   v4  -> ATTR4  -> SPECULAR (D3DCOLOR)
 *   v5  -> ATTR5  -> FOG (float)
 *   v6  -> ATTR6  -> POINT SIZE (float)
 *   v7  -> ATTR7  -> BACK DIFFUSE (D3DCOLOR)
 *   v8  -> ATTR8  -> BACK SPECULAR (D3DCOLOR)
 *   v9  -> ATTR9  -> TEXCOORD0 (float2)
 *   v10 -> ATTR10 -> TEXCOORD1 (float2)
 *   v11 -> ATTR11 -> TEXCOORD2 (float2)
 *   v12 -> ATTR12 -> TEXCOORD3 (float2)
 *   v13-v15 -> ATTR13-15 -> reserved/additional
 *
 * This table previously started TEXCOORD0 at v8 and had no slot at all
 * for BACK DIFFUSE, silently shifting every slot from 7 up by one
 * relative to real hardware -- harmless for texcoords specifically (v8-
 * v11 there all shared the same float2 format v9-v12 use here, so no
 * title that only reads texcoords through this table was ever affected),
 * but wrong for slot 8 (BACK SPECULAR, a packed color) which read as a
 * texcoord-shaped float2 instead of D3DCOLOR, and wrong for slot 12
 * (real TEXCOORD3) which fell through to the 16-byte generic default
 * instead of float2. Not otherwise exercised until this rewrite, since
 * nothing fed this translator real microcode before now (see the parser
 * above) -- caught alongside that bug, not a regression of it.
 * ================================================================ */

/**
 * Default format for each input register.
 * This is the common Xbox convention; games may vary.
 */
DXGI_FORMAT d3d8_vsh_default_input_format(int vreg)
{
    switch (vreg) {
    case 0:  return DXGI_FORMAT_R32G32B32_FLOAT;    /* Position (xyz) */
    case 1:  return DXGI_FORMAT_R32G32B32A32_FLOAT;  /* Blend weights */
    case 2:  return DXGI_FORMAT_R32G32B32_FLOAT;     /* Normal */
    case 3:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Diffuse (D3DCOLOR) */
    case 4:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Specular (D3DCOLOR) */
    case 5:  return DXGI_FORMAT_R32_FLOAT;            /* Fog */
    case 6:  return DXGI_FORMAT_R32_FLOAT;            /* Point size */
    case 7:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Back diffuse */
    case 8:  return DXGI_FORMAT_R8G8B8A8_UNORM;      /* Back specular */
    case 9:  return DXGI_FORMAT_R32G32_FLOAT;         /* Texcoord 0 */
    case 10: return DXGI_FORMAT_R32G32_FLOAT;         /* Texcoord 1 */
    case 11: return DXGI_FORMAT_R32G32_FLOAT;         /* Texcoord 2 */
    case 12: return DXGI_FORMAT_R32G32_FLOAT;         /* Texcoord 3 */
    default: return DXGI_FORMAT_R32G32B32A32_FLOAT;   /* Generic (13-15) */
    }
}

UINT d3d8_vsh_input_format_size(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R32_FLOAT:            return 4;
    case DXGI_FORMAT_R32G32_FLOAT:         return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:      return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   return 16;
    case DXGI_FORMAT_R8G8B8A8_UNORM:       return 4;
    default:                                return 16;
    }
}

static ID3D11InputLayout *create_vsh_input_layout(
    uint16_t inputs_read, ID3DBlob *vs_blob)
{
    D3D11_INPUT_ELEMENT_DESC elems[NV2A_VS_MAX_INPUTS];
    UINT elem_count = 0;
    UINT offset = 0;
    ID3D11InputLayout *layout = NULL;
    HRESULT hr;
    int i;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(inputs_read & (1u << i)))
            continue;

        DXGI_FORMAT fmt = d3d8_vsh_default_input_format(i);

        elems[elem_count].SemanticName      = "ATTR";
        elems[elem_count].SemanticIndex      = (UINT)i;
        elems[elem_count].Format             = fmt;
        elems[elem_count].InputSlot          = 0;
        elems[elem_count].AlignedByteOffset  = offset;
        elems[elem_count].InputSlotClass     = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;

        offset += d3d8_vsh_input_format_size(fmt);
    }

    if (elem_count == 0) return NULL;

    hr = ID3D11Device_CreateInputLayout(
        d3d8_GetD3D11Device(),
        elems, elem_count,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &layout);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateInputLayout failed: 0x%08lX\n", hr);
        return NULL;
    }

    return layout;
}

/* ================================================================
 * Shader Compilation and Caching
 * ================================================================ */

static VshCacheEntry *cache_lookup(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use)
            return NULL;
        if (g_vsh_cache[probe].hash == hash)
            return &g_vsh_cache[probe];
    }
    return NULL;
}

static VshCacheEntry *cache_insert(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;

    /* Find an empty slot or reuse the probed slot */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use) {
            g_vsh_cache[probe].hash   = hash;
            g_vsh_cache[probe].in_use = 1;
            return &g_vsh_cache[probe];
        }
    }

    /* Cache full: evict the first probed entry */
    {
        VshCacheEntry *evict = &g_vsh_cache[idx];
        int j;

        if (evict->vs)
            ID3D11VertexShader_Release(evict->vs);
        if (evict->vs_blob)
            ID3D10Blob_Release(evict->vs_blob);
        for (j = 0; j < evict->layout_count; j++) {
            if (evict->layouts[j])
                ID3D11InputLayout_Release(evict->layouts[j]);
        }
        memset(evict, 0, sizeof(*evict));
        evict->hash   = hash;
        evict->in_use = 1;
        return evict;
    }
}

/**
 * Compile a vertex shader from microcode.
 *
 * Parses, generates HLSL, compiles, and caches the result.
 * Returns the cache entry (with compiled VS and blob).
 */
static VshCacheEntry *compile_shader(const DWORD *microcode, int num_insns,
                                      uint32_t hash)
{
    NV2AVshProgram program;
    char hlsl_buf[16384];  /* 16KB should be enough for any VS */
    int hlsl_len;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;
    VshCacheEntry *entry;

    /* Parse microcode */
    d3d8_vsh_parse(microcode, num_insns, &program);

    /* Generate HLSL */
    hlsl_len = d3d8_vsh_generate_hlsl(&program, hlsl_buf, sizeof(hlsl_buf));
    if (hlsl_len <= 0) {
        fprintf(stderr, "D3D8 VSH: HLSL generation failed\n");
        return NULL;
    }

    /* Compile HLSL to bytecode */
    hr = D3DCompile(hlsl_buf, (SIZE_T)hlsl_len, "nv2a_vsh",
                    NULL, NULL, "main", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End ---\n", hlsl_buf);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    /* Insert into cache */
    entry = cache_insert(hash);
    if (!entry) {
        ID3D10Blob_Release(code);
        return NULL;
    }

    /* Create D3D11 vertex shader */
    hr = ID3D11Device_CreateVertexShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &entry->vs);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: CreateVertexShader failed: 0x%08lX\n", hr);
        ID3D10Blob_Release(code);
        entry->in_use = 0;
        return NULL;
    }

    entry->vs_blob     = code;
    entry->inputs_read = program.inputs_read;
    entry->layout_count = 0;

    fprintf(stderr, "D3D8 VSH: Compiled shader (hash 0x%08X, %d insns, inputs 0x%04X)\n",
            hash, program.length, program.inputs_read);

    /* Opt-in instrument, same pattern as XBOXRECOMP_FRAMEDUMP: dump the
     * translated HLSL for every newly-compiled (cache-miss) shader when
     * asked, so a wrong translation (bad transform, wrong output register,
     * etc.) can be inspected directly instead of guessed at from pixel
     * output alone. Off by default -- this fires once per distinct
     * microcode, not per draw, so it is cheap to leave the check in. */
    if (getenv("XBOXRECOMP_VSHDUMP")) {
        fprintf(stderr, "--- D3D8 VSH HLSL (hash 0x%08X) ---\n%s\n--- End ---\n",
                hash, hlsl_buf);
    }

    return entry;
}

/**
 * Get the input layout for a cache entry.
 * Creates and caches the layout on first request per input mask.
 */
static ID3D11InputLayout *get_cached_layout(VshCacheEntry *entry)
{
    uint16_t mask = entry->inputs_read;
    int i;

    /* Check if we already created a layout for this mask */
    for (i = 0; i < entry->layout_count; i++) {
        if (entry->layout_masks[i] == mask)
            return entry->layouts[i];
    }

    /* Create new layout */
    if (entry->layout_count >= 16)
        return entry->layouts[0]; /* Fallback to first */

    ID3D11InputLayout *layout = create_vsh_input_layout(mask, entry->vs_blob);
    entry->layouts[entry->layout_count]      = layout;
    entry->layout_masks[entry->layout_count] = mask;
    entry->layout_count++;

    return layout;
}

/* ================================================================
 * Public API Implementation
 * ================================================================ */

HRESULT d3d8_vsh_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    g_vsh_slot_count = 0;
    g_vsh_constants_dirty = TRUE;

    /* Create the constant buffer for VS constants (192 * float4 = 3072 bytes) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth      = sizeof(NV2AVSConstants);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vsh_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8 VSH: Failed to create constant buffer: 0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "D3D8 VSH: Vertex shader translator initialized\n");
    return S_OK;
}

void d3d8_vsh_shutdown(void)
{
    int i, j;

    /* Release all cached shaders and layouts */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        VshCacheEntry *e = &g_vsh_cache[i];
        if (!e->in_use) continue;
        if (e->vs)      ID3D11VertexShader_Release(e->vs);
        if (e->vs_blob) ID3D10Blob_Release(e->vs_blob);
        for (j = 0; j < e->layout_count; j++) {
            if (e->layouts[j])
                ID3D11InputLayout_Release(e->layouts[j]);
        }
    }
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));

    if (g_vsh_cb) {
        ID3D11Buffer_Release(g_vsh_cb);
        g_vsh_cb = NULL;
    }

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    g_vsh_slot_count = 0;
}

HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle)
{
    int slot;

    if (!microcode || num_insns <= 0 || !out_handle)
        return E_INVALIDARG;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    /* Find a free slot */
    slot = -1;
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        if (!g_vsh_slots[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "D3D8 VSH: No free shader slots\n");
        return E_OUTOFMEMORY;
    }

    /* Store microcode (deferred compilation) */
    memcpy(g_vsh_slots[slot].microcode, microcode,
           (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].length = num_insns;
    g_vsh_slots[slot].in_use = 1;
    g_vsh_slot_count++;

    /* Generate handle: slot index + 0x10000 to distinguish from FVF codes.
     * Xbox D3D8 uses handles with the high bit set (> 0xFFFF). */
    *out_handle = (DWORD)(slot + 0x10000);

    fprintf(stderr, "D3D8 VSH: Created shader handle 0x%lX (%d instructions)\n",
            *out_handle, num_insns);

    return S_OK;
}

HRESULT d3d8_vsh_delete_shader(DWORD handle)
{
    int slot;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;

    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return E_INVALIDARG;

    if (g_vsh_slots[slot].in_use) {
        g_vsh_slots[slot].in_use = 0;
        g_vsh_slot_count--;
    }

    return S_OK;
}

void d3d8_vsh_set_surface(float width, float height, float zmax)
{
    if (g_vsh_constants.surface[0] == width &&
        g_vsh_constants.surface[1] == height &&
        g_vsh_constants.surface[2] == zmax)
        return;
    g_vsh_constants.surface[0] = width;
    g_vsh_constants.surface[1] = height;
    g_vsh_constants.surface[2] = zmax;
    {
        static float fogf = -1.0f;
        if (fogf < 0.0f) {
            const char *e = getenv("XBOXRECOMP_DBG_FOGF");
            fogf = e ? (float)atof(e) : 0.0f;
        }
        g_vsh_constants.surface[3] = fogf;
    }
    g_vsh_constants_dirty = TRUE;
}

const float *d3d8_vsh_get_constant(int reg)
{
    return (reg >= 0 && reg < NV2A_VS_MAX_CONSTANTS) ? g_vsh_constants.c[reg] : NULL;
}

void d3d8_vsh_set_fog(int enable, int mode, float param0, float param1)
{
    float f[4] = { enable ? 1.0f : 0.0f, (float)mode, param0, param1 };
    if (memcmp(g_vsh_constants.fog, f, sizeof(f)) == 0)
        return;
    memcpy(g_vsh_constants.fog, f, sizeof(f));
    g_vsh_constants_dirty = TRUE;
}

void d3d8_vsh_set_constant(int start_reg, const float *data, int count)
{
    int end_reg;

    if (!data || start_reg < 0)
        return;

    end_reg = start_reg + count;
    if (end_reg > NV2A_VS_MAX_CONSTANTS)
        end_reg = NV2A_VS_MAX_CONSTANTS;

    for (int i = start_reg; i < end_reg; i++) {
        int src_offset = (i - start_reg) * 4;
        g_vsh_constants.c[i][0] = data[src_offset + 0];
        g_vsh_constants.c[i][1] = data[src_offset + 1];
        g_vsh_constants.c[i][2] = data[src_offset + 2];
        g_vsh_constants.c[i][3] = data[src_offset + 3];
    }

    g_vsh_constants_dirty = TRUE;
}

BOOL d3d8_vsh_is_programmable(DWORD handle)
{
    return (handle >= 0x10000) ? TRUE : FALSE;
}

BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    ID3D11DeviceContext *ctx;
    int slot;
    NV2AVshSlot *vsh;
    uint32_t hash;
    VshCacheEntry *entry;
    ID3D11InputLayout *layout;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!d3d8_vsh_is_programmable(handle))
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx) return FALSE;

    /* Resolve handle to shader slot */
    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return FALSE;

    vsh = &g_vsh_slots[slot];
    if (!vsh->in_use)
        return FALSE;

    /* Hash the microcode to look up in cache */
    hash = fnv1a_hash(vsh->microcode, (size_t)vsh->length * 4 * sizeof(DWORD));

    /* Look up in cache */
    entry = cache_lookup(hash);
    if (!entry) {
        /* Cache miss: parse, generate HLSL, compile */
        entry = compile_shader(vsh->microcode, vsh->length, hash);
        if (!entry)
            return FALSE;
    }

    /* Bind the vertex shader */
    ID3D11DeviceContext_VSSetShader(ctx, entry->vs, NULL, 0);

    /* Bind the input layout */
    layout = get_cached_layout(entry);
    if (layout)
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);

    /* Update constant buffer if dirty */
    if (g_vsh_constants_dirty) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vsh_cb,
                                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &g_vsh_constants, sizeof(g_vsh_constants));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vsh_cb, 0);
        }
        g_vsh_constants_dirty = FALSE;
    }

    /* Bind constant buffer to slot b1 */
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &g_vsh_cb);

    return TRUE;
}
