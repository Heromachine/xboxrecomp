/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * Decodes x86-64 MOV instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include <stdio.h>

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;

/* Statistics */
static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;

/* Global APU state pointer is declared in apu.h and referenced from main.c
 * (regardless of whether the MMIO hook is active). Keep its definition
 * outside the Win32 guard. */

#if defined(_WIN32)

/* ============================================================
 * x86-64 register access helpers
 * ============================================================ */

/* Map ModRM reg field (+ REX.R) to CONTEXT register pointer */
static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

/* ============================================================
 * x86-64 instruction decoder (focused on MOV patterns)
 *
 * We only need to handle the patterns MSVC generates for
 * volatile memory access (MEM8/MEM16/MEM32 macros):
 *
 * Writes:
 *   89 /r      MOV r/m32, r32       (32-bit reg → memory)
 *   88 /r      MOV r/m8, r8         (8-bit reg → memory)
 *   66 89 /r   MOV r/m16, r16       (16-bit reg → memory)
 *   C7 /0 id   MOV r/m32, imm32     (32-bit immediate → memory)
 *   C6 /0 ib   MOV r/m8, imm8       (8-bit immediate → memory)
 *
 * Reads:
 *   8B /r      MOV r32, r/m32       (memory → 32-bit reg)
 *   8A /r      MOV r8, r/m8         (memory → 8-bit reg)
 *   66 8B /r   MOV r16, r/m16       (memory → 16-bit reg)
 *   0F B6 /r   MOVZX r32, r/m8      (zero-extend byte → 32-bit)
 *   0F B7 /r   MOVZX r32, r/m16     (zero-extend word → 32-bit)
 *
 * With REX prefixes for 64-bit register extension.
 * ============================================================ */

/* Decode ModRM + optional SIB + displacement, return instruction length */
static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1; /* modrm byte */

    if (mod == 3) {
        /* Register-direct, no memory access - shouldn't happen for MMIO */
        return len;
    }

    /* Check for SIB byte */
    if ((rm & 7) == 4) {
        len++; /* SIB byte */
    }

    /* Displacement */
    if (mod == 0) {
        if ((rm & 7) == 5) len += 4; /* disp32 (RIP-relative or [disp32]) */
    } else if (mod == 1) {
        len += 1; /* disp8 */
    } else if (mod == 2) {
        len += 4; /* disp32 */
    }

    return len;
}

/*
 * Try to decode and handle the faulting instruction.
 * Returns true if successfully handled, false if unrecognized.
 */
static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;

    int prefix_len = 0;
    int has_66 = 0;     /* operand size override */
    int rex = 0;        /* REX prefix byte */
    int has_rex = 0;

    /* Parse prefixes */
    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) {
            has_66 = 1;
            prefix_len++;
        } else if (b == 0xF2 || b == 0xF3) {
            /* REP/REPNE prefix - skip */
            prefix_len++;
        } else if (b >= 0x40 && b <= 0x4F) {
            /* REX prefix */
            rex = b;
            has_rex = 1;
            prefix_len++;
        } else {
            break;
        }
    }

    int rex_w = has_rex && (rex & 0x08); /* 64-bit operand */
    int rex_r = has_rex && (rex & 0x04); /* extends ModRM reg */
    int rex_b = has_rex && (rex & 0x01); /* extends ModRM r/m */

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4; /* default 32-bit */
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* ── MOV r/m, r (write: 88/89) ── */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);

        /* Mask to access size */
        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;

        nv2a_mmio_write(nv2a, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r, r/m (read: 8A/8B) ── */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, access_size);

        uint64_t *dest = ctx_reg64(ctx, reg);
        if (access_size == 1) {
            *dest = (*dest & ~0xFFULL) | (val & 0xFF);
        } else if (access_size == 2) {
            *dest = (*dest & ~0xFFFFULL) | (val & 0xFFFF);
        } else if (access_size == 4) {
            *dest = val & 0xFFFFFFFF; /* 32-bit write zero-extends */
        } else {
            *dest = val;
        }

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOV r/m32, imm32 (C7 /0) ── */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        /* immediate follows modrm+sib+disp */
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint32_t imm = *(const uint32_t *)imm_ptr;
        int imm_len = (rex_w ? 4 : 4); /* still 32-bit imm even with REX.W */

        nv2a_mmio_write(nv2a, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOV r/m8, imm8 (C6 /0) ── */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);

        nv2a_mmio_write(nv2a, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_mmio_write_count++;
        return true;
    }

    /* ── MOVZX r32, r/m8 (0F B6) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 1) & 0xFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val; /* zero-extend to 64-bit */

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── MOVZX r32, r/m16 (0F B7) ── */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 2) & 0xFFFF;

        uint64_t *dest = ctx_reg64(ctx, reg);
        *dest = val;

        ctx->Rip += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── TEST r/m, r (84/85) - reads memory for flag comparison ── */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size == 1) { mem_val &= 0xFF; reg_val &= 0xFF; }
        else if (access_size == 2) { mem_val &= 0xFFFF; reg_val &= 0xFFFF; }
        else if (access_size == 4) { mem_val &= 0xFFFFFFFF; reg_val &= 0xFFFFFFFF; }

        uint64_t result = mem_val & reg_val;

        /* Update flags: ZF, SF, PF; clear OF, CF */
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800); /* CF, ZF, SF, OF */
        if (result == 0) ctx->EFlags |= 0x0040; /* ZF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */

        ctx->Rip += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* ── ALU r/m,r and r,r/m: the classic eight (opcodes 0x00-0x3B) ──
     *
     * x86 encodes ADD/OR/ADC/SBB/AND/SUB/XOR/CMP as one regular block:
     * opcode = op*8 + form, where form 0 = r/m8,r8   1 = r/m32,r32
     *                             form 2 = r8,r/m8   3 = r32,r/m32.
     * Forms 0/1 write memory (read-modify-write); forms 2/3 write the
     * register; CMP writes neither. `(opcode & 7) <= 3` selects exactly the
     * 32 ModRM-carrying opcodes and naturally excludes the imm-to-accumulator
     * forms (op*8+4/5) and the segment/BCD one-byte opcodes (op*8+6/7).
     *
     * Handling the block as a block is deliberate. Adding these one opcode at
     * a time is what made bringing the GPU up a sequence of crash-fix-rerun
     * cycles: OR and AND r/m,r were present, so `MEM32(x) |= y` worked, while
     * `xor r8d, [rcx+rax]` -- the same shape in the other direction -- took
     * the process down.
     *
     * Note the 8-bit forms address al/cl/dl/bl only; without a REX prefix
     * regs 4-7 would mean ah/ch/dh/bh, which ctx_reg64 cannot express. That
     * limitation is shared with the MOV and MOVZX paths above and has not
     * been hit, since the lifted code is compiled as x86-64.
     */
    if (opcode[0] < 0x40 && (opcode[0] & 0x07) <= 0x03) {
        int op   = (opcode[0] >> 3) & 7;
        int form = opcode[0] & 0x03;
        int to_reg = (form >= 2);
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t *rp = ctx_reg64(ctx, reg);
        uint64_t cf_in = (ctx->EFlags & 0x0001) ? 1 : 0;
        uint64_t mask, mem_val, reg_val, lhs, rhs, result;
        int carry = 0;

        if (form == 0 || form == 2) access_size = 1;
        mask = (access_size >= 8) ? ~0ULL : ((1ULL << (access_size * 8)) - 1);

        mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
        reg_val = *rp & mask;
        lhs = to_reg ? reg_val : mem_val;
        rhs = to_reg ? mem_val : reg_val;

        switch (op) {
        case 0: result = lhs + rhs;         carry = ((result & mask) < lhs); break; /* ADD */
        case 1: result = lhs | rhs;                                          break; /* OR  */
        case 2: result = lhs + rhs + cf_in; carry = ((result & mask) < lhs); break; /* ADC */
        case 3: result = lhs - rhs - cf_in; carry = (lhs < rhs + cf_in);     break; /* SBB */
        case 4: result = lhs & rhs;                                          break; /* AND */
        case 5: result = lhs - rhs;         carry = (lhs < rhs);             break; /* SUB */
        case 6: result = lhs ^ rhs;                                          break; /* XOR */
        default: result = lhs - rhs;        carry = (lhs < rhs);             break; /* CMP */
        }
        result &= mask;

        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;                              /* ZF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */
        if (carry) ctx->EFlags |= 0x0001;                                    /* CF */

        if (op == 7) {                       /* CMP: flags only */
            g_mmio_read_count++;
        } else if (to_reg) {
            if (access_size == 1)      *rp = (*rp & ~0xFFULL)   | result;
            else if (access_size == 2) *rp = (*rp & ~0xFFFFULL) | result;
            else                       *rp = result;  /* 32-bit zero-extends */
            g_mmio_read_count++;
        } else {
            nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
            g_mmio_write_count++;
        }

        ctx->Rip += prefix_len + 1 + modrm_len;
        return true;
    }

    /* ── Group 1: ALU r/m, imm (80 /r ib, 81 /r id, 83 /r ib sign-extended) ──
     *
     * The operation lives in the ModRM reg field, not the opcode. This is what
     * `MEM32(reg) |= BIT` and `MEM32(reg) &= ~BIT` compile to, which makes it
     * the single most common way lifted code drives a hardware register: every
     * set-a-bit / wait-for-hardware-to-clear-it handshake goes through here.
     * Its absence is why the very first NV2A access this title makes --
     * `or dword [rdx+rcx], 4` at guest 0xFD001804 -- could not be decoded.
     */
    if (opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83) {
        int op = (opcode[1] >> 3) & 7;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        int imm_len;
        uint64_t imm;

        if (opcode[0] == 0x80) {
            access_size = 1;
            imm_len = 1;
            imm = *imm_ptr;
        } else if (opcode[0] == 0x83) {
            /* imm8 sign-extended to the operand size */
            imm_len = 1;
            imm = (uint64_t)(int64_t)(int8_t)*imm_ptr;
        } else if (access_size == 2) {
            imm_len = 2;
            imm = *(const uint16_t *)imm_ptr;
        } else {
            imm_len = 4;
            imm = (uint64_t)(int64_t)*(const int32_t *)imm_ptr;
        }

        /* 1ULL << 64 is undefined, so build the mask without shifting by 64 */
        uint64_t mask = (access_size >= 8) ? ~0ULL
                                           : ((1ULL << (access_size * 8)) - 1);
        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
        uint64_t rhs = imm & mask;
        uint64_t cf_in = (ctx->EFlags & 0x0001) ? 1 : 0;
        uint64_t result;
        int carry = 0;
        int is_cmp = 0;

        switch (op) {
        case 0: result = mem_val + rhs;         carry = ((result & mask) < mem_val); break; /* ADD */
        case 1: result = mem_val | rhs;                                              break; /* OR  */
        case 2: result = mem_val + rhs + cf_in; carry = ((result & mask) < mem_val); break; /* ADC */
        case 3: result = mem_val - rhs - cf_in; carry = (mem_val < rhs + cf_in);     break; /* SBB */
        case 4: result = mem_val & rhs;                                              break; /* AND */
        case 5: result = mem_val - rhs;         carry = (mem_val < rhs);             break; /* SUB */
        case 6: result = mem_val ^ rhs;                                              break; /* XOR */
        default: result = mem_val - rhs;        carry = (mem_val < rhs); is_cmp = 1; break; /* CMP */
        }
        result &= mask;

        /* Logical ops clear CF and OF on x86; arithmetic ones get a real CF. */
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;                          /* ZF */
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */
        if (carry) ctx->EFlags |= 0x0001;                                /* CF */

        if (is_cmp) {
            g_mmio_read_count++;
        } else {
            nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
            g_mmio_write_count++;
        }

        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        return true;
    }

    /* ── Group 3: TEST/NOT/NEG r/m (F6 /0-/3 imm8, F7 /0-/3 imm32) ──
     *
     * Also selected by the ModRM reg field. `test [reg], BIT` is the read half
     * of the same handshakes Group 1 writes -- the title sets a busy bit, then
     * spins here waiting for the GPU to clear it. MUL/DIV (/4-/7) take EAX
     * implicitly and are left to the decode-fail path; nothing drives a
     * hardware register through them.
     */
    if (opcode[0] == 0xF6 || opcode[0] == 0xF7) {
        int op = (opcode[1] >> 3) & 7;

        if (op <= 3) {
            int modrm_len = decode_modrm_len(opcode + 1, rex_b);
            const uint8_t *imm_ptr = opcode + 1 + modrm_len;
            int imm_len = 0;
            uint64_t imm = 0;

            if (opcode[0] == 0xF6) access_size = 1;

            if (op <= 1) {  /* TEST takes an immediate; NOT/NEG do not */
                if (access_size == 1)      { imm_len = 1; imm = *imm_ptr; }
                else if (access_size == 2) { imm_len = 2; imm = *(const uint16_t *)imm_ptr; }
                else                       { imm_len = 4;
                                             imm = (uint64_t)(int64_t)*(const int32_t *)imm_ptr; }
            }

            uint64_t mask = (access_size >= 8) ? ~0ULL
                                               : ((1ULL << (access_size * 8)) - 1);
            uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size) & mask;
            uint64_t result;
            int writes_back = 0;
            int carry = 0;
            int set_flags = 1;

            if (op <= 1) {
                result = mem_val & (imm & mask);          /* TEST */
            } else if (op == 2) {
                result = (~mem_val) & mask;               /* NOT: no flags */
                writes_back = 1;
                set_flags = 0;
            } else {
                result = (0 - mem_val) & mask;            /* NEG */
                writes_back = 1;
                carry = (mem_val != 0);
            }

            if (set_flags) {
                ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
                if (result == 0) ctx->EFlags |= 0x0040;                              /* ZF */
                if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080; /* SF */
                if (carry) ctx->EFlags |= 0x0001;                                    /* CF */
            }

            if (writes_back) {
                nv2a_mmio_write(nv2a, mmio_offset, result, access_size);
                g_mmio_write_count++;
            } else {
                g_mmio_read_count++;
            }

            ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
            return true;
        }
    }

    /* Unrecognized instruction */
    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API
 * ============================================================ */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    g_mem_offset = xbox_mem_offset;

    /* Allocate NV2A VRAM (64MB) */
    g_nv2a_vram = (uint8_t *)VirtualAlloc(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_nv2a_vram) {
        fprintf(stderr, "[NV2A] Failed to allocate %u MB for VRAM!\n",
                (NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE) / (1024*1024));
        return;
    }

    uint8_t *ramin_ptr = g_nv2a_vram + NV2A_VRAM_SIZE;

    /* Initialize NV2A state machine */
    nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE,
                         ramin_ptr, NV2A_RAMIN_SIZE);

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void*)g_nv2a_vram, (void*)ramin_ptr);
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    /* Compute MMIO offset within NV2A register space */
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;

    /* Periodic traffic summary. A title waiting on a bit that never changes
     * spins on one offset forever, so the last offset seen names the stall
     * outright -- the thing that was impossible to see while this aperture
     * was plain RAM. Rate-limited, so a busy frame costs one line every 2s. */
    {
        static DWORD last_tick;
        static uint64_t accesses;
        DWORD now = GetTickCount();

        accesses++;
        if (last_tick == 0) last_tick = now;
        if (now - last_tick >= 2000) {
            fprintf(stderr, "[NV2A] mmio: %llu accesses (%d rd / %d wr), "
                    "last offset 0x%06X (%s)\n",
                    (unsigned long long)accesses, g_mmio_read_count,
                    g_mmio_write_count, mmio_offset, is_write ? "write" : "read");
            fflush(stderr);
            last_tick = now;
        }
    }

    return decode_and_handle(ctx, mmio_offset, is_write);
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{
    /* For VRAM range (0xF0000000-0xF3FFFFFF), allocate pages as before.
     * In the future, we can map these to NV2A VRAM for push buffer DMA.
     * For now, just allocate writable pages. */
    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
}

#else /* !_WIN32 -- SIGSEGV-based MMIO trapping deferred to main.c port */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }

#endif /* _WIN32 */
