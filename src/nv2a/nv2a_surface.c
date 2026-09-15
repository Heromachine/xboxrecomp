/*
 * NV2A render-target surface geometry. See nv2a_surface.h.
 */
#include "nv2a_surface.h"

uint32_t nv2a_surf_color_bpp(uint32_t color_fmt)
{
    switch (color_fmt) {
    case 0x01:                 /* X1R5G5B5_Z1R5G5B5 */
    case 0x02:                 /* X1R5G5B5_O1R5G5B5 */
    case 0x03:                 /* R5G6B5 */
    case 0x0A:                 /* G8B8 */
        return 2;
    case 0x04: case 0x05:      /* X8R8G8B8 */
    case 0x06: case 0x07:      /* X1A7R8G8B8 */
    case 0x08:                 /* A8R8G8B8 */
        return 4;
    case 0x09:                 /* B8 */
        return 1;
    default:
        return 0;
    }
}

uint32_t nv2a_surf_zeta_bpp(uint32_t zeta_fmt)
{
    switch (zeta_fmt) {
    case 1: return 2;          /* Z16 */
    case 2: return 4;          /* Z24S8 */
    default: return 0;
    }
}

void nv2a_surf_dims(uint32_t format, uint32_t clip_h, uint32_t clip_v,
                    uint32_t *width, uint32_t *height)
{
    if (((format >> 8) & 0xF) == 2) {          /* TYPE_SWIZZLE */
        *width = 1u << ((format >> 16) & 0xFF);
        *height = 1u << ((format >> 24) & 0xFF);
    } else {
        *width = (clip_h >> 16) + (clip_h & 0xFFFF);
        *height = (clip_v >> 16) + (clip_v & 0xFFFF);
    }
}

int nv2a_surf_locate(const NV2ASurfDesc *s, uint32_t addr, uint32_t tw,
                     uint32_t th, uint32_t *x, uint32_t *y)
{
    uint32_t off, row, col;

    if (!s->pitch || !s->bpp || addr < s->addr || tw == 0 || th == 0)
        return 0;
    off = addr - s->addr;
    row = off / s->pitch;
    col = off % s->pitch;
    if (col % s->bpp)
        return 0;
    col /= s->bpp;
    if (col >= s->width || row >= s->height ||
        tw > s->width - col || th > s->height - row)
        return 0;
    *x = col;
    *y = row;
    return 1;
}

int nv2a_surf_clear_rect(uint32_t rect_h, uint32_t rect_v, uint32_t width,
                         uint32_t height, uint32_t *x0, uint32_t *y0,
                         uint32_t *x1, uint32_t *y1)
{
    /* NV_PGRAPH_CLEARRECTX: XMIN 0x00000FFF, XMAX 0x0FFF0000, inclusive. */
    uint32_t xmin = rect_h & 0xFFF, xmax = (rect_h >> 16) & 0xFFF;
    uint32_t ymin = rect_v & 0xFFF, ymax = (rect_v >> 16) & 0xFFF;

    if (width == 0 || height == 0 || xmin > xmax || ymin > ymax ||
        xmin >= width || ymin >= height)
        return 0;
    if (xmax >= width)
        xmax = width - 1;
    if (ymax >= height)
        ymax = height - 1;
    *x0 = xmin;
    *y0 = ymin;
    *x1 = xmax + 1;            /* exclusive */
    *y1 = ymax + 1;
    return 1;
}
