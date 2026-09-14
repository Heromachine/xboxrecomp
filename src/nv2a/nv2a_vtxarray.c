#include "nv2a_vtxarray.h"

#include <stdlib.h>
#include <string.h>

void nv2a_vtx_set_format(NV2AVtxAttr *attr, uint32_t param)
{
    attr->format = param & 0x0F;
    attr->count = (param >> 4) & 0x0F;
    attr->stride = param >> 8;
    switch (attr->format) {
    case NV2A_VTX_FMT_UB_D3D:
    case NV2A_VTX_FMT_UB_OGL:
        attr->size = 1;
        break;
    case NV2A_VTX_FMT_S1:
    case NV2A_VTX_FMT_S32K:
        attr->size = 2;
        break;
    default:
        attr->size = 4;
        break;
    }
}

void nv2a_vtx_set_offset(NV2AVtxAttr *attr, uint32_t param)
{
    attr->dma_select = (param & 0x80000000u) != 0;
    attr->offset = param & 0x7FFFFFFFu;
}

static int32_t sign_extend(uint32_t v, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return (int32_t)((v ^ m) - m);
}

void nv2a_vtx_decode_cmp(uint32_t packed, float out[4])
{
    out[0] = sign_extend(packed, 11) / 1023.0f;
    out[1] = sign_extend(packed >> 11, 11) / 1023.0f;
    out[2] = sign_extend(packed >> 22, 10) / 511.0f;
    out[3] = 1.0f;
}

void nv2a_vtx_batch_reset(NV2AVtxBatch *b)
{
    b->element_count = 0;
    b->run_total = 0;
    b->oom = 0;
}

void nv2a_vtx_batch_free(NV2AVtxBatch *b)
{
    free(b->elements);
    b->elements = NULL;
    b->element_capacity = 0;
    nv2a_vtx_batch_reset(b);
}

static void push_element(NV2AVtxBatch *b, uint32_t e)
{
    if (b->element_count == b->element_capacity) {
        uint32_t cap = b->element_capacity ? b->element_capacity * 2 : 4096;
        uint32_t *grown = realloc(b->elements, (size_t)cap * sizeof(*grown));
        if (!grown) {
            b->oom = 1;
            return;
        }
        b->elements = grown;
        b->element_capacity = cap;
    }
    b->elements[b->element_count++] = e;
}

uint32_t nv2a_vtx_batch_leading_runs(const NV2AVtxBatch *b)
{
    return b->run_total > 1 ? b->run_total - 1 : 0;
}

static void expand_last_run(NV2AVtxBatch *b)
{
    if (b->run_total == 0)
        return;
    uint32_t start = b->run_start[b->run_total - 1];
    uint32_t count = b->run_count[b->run_total - 1];
    b->run_total = 0;
    for (uint32_t i = 0; i < count; i++)
        push_element(b, start + i);
}

void nv2a_vtx_batch_element16(NV2AVtxBatch *b, uint32_t param)
{
    expand_last_run(b);
    push_element(b, param & 0xFFFF);
    push_element(b, param >> 16);
}

void nv2a_vtx_batch_element32(NV2AVtxBatch *b, uint32_t param)
{
    expand_last_run(b);
    push_element(b, param);
}

void nv2a_vtx_batch_draw_arrays(NV2AVtxBatch *b, uint32_t param)
{
    uint32_t start = param & 0x00FFFFFF;
    uint32_t count = (param >> 24) + 1;

    if (b->element_count) {
        for (uint32_t i = 0; i < count; i++)
            push_element(b, start + i);
        return;
    }
    if (b->run_total > 0) {
        uint32_t last = b->run_total - 1;
        if (start == b->run_start[last] + b->run_count[last]) {
            b->run_count[last] += count;
            return;
        }
    }
    if (b->run_total == NV2A_VTX_MAX_RUNS) {
        /* Out of run slots: fall back to one element list. Joins strips that
         * should stay separate, but only past a limit xemu asserts on. */
        for (uint32_t r = 0; r < b->run_total; r++)
            for (uint32_t i = 0; i < b->run_count[r]; i++)
                push_element(b, b->run_start[r] + i);
        b->run_total = 0;
        for (uint32_t i = 0; i < count; i++)
            push_element(b, start + i);
        return;
    }
    b->run_start[b->run_total] = start;
    b->run_count[b->run_total] = count;
    b->run_total++;
}

int nv2a_vtx_gather(const NV2AVtxAttr attrs[NV2A_VTX_ATTRS],
                    const uint32_t *elements, uint32_t n,
                    NV2AVtxSpanFn span, void *ctx,
                    const uint32_t dst_offset[NV2A_VTX_ATTRS],
                    uint32_t dst_stride, uint8_t *dst)
{
    uint32_t max_e = 0;
    for (uint32_t i = 0; i < n; i++)
        if (elements[i] > max_e)
            max_e = elements[i];

    for (int slot = 0; slot < NV2A_VTX_ATTRS; slot++) {
        const NV2AVtxAttr *a = &attrs[slot];
        if (a->count == 0)
            continue;
        size_t bytes = (size_t)a->size * a->count;
        if ((size_t)dst_offset[slot] + bytes > dst_stride)
            return -1;

        /* Resolve the whole reachable range once, so every element read below
         * is inside a span the resolver bounds-checked. */
        uint64_t span_len = a->stride ? (uint64_t)max_e * a->stride + bytes : bytes;
        if ((uint64_t)a->offset + span_len > 0xFFFFFFFFu)
            return -1;
        const uint8_t *src = span(ctx, a->dma_select, a->offset, (size_t)span_len);
        if (!src)
            return -1;

        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *p = a->stride ? src + (size_t)elements[i] * a->stride : src;
            memcpy(dst + (size_t)i * dst_stride + dst_offset[slot], p, bytes);
        }
    }
    return 0;
}
