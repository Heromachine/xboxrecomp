"""
Self-check for sar on 8- and 16-bit operands.

Run: py -3 -m pytest tools/recomp/test_lifter_sar_width.py

Breakdown's pad layer scales each stick axis with `sar ax, 7`. The lifter cast
the zero-extended LO16 value to int32_t before shifting, so 0x8000 became
0x0100 rather than 0xFF00: every negative axis read positive, and the player
could not turn left or look down.
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


def _lift(op_str, code, dst, count):
    insn = Instruction(0, len(code) // 2, "sar", op_str, code)
    insn.operands = [dst, Operand(type="imm", imm=count)]
    return "\n".join(Lifter().lift_instruction(insn))


def _reg(name, _size):
    return Operand(type="reg", reg=name)


_PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
static uint8_t mem[256];
#define MEM8(a)  (*(uint8_t  *)(mem + (uint32_t)(a)))
#define MEM16(a) (*(uint16_t *)(mem + (uint32_t)(a)))
#define MEM32(a) (*(uint32_t *)(mem + (uint32_t)(a)))
#define LO8(r) ((uint8_t)(r))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r, v) ((r) = ((r) & ~0xFFu) | (uint8_t)(v))
#define SET_LO16(r, v) ((r) = ((r) & ~0xFFFFu) | (uint16_t)(v))
uint32_t eax, ecx, edx, esi, edi;
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
class SarWidthTest(unittest.TestCase):
    def test_sar_ax_keeps_the_sign(self):
        out = _run("eax = 0x12348000u;" + _lift("ax, 7", "66c1f807", _reg("ax", 2), 7)
                   + 'printf("%08X\\n", eax);')
        self.assertEqual(out, ["1234FF00"])

    def test_sar_al_keeps_the_sign(self):
        out = _run("eax = 0x55AA0080u;" + _lift("al, 1", "d0f8", _reg("al", 1), 1)
                   + 'printf("%08X\\n", eax);')
        self.assertEqual(out, ["55AA00C0"])

    def test_sar_word_memory_keeps_the_sign(self):
        out = _run("MEM16(8) = 0xFF00;" + _lift("word ptr [8], 4", "66c13d0800000004",
                        Operand(type="mem", mem_disp=8, mem_size=2), 4)
                   + 'printf("%04X\\n", MEM16(8));')
        self.assertEqual(out, ["FFF0"])

    def test_sar_eax_unchanged(self):
        out = _run("eax = 0x80000000u;" + _lift("eax, 4", "c1f804", _reg("eax", 4), 4)
                   + 'printf("%08X\\n", eax);')
        self.assertEqual(out, ["F8000000"])


if __name__ == "__main__":
    unittest.main()
