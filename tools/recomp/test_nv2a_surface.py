"""Compile src/nv2a/nv2a_surface.c and check render-target surface geometry.

Breakdown renders its frame, bloom and shadow maps into guest-RAM surfaces and
samples them back as textures, including a 640x448 texture that starts 16 rows
into the 640x480 frame surface. These checks pin the sizing and lookup to xemu
(pgraph/gl/surface.c populate_surface_binding_entry, surface_get_within, and
pgraph/gl/draw.c's inclusive clear rectangle).
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "nv2a_surface.h"

int main(void) {
    char op[16];
    while (scanf(" %15s", op) == 1) {
        if (!strcmp(op, "dims")) {
            unsigned f, h, v, w = 0, hh = 0;
            scanf("%x %x %x", &f, &h, &v);
            nv2a_surf_dims(f, h, v, &w, &hh);
            printf("%u %u\n", w, hh);
        } else if (!strcmp(op, "bpp")) {
            unsigned c, z;
            scanf("%x %x", &c, &z);
            printf("%u %u\n", nv2a_surf_color_bpp(c), nv2a_surf_zeta_bpp(z));
        } else if (!strcmp(op, "loc")) {
            NV2ASurfDesc s; unsigned a, w, h, x = 0, y = 0;
            scanf("%x %u %u %u %u %x %u %u", &s.addr, &s.width, &s.height,
                  &s.pitch, &s.bpp, &a, &w, &h);
            int r = nv2a_surf_locate(&s, a, w, h, &x, &y);
            printf("%d %u %u\n", r, x, y);
        } else if (!strcmp(op, "clr")) {
            unsigned h, v, w, hh, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            scanf("%x %x %u %u", &h, &v, &w, &hh);
            int r = nv2a_surf_clear_rect(h, v, w, hh, &x0, &y0, &x1, &y1);
            printf("%d %u %u %u %u\n", r, x0, y0, x1, y1);
        }
    }
    return 0;
}
"""


def _compiler():
    for cc in ("cc", "gcc", "clang"):
        if shutil.which(cc):
            return cc
    return None


class SurfaceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = _compiler()
        if cc is None:
            raise unittest.SkipTest("no C compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = pathlib.Path(cls.tmp.name)
        (tmp / "driver.c").write_text(DRIVER)
        cls.exe = tmp / "driver"
        subprocess.run(
            [cc, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "src/nv2a"),
             str(tmp / "driver.c"), str(ROOT / "src/nv2a/nv2a_surface.c"),
             "-o", str(cls.exe)],
            check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_ops(self, text):
        out = subprocess.run([str(self.exe)], input=text, capture_output=True,
                             text=True, check=True).stdout
        return out.split("\n")[:-1]

    def test_pitch_surface_sized_by_clip_including_origin(self):
        # Breakdown's frame: format 0x128, clip 640 wide, 448 high at y=16.
        self.assertEqual(self.run_ops("dims 128 02800000 01C00010\n"),
                         ["640 464"])
        self.assertEqual(self.run_ops("dims 128 02800000 01E00000\n"),
                         ["640 480"])

    def test_swizzle_surface_sized_by_format_log2(self):
        # Shadow maps: 0x09090213 is 512x512, 0x08080213 is 256x256.
        self.assertEqual(self.run_ops("dims 09090213 02000000 02000000\n"),
                         ["512 512"])
        self.assertEqual(self.run_ops("dims 08080228 01000000 00800000\n"),
                         ["256 256"])

    def test_bytes_per_texel(self):
        self.assertEqual(self.run_ops("bpp 8 2\nbpp 3 1\nbpp 9 0\nbpp 0 3\n"),
                         ["4 4", "2 2", "1 0", "0 0"])

    def test_texture_inside_frame_surface(self):
        # 0x03AE6000 is 16 rows (16*2560 bytes) into 0x03ADC000.
        out = self.run_ops("loc 03ADC000 640 480 2560 4 03AE6000 640 448\n")
        self.assertEqual(out, ["1 0 16"])

    def test_exact_match(self):
        out = self.run_ops("loc 021EA000 256 128 1024 4 021EA000 256 128\n")
        self.assertEqual(out, ["1 0 0"])

    def test_rejects_overhang_misalignment_and_before(self):
        out = self.run_ops(
            "loc 03ADC000 640 480 2560 4 03AE6000 640 465\n"   # too tall
            "loc 03ADC000 640 480 2560 4 03AE6002 16 16\n"     # mid-texel
            "loc 03ADC000 640 480 2560 4 03ADB000 16 16\n"     # before start
            "loc 03ADC000 640 480 2560 4 03ADC9F0 16 1\n"      # x=636, 16 wide
            "loc 03ADC000 640 480 0 4 03ADC000 16 16\n")       # no pitch
        self.assertEqual([o.split()[0] for o in out], ["0"] * 5)

    def test_clear_rect_is_inclusive_and_clamped(self):
        # Breakdown's letterbox clear: x 0..639, y 16..463.
        out = self.run_ops("clr 027F0000 01CF0010 640 480\n"
                           "clr 0FFF0000 0FFF0000 256 128\n"
                           "clr 00000300 0 256 128\n")
        self.assertEqual(out, ["1 0 16 640 464", "1 0 0 256 128",
                               "0 0 0 0 0"])


if __name__ == "__main__":
    unittest.main()
