"""Flag consumers at a merge whose predecessors set the flags differently.

Regression guard for the translator giving up at such a merge and emitting
`if (_flags)` -- a variable nothing assigns, so the branch was never taken.
Breakdown's movie decoder joins its normal and escape paths that way at the
coefficient-sign `je` (a `test` on one path, a `cmp` on the other), and every
coefficient came out negated. Each predecessor now stores the condition it
would give the consumer, and the consumer reads that.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from . import config
from .translator import FunctionTranslator

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = 0x00010000
M32 = 0xFFFFFFFF

# Two paths that set ZF differently, joined at a je over `neg edi`:
#   eax >= ebx (unsigned): ZF from test ecx, ecx
#   eax <  ebx           : ZF from cmp esi, 5
SIGN_MERGE = bytes.fromhex(
    "39D8"      # 00 cmp eax, ebx
    "7206"      # 02 jb 0x0A
    "85C9"      # 04 test ecx, ecx
    "89D7"      # 06 mov edi, edx
    "EB05"      # 08 jmp 0x0F
    "83FE05"    # 0A cmp esi, 5
    "89D7"      # 0D mov edi, edx
    "7402"      # 0F je 0x13            <- the merge
    "F7DF"      # 11 neg edi
    "C3")       # 13 ret

# Same shape with the merge consumed by sete cl instead of a jump.
SETCC_MERGE = bytes.fromhex(
    "39D8" "7206" "85C9" "89D7" "EB05" "83FE05" "89D7"
    "0F94C1"    # 0F sete cl            <- the merge
    "C3")       # 12 ret

# The first path's flags come from `rol`, which gives no ZF condition: the
# merge must keep the old fallback rather than materialise half of it.
UNPLANNABLE_MERGE = bytes.fromhex(
    "39D8" "7206"
    "D1C1"      # 04 rol ecx, 1
    "89D7" "EB05" "83FE05" "89D7" "7402" "F7DF" "C3")


def _translate(code):
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-merge-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(code),
                 "_addr": BASE, "size": len(code)}}
    return FunctionTranslator(code, db).translate_function(BASE, db[BASE])


def _run(c_function, cases):
    """Compile one generated function against the runtime header and run it."""
    cc = shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("C compiler required")
    source = (
        "#define RECOMP_GENERATED_CODE\n"
        '#include "recomp_types.h"\n'
        "#include <stdio.h>\n"
        "RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;\n"
        "RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;\n"
        + c_function +
        "\nint main(void) {\n"
        "    unsigned a, b, c, d, s;\n"
        "    while (scanf(\"%u %u %u %u %u\", &a, &b, &c, &d, &s) == 5) {\n"
        "        g_eax = a; g_ebx = b; g_ecx = c; g_edx = d; g_esi = s;\n"
        "        g_esp = 0x1000;\n"
        "        sub_00010000();\n"
        "        printf(\"%u %u\\n\", g_edi, g_ecx);\n"
        "    }\n"
        "    return 0;\n"
        "}\n")
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "merge.c")
        exe = os.path.join(tmp, "merge")
        pathlib.Path(src).write_text(source)
        subprocess.run([cc, "-std=c11", "-I", str(ROOT / "templates" / "runtime"),
                        src, "-o", exe], check=True, capture_output=True, text=True)
        run = subprocess.run([exe], text=True, check=True, capture_output=True,
                             input="".join(" ".join(map(str, k)) + "\n" for k in cases))
    return [tuple(map(int, line.split())) for line in run.stdout.splitlines()]


def _zf(eax, ebx, ecx, esi):
    return esi == 5 if eax < ebx else ecx == 0


CASES = [  # eax, ebx, ecx, edx, esi
    (5, 1, 0, 7, 9), (5, 1, 3, 7, 9), (1, 5, 3, 7, 5), (1, 5, 0, 7, 9),
    (0, 0, 0, 0x80000000, 0), (0, 0xFFFFFFFF, 0, 42, 5), (0xFFFFFFFF, 0, 1, 42, 5),
]


class FlagMergeTest(unittest.TestCase):
    def test_disagreeing_predecessors_each_store_the_condition(self):
        c = _translate(SIGN_MERGE)
        self.assertNotIn("if (_flags", c)
        self.assertIn("int _mf_0001000F = 0;", c)
        self.assertIn("_mf_0001000F = (TEST_Z(_fa, _fb)) ? 1 : 0;", c)
        self.assertIn("_mf_0001000F = (CMP_EQ(_fa, _fb)) ? 1 : 0;", c)
        self.assertIn("if (_mf_0001000F) goto loc_00010013;", c)
        # The jumping predecessor stores it before it leaves.
        self.assertLess(c.index("_mf_0001000F = (TEST_Z"), c.index("goto loc_0001000F;"))

    def test_merged_je_executes_as_x86(self):
        results = _run(_translate(SIGN_MERGE), CASES)
        expected = [(edx if _zf(eax, ebx, ecx, esi) else (-edx) & M32, ecx)
                    for eax, ebx, ecx, edx, esi in CASES]
        self.assertEqual(results, expected)

    def test_merged_setcc_executes_as_x86(self):
        c = _translate(SETCC_MERGE)
        self.assertNotIn("_flags /* sete", c)
        self.assertIn("_mf_0001000F ? 1 : 0", c)
        results = _run(c, CASES)
        expected = [(edx, (ecx & ~0xFF) | int(_zf(eax, ebx, ecx, esi)))
                    for eax, ebx, ecx, edx, esi in CASES]
        self.assertEqual(results, expected)

    def test_merge_without_a_condition_for_every_path_keeps_the_fallback(self):
        c = _translate(UNPLANNABLE_MERGE)
        self.assertIn("if (_flags", c)
        self.assertNotIn("_mf_", c)


if __name__ == "__main__":
    unittest.main()
