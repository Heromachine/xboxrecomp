"""Compile src/input/pad_script.c and check XBOXRECOMP_PAD_SCRIPT parsing.

The script is how an automated run presses a controller button -- Start on a
title screen, say -- so a mistake here presses the wrong button, or none,
without saying so. These pin the grammar ("SECONDS=BUTTONS", separated by ';'
or ','), the button bits and analog slots each name sets, ordering, and
rejection of anything malformed.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <stdio.h>
#include "pad_script.h"
/* argv[1] = script; stdin = times. Prints the parse result, then for each time
 * "buttons a0 a1 a2 a3 a4 a5 a6 a7". */
int main(int argc, char **argv) {
    XboxPadScript s;
    double t;
    int n = xbox_pad_script_parse(argc > 1 ? argv[1] : NULL, &s);
    printf("%d\n", n);
    while (scanf("%lf", &t) == 1) {
        XboxPadHeld h = xbox_pad_script_at(&s, t);
        printf("%04x", h.buttons);
        for (int i = 0; i < 8; i++) printf(" %d", h.analog[i]);
        printf("\n");
    }
    return 0;
}
"""

START, BACK, UP, DOWN, LEFT, RIGHT, LTHUMB, RTHUMB = (
    0x10, 0x20, 0x01, 0x02, 0x04, 0x08, 0x40, 0x80)
ANALOG = {"A": 0, "B": 1, "X": 2, "Y": 3, "BLACK": 4, "WHITE": 5, "LT": 6, "RT": 7}


class PadScriptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        cls.tmp = tempfile.TemporaryDirectory()
        src = pathlib.Path(cls.tmp.name) / "driver.c"
        src.write_text(DRIVER)
        cls.exe = pathlib.Path(cls.tmp.name) / "driver"
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src/input"), str(src),
                        str(ROOT / "src/input/pad_script.c"), "-o", str(cls.exe)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_script(self, script, times=()):
        args = [str(self.exe)] + ([] if script is None else [script])
        out = subprocess.run(args, input="\n".join(map(str, times)) + "\n",
                             capture_output=True, text=True, check=True).stdout.split("\n")
        held = []
        for line in out[1:1 + len(times)]:
            fields = line.split()
            held.append((int(fields[0], 16), [int(v) for v in fields[1:]]))
        return int(out[0]), held

    def test_press_and_release_start(self):
        n, held = self.run_script("135=START;135.2=", [0, 134.99, 135, 135.1, 135.2, 500])
        self.assertEqual(n, 2)
        self.assertEqual([h[0] for h in held], [0, 0, START, START, 0, 0])

    def test_every_name_sets_its_bit_or_analog_slot(self):
        digital = {"START": START, "BACK": BACK, "UP": UP, "DOWN": DOWN,
                   "LEFT": LEFT, "RIGHT": RIGHT, "LTHUMB": LTHUMB, "RTHUMB": RTHUMB}
        for name, bit in digital.items():
            _, held = self.run_script(f"1={name}", [1])
            self.assertEqual(held[0], (bit, [0] * 8), name)
        for name, slot in ANALOG.items():
            _, held = self.run_script(f"1={name.lower()}", [1])   # any case
            want = [0] * 8
            want[slot] = 255
            self.assertEqual(held[0], (0, want), name)

    def test_combinations_commas_and_spaces(self):
        n, held = self.run_script(" 2 = a + START , 3=  ", [2.5, 3])
        self.assertEqual(n, 2)
        self.assertEqual(held[0], (START, [255, 0, 0, 0, 0, 0, 0, 0]))
        self.assertEqual(held[1], (0, [0] * 8))

    def test_events_apply_in_time_order_whatever_the_order_written(self):
        _, held = self.run_script("10=B;5=A;7=", [5, 7, 10])
        self.assertEqual(held[0][1][ANALOG["A"]], 255)
        self.assertEqual(held[1], (0, [0] * 8))
        self.assertEqual(held[2][1][ANALOG["B"]], 255)

    def test_malformed_scripts_are_rejected_whole(self):
        for bad in ("135=START;x=A", "135=JUMP", "=START", "135", "135=A++B",
                    "135=A+", "-1=A", "1.5.2=A"):
            n, held = self.run_script(bad, [200])
            self.assertEqual(n, -1, bad)
            self.assertEqual(held[0], (0, [0] * 8), bad)

    def test_empty_and_missing_scripts_hold_nothing(self):
        for script in ("", " ; ;", None):
            n, held = self.run_script(script, [0, 100])
            self.assertEqual(n, 0, repr(script))
            self.assertEqual(held, [(0, [0] * 8)] * 2)

    def test_too_many_events_is_an_error(self):
        script = ";".join(f"{i}=A" for i in range(65))
        n, _ = self.run_script(script, [])
        self.assertEqual(n, -1)
        n, _ = self.run_script(";".join(f"{i}=A" for i in range(64)), [])
        self.assertEqual(n, 64)


if __name__ == "__main__":
    unittest.main()
