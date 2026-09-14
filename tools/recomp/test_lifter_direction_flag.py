"""
Self-check for the direction flag on string instructions.

Run: py -3 -m pytest tools/recomp/test_lifter_direction_flag.py

The CRT's memmove copies an overlapping block whose destination is above its
source backward: `std; rep movsd; cld`, with esi/edi pointing at the LAST
element. The lifter used to drop std, so the copy ran forward from those end
pointers -- ecx*4 bytes past the destination -- and on Breakdown the first
menu that shuffled a list smeared text ("EVEN", "HIT_") over live objects,
which later surfaced as calls through ASCII vtables.

These lift a backward memmove and a forward one, compile the output against a
flat byte array, and compare the result with what the x86 would have done.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Instruction  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _lift(mnemonic, op_str=""):
    return "\n".join(Lifter().lift_instruction(
        Instruction(0, 1, mnemonic, op_str, "")))


_PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t mem[256];
#define XBOX_PTR(a) (mem + (uint32_t)(a))
#define MEM8(a)  (*(uint8_t  *)XBOX_PTR(a))
#define MEM16(a) (*(uint16_t *)XBOX_PTR(a))
#define MEM32(a) (*(uint32_t *)XBOX_PTR(a))
#define LO8(r) ((uint8_t)(r))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r, v) ((r) = ((r) & ~0xFFu) | (uint8_t)(v))
int g_df;
#define RECOMP_DF_STEP(n) (g_df ? (uint32_t)0 - (uint32_t)(n) : (uint32_t)(n))
uint32_t eax, ecx, esi, edi;
int _flags;
"""


def _find_cc():
    for cc in ("gcc", "clang", "cc"):
        if shutil.which(cc):
            return cc
    return None


class DirectionFlagTest(unittest.TestCase):
    def test_std_and_cld_set_the_runtime_flag(self):
        self.assertIn("g_df = 1;", _lift("std"))
        self.assertIn("g_df = 0;", _lift("cld"))

    def test_every_rep_form_consults_the_flag(self):
        for m in ("rep movsb", "rep movsw", "rep movsd", "rep stosb",
                  "rep stosw", "rep stosd", "repe cmpsb", "repne scasb"):
            text = _lift(m)
            self.assertTrue("g_df" in text or "RECOMP_DF_STEP" in text,
                            f"{m}: {text}")

    @unittest.skipUnless(_find_cc(), "no C compiler")
    def test_backward_overlapping_movsd_matches_x86(self):
        # memmove(mem+12, mem+4, 16) done the CRT way: pointers at the last
        # dword, std, rep movsd, cld.
        body = "\n".join([
            "for (int i = 0; i < 64; i++) mem[i] = (uint8_t)i;",
            "esi = 16; edi = 24; ecx = 4;",
            _lift("std"),
            _lift("rep movsd", "dword ptr es:[edi], dword ptr [esi]"),
            _lift("cld"),
            # A forward stosb after cld must go forward again.
            "eax = 0xAA; edi = 40; ecx = 2;",
            _lift("rep stosb", "byte ptr es:[edi], al"),
            "for (int i = 0; i < 48; i++) printf(\"%d \", mem[i]);",
            "printf(\"| %u %u\\n\", esi, edi);",
        ])
        out = self._run(_PRELUDE + "int main(void) {\n" + body
                        + "\nreturn 0; }\n")

        want = list(range(64))
        want[12:28] = list(range(4, 20))      # memmove semantics
        want[40:42] = [0xAA, 0xAA]
        # esi/edi end one dword below the block; edi then reloaded to 40 + 2.
        self.assertEqual(out, " ".join(map(str, want[:48])) + " | 0 42")

    def _run(self, source):
        cc = _find_cc()
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "df.c")
            exe = os.path.join(tmp, "df")
            with open(src, "w") as f:
                f.write(source)
            subprocess.run([cc, "-std=c99", "-O1", "-o", exe, src],
                           check=True, capture_output=True)
            return subprocess.run([exe], check=True, capture_output=True,
                                  text=True).stdout.strip()


if __name__ == "__main__":
    unittest.main()
