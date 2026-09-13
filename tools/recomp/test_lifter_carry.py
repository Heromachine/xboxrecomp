import pathlib
import shutil
import subprocess
import tempfile
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class CarryLifterTest(unittest.TestCase):
    def test_neg_carry_feeds_adjacent_sbb(self):
        neg = Instruction(0, 2, "neg", "esi", "f7de")
        neg.operands = [Operand(type="reg", reg="esi")]
        sbb = Instruction(2, 2, "sbb", "esi, esi", "19f6")
        sbb.operands = [
            Operand(type="reg", reg="esi"),
            Operand(type="reg", reg="esi"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, sbb]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)((esi) != 0);", generated)
        self.assertIn(
            "esi = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */",
            generated,
        )

    def test_neg_carry_feeds_adjacent_adc(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        adc = Instruction(2, 3, "adc", "edx, 0", "83d200")
        adc.operands = [
            Operand(type="reg", reg="edx"),
            Operand(type="imm", imm=0),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, adc]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)((eax) != 0);", generated)
        self.assertIn("+ (uint64_t)_cf;", generated)
        self.assertIn("edx = (uint32_t)_t;", generated)

    def test_neg_carry_feeds_sbb_across_push(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        push = Instruction(2, 1, "push", "edi", "57")
        push.operands = [Operand(type="reg", reg="edi")]
        sbb = Instruction(3, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, push, sbb]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)((eax) != 0);", generated)
        self.assertIn(
            "eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */",
            generated,
        )

    def test_neg_carry_not_preserved_across_flag_setter(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        add = Instruction(2, 3, "add", "ecx, 1", "83c101")
        add.operands = [
            Operand(type="reg", reg="ecx"),
            Operand(type="imm", imm=1),
        ]
        sbb = Instruction(5, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, add, sbb]))
        generated = "\n".join(lifted)

        self.assertNotIn("_cf = (int)((eax) != 0);", generated)

    def test_neg_carry_not_preserved_across_branch(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        jmp = Instruction(2, 2, "jmp", "0x10", "eb0c")
        jmp.jump_target = 0x10
        sbb = Instruction(4, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, jmp, sbb]))
        generated = "\n".join(lifted)

        self.assertNotIn("_cf = (int)((eax) != 0);", generated)

    def test_neg_carry_not_preserved_across_popfd(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        popfd = Instruction(2, 1, "popfd", "", "9d")
        sbb = Instruction(3, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, popfd, sbb]))
        generated = "\n".join(lifted)

        self.assertNotIn("_cf = (int)((eax) != 0);", generated)


def _reg(name):
    return Operand(type="reg", reg=name)


def _cmp_sbb_block(lhs, rhs):
    """cmp lhs, rhs / mov ecx, edx / sbb ebx, ebx / inc ebx.

    The shape Breakdown's movie decoder uses for its end-of-block flag
    (ebx = lhs >= rhs), with a flag-preserving instruction in between.
    """
    cmp = Instruction(0, 2, "cmp", f"{lhs}, {rhs}", "39d0")
    cmp.operands = [_reg(lhs), _reg(rhs)]
    mov = Instruction(2, 2, "mov", "ecx, edx", "89d1")
    mov.operands = [_reg("ecx"), _reg("edx")]
    sbb = Instruction(4, 2, "sbb", "ebx, ebx", "19db")
    sbb.operands = [_reg("ebx"), _reg("ebx")]
    inc = Instruction(6, 1, "inc", "ebx", "43")
    inc.operands = [_reg("ebx")]
    lifter = Lifter()
    lifter.needs_cf = True   # the translator sets this for any function with sbb
    lifted, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=[cmp, mov, sbb, inc]))
    return "\n".join(lifted)


class CmpCarryLifterTest(unittest.TestCase):
    def test_cmp_carry_feeds_sbb_across_mov(self):
        generated = _cmp_sbb_block("eax", "edx")
        self.assertIn("_cf = (int)(_fa < _fb);", generated)
        self.assertLess(generated.index("_cf = (int)(_fa < _fb);"),
                        generated.index("ebx = _cf ? 0xFFFFFFFF : 0;"))

    def test_cmp_sbb_executes_as_x86(self):
        """Compile the lifted block and check it against x86 borrow semantics.

        _cf starts at both 0 and 1 to stand in for a carry left behind by
        earlier code: the result must not depend on it.
        """
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("C compiler required")
        cases = [(0, 0), (0, 1), (1, 0), (5, 5), (0x7FFFFFFF, 0x80000000),
                 (0xFFFFFFFF, 0), (0, 0xFFFFFFFF), (0x100, 0x001), (0x1FF, 0x2FE)]
        for lhs, rhs, width in (("eax", "edx", 32), ("al", "dl", 8)):
            body = _cmp_sbb_block(lhs, rhs)
            source = (
                "#include <stdint.h>\n#include <stdio.h>\n"
                "#define LO8(r) ((uint8_t)((r) & 0xFF))\n"
                "int main(void) {\n"
                "    uint32_t eax, ebx, ecx, edx, _fa, _fb; int32_t _fas, _fbs; int _cf, stale;\n"
                "    while (scanf(\"%u %u %d\", &eax, &edx, &stale) == 3) {\n"
                "        _cf = stale; ebx = 0x5A5A5A5A;\n"
                f"{body}\n"
                "        (void)ecx; (void)_fas; (void)_fbs;\n"
                "        printf(\"%u\\n\", ebx);\n"
                "    }\n    return 0;\n}\n")
            with tempfile.TemporaryDirectory() as tmp:
                src = pathlib.Path(tmp) / "cmp_sbb.c"
                exe = pathlib.Path(tmp) / "cmp_sbb"
                src.write_text(source)
                subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(src), "-o", str(exe)],
                               check=True)
                feed = "".join(f"{a} {b} {s}\n" for a, b in cases for s in (0, 1))
                run = subprocess.run([str(exe)], input=feed, text=True,
                                     capture_output=True, check=True)
            mask = (1 << width) - 1
            expected = [0 if (a & mask) < (b & mask) else 1 for a, b in cases for _ in (0, 1)]
            self.assertEqual([int(x) for x in run.stdout.split()], expected,
                             f"{width}-bit cmp/sbb")


if __name__ == "__main__":
    unittest.main()
