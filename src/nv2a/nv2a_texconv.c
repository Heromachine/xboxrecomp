/*
 * NV2A texture decode: S3TC blocks and swizzled palettised textures to BGRA.
 *
 * The S3TC decoder follows xemu's hw/xbox/nv2a/pgraph/s3tc.c (Copyright (c)
 * 2020 Wilhelm Kovatch, MIT licence), including its integer rounding, so a
 * texel decodes to the same value xemu shows. The Z-order walk follows xemu's
 * hw/xbox/nv2a/pgraph/swizzle.c (Copyright (c) 2015 Jannik Vogel, 2013 espes,
 * 2007-2010 The Nouveau Project, 2025 Matt Borgerson, LGPL v2+) and the
 * palette lookup its pgraph/texture.c. xemu writes R,G,B,A; this file writes
 * B,G,R,A for DXGI_FORMAT_B8G8R8A8_UNORM.
 */

#include "nv2a_texconv.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

size_t nv2a_texconv_s3tc_level_size(unsigned color_format,
                                    unsigned width, unsigned height)
{
    size_t block;

    switch (color_format) {
    case NV2A_TEXCONV_L_DXT1_A1R5G5B5:  block = 8;  break;
    case NV2A_TEXCONV_L_DXT23_A8R8G8B8:
    case NV2A_TEXCONV_L_DXT45_A8R8G8B8: block = 16; break;
    default: return 0;
    }
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    return (size_t)((width + 3) / 4) * ((height + 3) / 4) * block;
}

/* The two endpoint colours and their interpolants. `transparent` is DXT1's
 * three-colour mode (c0 <= c1), whose fourth entry is transparent black. */
static void decode_bc1_colors(uint16_t c0, uint16_t c1, uint8_t r[4],
                              uint8_t g[4], uint8_t b[4], uint8_t a[4],
                              int transparent)
{
    r[0] = (uint8_t)(((c0 & 0xF800) >> 8) * 0xFF / 0xF8);
    g[0] = (uint8_t)(((c0 & 0x07E0) >> 3) * 0xFF / 0xFC);
    b[0] = (uint8_t)(((c0 & 0x001F) << 3) * 0xFF / 0xF8);
    a[0] = 255;

    r[1] = (uint8_t)(((c1 & 0xF800) >> 8) * 0xFF / 0xF8);
    g[1] = (uint8_t)(((c1 & 0x07E0) >> 3) * 0xFF / 0xFC);
    b[1] = (uint8_t)(((c1 & 0x001F) << 3) * 0xFF / 0xF8);
    a[1] = 255;

    if (transparent) {
        r[2] = (uint8_t)((r[0] + r[1]) / 2);
        g[2] = (uint8_t)((g[0] + g[1]) / 2);
        b[2] = (uint8_t)((b[0] + b[1]) / 2);
        a[2] = 255;
        r[3] = g[3] = b[3] = a[3] = 0;
    } else {
        r[2] = (uint8_t)((2 * r[0] + r[1]) / 3);
        g[2] = (uint8_t)((2 * g[0] + g[1]) / 3);
        b[2] = (uint8_t)((2 * b[0] + b[1]) / 3);
        a[2] = 255;
        r[3] = (uint8_t)((r[0] + 2 * r[1]) / 3);
        g[3] = (uint8_t)((g[0] + 2 * g[1]) / 3);
        b[3] = (uint8_t)((b[0] + 2 * b[1]) / 3);
        a[3] = 255;
    }
}

/* Write one 4x4 block at block coordinates (bx, by), clipped to the texture.
 * With `alpha` NULL each texel takes the alpha of its colour index; otherwise
 * alpha[16] holds one value per texel. */
static void write_block(uint8_t *bgra, unsigned width, unsigned height,
                        unsigned bx, unsigned by, uint32_t indices,
                        const uint8_t r[4], const uint8_t g[4],
                        const uint8_t b[4], const uint8_t a[4],
                        const uint8_t *alpha)
{
    unsigned x0 = bx * 4, y0 = by * 4, x, y;

    for (y = y0; y < y0 + 4 && y < height; y++) {
        for (x = x0; x < x0 + 4 && x < width; x++) {
            unsigned texel = 4 * (y - y0) + (x - x0);
            unsigned index = (indices >> (2 * texel)) & 3;
            uint8_t *p = bgra + ((size_t)y * width + x) * 4;
            p[0] = b[index];
            p[1] = g[index];
            p[2] = r[index];
            p[3] = alpha ? alpha[texel] : a[index];
        }
    }
}

int nv2a_texconv_decode_s3tc(unsigned color_format, const uint8_t *src,
                             unsigned width, unsigned height, uint8_t *bgra)
{
    unsigned blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
    unsigned bx, by;

    if (nv2a_texconv_s3tc_level_size(color_format, width, height) == 0)
        return -1;

    for (by = 0; by < blocks_y; by++) {
        for (bx = 0; bx < blocks_x; bx++) {
            uint8_t r[4], g[4], b[4], a[4], alpha[16];
            unsigned i;

            if (color_format == NV2A_TEXCONV_L_DXT1_A1R5G5B5) {
                uint16_t c0 = rd16(src), c1 = rd16(src + 2);
                decode_bc1_colors(c0, c1, r, g, b, a, c0 <= c1);
                write_block(bgra, width, height, bx, by, rd32(src + 4),
                            r, g, b, a, NULL);
                src += 8;
                continue;
            }

            /* DXT2-5: alpha in the first 8 bytes, then a DXT1-style colour
             * block that is always four-colour. */
            decode_bc1_colors(rd16(src + 8), rd16(src + 10), r, g, b, a, 0);
            if (color_format == NV2A_TEXCONV_L_DXT23_A8R8G8B8) {
                uint64_t bits = rd64(src);
                for (i = 0; i < 16; i++)
                    alpha[i] = (uint8_t)((((bits >> (4 * i)) & 0x0F) << 4)
                                         * 0xFF / 0xF0);
            } else {
                uint8_t a0 = src[0], a1 = src[1], pal[8];
                uint64_t bits = rd64(src);
                pal[0] = a0;
                pal[1] = a1;
                if (a0 > a1) {
                    pal[2] = (uint8_t)((6 * a0 + 1 * a1) / 7);
                    pal[3] = (uint8_t)((5 * a0 + 2 * a1) / 7);
                    pal[4] = (uint8_t)((4 * a0 + 3 * a1) / 7);
                    pal[5] = (uint8_t)((3 * a0 + 4 * a1) / 7);
                    pal[6] = (uint8_t)((2 * a0 + 5 * a1) / 7);
                    pal[7] = (uint8_t)((1 * a0 + 6 * a1) / 7);
                } else {
                    pal[2] = (uint8_t)((4 * a0 + 1 * a1) / 5);
                    pal[3] = (uint8_t)((3 * a0 + 2 * a1) / 5);
                    pal[4] = (uint8_t)((2 * a0 + 3 * a1) / 5);
                    pal[5] = (uint8_t)((1 * a0 + 4 * a1) / 5);
                    pal[6] = 0;
                    pal[7] = 255;
                }
                for (i = 0; i < 16; i++)
                    alpha[i] = pal[(bits >> (16 + 3 * i)) & 7];
            }
            write_block(bgra, width, height, bx, by, rd32(src + 12),
                        r, g, b, a, alpha);
            src += 16;
        }
    }
    return 0;
}

/* Bit masks that interleave the x and y bits of a texel's Z-order position,
 * lowest bits first, for as long as each dimension still has bits. An 8x32
 * texture needs 3 bits of x and 5 of y: x = 00010101, y = 11101010. */
static void swizzle_masks(unsigned width, unsigned height,
                          uint32_t *mask_x, uint32_t *mask_y)
{
    uint32_t x = 0, y = 0, bit = 1, mask_bit = 1;
    int more;

    do {
        more = 0;
        if (bit < width)  { x |= mask_bit; mask_bit <<= 1; more = 1; }
        if (bit < height) { y |= mask_bit; mask_bit <<= 1; more = 1; }
        bit <<= 1;
    } while (more);
    *mask_x = x;
    *mask_y = y;
}

void nv2a_texconv_unswizzle_2d(const uint8_t *src, unsigned width,
                               unsigned height, unsigned bytes_per_texel,
                               uint8_t *dst)
{
    uint32_t mask_x, mask_y, off_y = 0;
    unsigned x, y;

    swizzle_masks(width, height, &mask_x, &mask_y);
    for (y = 0; y < height; y++) {
        uint32_t off_x = 0;
        for (x = 0; x < width; x++) {
            memcpy(dst + ((size_t)y * width + x) * bytes_per_texel,
                   src + ((size_t)off_y + off_x) * bytes_per_texel,
                   bytes_per_texel);
            /* Add one to the x bits, carrying through the y bits between. */
            off_x = (off_x - mask_x) & mask_x;
        }
        off_y = (off_y - mask_y) & mask_y;
    }
}

void nv2a_texconv_decode_i8(const uint8_t *indices, unsigned width,
                            unsigned height, const uint8_t *palette,
                            unsigned palette_entries, uint8_t *bgra)
{
    size_t i, n = (size_t)width * height;

    /* Unswizzle straight into the output's first n bytes, then expand from
     * the end backwards so each index is read before its texel overwrites it. */
    nv2a_texconv_unswizzle_2d(indices, width, height, 1, bgra);
    for (i = n; i-- > 0;) {
        unsigned index = bgra[i];
        uint8_t *p = bgra + i * 4;
        if (index < palette_entries)
            memcpy(p, palette + (size_t)index * 4, 4);
        else
            memset(p, 0, 4);
    }
}
