"""Tests for IN/OUT (x86 port I/O) lifting.

These used to fall through to the generic unhandled-instruction fallback,
which emits a comment and no code at all: `in al, dx` left AL holding
whatever the previous instruction had put in eax, so the guest acted on a
port value that drifted with unrelated edits, and `out` vanished silently.
"""

from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter


def _insn(mnemonic, op_str, operands, bytes_hex):
    return Instruction(0x1000, 1, mnemonic, op_str, bytes_hex, operands=operands)


def test_in_reads_the_port_into_the_destination_register():
    insn = _insn("in", "al, dx",
                 [Operand("reg", reg="al"), Operand("reg", reg="dx")], "ec")

    assert Lifter().lift_instruction(insn) == [
        "SET_LO8(eax, recomp_port_in((uint16_t)(LO16(edx)), 1)); /* in al, dx */",
    ]


def test_out_writes_the_source_register_to_the_port():
    insn = _insn("out", "dx, al",
                 [Operand("reg", reg="dx"), Operand("reg", reg="al")], "ee")

    assert Lifter().lift_instruction(insn) == [
        "recomp_port_out((uint16_t)(LO16(edx)), (uint32_t)(LO8(eax)), 1); "
        "/* out dx, al */",
    ]


def test_width_comes_from_the_data_operand_not_the_immediate_port():
    """An immediate port carries no width of its own, so a 4-byte access must
    still be lifted as one rather than defaulting off the port operand."""
    insn = _insn("in", "eax, 0x40",
                 [Operand("reg", reg="eax"), Operand("imm", imm=0x40)], "e540")

    assert Lifter().lift_instruction(insn) == [
        "eax = recomp_port_in((uint16_t)(0x40), 4); /* in eax, 0x40 */",
    ]


def test_out_accepts_the_short_immediate_port_form():
    insn = _insn("out", "0x80, al",
                 [Operand("imm", imm=0x80), Operand("reg", reg="al")], "e680")

    assert Lifter().lift_instruction(insn) == [
        "recomp_port_out((uint16_t)(0x80), (uint32_t)(LO8(eax)), 1); "
        "/* out 0x80, al */",
    ]


def test_port_io_preserves_flag_tracking():
    """IN/OUT move data and touch no flags. Left unclassified they would drop
    the translator's flag state, costing a fallback condition in any block
    that reads flags set before the access."""
    from tools.recomp.lifter import _EFLAGS_PRESERVE

    assert "in" in _EFLAGS_PRESERVE
    assert "out" in _EFLAGS_PRESERVE
