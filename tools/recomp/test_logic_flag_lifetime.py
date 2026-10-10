"""A logical op's flags survive register writes by MOV, LEA, and PUSH.

Burnout's traffic LOD branch at 0x9761E uses ZF from AND EAX,1 even though
MOV EAX,[ESI+0x268] replaced EAX before JZ. Re-reading EAX at JZ made the
branch choose low-detail setup for high-detail cars.
"""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

from . import config
from .translator import FunctionTranslator

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = 0x10000


def _translate(code):
    config._install(
        [config.Section(".text", BASE, len(code), 0, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="logic-flag-lifetime-test")
    info = {"start": f"0x{BASE:08X}", "end": BASE + len(code),
            "_addr": BASE, "size": len(code)}
    return FunctionTranslator(code, {BASE: info}).translate_function(BASE, info)


def _run(c_function, values):
    cc = shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("C compiler required")
    source = (
        "#define RECOMP_GENERATED_CODE\n"
        '#include "recomp_types.h"\n'
        "#include <stdio.h>\n"
        "RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esp;\n"
        "RECOMP_TLS uint32_t g_esi, g_edi;\n"
        + c_function +
        "\nint main(void) {\n"
        "    unsigned x;\n"
        "    while (scanf(\"%u\", &x) == 1) {\n"
        "        g_eax = x; g_ecx = 99; g_esp = 0x1000;\n"
        "        sub_00010000();\n"
        "        printf(\"%u\\n\", g_ecx);\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
    )
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "flags.c"
        exe = pathlib.Path(tmp) / "flags"
        src.write_text(source)
        compile_result = subprocess.run(
            [cc, "-std=c11", "-I", str(ROOT / "templates" / "runtime"),
             str(src), "-o", str(exe)], capture_output=True, text=True)
        if compile_result.returncode:
            raise AssertionError(compile_result.stderr)
        run = subprocess.run([str(exe)], check=True, capture_output=True, text=True,
                             input="".join(f"{x}\n" for x in values))
    return [int(line) for line in run.stdout.splitlines()]


class LogicFlagLifetimeTest(unittest.TestCase):
    def test_and_zf_survives_mov_to_same_register(self):
        # and eax,1; mov ebx,eax; mov eax,0x1234; jz ret; mov ecx,7; ret
        code = bytes.fromhex("83E001 89C3 B834120000 7405 B907000000 C3")
        c = _translate(code)
        self.assertIn("_fa = (uint32_t)(eax) & 0xFFFFFFFFu;", c)
        self.assertIn("if ((_fa == 0)) goto", c)
        self.assertEqual(_run(c, [0, 1, 2, 3]), [99, 7, 99, 7])

    def test_and_al_sf_keeps_8bit_width_after_mov(self):
        # and al,0x80; mov eax,0; jns ret; mov ecx,7; ret
        code = bytes.fromhex("2480 B800000000 7905 B907000000 C3")
        c = _translate(code)
        self.assertIn("_fas = (int32_t)(int8_t)(_fa)", c)
        self.assertEqual(_run(c, [0, 0x80, 0x180]), [99, 7, 7])


if __name__ == "__main__":
    unittest.main()
