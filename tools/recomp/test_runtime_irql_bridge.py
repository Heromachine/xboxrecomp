"""Execute the KfRaiseIrql / KfLowerIrql bridges against the real HAL functions.

Regression guard: both exports are __fastcall (new IRQL in cl), but the
bridges read the level off the guest stack, so the IRQL became whatever the
caller had last pushed. The test compiles the actual bridge and HAL code with
the stack argument rigged to a value no caller passes, and checks that the
level each call reports and records is the one in ecx.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
STACK_GARBAGE = 0x1F    # what STACK_ARG(0) returns in the harness


def _extract(path, pattern):
    match = re.search(pattern, (ROOT / path).read_text(), re.M | re.S)
    if not match:
        raise AssertionError(f"not found in {path}: {pattern}")
    return match.group(0)


class IrqlBridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        hal = "src/kernel/kernel_hal.c"
        bridge = "src/kernel/kernel_bridge.c"
        pieces = [
            _extract(hal, r"^static XBOX_THREAD_LOCAL KIRQL g_current_irql = PASSIVE_LEVEL;"),
            _extract(hal, r"^KIRQL __fastcall xbox_KfRaiseIrql\(KIRQL NewIrql\)\n\{.*?^\}"),
            _extract(hal, r"^VOID __fastcall xbox_KfLowerIrql\(KIRQL NewIrql\)\n\{.*?^\}"),
            _extract(bridge, r"^static void bridge_KfRaiseIrql\(void\)\n\{.*?^\}"),
            _extract(bridge, r"^static void bridge_KfLowerIrql\(void\)\n\{.*?^\}"),
        ]
        harness = r"""
#include <stdint.h>
#include <stdio.h>
typedef unsigned char UCHAR;
typedef UCHAR KIRQL;
#define VOID void
#define __fastcall
#define XBOX_THREAD_LOCAL _Thread_local
#define PASSIVE_LEVEL 0
#define XBOX_LOG_WARN 0
#define XBOX_LOG_HAL 0
#define xbox_log(...) ((void)0)
static uint32_t g_eax, g_ecx;
""" + f"#define STACK_ARG(n) ((uint32_t){STACK_GARBAGE:#x})\n" + "\n".join(pieces) + r"""
int main(void) {
    char op; unsigned level;
    while (scanf(" %c %u", &op, &level) == 2) {
        g_ecx = 0xABCD0000u | level;   /* only cl carries the level */
        if (op == 'r') { bridge_KfRaiseIrql(); printf("%u\n", g_eax); }
        else           { bridge_KfLowerIrql(); printf("-\n"); }
    }
    return 0;
}
"""
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = pathlib.Path(cls.tmp.name)
        (path / "irql.c").write_text(harness)
        cls.binary = path / "irql"
        subprocess.run([cc, "-std=c11", "-Wall", str(path / "irql.c"), "-o",
                        str(cls.binary)], check=True)

    def run_ops(self, ops):
        run = subprocess.run([str(self.binary)], text=True, check=True, capture_output=True,
                             input="".join(f"{op} {level}\n" for op, level in ops))
        return run.stdout.split()

    def test_raise_and_lower_take_the_level_from_ecx(self):
        # raise(2) from PASSIVE, raise(5) reports 2, lower(2), raise(2) reports 2,
        # lower(0), raise(1) reports 0. Reading the stack would report 0x1F.
        out = self.run_ops([("r", 2), ("r", 5), ("l", 2), ("r", 2), ("l", 0), ("r", 1)])
        self.assertEqual(out, ["0", "2", "-", "2", "-", "0"])


if __name__ == "__main__":
    unittest.main()
