"""Compile src/nv2a/nv2a_vtxarray.c and check the vertex-data array decode.

Breakdown's level geometry is drawn with ARRAY_ELEMENT16 and DRAW_ARRAYS over
vertex arrays, and the D3D11 backend used to implement only INLINE_ARRAY, so a
level rendered as its HUD over an empty clear. These checks pin the packing to
xemu (pgraph/pgraph.c ARRAY_ELEMENT16/DRAW_ARRAYS/SET_VERTEX_DATA_ARRAY_*,
pgraph/glsl/vsh.c decompress_11_11_10, pgraph/gl/vertex.c attribute binding).
"""
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "nv2a_vtxarray.h"

static uint8_t mem[256];

static const uint8_t *span(void *ctx, uint32_t dma, uint32_t addr, size_t len) {
    (void)ctx; (void)dma;
    if (addr > sizeof(mem) || len > sizeof(mem) - addr) return NULL;
    return mem + addr;
}

int main(void) {
    char op[16];
    while (scanf(" %15s", op) == 1) {
        if (!strcmp(op, "fmt")) {
            unsigned p; NV2AVtxAttr a = {0};
            scanf("%x", &p);
            nv2a_vtx_set_format(&a, p);
            printf("%u %u %u %u\n", a.format, a.count, a.size, a.stride);
        } else if (!strcmp(op, "off")) {
            unsigned p; NV2AVtxAttr a = {0};
            scanf("%x", &p);
            nv2a_vtx_set_offset(&a, p);
            printf("%u %x\n", a.dma_select, a.offset);
        } else if (!strcmp(op, "cmp")) {
            unsigned p; float f[4];
            scanf("%x", &p);
            nv2a_vtx_decode_cmp(p, f);
            printf("%.6f %.6f %.6f %.6f\n", f[0], f[1], f[2], f[3]);
        } else if (!strcmp(op, "batch")) {
            /* ops: e16 X | e32 X | da X | lead ; ends with "end" */
            NV2AVtxBatch b = {0}; char sub[8]; unsigned x;
            while (scanf(" %7s", sub) == 1 && strcmp(sub, "end")) {
                if (!strcmp(sub, "lead")) {
                    printf("lead %u\n", nv2a_vtx_batch_leading_runs(&b));
                    continue;
                }
                scanf("%x", &x);
                if (!strcmp(sub, "e16")) nv2a_vtx_batch_element16(&b, x);
                else if (!strcmp(sub, "e32")) nv2a_vtx_batch_element32(&b, x);
                else nv2a_vtx_batch_draw_arrays(&b, x);
            }
            printf("runs");
            for (unsigned r = 0; r < b.run_total; r++) printf(" %u+%u", b.run_start[r], b.run_count[r]);
            printf("\nelems");
            for (unsigned i = 0; i < b.element_count; i++) printf(" %u", b.elements[i]);
            printf("\n");
            nv2a_vtx_batch_free(&b);
        } else if (!strcmp(op, "gather")) {
            /* two attrs: slot0 = float x3 stride 16 offset 8, slot3 = UB_D3D x4
             * stride 0 offset 200 (constant). elements from stdin. */
            NV2AVtxAttr a[16]; unsigned n, e[8], off0, len0;
            memset(a, 0, sizeof(a));
            for (unsigned i = 0; i < sizeof(mem); i++) mem[i] = (uint8_t)i;
            scanf("%x %x %u", &off0, &len0, &n);
            for (unsigned i = 0; i < n; i++) scanf("%u", &e[i]);
            nv2a_vtx_set_format(&a[0], 0x1032);
            nv2a_vtx_set_offset(&a[0], off0);
            nv2a_vtx_set_format(&a[3], 0x40);
            nv2a_vtx_set_offset(&a[3], 200);
            uint32_t dst_off[16] = {0}; dst_off[3] = 12;
            uint8_t out[8 * 16];
            int rc = nv2a_vtx_gather(a, e, n, span, NULL, dst_off, len0, out);
            printf("%d", rc);
            if (rc == 0) for (unsigned i = 0; i < n * len0; i++) printf(" %u", out[i]);
            printf("\n");
        }
        fflush(stdout);
    }
    return 0;
}
"""


class VtxArrayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("gcc") or shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("no C compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = pathlib.Path(cls.tmp.name)
        (tmp / "driver.c").write_text(DRIVER)
        cls.exe = tmp / "driver"
        subprocess.run(
            [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1",
             "-I", str(ROOT / "src/nv2a"), str(tmp / "driver.c"),
             str(ROOT / "src/nv2a/nv2a_vtxarray.c"), "-o", str(cls.exe)],
            check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_driver(self, text):
        return subprocess.run([str(self.exe)], input=text, capture_output=True,
                              text=True, check=True).stdout.splitlines()

    def test_format_fields(self):
        # float x3, stride 0x10 (xemu masks TYPE 0xF, SIZE 0xF0, STRIDE ~0xFF)
        self.assertEqual(self.run_driver("fmt 1032"), ["2 3 4 16"])
        self.assertEqual(self.run_driver("fmt 440"), ["0 4 1 4"])      # UB_D3D x4
        self.assertEqual(self.run_driver("fmt 2821"), ["1 2 2 40"])    # S1 x2
        self.assertEqual(self.run_driver("fmt 416"), ["6 1 4 4"])      # CMP x1
        self.assertEqual(self.run_driver("fmt 0"), ["0 0 1 0"])        # disabled

    def test_offset_dma_select(self):
        self.assertEqual(self.run_driver("off 80001234"), ["1 1234"])
        self.assertEqual(self.run_driver("off 7fffffff"), ["0 7fffffff"])

    def test_cmp_signed_11_11_10(self):
        def pack(x, y, z):
            return (x & 0x7FF) | ((y & 0x7FF) << 11) | ((z & 0x3FF) << 22)
        out = self.run_driver(f"cmp {pack(1023, -1023, 511):x} cmp {pack(0, -1, -511):x}")
        self.assertEqual([float(v) for v in out[0].split()], [1.0, -1.0, 1.0, 1.0])
        got = [float(v) for v in out[1].split()]
        self.assertAlmostEqual(got[0], 0.0)
        self.assertAlmostEqual(got[1], -1 / 1023, places=6)
        self.assertAlmostEqual(got[2], -1.0)

    def test_element16_packs_two_indices_low_first(self):
        out = self.run_driver("batch e16 00050003 e16 00010002 end")
        self.assertEqual(out, ["runs", "elems 3 5 2 1"])

    def test_draw_arrays_start_and_count(self):
        # START bits 0-23, COUNT-1 bits 24-31
        out = self.run_driver("batch da 02000010 end")
        self.assertEqual(out, ["runs 16+3", "elems"])

    def test_contiguous_runs_join_others_stay_separate(self):
        out = self.run_driver("batch da 02000000 da 01000003 da 00000009 end")
        self.assertEqual(out, ["runs 0+5 9+1", "elems"])

    def test_element_after_runs_expands_only_last(self):
        out = self.run_driver("batch da 01000000 da 01000008 lead e32 7 end")
        self.assertEqual(out, ["lead 1", "runs", "elems 8 9 7"])

    def test_draw_arrays_after_elements_appends(self):
        out = self.run_driver("batch e32 4 da 01000020 end")
        self.assertEqual(out, ["runs", "elems 4 32 33"])

    def test_gather_places_bytes_by_element_and_stride(self):
        # slot0: 12 bytes at 8 + e*16; slot3: 4 bytes at 200, same every vertex
        out = self.run_driver("gather 8 10 2 2 0")[0].split()
        self.assertEqual(out[0], "0")
        vals = list(map(int, out[1:]))
        self.assertEqual(vals[0:12], list(range(40, 52)))
        self.assertEqual(vals[12:16], [200, 201, 202, 203])
        self.assertEqual(vals[16:28], list(range(8, 20)))
        self.assertEqual(vals[28:32], [200, 201, 202, 203])

    def test_gather_rejects_out_of_range_element(self):
        # elements 1 and 20; 20 reads 8 + 320 bytes, past the 256-byte arena
        self.assertEqual(self.run_driver("gather 8 10 2 1 20"), ["-1"])

    def test_gather_rejects_unmapped_offset(self):
        self.assertEqual(self.run_driver("gather 7ffffff8 10 1 0"), ["-1"])

    def test_gather_rejects_layout_wider_than_stride(self):
        self.assertEqual(self.run_driver("gather 8 c 1 0"), ["-1"])


if __name__ == "__main__":
    unittest.main()
