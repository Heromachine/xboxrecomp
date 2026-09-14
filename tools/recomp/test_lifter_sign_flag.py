"""Regression tests for operand-width-correct SF consumers."""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _lift_sign_branch(setter, reg, branch):
    op = Instruction(0, 2, setter, reg, "")
    op.operands = [Operand(type="reg", reg=reg)]
    jump = Instruction(2, 2, branch, "0x10", "")
    jump.jump_target = 0x10
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[op, jump]))
    return "\n".join(lifted)


class SignFlagWidthTest(unittest.TestCase):
    def test_dec_al_js_tests_bit_7(self):
        generated = _lift_sign_branch("dec", "al", "js")
        self.assertIn("((int8_t)(LO8(eax)) < 0)", generated)
        self.assertNotIn("(int32_t)LO8(eax)", generated)

    def test_dec_ax_jns_tests_bit_15(self):
        generated = _lift_sign_branch("dec", "ax", "jns")
        self.assertIn("((int16_t)(LO16(eax)) >= 0)", generated)

    def test_dec_eax_js_tests_bit_31(self):
        generated = _lift_sign_branch("dec", "eax", "js")
        self.assertIn("((int32_t)(eax) < 0)", generated)

    def test_narrow_add_js_uses_the_same_width_rule(self):
        add = Instruction(0, 2, "add", "cl, dl", "")
        add.operands = [Operand(type="reg", reg="cl"),
                        Operand(type="reg", reg="dl")]
        jump = Instruction(2, 2, "js", "0x10", "")
        jump.jump_target = 0x10
        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[add, jump]))
        self.assertIn("((int8_t)(LO8(ecx)) < 0)", "\n".join(lifted))


if __name__ == "__main__":
    unittest.main()
