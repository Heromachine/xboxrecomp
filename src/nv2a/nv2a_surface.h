/*
 * NV2A render-target surface geometry, kept free of D3D11 so it can be tested
 * on the host.
 *
 * A title renders into guest-RAM addresses it picks with SET_SURFACE_COLOR_
 * OFFSET / SET_SURFACE_ZETA_OFFSET, then often samples those same addresses
 * as textures (bloom, shadow maps, a copy of the frame). xemu keeps a binding
 * per address and matches texture offsets against it (pgraph/gl/surface.c,
 * surface_get and surface_get_within); this is the address and size half of
 * that model.
 */
#ifndef NV2A_SURFACE_H
#define NV2A_SURFACE_H

#include <stdint.h>

typedef struct {
    uint32_t addr;      /* guest-RAM offset of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;     /* bytes per row */
    uint32_t bpp;       /* bytes per texel */
} NV2ASurfDesc;

/* Bytes per texel of a SET_SURFACE_FORMAT colour code, 0 if unknown. */
uint32_t nv2a_surf_color_bpp(uint32_t color_fmt);

/* Bytes per texel of a SET_SURFACE_FORMAT zeta code, 0 if unknown. */
uint32_t nv2a_surf_zeta_bpp(uint32_t zeta_fmt);

/* Surface size from SET_SURFACE_FORMAT and the clip registers. A swizzled
 * surface is sized by the format's log2 fields; any other by the clip
 * rectangle including its origin, since the clip is what bounds drawing
 * (xemu populate_surface_binding_entry). */
void nv2a_surf_dims(uint32_t format, uint32_t clip_h, uint32_t clip_v,
                    uint32_t *width, uint32_t *height);

/* Where a tw x th texture at `addr` sits inside surface `s`. Returns 1 and
 * the texel origin when the whole texture lies on texel boundaries inside
 * the surface, else 0. */
int nv2a_surf_locate(const NV2ASurfDesc *s, uint32_t addr, uint32_t tw,
                     uint32_t th, uint32_t *x, uint32_t *y);

/* NV2A clear rectangle, as its inclusive min/max registers, clamped to a
 * width x height surface. Returns 0 when nothing is left to clear. */
int nv2a_surf_clear_rect(uint32_t rect_h, uint32_t rect_v, uint32_t width,
                         uint32_t height, uint32_t *x0, uint32_t *y0,
                         uint32_t *x1, uint32_t *y1);

#endif
