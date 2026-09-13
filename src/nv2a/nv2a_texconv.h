/*
 * NV2A texture decode: S3TC blocks and swizzled palettised textures to BGRA.
 *
 * Kept free of D3D and Windows headers so the host test suite can compile it
 * (tools/recomp/test_nv2a_texconv.py). nv2a_pgraph_d3d11.c uploads the result
 * as DXGI_FORMAT_B8G8R8A8_UNORM, the same target its A8R8G8B8 path uses.
 */

#ifndef NV2A_TEXCONV_H
#define NV2A_TEXCONV_H

#include <stddef.h>
#include <stdint.h>

/* NV097_SET_TEXTURE_FORMAT_COLOR codes this file decodes. */
#define NV2A_TEXCONV_SZ_I8_A8R8G8B8   0x0B
#define NV2A_TEXCONV_L_DXT1_A1R5G5B5  0x0C
#define NV2A_TEXCONV_L_DXT23_A8R8G8B8 0x0E
#define NV2A_TEXCONV_L_DXT45_A8R8G8B8 0x0F

/* Bytes of one mip level of an S3TC texture: whole 4x4 blocks, 8 bytes each
 * for DXT1 and 16 for DXT2-5. 0 for a format that is not S3TC. */
size_t nv2a_texconv_s3tc_level_size(unsigned color_format,
                                    unsigned width, unsigned height);

/* Decode one S3TC mip level to width*height BGRA texels. `src` must hold
 * nv2a_texconv_s3tc_level_size() bytes. Returns 0, or -1 for a format that
 * is not S3TC. */
int nv2a_texconv_decode_s3tc(unsigned color_format, const uint8_t *src,
                             unsigned width, unsigned height, uint8_t *bgra);

/* Reorder a swizzled (Z-order) power-of-two texture into rows. `src` holds
 * width*height texels of `bytes_per_texel` bytes in Z order; `dst` gets the
 * same texels row by row. */
void nv2a_texconv_unswizzle_2d(const uint8_t *src, unsigned width,
                               unsigned height, unsigned bytes_per_texel,
                               uint8_t *dst);

/* Decode a swizzled SZ_I8_A8R8G8B8 texture: unswizzle the 8-bit indices and
 * look each up in `palette`, whose `palette_entries` A8R8G8B8 entries are
 * already B,G,R,A in memory. An index past the palette decodes to transparent
 * black rather than reading beyond it. */
void nv2a_texconv_decode_i8(const uint8_t *indices, unsigned width,
                            unsigned height, const uint8_t *palette,
                            unsigned palette_entries, uint8_t *bgra);

#endif /* NV2A_TEXCONV_H */
