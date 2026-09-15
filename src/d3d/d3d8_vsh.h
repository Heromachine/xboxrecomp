/**
 * NV2A Vertex Shader Microcode to HLSL Translator
 *
 * The Xbox NV2A GPU has a programmable vertex shader unit compatible with
 * (and extending) the original GeForce3/4 vertex shader architecture.
 * Games upload sequences of 128-bit microcode instructions via
 * D3DDevice_CreateVertexShader(). At draw time, the NV2A executes
 * these instructions in its vertex shader pipeline.
 *
 * This module translates NV2A vertex shader microcode into HLSL source
 * code, compiles it with D3DCompile, and caches the resulting
 * ID3D11VertexShader for use by the D3D8->D3D11 compatibility layer.
 *
 * NV2A Vertex Shader Architecture:
 *
 *   Registers:
 *     v0  - v15   Input vertex attribute registers (read-only)
 *     R0  - R11   Temporary registers (read/write)
 *     R12         Aliased to oPos (output position)
 *     c0  - c191  Constant registers (set by SetVertexShaderConstant)
 *     a0          Address register (integer, for indexed c[] access)
 *     oPos        Output position (= R12)
 *     oD0, oD1    Output diffuse / specular color
 *     oFog        Output fog factor
 *     oPts        Output point size
 *     oB0, oB1    Output back-face diffuse / specular
 *     oT0 - oT3   Output texture coordinates
 *
 *   Execution Units (per instruction slot, execute in parallel):
 *     MAC (Multiply-Accumulate):
 *       NOP, MOV, MUL, ADD, MAD, DP3, DP4, DPH, DST, MIN, MAX,
 *       SLT, SGE, ARL
 *     ILU (Inverse Logic Unit):
 *       NOP, MOV, RCP, RCC, RSQ, EXP, LOG, LIT
 *
 *   Programs are up to 136 instruction slots.
 *   Each slot is 128 bits (4 DWORDs) encoding both MAC and ILU ops.
 *
 * References:
 *   - envytools NV20 vertex shader documentation
 *   - xemu NV2A vertex shader implementation
 *   - Xbox SDK D3D vertex shader programming guide
 *   - US Patent 7,002,588 (Microsoft/Nvidia vertex shader architecture)
 */

#ifndef XBOXRECOMP_D3D8_VSH_H
#define XBOXRECOMP_D3D8_VSH_H

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NV2A Vertex Shader Constants
 * ================================================================ */

/** Maximum program length in 128-bit instruction slots. */
#define NV2A_VS_MAX_INSTRUCTIONS    136

/** Number of constant registers (c0 - c191). */
#define NV2A_VS_MAX_CONSTANTS       192

/** Number of input attribute registers (v0 - v15). */
#define NV2A_VS_MAX_INPUTS          16

/** Number of temporary registers (R0 - R11, plus R12 = oPos). */
#define NV2A_VS_MAX_TEMPS           13

/** Maximum number of shader programs that can be stored. */
#define NV2A_VS_MAX_SLOTS           128

/** Shader cache size (hashed microcode -> compiled shader). */
#define NV2A_VS_CACHE_SIZE          64

/**
 * Fixed hardware constant-file slots for the viewport scale/offset, NOT
 * chosen by this translator -- these are where real NV2A hardware itself
 * writes SET_VIEWPORT_SCALE/_OFFSET (see NV_IGRAPH_XF_XFCTX_VPSCL/_VPOFF
 * in nv2a_regs.h, and xemu's pgraph.c SET_VIEWPORT_SCALE/_OFFSET method
 * handlers, which write these exact indices directly into the same
 * constant array a vertex program reads via c[], bypassing the generic
 * SET_TRANSFORM_CONSTANT path entirely). Compiled Xbox vertex programs
 * read them as a matter of course: the Xbox SDK/nxdk vertex shader
 * compiler appends a standard "multiply by VPSCL, optionally divide by w,
 * add VPOFF" epilogue to every compiled program, because real NV2A
 * hardware has no separate post-vertex-shader viewport-transform stage --
 * a program's oPos output IS already device/pixel space by the time it
 * leaves the vertex unit. D3D11 has no such flexibility (SV_POSITION is
 * always pre-divide clip space, mapped to the viewport by fixed-function
 * hardware afterwards), so d3d8_vsh_generate_hlsl() undoes this bake-in
 * generically at the end of every generated shader using these same two
 * registers, rather than guessing at it per-shader or leaving oPos in the
 * wrong coordinate space. The caller must keep these mirrored into the
 * constant array whenever SET_VIEWPORT_SCALE/_OFFSET fire -- see
 * nv2a_pgraph_d3d11.c's handlers for those methods. */
#define NV2A_VS_VPSCL_REG           0x3a  /* NV_IGRAPH_XF_XFCTX_VPSCL, c[58] */
#define NV2A_VS_VPOFF_REG           0x3b  /* NV_IGRAPH_XF_XFCTX_VPOFF, c[59] */

/* ================================================================
 * NV2A VS Instruction Encoding (128 bits = 4 DWORDs)
 *
 * Ported field-for-field from xemu's own reference decoder,
 * subprojects/nv2a_vsh_cpu/src/nv2a_vsh_disassembler.c (its `parse_*`
 * functions and `EXTRACT(token, word, start, size)` macro), cross-checked
 * against hw/xbox/nv2a/pgraph/glsl/vsh-prog.c's `out_reg_name[]` table.
 * Do not restate these offsets from memory or re-derive them -- the
 * previous version of this comment (and the VSH_FIELD_* table it
 * described) was invented locally, claimed to follow "xemu/envytools
 * conventions" without actually having been checked against either, and
 * was wrong in every field that mattered: it treated the instruction as a
 * single flat 128-bit value with word[0] holding the opcodes, when real
 * hardware never reads word[0] AT ALL -- every field lives in word[1],
 * [2], or [3]. That bug made every MAC/ILU opcode decode as 0 (NOP) for
 * this title's actual microcode, so oPos was never written by any
 * instruction and stayed at its default (0,0,0,1) for every vertex --
 * wiring the translator in (see nv2a_pgraph_d3d11.c) correctly produced
 * an all-black frame, because the decoder underneath it had never been
 * exercised by real microcode until then.
 *
 * `EXTRACT(token, index, start, size)` below always means: word `index`
 * of the 4-DWORD instruction, bits [start, start+size).
 *
 * Word 0: entirely unused. Real NV2A hardware does not read it for
 *         anything this decoder needs.
 *
 * Word 1:
 *   [0:1]   - Source A swizzle W
 *   [2:3]   - Source A swizzle Z
 *   [4:5]   - Source A swizzle Y
 *   [6:7]   - Source A swizzle X
 *   [8]     - Source A negate
 *   [9:12]  - Input register (v#) -- SHARED: whichever operand(s) decode
 *             as type INPUT this instruction all read this one v#.
 *   [13:20] - Constant/context register index -- SHARED the same way for
 *             type CONST/CONTEXT operands.
 *   [21:24] - MAC opcode (4 bits)
 *   [25:27] - ILU opcode (3 bits, NOT 4 -- the old table's most literal
 *             single-bit bug: NV2AVshIluOp only ever has 8 values, but
 *             the old code read a 4th bit that belongs to something else)
 *
 * Word 2:
 *   [0:1]   - Source C temp register index, HIGH 2 bits (see word 3 [30:31]
 *             for the low 2 bits -- this field is genuinely split across
 *             two words, not a typo)
 *   [2:3]   - Source C swizzle W
 *   [4:5]   - Source C swizzle Z
 *   [6:7]   - Source C swizzle Y
 *   [8:9]   - Source C swizzle X
 *   [10]    - Source C negate
 *   [11:12] - Source B register type (0=none,1=temp,2=input,3=const)
 *   [13:16] - Source B temp register index
 *   [17:18] - Source B swizzle W
 *   [19:20] - Source B swizzle Z
 *   [21:22] - Source B swizzle Y
 *   [23:24] - Source B swizzle X
 *   [25]    - Source B negate
 *   [26:27] - Source A register type
 *   [28:31] - Source A temp register index
 *
 *   Note operand A's type/temp-index (here, word 2) and its swizzle/
 *   negate (word 1, above) are NOT adjacent -- each logical operand's
 *   fields are scattered across whichever words had room, not laid out
 *   as one contiguous block per operand. This is the second way the old
 *   flat-128-bit model was structurally wrong, independent of any single
 *   offset being off: no reshuffling of start positions within that model
 *   could have produced a correct decode.
 *
 * Word 3:
 *   [0]     - Final instruction flag (1 = last instruction in program)
 *   [1]     - Relative-addressing flag (a0.x offsets the CONST/CONTEXT
 *             register above) -- applies to whichever operand(s) are
 *             CONST type, same shared-field pattern as the v#/c# indices.
 *   [2]     - 1 = the single shared "real output/context write" below
 *             belongs to the ILU op; 0 = it belongs to MAC.
 *   [3:10]  - That shared output/context register index (8 bits, but
 *             only the low nibble is ever meaningful -- see
 *             decode_output_mux()/out_reg_name[] in vsh-prog.c: index 0 =
 *             oPos, 3=oD0, 4=oD1, 5=oFog, 6=oPts, 7=oB0, 8=oB1, 9-12=oT0-3,
 *             others reserved/unused).
 *   [11]    - 1 = that write targets a real output register; 0 = it
 *             writes back into a constant/context register instead (a
 *             genuine but rare hardware feature for context-switch
 *             microcode; not modeled here -- see emit_one_dest()).
 *   [12:15] - Write mask for the shared output/context write above.
 *   [16:19] - Write mask for a temp-register write from the ILU op.
 *   [20:23] - Temp register index -- SHARED between MAC and ILU temp
 *             writes below (same physical destination register slot).
 *   [24:27] - Write mask for a temp-register write from the MAC op.
 *   [28:29] - Source C register type.
 *   [30:31] - Source C temp register index, LOW 2 bits (see word 2 [0:1]).
 *
 * MAC and ILU can each independently write the shared temp register
 * (with their own separate write masks), and/or one of them can claim the
 * single shared "real output" write per instruction (bit [2] above says
 * which) -- there are no independent per-unit destination-mux fields the
 * way the old model assumed. One more real-hardware quirk carried over
 * from xemu's decoder: if BOTH units write the temp register in the same
 * instruction, the ILU write is hard-wired to R1 regardless of the
 * decoded temp index -- see parse_outputs() in d3d8_vsh.c.
 * ================================================================ */

/* ================================================================
 * Opcode Enumerations
 * ================================================================ */

/**
 * MAC unit opcodes.
 *
 * The MAC unit handles multiply-accumulate class operations.
 * It reads up to 3 source operands (A, B, C) and writes one destination.
 */
typedef enum NV2AVshMacOp {
    NV2A_VSH_MAC_NOP = 0,   /* No operation */
    NV2A_VSH_MAC_MOV = 1,   /* dst = A */
    NV2A_VSH_MAC_MUL = 2,   /* dst = A * B */
    NV2A_VSH_MAC_ADD = 3,   /* dst = A + C */
    NV2A_VSH_MAC_MAD = 4,   /* dst = A * B + C */
    NV2A_VSH_MAC_DP3 = 5,   /* dst = dot3(A.xyz, B.xyz) */
    NV2A_VSH_MAC_DPH = 6,   /* dst = dot3(A.xyz, B.xyz) + B.w */
    NV2A_VSH_MAC_DP4 = 7,   /* dst = dot4(A, B) */
    NV2A_VSH_MAC_DST = 8,   /* dst = distance vector */
    NV2A_VSH_MAC_MIN = 9,   /* dst = min(A, B) */
    NV2A_VSH_MAC_MAX = 10,  /* dst = max(A, B) */
    NV2A_VSH_MAC_SLT = 11,  /* dst = (A < B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_SGE = 12,  /* dst = (A >= B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_ARL = 13,  /* a0.x = floor(A.x) */
    NV2A_VSH_MAC_COUNT = 14,
} NV2AVshMacOp;

/**
 * ILU unit opcodes.
 *
 * The ILU unit handles transcendental/reciprocal operations.
 * It reads source operand C and writes one destination.
 * The ILU operates in parallel with the MAC unit.
 */
typedef enum NV2AVshIluOp {
    NV2A_VSH_ILU_NOP = 0,   /* No operation */
    NV2A_VSH_ILU_MOV = 1,   /* dst = C */
    NV2A_VSH_ILU_RCP = 2,   /* dst = 1.0 / C.x (scalar, replicated) */
    NV2A_VSH_ILU_RCC = 3,   /* dst = clamp(1.0/C.x, 5.42e-36, 1.884e+19) */
    NV2A_VSH_ILU_RSQ = 4,   /* dst = 1.0 / sqrt(abs(C.x)) */
    NV2A_VSH_ILU_EXP = 5,   /* dst = exp2(C.x) */
    NV2A_VSH_ILU_LOG = 6,   /* dst = log2(abs(C.x)) */
    NV2A_VSH_ILU_LIT = 7,   /* dst = lighting helper */
    NV2A_VSH_ILU_COUNT = 8,
} NV2AVshIluOp;

/**
 * Source operand register types.
 *
 * Values match the raw 2-bit hardware type field directly (see
 * parse_a_type/parse_b_type/parse_c_type in nv2a_vsh_disassembler.c) so
 * decoding is a plain cast, not a lookup table: 0=none, 1=temp, 2=input,
 * 3=const. NONE is a real, common case -- most opcodes only read 1 or 2
 * of the 3 possible operand slots (see parse_source()'s per-opcode
 * switch), and the unused slot(s) decode with whatever type bits happen
 * to be there; leaving them at NONE keeps them out of inputs_read.
 */
typedef enum NV2AVshRegType {
    NV2A_VSH_REG_NONE   = 0,  /* Operand slot unused by this opcode */
    NV2A_VSH_REG_TEMP   = 1,  /* R0-R11 (R12 = oPos alias) */
    NV2A_VSH_REG_INPUT  = 2,  /* v0-v15 */
    NV2A_VSH_REG_CONST  = 3,  /* c0-c191 (may be indexed via a0) */
} NV2AVshRegType;

/**
 * Output register selectors.
 *
 * When a MAC/ILU destination targets an output register, these
 * identify which output. If the mux value is 0xFF, the write
 * goes only to a temp register.
 */
typedef enum NV2AVshOutputReg {
    NV2A_VSH_OUT_POS  = 0,   /* oPos (clip-space position) */
    NV2A_VSH_OUT_D0   = 3,   /* oD0 (diffuse color) */
    NV2A_VSH_OUT_D1   = 4,   /* oD1 (specular color) */
    NV2A_VSH_OUT_FOG  = 5,   /* oFog (fog factor) */
    NV2A_VSH_OUT_PTS  = 6,   /* oPts (point size) */
    NV2A_VSH_OUT_B0   = 7,   /* oB0 (back diffuse) */
    NV2A_VSH_OUT_B1   = 8,   /* oB1 (back specular) */
    NV2A_VSH_OUT_T0   = 9,   /* oT0 (texcoord 0) */
    NV2A_VSH_OUT_T1   = 10,  /* oT1 (texcoord 1) */
    NV2A_VSH_OUT_T2   = 11,  /* oT2 (texcoord 2) */
    NV2A_VSH_OUT_T3   = 12,  /* oT3 (texcoord 3) */
    NV2A_VSH_OUT_NONE = 0xFF, /* No output register write */
} NV2AVshOutputReg;

/* ================================================================
 * Parsed Instruction Representation
 * ================================================================ */

/**
 * Swizzle encoding for one component.
 * Each component selector picks from {x=0, y=1, z=2, w=3}.
 */
typedef struct NV2AVshSwizzle {
    uint8_t x;  /* 0=x, 1=y, 2=z, 3=w */
    uint8_t y;
    uint8_t z;
    uint8_t w;
} NV2AVshSwizzle;

/**
 * A fully decoded source operand.
 */
typedef struct NV2AVshSrcOperand {
    NV2AVshRegType  reg_type;    /* TEMP, INPUT, or CONST */
    int             reg_index;   /* Register number within the bank */
    int             negate;      /* 1 = negate the value */
    NV2AVshSwizzle  swizzle;     /* Per-component swizzle */
    int             rel_addr;    /* 1 = use a0.x relative addressing (CONST only) */
} NV2AVshSrcOperand;

/**
 * What kind of register a destination write targets.
 *
 * Unlike source operands, this is NOT the raw hardware bit pattern --
 * real hardware only has an is_output/is_context bit (word 3 [11]) for
 * the single shared "real" write, plus separate temp-writemask fields.
 * This enum names the four outcomes parse_outputs() can produce for one
 * destination slot, matching the shape of xemu's Nv2aVshRegisterType
 * (NV2ART_NONE/TEMPORARY/OUTPUT/CONTEXT/ADDRESS) closely enough to port
 * parse_outputs() against directly.
 */
typedef enum NV2AVshDstRegType {
    NV2A_VSH_DST_NONE    = 0,  /* Slot unused */
    NV2A_VSH_DST_TEMP,         /* R0-R11 (R12 = oPos alias) */
    NV2A_VSH_DST_OUTPUT,       /* A real output register -- reg_index holds
                                 * an NV2AVshOutputReg value */
    NV2A_VSH_DST_CONST,        /* Write back into a constant/context
                                 * register (context-switch microcode).
                                 * Decoded but not modeled -- see
                                 * emit_one_dest() in d3d8_vsh.c. */
    NV2A_VSH_DST_ADDRESS,      /* a0 (ARL's implicit destination only) */
} NV2AVshDstRegType;

/**
 * A fully decoded destination write.
 */
typedef struct NV2AVshDstOperand {
    NV2AVshDstRegType dst_type;
    int               reg_index;   /* Meaning depends on dst_type: temp#,
                                     * NV2AVshOutputReg, or raw const/
                                     * context register#. Unused for
                                     * NONE/ADDRESS. */
    uint8_t           write_mask;  /* Bitmask: bit3=x, bit2=y, bit1=z, bit0=w */
} NV2AVshDstOperand;

/**
 * The MAC unit's decoded operation: opcode, up to 3 source operands (A,
 * B, C -- unused slots per select_mac_inputs()'s per-opcode switch stay
 * type NONE), and up to 2 destination writes (a temp-register write
 * and/or the single shared real-output/context write -- see
 * parse_destinations()).
 */
typedef struct NV2AVshMacOperation {
    NV2AVshMacOp      opcode;
    NV2AVshSrcOperand inputs[3];
    NV2AVshDstOperand outputs[2];
} NV2AVshMacOperation;

/**
 * The ILU unit's decoded operation. Real hardware only ever gives ILU one
 * input (source C -- see select_ilu_input()), unlike MAC's up to three.
 */
typedef struct NV2AVshIluOperation {
    NV2AVshIluOp      opcode;
    NV2AVshSrcOperand inputs[1];
    NV2AVshDstOperand outputs[2];
} NV2AVshIluOperation;

/**
 * A fully decoded NV2A vertex shader instruction.
 *
 * Each 128-bit instruction slot can encode both a MAC operation
 * and an ILU operation that execute in parallel. Either (or both)
 * may be NOP.
 */
typedef struct NV2AVshInstruction {
    NV2AVshMacOperation mac;
    NV2AVshIluOperation ilu;

    /* Final instruction flag */
    int             is_final;
} NV2AVshInstruction;

/**
 * A complete parsed vertex shader program.
 */
typedef struct NV2AVshProgram {
    NV2AVshInstruction  insns[NV2A_VS_MAX_INSTRUCTIONS];
    int                 length;     /* Number of instructions */

    /* Bitmask of input registers read (v0-v15). Bit N = vN is used.
     * Used to determine the required input layout. */
    uint16_t            inputs_read;

    /* True if any instruction reads c[NV2A_VS_VPSCL_REG] or
     * c[NV2A_VS_VPOFF_REG] -- i.e. this program follows the standard Xbox
     * SDK/nxdk compiled-vertex-shader epilogue convention and its oPos
     * output is in NV2A device/pixel space, not D3D11 clip space. Gates
     * d3d8_vsh_generate_hlsl()'s undo-the-bake-in epilogue so a program
     * that never reads those registers (and so must already be producing
     * genuine clip-space output some other way) is left alone rather than
     * having a transform applied that was never in its own instructions. */
    int                 uses_viewport_ctx;
} NV2AVshProgram;

/* ================================================================
 * Shader Slot (stored microcode)
 * ================================================================ */

/**
 * A stored vertex shader program slot.
 *
 * Created by CreateVertexShader(), indexed by handle.
 * The microcode is stored as raw DWORDs; parsing and compilation
 * are deferred until the shader is first used in a draw call.
 */
typedef struct NV2AVshSlot {
    DWORD   microcode[NV2A_VS_MAX_INSTRUCTIONS * 4]; /* Raw 128-bit instructions */
    int     length;         /* Number of instructions */
    int     in_use;         /* 1 if this slot is allocated */
} NV2AVshSlot;

/* ================================================================
 * VS Constant Buffer Layout (HLSL)
 *
 * Uploaded to register(b1) so it doesn't conflict with the
 * fixed-function transform CB at b0.
 *
 * Must be 16-byte aligned and match the HLSL cbuffer declaration.
 * ================================================================ */

typedef struct NV2AVSConstants {
    float c[NV2A_VS_MAX_CONSTANTS][4];  /* 192 float4 constants */
    float surface[4];  /* bound render target width, height, depth range
                        * (0 when unknown); see d3d8_vsh_set_surface() */
    float fog[4];      /* enable, mode, param0, param1; see d3d8_vsh_set_fog() */
} NV2AVSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the vertex shader translator.
 * Allocates the constant buffer and shader cache.
 * Must be called after D3D11 device creation.
 */
HRESULT d3d8_vsh_init(void);

/**
 * Shut down the vertex shader translator.
 * Releases all cached shaders, input layouts, and buffers.
 */
void d3d8_vsh_shutdown(void);

/**
 * Store a vertex shader program (CreateVertexShader).
 *
 * Copies the microcode into an internal slot. The shader is not
 * compiled until first use.
 *
 * @param microcode   Pointer to the 128-bit instruction array (4 DWORDs each)
 * @param num_insns   Number of instructions
 * @param out_handle  Receives the shader handle (>= 0x10000 to distinguish from FVF)
 * @return S_OK on success
 */
HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle);

/**
 * Delete a previously created vertex shader.
 *
 * @param handle  The shader handle from d3d8_vsh_create_shader
 * @return S_OK on success
 */
HRESULT d3d8_vsh_delete_shader(DWORD handle);

/**
 * Set a vertex shader constant register.
 *
 * @param start_reg  First register index (0-191)
 * @param data       Pointer to float4 data (4 floats per register)
 * @param count      Number of float4 registers to set
 */
void d3d8_vsh_set_constant(int start_reg, const float *data, int count);

/**
 * Tell the epilogue the size of the render target being drawn into.
 *
 * A compiled NV2A vertex program leaves oPos in the target's pixel space;
 * mapping that to clip space needs the target's size, as xemu's vsh-prog.c
 * does with its surfaceSize uniform. zmax is the depth that maps to 1.0
 * (SET_CLIP_MAX). A width of 0 falls back to undoing c[VPSCL]/c[VPOFF].
 */
void d3d8_vsh_set_surface(float width, float height, float zmax);

/**
 * Fog as NV2A computes it after the vertex program (xemu vsh.c): with fog
 * off the fog output is 1 (no fog); with it on, the program's oFog.x is a
 * distance turned into a factor by `mode` (NV_PGRAPH_CONTROL_3_FOG_MODE: 0
 * linear, 1 exp, 3 exp2, 4/5/7 their abs forms) and SET_FOG_PARAMS[0..1].
 */
void d3d8_vsh_set_fog(int enable, int mode, float param0, float param1);

/** Current value of constant register `reg` (debug tracing). */
const float *d3d8_vsh_get_constant(int reg);

/**
 * Check if a shader handle refers to a programmable vertex shader
 * (as opposed to an FVF code).
 *
 * On Xbox, handles > 0xFFFF are shader handles.
 */
BOOL d3d8_vsh_is_programmable(DWORD handle);

/**
 * Prepare for a draw call using a programmable vertex shader.
 *
 * - Parses microcode if not yet parsed
 * - Generates HLSL and compiles if not cached
 * - Updates the constant buffer
 * - Binds the vertex shader, input layout, and constant buffer
 *
 * @param handle  The active vertex shader handle
 * @return TRUE if a programmable VS was bound, FALSE on fallback
 */
BOOL d3d8_vsh_prepare_draw(DWORD handle);

/**
 * Parse NV2A vertex shader microcode into intermediate representation.
 *
 * @param microcode  Raw instruction data (4 DWORDs per instruction)
 * @param num_insns  Number of instructions
 * @param program    Output parsed program
 */
void d3d8_vsh_parse(const DWORD *microcode, int num_insns,
                     NV2AVshProgram *program);

/**
 * Generate HLSL vertex shader source from parsed program.
 *
 * @param program   Parsed program
 * @param buf       Output buffer for HLSL source
 * @param bufsize   Size of output buffer
 * @return Number of characters written, or -1 on error
 */
int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize);

/**
 * Default D3D11 vertex-buffer format/size for a given NV2A input register
 * (v0-v15), matching the "common Xbox convention" table create_vsh_input_
 * layout() builds the bound input layout from. Exposed so any caller that
 * assembles raw vertex bytes for a programmable draw (e.g. a pgraph path
 * that bypasses the D3D8 CreateVertexShader/DrawPrimitiveUP API entirely)
 * can match that layout exactly instead of keeping a second, driftable
 * copy of the table.
 */
DXGI_FORMAT d3d8_vsh_default_input_format(int vreg);
UINT d3d8_vsh_input_format_size(DXGI_FORMAT fmt);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VSH_H */
