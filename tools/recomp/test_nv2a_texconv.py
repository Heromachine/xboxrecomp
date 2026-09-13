"""Compile src/nv2a/nv2a_texconv.c and check its texture decoding.

Breakdown's title screen draws its sprites from DXT4/5 and palettised I8
textures, and the D3D11 backend used to decode neither, so the screen stayed
black. These checks pin the decoder to xemu's arithmetic (pgraph/s3tc.c,
pgraph/swizzle.c, pgraph/texture.c): worked examples for each mode, then random
data against a reference that decodes texel by texel instead of block by block.
"""
import pathlib
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
DXT1, DXT3, DXT5, I8 = 0x0C, 0x0E, 0x0F, 0x0B

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "nv2a_texconv.h"
/* stdin: op fmt width height nbytes, then nbytes hex pairs (and, for 'i',
 * a palette entry count and its bytes). stdout: decoded bytes as hex. */
static unsigned char *read_bytes(size_t n) {
    unsigned char *p = malloc(n ? n : 1); size_t i; unsigned v;
    for (i = 0; i < n; i++) { if (scanf("%2x", &v) != 1) exit(2); p[i] = (unsigned char)v; }
    return p;
}
int main(void) {
    char op; unsigned fmt, w, h; size_t n;
    while (scanf(" %c %u %u %u %zu", &op, &fmt, &w, &h, &n) == 5) {
        unsigned char *src = read_bytes(n), *out;
        size_t i, outn = (size_t)w * h * 4;
        out = calloc(outn, 1);
        if (op == 's') {
            printf("%zu ", nv2a_texconv_s3tc_level_size(fmt, w, h));
            if (nv2a_texconv_decode_s3tc(fmt, src, w, h, out) != 0) { printf("ERR\n"); continue; }
        } else if (op == 'u') {
            outn = (size_t)w * h * fmt;       /* fmt carries bytes per texel */
            nv2a_texconv_unswizzle_2d(src, w, h, fmt, out);
        } else if (op == 'i') {
            unsigned entries; unsigned char *pal;
            if (scanf(" %u", &entries) != 1) return 2;
            pal = read_bytes((size_t)entries * 4);
            nv2a_texconv_decode_i8(src, w, h, pal, entries, out);
            free(pal);
        }
        for (i = 0; i < outn; i++) printf("%02x", out[i]);
        printf("\n");
        free(src); free(out);
    }
    return 0;
}
"""


def _rgb565(c):
    return (((c & 0xF800) >> 8) * 0xFF // 0xF8,
            ((c & 0x07E0) >> 3) * 0xFF // 0xFC,
            ((c & 0x001F) << 3) * 0xFF // 0xF8)


def _palette4(c0, c1, three_colour):
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if three_colour:
        mid = tuple((a + b) // 2 for a, b in zip(p0, p1))
        return [p0 + (255,), p1 + (255,), mid + (255,), (0, 0, 0, 0)]
    return [p0 + (255,), p1 + (255,),
            tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
            tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]


def reference_s3tc(fmt, data, w, h):
    """Texel-at-a-time decode, returning B,G,R,A bytes."""
    block = 8 if fmt == DXT1 else 16
    blocks_x = (w + 3) // 4
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            b = data[((y // 4) * blocks_x + x // 4) * block:][:block]
            texel = (y % 4) * 4 + (x % 4)
            if fmt == DXT1:
                c0, c1, idx = struct.unpack_from("<HHI", b)
                rgba = _palette4(c0, c1, c0 <= c1)[(idx >> 2 * texel) & 3]
            else:
                alpha_bits, c0, c1, idx = struct.unpack_from("<QHHI", b)
                r, g, bl, _ = _palette4(c0, c1, False)[(idx >> 2 * texel) & 3]
                if fmt == DXT3:
                    a = (((alpha_bits >> 4 * texel) & 0xF) << 4) * 0xFF // 0xF0
                else:
                    a0, a1 = b[0], b[1]
                    if a0 > a1:
                        pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                    else:
                        pal = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                    a = pal[(alpha_bits >> (16 + 3 * texel)) & 7]
                rgba = (r, g, bl, a)
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((rgba[2], rgba[1], rgba[0], rgba[3]))
    return bytes(out)


def zorder(x, y, w, h):
    """Z-order position: x and y bits interleaved, x first, each dimension
    contributing bits only while it still has them."""
    pos, out_bit, bit = 0, 0, 0
    while (1 << bit) < w or (1 << bit) < h:
        if (1 << bit) < w:
            pos |= ((x >> bit) & 1) << out_bit
            out_bit += 1
        if (1 << bit) < h:
            pos |= ((y >> bit) & 1) << out_bit
            out_bit += 1
        bit += 1
    return pos


class TexconvTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        cls.tmp = tempfile.TemporaryDirectory()
        src = pathlib.Path(cls.tmp.name) / "driver.c"
        src.write_text(DRIVER)
        cls.exe = pathlib.Path(cls.tmp.name) / "driver"
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-I", str(ROOT / "src/nv2a"), str(src),
                        str(ROOT / "src/nv2a/nv2a_texconv.c"), "-o", str(cls.exe)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cases(self, lines):
        res = subprocess.run([str(self.exe)], input="\n".join(lines) + "\n",
                             capture_output=True, text=True, check=True)
        return res.stdout.splitlines()

    def s3tc(self, fmt, data, w, h):
        size, hexout = self.run_cases([f"s {fmt} {w} {h} {len(data)} {data.hex()}"])[0].split()
        return int(size), bytes.fromhex(hexout)

    def test_dxt1_four_colour_block(self):
        # red c0 > blue c1: four colours, the two interpolants weighted 2:1
        data = struct.pack("<HHI", 0xF800, 0x001F, 0b11100100)
        size, out = self.s3tc(DXT1, data, 4, 4)
        self.assertEqual(size, 8)
        texels = [tuple(out[i:i + 4]) for i in range(0, 16, 4)]
        self.assertEqual(texels, [(0, 0, 255, 255), (255, 0, 0, 255),
                                  (85, 0, 170, 255), (170, 0, 85, 255)])

    def test_dxt1_three_colour_block_is_transparent_at_index_3(self):
        data = struct.pack("<HHI", 0x001F, 0xF800, 0b11)
        _, out = self.s3tc(DXT1, data, 4, 4)
        self.assertEqual(tuple(out[0:4]), (0, 0, 0, 0))
        self.assertEqual(tuple(out[4:8]), (255, 0, 0, 255))  # index 0 = c0, blue

    def test_dxt3_explicit_alpha(self):
        data = struct.pack("<QHHI", 0x8F, 0xFFFF, 0xFFFF, 0)
        _, out = self.s3tc(DXT3, data, 4, 4)
        self.assertEqual(out[3], 255)                    # nibble F
        self.assertEqual(out[7], 0x80 * 0xFF // 0xF0)    # nibble 8 -> 136

    def test_dxt5_interpolated_alpha(self):
        # a0=255 > a1=0: eight-step ramp; texel 0 index 2, texel 1 index 7
        bits = (2 << 16) | (7 << 19)
        data = struct.pack("<QHHI", bits | 0x00FF, 0xFFFF, 0xFFFF, 0)
        _, out = self.s3tc(DXT5, data, 4, 4)
        self.assertEqual(out[3], 6 * 255 // 7)
        self.assertEqual(out[7], 255 // 7)
        # a0 <= a1: six-step ramp plus fixed 0 and 255
        data = struct.pack("<QHHI", (6 << 16) | (7 << 19) | (0xFF << 8), 0, 0, 0)
        _, out = self.s3tc(DXT5, data, 4, 4)
        self.assertEqual((out[3], out[7]), (0, 255))

    def test_s3tc_matches_reference_on_random_textures(self):
        rng = random.Random(1234)
        lines, expected = [], []
        for fmt in (DXT1, DXT3, DXT5):
            for w, h in ((4, 4), (8, 4), (16, 16), (6, 2), (1, 1), (256, 32)):
                block = 8 if fmt == DXT1 else 16
                data = bytes(rng.randrange(256)
                             for _ in range(((w + 3) // 4) * ((h + 3) // 4) * block))
                lines.append(f"s {fmt} {w} {h} {len(data)} {data.hex()}")
                expected.append((fmt, w, h, len(data), reference_s3tc(fmt, data, w, h)))
        for got, (fmt, w, h, size, ref) in zip(self.run_cases(lines), expected):
            got_size, got_hex = got.split()
            self.assertEqual(int(got_size), size, (fmt, w, h))
            self.assertEqual(bytes.fromhex(got_hex), ref, (fmt, w, h))

    def test_s3tc_size_and_unknown_format(self):
        size, _ = self.s3tc(DXT5, b"\0" * 16, 1, 1)
        self.assertEqual(size, 16)
        self.assertEqual(self.run_cases(["s 18 4 4 0 "])[0], "0 ERR")

    def test_unswizzle_worked_example(self):
        # 4x2: x takes bits 0 and 2, y bit 1, so row 0 reads Z positions
        # 0,1,4,5 and row 1 reads 2,3,6,7.
        out = self.run_cases([f"u 1 4 2 8 {bytes(range(8)).hex()}"])[0]
        self.assertEqual(bytes.fromhex(out), bytes([0, 1, 4, 5, 2, 3, 6, 7]))

    def test_unswizzle_matches_reference(self):
        rng = random.Random(99)
        lines, expected = [], []
        for w, h, bpp in ((8, 8, 1), (8, 32, 1), (32, 8, 4), (1, 16, 2), (512, 512, 1)):
            src = bytes(rng.randrange(256) for _ in range(w * h * bpp))
            ref = bytearray(w * h * bpp)
            for y in range(h):
                for x in range(w):
                    z = zorder(x, y, w, h)
                    ref[(y * w + x) * bpp:(y * w + x + 1) * bpp] = src[z * bpp:(z + 1) * bpp]
            lines.append(f"u {bpp} {w} {h} {len(src)} {src.hex()}")
            expected.append(bytes(ref))
        for got, ref in zip(self.run_cases(lines), expected):
            self.assertEqual(bytes.fromhex(got), ref)

    def test_i8_palette_lookup(self):
        palette = bytes([10, 20, 30, 40]) + bytes([50, 60, 70, 80])
        # 2x2 swizzled indices: Z order matches row order at this size
        out = self.run_cases([f"i 0 2 2 4 {bytes([1, 0, 1, 9]).hex()} 2 {palette.hex()}"])[0]
        self.assertEqual(bytes.fromhex(out),
                         palette[4:8] + palette[0:4] + palette[4:8] + bytes(4))

    def test_i8_matches_reference(self):
        rng = random.Random(7)
        w, h, entries = 16, 8, 32
        idx = bytes(rng.randrange(64) for _ in range(w * h))   # some past the palette
        pal = bytes(rng.randrange(256) for _ in range(entries * 4))
        ref = bytearray()
        for y in range(h):
            for x in range(w):
                i = idx[zorder(x, y, w, h)]
                ref += pal[i * 4:i * 4 + 4] if i < entries else bytes(4)
        out = self.run_cases([f"i 0 {w} {h} {len(idx)} {idx.hex()} {entries} {pal.hex()}"])[0]
        self.assertEqual(bytes.fromhex(out), bytes(ref))


if __name__ == "__main__":
    unittest.main()
