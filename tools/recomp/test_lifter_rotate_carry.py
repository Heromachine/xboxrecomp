"""
Self-check for rcl/rcr and for 8/16-bit rol/ror.

Run: py -3 -m pytest tools/recomp/test_lifter_rotate_carry.py

rcl/rcr lifted to a `/* TODO */` comment, so they were dropped. Breakdown's
CRT 64-bit divide shifts a 64-bit value with `shr ecx, 1; rcr ebx, 1`: with
the rcr gone the low half never moved and every 64-bit division in the title
came out wrong. rol/ror on a byte or word also went through ROL32/ROR32,
which rotates the whole 32-bit register instead of the operand.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Instruction, Operand  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _reg(name):
    return Operand(type="reg", reg=name)


def _lift(mnemonic, op_str, dst, count):
    # The translator sets needs_cf for any function containing adc/sbb/rcl/rcr,
    # so CF is live wherever these appear.
    lifter = Lifter()
    lifter.needs_cf = True
    insn = Instruction(0, 2, mnemonic, op_str, "")
    insn.operands = [dst, Operand(type="imm", imm=count)]
    return "\n".join(lifter.lift_instruction(insn))


_PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r) ((uint8_t)(r))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r, v) ((r) = ((r) & ~0xFFu) | (uint8_t)(v))
#define SET_LO16(r, v) ((r) = ((r) & ~0xFFFFu) | (uint16_t)(v))
static inline uint32_t ROL32(uint32_t val, int n) { n &= 31; return n ? (val << n) | (val >> (32 - n)) : val; }
static inline uint32_t ROR32(uint32_t val, int n) { n &= 31; return n ? (val >> n) | (val << (32 - n)) : val; }
uint32_t eax, ebx, ecx, edx;
int _cf, _flags;
"""


def _find_cc():
    for cc in ("gcc", "clang", "cc"):
        if shutil.which(cc):
            return cc
    return None


def _run(body):
    src = _PRELUDE + "int main(void){" + body + "return 0;}"
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(c, "w").write(src)
        subprocess.check_call([_find_cc(), "-w", "-o", exe, c])
        return subprocess.check_output([exe]).decode().split()


@unittest.skipUnless(_find_cc(), "no C compiler")
class RotateCarryTest(unittest.TestCase):
    def test_rcr_shifts_carry_in_and_bit_out(self):
        # CF=1, 0x00000002 -> 0x80000001, CF=0
        out = _run("eax = 2; _cf = 1;" + _lift("rcr", "eax, 1", _reg("eax"), 1)
                   + 'printf("%08X %d\\n", eax, _cf);')
        self.assertEqual(out, ["80000001", "0"])

    def test_rcl_shifts_carry_in_and_bit_out(self):
        # CF=1, 0x80000000 -> 0x00000001, CF=1
        out = _run("eax = 0x80000000u; _cf = 1;" + _lift("rcl", "eax, 1", _reg("eax"), 1)
                   + 'printf("%08X %d\\n", eax, _cf);')
        self.assertEqual(out, ["00000001", "1"])

    def test_shr_rcr_pair_shifts_a_64_bit_value(self):
        # The _aulldiv shape: edx:eax >> 1 as `shr edx,1; rcr eax,1`.
        body = ("edx = 1; eax = 0;"
                + _lift("shr", "edx, 1", _reg("edx"), 1)
                + _lift("rcr", "eax, 1", _reg("eax"), 1)
                + 'printf("%08X%08X\\n", edx, eax);')
        self.assertEqual(_run(body), ["0000000080000000"])

    def test_rcr_count_zero_changes_nothing(self):
        out = _run("eax = 0x12345678u; _cf = 1;" + _lift("rcr", "eax, 0", _reg("eax"), 0)
                   + 'printf("%08X %d\\n", eax, _cf);')
        self.assertEqual(out, ["12345678", "1"])

    def test_rol_on_a_byte_stays_in_the_byte(self):
        out = _run("eax = 0xAABBCC81u;" + _lift("rol", "al, 1", _reg("al"), 1)
                   + 'printf("%08X\\n", eax);')
        self.assertEqual(out, ["AABBCC03"])

    def test_ror_on_a_word_stays_in_the_word(self):
        out = _run("eax = 0xAABB0001u;" + _lift("ror", "ax, 1", _reg("ax"), 1)
                   + 'printf("%08X\\n", eax);')
        self.assertEqual(out, ["AABB8000"])


def _insn(mnemonic, op_str, dst, count):
    insn = Instruction(0, 2, mnemonic, op_str, "")
    insn.operands = [dst, Operand(type="imm", imm=count)]
    return insn


if __name__ == "__main__":
    unittest.main()
