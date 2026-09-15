"""Compile src/nv2a/nv2a_psh.c and check the HLSL it generates.

The D3D11 backend used one fixed "texture times diffuse" pixel shader for
every draw, so Breakdown's shadow pass (depth-texture compares feeding a
multiplicative blend) and its full-screen composites came out as solid
black. nv2a_psh.c ports xemu's register-combiner generator
(pgraph/glsl/psh.c); these checks pin the register decode to it. The HLSL is
compiled for real at run time -- here only its structure is checked.
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
#include "nv2a_psh.h"

int main(int argc, char **argv) {
    NV2APshState s;
    static char buf[65536];
    unsigned v[32];
    int i, n = 0;
    memset(&s, 0, sizeof(s));
    while (n < 32 && scanf("%x", &v[n]) == 1) n++;
    /* control stageprog final0 final1 rgbin0 rgbout0 ain0 aout0
       rect0 shadow0 func alphatest alphafunc ... */
    s.combiner_control = v[0]; s.shader_stage_program = v[1];
    s.final_inputs_0 = v[2]; s.final_inputs_1 = v[3];
    s.rgb_inputs[0] = v[4]; s.rgb_outputs[0] = v[5];
    s.alpha_inputs[0] = v[6]; s.alpha_outputs[0] = v[7];
    for (i = 0; i < 4; i++) {
        s.rect_tex[i] = (v[8] >> i) & 1;
        s.shadow_map[i] = (v[9] >> i) & 1;
    }
    s.shadow_depth_func = v[10];
    s.alpha_test = v[11]; s.alpha_func = v[12];
    s.rgb_inputs[1] = v[13]; s.rgb_outputs[1] = v[14];
    s.z_perspective = v[15];
    if (argc > 1 && !strcmp(argv[1], "tiny")) {
        printf("%d\n", nv2a_psh_generate(&s, buf, 64));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "fmt")) {
        printf("%d %d %d %.1f %.1f %.1f\n", nv2a_psh_tex_is_linear(0x12),
               nv2a_psh_tex_is_linear(0x06), nv2a_psh_tex_is_linear(0x0C),
               nv2a_psh_tex_depth_max(0x2C), nv2a_psh_tex_depth_max(0x2E),
               nv2a_psh_tex_depth_max(0x12));
        return 0;
    }
    if (nv2a_psh_generate(&s, buf, sizeof(buf)) < 0) { puts("OVERFLOW"); return 1; }
    fputs(buf, stdout);
    return 0;
}
"""

# D3D's default: one stage, R0 = T0 * V0, final colour D = R0, alpha G = R0.
DEFAULT = ["1", "1", "c", "1c00", "08040000", "c00", "18140000", "c00",
           "0", "0", "0", "0", "0", "0", "0", "0"]


def _compiler():
    for cc in ("cc", "gcc", "clang"):
        if shutil.which(cc):
            return cc
    return None


class PshTest(unittest.TestCase):
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
             str(tmp / "driver.c"), str(ROOT / "src/nv2a/nv2a_psh.c"),
             "-o", str(cls.exe)],
            check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def gen(self, fields, *args):
        return subprocess.run([str(self.exe), *args], input=" ".join(fields),
                              capture_output=True, text=True).stdout

    def test_default_modulate(self):
        out = self.gen(DEFAULT)
        self.assertIn("float4 t0 = texSamp0.Sample(samp0, (pT0.xy / pT0.w));", out)
        self.assertIn("mux_sum.rgb = clamp((float3)(((max(t0.rgb, 0.0) * max(v0.rgb, 0.0))"
                      " + (max(float4(0.0, 0.0, 0.0, 0.0).rgb, 0.0) *"
                      " max(float4(0.0, 0.0, 0.0, 0.0).rgb, 0.0)))), -1.0, 1.0);", out)
        self.assertIn("r0.rgb = mux_sum.rgb;", out)
        self.assertIn("r0.a = mux_sum.a;", out)
        self.assertIn("fragColor.rgb = max(r0.rgb, 0.0) + lerp(", out)
        self.assertIn("fragColor.a = max(r0.a, 0.0);", out)
        # r0 starts with t0's alpha, and is declared before any stage uses it.
        self.assertLess(out.index("float4 r0 = float4(0.0, 0.0, 0.0, t0.a);"),
                        out.index("// Stage 0"))

    def test_linear_texture_is_normalised_by_its_size(self):
        f = list(DEFAULT)
        f[8] = "1"
        out = self.gen(f)
        self.assertIn("texSamp0.GetDimensions(texSize0.x, texSize0.y);", out)
        self.assertIn("Sample(samp0, ((pT0.xy / pT0.w) / texSize0))", out)

    def test_shadow_map_compare_in_project3d(self):
        f = list(DEFAULT)
        f[1] = "2"          # stage 0 PROJECT3D
        f[9] = "1"          # stage 0 holds depth
        f[10] = "3"         # LEQUAL
        out = self.gen(f)
        self.assertIn("float t0_depth = texSamp0.Sample(samp0, (pT0.xy / pT0.w)).r"
                      " * depthMax[0];", out)
        self.assertIn("float t0_ref = clamp(pT0.z / pT0.w, 0.0, depthMax[0]);", out)
        self.assertIn("float4 t0 = (float4)(t0_depth <= t0_ref ? 1.0 : 0.0);", out)

    def test_shadow_func_never_and_always(self):
        f = list(DEFAULT)
        f[1], f[9] = "2", "1"
        f[10] = "0"
        self.assertIn("float4 t0 = float4(0.0, 0.0, 0.0, 0.0);", self.gen(f))
        f[10] = "7"
        self.assertIn("float4 t0 = float4(1.0, 1.0, 1.0, 1.0);", self.gen(f))

    def test_output_decode_matches_xemu(self):
        # xemu: cd = low nibble, ab = next, muxsum = next, flags from bit 12.
        f = list(DEFAULT)
        f[5] = "1bc0"       # cd discarded, ab -> R0, muxsum -> T3, CD dot
        out = self.gen(f)
        # ab nibble 0xC -> r0, muxsum nibble 0xB -> t3, flags 1 -> CD dot product.
        self.assertIn("r0.rgb = ab.rgb;", out)
        self.assertIn("t3.rgb = mux_sum.rgb;", out)
        self.assertIn("dot(max(float4(0.0, 0.0, 0.0, 0.0).rgb, 0.0),", out)

    def test_unique_constants_and_final_constants(self):
        f = list(DEFAULT)
        f[0] = "11001"      # 1 stage, unique c0 (0x10 << 8) and c1 (0x100 << 8)
        f[4] = "01020000"   # A = C0, B = C1
        f[2] = "0102000c"   # final A = C0, B = C1
        out = self.gen(f)
        self.assertIn("max(consts[0].rgb, 0.0) * max(consts[1].rgb, 0.0)", out)
        self.assertIn("(float3)(max(consts[17].rgb, 0.0)), (float3)(max(consts[16].rgb, 0.0)))",
                      out)
        self.assertNotIn("// Stage 1", out)

    def test_alpha_test(self):
        f = list(DEFAULT)
        f[11], f[12] = "1", "4"     # GREATER
        self.assertIn("if (!(round(fragColor.a * 255.0) > alphaRef)) { discard; }",
                      self.gen(f))
        f[12] = "7"                 # ALWAYS: no test
        self.assertNotIn("alphaRef)) { discard; }", self.gen(f))

    def test_w_buffer_writes_depth_from_clip_w(self):
        self.assertNotIn("SV_Depth", self.gen(DEFAULT))
        f = list(DEFAULT)
        f[15] = "1"
        out = self.gen(f)
        self.assertIn("out float oDepth : SV_Depth", out)
        self.assertIn("oDepth = saturate(input.pos.w * depthScale);", out)

    def test_overflow_is_reported(self):
        self.assertEqual(self.gen(DEFAULT, "tiny").strip(), "-1")

    def test_format_tables(self):
        self.assertEqual(self.gen(DEFAULT, "fmt").split(),
                         ["1", "0", "0", "65535.0", "16777215.0", "0.0"])


if __name__ == "__main__":
    unittest.main()
