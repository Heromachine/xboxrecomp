"""Tests for MMX (mm0-mm7 packed-integer) lifting.

Before this lifter had any MMX support, every MMX mnemonic fell through to
the generic unhandled-instruction fallback, which emits a comment and no
code at all. In Breakdown that made the XMV movie codec's IDCT, motion
compensation and YUV->RGB inner loops -- 20 functions, ~1,600 instructions --
complete no-ops: the intro movie could not possibly decode correctly.

movd/movq/pand/pandn/por/pxor/pcmpgtd share their mnemonic with an XMM/SSE2
form and are lifted by `_lift_sse` (which already tells an mm operand from
an xmm one via `_is_mmx`/`_is_xmm`). Every other MMX mnemonic here has no
XMM form on this CPU generation (Xbox is SSE1, not SSE2) and is lifted by
the dedicated `_lift_mmx`.
"""

from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter, _EFLAGS_PRESERVE


def _insn(mnemonic, op_str, operands):
    return Instruction(0x1000, 4, mnemonic, op_str, "aa", operands=operands)


def _mm(name):
    return Operand("reg", reg=name)


def _xmm(name):
    return Operand("reg", reg=name)


def _mem64(base=None, disp=0, index=None, scale=1):
    return Operand("mem", mem_base=base, mem_index=index, mem_scale=scale,
                    mem_disp=disp, mem_size=8)


def _imm(v):
    return Operand("imm", imm=v)


# ── movq (the critical movd/movq finding) ──────────────────────────

def test_movq_register_to_register_is_a_plain_scalar_copy():
    """mm0-mm7 are plain uint64_t globals, so a whole-register move is a
    bare assignment -- no lane view needed."""
    insn = _insn("movq", "mm2, mm4", [_mm("mm2"), _mm("mm4")])
    assert Lifter().lift_instruction(insn) == ["mm2 = mm4; /* movq */"]


def test_movq_loads_a_full_qword_not_half_of_one():
    """Before MEM64 existed, an 8-byte memory operand fell back to MEM32
    (see _mem_accessor), silently reading only the low 32 bits."""
    insn = _insn("movq", "mm2, qword ptr [ecx]",
                 [_mm("mm2"), _mem64(base="ecx")])
    assert Lifter().lift_instruction(insn) == ["mm2 = MEM64(ecx); /* movq */"]


def test_movq_stores_a_full_qword():
    insn = _insn("movq", "qword ptr [edi], mm3",
                 [_mem64(base="edi"), _mm("mm3")])
    assert Lifter().lift_instruction(insn) == [
        "MEM64(edi) = mm3; /* movq */"]


def test_movq_used_to_be_a_dead_comment():
    """Regression guard for the exact failure this task's brief called out:
    movq previously fell all the way through _lift_sse to its final
    unhandled-SSE catch-all and produced no code."""
    insn = _insn("movq", "mm0, qword ptr [esi]",
                 [_mm("mm0"), _mem64(base="esi")])
    out = Lifter().lift_instruction(insn)
    assert not any("SSE:" in line or "TODO" in line for line in out)


# ── pand / pandn / por / pxor / pcmpgtd (shared with SSE2, MMX side) ──

def test_pxor_self_zeroes_rather_than_xoring():
    insn = _insn("pxor", "mm0, mm0", [_mm("mm0"), _mm("mm0")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = 0; /* pxor self = zero */"]


def test_pxor_distinct_registers_xors():
    insn = _insn("pxor", "mm1, mm2", [_mm("mm1"), _mm("mm2")])
    assert Lifter().lift_instruction(insn) == [
        "mm1 = mm1 ^ (mm2); /* pxor */"]


def test_pand():
    insn = _insn("pand", "mm0, mm2", [_mm("mm0"), _mm("mm2")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = mm0 & (mm2); /* pand */"]


def test_pand_with_memory_operand_reads_a_full_qword():
    """Seen in Breakdown as `pand mm0, qword ptr [addr]` against a constant
    mask table -- another mem_size-8 case that must not fall back to a
    32-bit read."""
    insn = _insn("pand", "mm0, qword ptr [0x1e4450]",
                 [_mm("mm0"), _mem64(disp=0x1e4450)])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = mm0 & (MEM64(0x1E4450)); /* pand */"]


def test_pandn_is_not_dst_and_src():
    """PANDN dst, src computes (NOT dst) AND src -- the operands are not
    symmetric, and getting the negation on the wrong side is a classic
    bug for this instruction."""
    insn = _insn("pandn", "mm3, mm5", [_mm("mm3"), _mm("mm5")])
    assert Lifter().lift_instruction(insn) == [
        "mm3 = (~mm3) & (mm5); /* pandn */"]


def test_por():
    insn = _insn("por", "mm1, mm2", [_mm("mm1"), _mm("mm2")])
    assert Lifter().lift_instruction(insn) == [
        "mm1 = mm1 | (mm2); /* por */"]


def test_pcmpgtd_mmx_form_uses_the_runtime_helper():
    insn = _insn("pcmpgtd", "mm0, mm2", [_mm("mm0"), _mm("mm2")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PCMPGTD(mm0, mm2); /* pcmpgtd */"]


def test_pxor_xmm_self_zero_still_works():
    """pxor dispatches through the same _lift_sse case for both register
    kinds -- confirm adding the MMX path didn't regress the xmm one."""
    insn = _insn("pxor", "xmm0, xmm0", [_xmm("xmm0"), _xmm("xmm0")])
    assert Lifter().lift_instruction(insn) == [
        "xmm0 = XMM_ZERO(); /* pxor self = zero */"]


# ── arithmetic ──────────────────────────────────────────────────────

def test_paddw():
    insn = _insn("paddw", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PADDW(mm0, mm1); /* paddw */"]


def test_paddw_with_memory_source():
    insn = _insn("paddw", "mm4, qword ptr [esi + 8]",
                 [_mm("mm4"), _mem64(base="esi", disp=8)])
    assert Lifter().lift_instruction(insn) == [
        "mm4 = MMX_PADDW(mm4, MEM64(esi + 8)); /* paddw */"]


def test_paddd():
    insn = _insn("paddd", "mm3, mm1", [_mm("mm3"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm3 = MMX_PADDD(mm3, mm1); /* paddd */"]


def test_psubw():
    insn = _insn("psubw", "mm2, mm0", [_mm("mm2"), _mm("mm0")])
    assert Lifter().lift_instruction(insn) == [
        "mm2 = MMX_PSUBW(mm2, mm0); /* psubw */"]


def test_pmullw():
    insn = _insn("pmullw", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PMULLW(mm0, mm1); /* pmullw */"]


def test_pmaddwd():
    insn = _insn("pmaddwd", "mm3, mm0", [_mm("mm3"), _mm("mm0")])
    assert Lifter().lift_instruction(insn) == [
        "mm3 = MMX_PMADDWD(mm3, mm0); /* pmaddwd */"]


def test_pavgb():
    insn = _insn("pavgb", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PAVGB(mm0, mm1); /* pavgb */"]


def test_pcmpeqb():
    insn = _insn("pcmpeqb", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PCMPEQB(mm0, mm1); /* pcmpeqb */"]


def test_pcmpgtw():
    insn = _insn("pcmpgtw", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PCMPGTW(mm0, mm1); /* pcmpgtw */"]


# ── pack / unpack ───────────────────────────────────────────────────

def test_packuswb():
    insn = _insn("packuswb", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PACKUSWB(mm0, mm1); /* packuswb */"]


def test_packssdw():
    insn = _insn("packssdw", "mm3, mm4", [_mm("mm3"), _mm("mm4")])
    assert Lifter().lift_instruction(insn) == [
        "mm3 = MMX_PACKSSDW(mm3, mm4); /* packssdw */"]


def test_punpcklbw_is_the_most_frequent_stub_after_paddw():
    insn = _insn("punpcklbw", "mm4, mm2", [_mm("mm4"), _mm("mm2")])
    assert Lifter().lift_instruction(insn) == [
        "mm4 = MMX_PUNPCKLBW(mm4, mm2); /* punpcklbw */"]


def test_punpckhwd():
    insn = _insn("punpckhwd", "mm5, mm0", [_mm("mm5"), _mm("mm0")])
    assert Lifter().lift_instruction(insn) == [
        "mm5 = MMX_PUNPCKHWD(mm5, mm0); /* punpckhwd */"]


def test_punpckldq():
    insn = _insn("punpckldq", "mm0, mm1", [_mm("mm0"), _mm("mm1")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PUNPCKLDQ(mm0, mm1); /* punpckldq */"]


# ── shifts ──────────────────────────────────────────────────────────

def test_psraw_immediate_count():
    insn = _insn("psraw", "mm3, 5", [_mm("mm3"), _imm(5)])
    assert Lifter().lift_instruction(insn) == [
        "mm3 = MMX_PSRAW(mm3, (uint64_t)(5)); /* psraw */"]


def test_psrad_immediate_count():
    insn = _insn("psrad", "mm1, 0x10", [_mm("mm1"), _imm(0x10)])
    assert Lifter().lift_instruction(insn) == [
        "mm1 = MMX_PSRAD(mm1, (uint64_t)(0x10)); /* psrad */"]


def test_psllq_register_count():
    """Breakdown uses this shape (`psllq mmN, mmN`) directly -- the count
    comes from a whole mm register, not an immediate."""
    insn = _insn("psllq", "mm3, mm3", [_mm("mm3"), _mm("mm3")])
    assert Lifter().lift_instruction(insn) == [
        "mm3 = MMX_PSLLQ(mm3, (uint64_t)(mm3)); /* psllq */"]


def test_pslld():
    insn = _insn("pslld", "mm0, 6", [_mm("mm0"), _imm(6)])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_PSLLD(mm0, (uint64_t)(6)); /* pslld */"]


def test_psrlw():
    insn = _insn("psrlw", "mm2, 3", [_mm("mm2"), _imm(3)])
    assert Lifter().lift_instruction(insn) == [
        "mm2 = MMX_PSRLW(mm2, (uint64_t)(3)); /* psrlw */"]


# ── pshufw / movntq / cvtps2pi / cvtpi2ps ───────────────────────────

def test_pshufw():
    insn = _insn("pshufw", "mm1, mm0, 0xfa",
                 [_mm("mm1"), _mm("mm0"), _imm(0xfa)])
    assert Lifter().lift_instruction(insn) == [
        "mm1 = MMX_PSHUFW(mm0, 0xFA); /* pshufw */"]


def test_movntq_is_a_plain_store_ignoring_the_cache_hint():
    insn = _insn("movntq", "qword ptr [edi + 0x10], mm2",
                 [_mem64(base="edi", disp=0x10), _mm("mm2")])
    assert Lifter().lift_instruction(insn) == [
        "MEM64(edi + 0x10) = mm2; /* movntq */"]


def test_cvtps2pi_reads_xmm_writes_mm():
    insn = _insn("cvtps2pi", "mm0, xmm3", [_mm("mm0"), _xmm("xmm3")])
    assert Lifter().lift_instruction(insn) == [
        "mm0 = MMX_CVTPS2PI(xmm3); /* cvtps2pi */"]


def test_cvtpi2ps_reads_mm_writes_low_two_lanes_of_xmm():
    """The destination's own upper two lanes must pass through untouched,
    so the helper takes (and the lifted call passes) the whole xmm value,
    not just a fresh one."""
    insn = _insn("cvtpi2ps", "xmm0, mm0", [_xmm("xmm0"), _mm("mm0")])
    assert Lifter().lift_instruction(insn) == [
        "xmm0 = MMX_CVTPI2PS(xmm0, mm0); /* cvtpi2ps */"]


# ── flag tracking ───────────────────────────────────────────────────

def test_mmx_instructions_preserve_flag_tracking():
    """None of these touch EFLAGS. Left unclassified, the translator drops
    flag tracking at that point and a later jcc reads a dead placeholder
    condition -- this project has hit real infinite loops from exactly that
    shape before. pavgb/cvtps2pi/cvtpi2ps were missing from the set; the
    rest were already present.
    """
    for mnemonic in ("movq", "pand", "pandn", "por", "pxor", "pcmpgtd",
                      "paddb", "paddw", "paddd", "psubb", "psubw", "psubd",
                      "pmullw", "pmaddwd", "pavgb",
                      "pcmpeqb", "pcmpeqw", "pcmpgtb", "pcmpgtw",
                      "packsswb", "packssdw", "packuswb",
                      "punpcklbw", "punpckhbw", "punpcklwd", "punpckhwd",
                      "punpckldq", "punpckhdq",
                      "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq",
                      "psraw", "psrad", "pshufw", "movntq",
                      "cvtps2pi", "cvtpi2ps"):
        assert mnemonic in _EFLAGS_PRESERVE, mnemonic


# ── memory operand width (the general MEM64 fix) ────────────────────

def test_qword_memory_operand_routes_to_mem64_not_mem32():
    """_mem_accessor had no entry for an 8-byte operand and silently fell
    back to MEM32 -- exercised generically here rather than through one
    specific mnemonic, since the fix lives in the shared operand
    formatting helper."""
    from tools.recomp.lifter import _fmt_mem_read
    op = _mem64(base="eax")
    assert _fmt_mem_read(op) == "MEM64(eax)"
