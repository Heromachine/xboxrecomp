/*
 * NV2A vertex-data arrays: the element batch collected between BEGIN and END,
 * and the gather that turns array-sourced vertices into the packed per-vertex
 * layout the inline path already consumes.
 *
 * Follows xemu hw/xbox/nv2a/pgraph/pgraph.c (SET_VERTEX_DATA_ARRAY_OFFSET/
 * FORMAT, ARRAY_ELEMENT16/32, DRAW_ARRAYS) and pgraph/gl/vertex.c (attribute
 * binding). Kept free of D3D and Windows headers so the host test suite can
 * compile it (tools/recomp/test_nv2a_vtxarray.py).
 */

#ifndef NV2A_VTXARRAY_H
#define NV2A_VTXARRAY_H

#include <stddef.h>
#include <stdint.h>

#define NV2A_VTX_ATTRS          16
#define NV2A_VTX_MAX_RUNS       1250   /* xemu's draw_arrays_start[] size */

#define NV2A_VTX_FMT_UB_D3D     0
#define NV2A_VTX_FMT_S1         1
#define NV2A_VTX_FMT_F          2
#define NV2A_VTX_FMT_UB_OGL     4
#define NV2A_VTX_FMT_S32K       5
#define NV2A_VTX_FMT_CMP        6

typedef struct {
    uint32_t format;     /* SET_VERTEX_DATA_ARRAY_FORMAT TYPE, bits 0-3 */
    uint32_t count;      /* SIZE, bits 4-7: components; 0 = slot disabled */
    uint32_t size;       /* bytes per component, derived from format */
    uint32_t stride;     /* STRIDE, bits 8-31; 0 = only element 0 is used */
    uint32_t offset;     /* SET_VERTEX_DATA_ARRAY_OFFSET, low 31 bits */
    uint32_t dma_select; /* OFFSET bit 31: vertex DMA B instead of A */
} NV2AVtxAttr;

typedef struct {
    uint32_t *elements;
    uint32_t element_count, element_capacity;
    uint32_t run_start[NV2A_VTX_MAX_RUNS];
    uint32_t run_count[NV2A_VTX_MAX_RUNS];
    uint32_t run_total;
    int oom;             /* an element append failed; the batch is incomplete */
} NV2AVtxBatch;

/* Decode a SET_VERTEX_DATA_ARRAY_FORMAT parameter into format/count/size/
 * stride, leaving offset and dma_select alone. */
void nv2a_vtx_set_format(NV2AVtxAttr *attr, uint32_t param);
void nv2a_vtx_set_offset(NV2AVtxAttr *attr, uint32_t param);

/* CMP normal: signed 11/11/10 bits, normalised as xemu's decompress_11_11_10.
 * out[3] is 1. */
void nv2a_vtx_decode_cmp(uint32_t packed, float out[4]);

void nv2a_vtx_batch_reset(NV2AVtxBatch *b);
void nv2a_vtx_batch_free(NV2AVtxBatch *b);

/* ARRAY_ELEMENT16 carries two indices per parameter, low half first. The
 * caller must first drain every run but the last (see _leading_runs) --
 * xemu draws those before expanding the last run into the element list. */
void nv2a_vtx_batch_element16(NV2AVtxBatch *b, uint32_t param);
void nv2a_vtx_batch_element32(NV2AVtxBatch *b, uint32_t param);

/* Runs an ARRAY_ELEMENT arriving now would have to draw first: all but the
 * last. Returns how many; they are run_start/run_count[0..n-1]. */
uint32_t nv2a_vtx_batch_leading_runs(const NV2AVtxBatch *b);

/* DRAW_ARRAYS: START_INDEX is bits 0-23, COUNT-1 is bits 24-31. Appended to
 * the element list when one is already open, else recorded as a run, joined
 * to the previous run when it starts where that one ends. */
void nv2a_vtx_batch_draw_arrays(NV2AVtxBatch *b, uint32_t param);

/* Resolves [addr, addr+len) of vertex memory to host bytes, or NULL. */
typedef const uint8_t *(*NV2AVtxSpanFn)(void *ctx, uint32_t dma_select,
                                        uint32_t addr, size_t len);

/* Copy `n` vertices named by `elements` into `dst`, one `dst_stride`-byte
 * record per vertex, each enabled slot's bytes landing at dst_offset[slot].
 * Returns 0, or -1 if any source span does not resolve (dst is then
 * incomplete and must not be drawn). */
int nv2a_vtx_gather(const NV2AVtxAttr attrs[NV2A_VTX_ATTRS],
                    const uint32_t *elements, uint32_t n,
                    NV2AVtxSpanFn span, void *ctx,
                    const uint32_t dst_offset[NV2A_VTX_ATTRS],
                    uint32_t dst_stride, uint8_t *dst);

#endif
