/* Port-local copy of PS3Recomp libs/video/rsx_vertex_compact.c.
 * Copyright (c) 2026 Ned Heller / sp00nznet, MIT (see LICENSE).
 * Exact four-component SIMD decode paths; all other paths are unchanged. */
#if defined(__SSE4_1__)
#include <immintrin.h>
#endif
#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
#if defined(__SSE4_1__)
    /* Read exactly the attribute's bytes, including for packed u8 streams.
     * Division (not reciprocal multiplication) preserves scalar rounding. */
    if (size == 4) {
        if (type == RSX_VTX_TYPE_FLOAT) {
            const __m128i bytes = _mm_loadu_si128((const __m128i*)source);
            const __m128i words = _mm_shuffle_epi8(bytes,
                _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12));
            _mm_storeu_ps(out, _mm_castsi128_ps(words));
            return 1;
        }
        if (type == RSX_VTX_TYPE_UNORM8 || type == RSX_VTX_TYPE_UINT8) {
            u32 packed; memcpy(&packed, source, sizeof packed);
            __m128 values = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)packed)));
            if (type == RSX_VTX_TYPE_UNORM8) values = _mm_div_ps(values, _mm_set1_ps(255.0f));
            _mm_storeu_ps(out, values);
            return 1;
        }
    }
#endif
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (frequency 0/1) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (fetch->desc.frequency <= 1) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix =
            (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        float* value = (float*)(out + slot * 16u);
        if (!fetch->desc.type || !fetch->desc.size) {
            memcpy(value, fetch->default_value, 16);
            continue;
        }

        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (plan->divider_mask >> attr) & 1u);
        const u8* source = NULL;
        if (fetch->stable_base &&
            element >= fetch->stable_first && element <= fetch->stable_last) {
            source = fetch->stable_base +
                (u64)(element - fetch->stable_first) * fetch->stride;
        } else if (plan->inline_data) {
            /* An inline draw has no vertex array to fall back to: the array
             * offsets still hold whatever the last ordinary draw bound, and
             * reading them would feed this draw another mesh's vertices.
             * An attribute the stream does not supply gets its default,
             * which is what the hardware feeds. */
            source = NULL;
        } else if (plan->guest_ptr) {
            const u32 offset =
                plan->base_offset + fetch->desc.offset +
                element * fetch->stride;
            source = plan->guest_ptr(
                plan->guest_user, fetch->desc.location, offset,
                fetch->elem_size);
        }

        if (source && rsx_vertex_decode_element(
                fetch->desc.type, fetch->desc.size, source, value))
            continue;
        if (attr == 0)
            return 0;
        memcpy(value, fetch->default_value, 16);
    }
    return 1;
}

/* Inline forms of rsx_vertex_decode_element for the formats STG02 combat
 * decodes most (share of decoded attributes): FLOAT x3 27%, UNORM8 x4 26%,
 * HALF x2 21%, UINT8 x4 13%, CMP32 9%, SNORM16 x4 4%. Through the call each
 * was 11% of the frame clock thread. Same arithmetic, same results. */
enum {
    VTX_KIND_CALL, VTX_KIND_FLOAT3, VTX_KIND_FLOAT4, VTX_KIND_UNORM8X4,
    VTX_KIND_UINT8X4, VTX_KIND_HALF, VTX_KIND_CMP32, VTX_KIND_SNORM16
};

static u32 rsx_vertex_decode_kind(u32 type, u32 size)
{
    switch (type) {
    case RSX_VTX_TYPE_FLOAT:   return size <= 3 ? VTX_KIND_FLOAT3 : VTX_KIND_FLOAT4;
    case RSX_VTX_TYPE_HALF:    return VTX_KIND_HALF;
    case RSX_VTX_TYPE_CMP32:   return VTX_KIND_CMP32;
    case RSX_VTX_TYPE_SNORM16: return VTX_KIND_SNORM16;
#if defined(__SSE4_1__)
    case RSX_VTX_TYPE_UNORM8:  return size == 4 ? VTX_KIND_UNORM8X4 : VTX_KIND_CALL;
    case RSX_VTX_TYPE_UINT8:   return size == 4 ? VTX_KIND_UINT8X4 : VTX_KIND_CALL;
#endif
    default:                   return VTX_KIND_CALL;
    }
}

/* Decodes one element of a kind other than VTX_KIND_CALL. size is 1..4 for
 * the float kinds; rsx_vertex_decode_element's own loop stops at 4. */
static inline __attribute__((always_inline)) void rsx_vertex_decode_inline(
    u32 kind, u32 size, const u8* source, float value[4])
{
    switch (kind) {
    case VTX_KIND_FLOAT3:
        value[0] = rsx_compact_be_f32(source);
        value[1] = size > 1 ? rsx_compact_be_f32(source + 4) : 0.0f;
        value[2] = size > 2 ? rsx_compact_be_f32(source + 8) : 0.0f;
        value[3] = 1.0f;
        return;
#if defined(__SSE4_1__)
    case VTX_KIND_FLOAT4: {
        const __m128i bytes = _mm_loadu_si128((const __m128i*)source);
        const __m128i words = _mm_shuffle_epi8(bytes,
            _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12));
        _mm_storeu_ps(value, _mm_castsi128_ps(words));
        return;
    }
    case VTX_KIND_UNORM8X4:
    case VTX_KIND_UINT8X4: {
        u32 packed; memcpy(&packed, source, sizeof packed);
        __m128 v = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)packed)));
        if (kind == VTX_KIND_UNORM8X4) v = _mm_div_ps(v, _mm_set1_ps(255.0f));
        _mm_storeu_ps(value, v);
        return;
    }
#else
    case VTX_KIND_FLOAT4:
        for (u32 c = 0; c < 4; c++) value[c] = rsx_compact_be_f32(source + c * 4);
        return;
#endif
    case VTX_KIND_HALF:
        value[0] = rsx_compact_be_f16(source);
        value[1] = size > 1 ? rsx_compact_be_f16(source + 2) : 0.0f;
        value[2] = size > 2 ? rsx_compact_be_f16(source + 4) : 0.0f;
        value[3] = size > 3 ? rsx_compact_be_f16(source + 6) : 1.0f;
        return;
    case VTX_KIND_SNORM16:
        for (u32 c = 0; c < 4; c++) {
            if (c < size) {
                const s16 v = (s16)((source[c * 2] << 8) | source[c * 2 + 1]);
                value[c] = v / 32767.0f;
            } else {
                value[c] = c == 3 ? 1.0f : 0.0f;
            }
        }
        return;
    case VTX_KIND_CMP32: {
        const u32 word = ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                         ((u32)source[2] << 8) | (u32)source[3];
        s32 x = (s32)(word & 0x7FF);
        s32 y = (s32)((word >> 11) & 0x7FF);
        s32 z = (s32)((word >> 22) & 0x3FF);
        if (x & 0x400) x -= 0x800;
        if (y & 0x400) y -= 0x800;
        if (z & 0x200) z -= 0x400;
        value[0] = (float)x / 1023.0f;
        value[1] = (float)y / 1023.0f;
        value[2] = (float)z / 511.0f;
        value[3] = 1.0f;
        return;
    }
    }
}

/* rsx_vertex_fetch_one for n references, attribute by attribute: the format
 * is looked at once per attribute instead of once per vertex, and the common
 * formats are converted inline (rsx_vertex_decode_inline). Elements outside
 * the stable range take rsx_vertex_fetch_one's own path. Output is byte for
 * byte what n calls of rsx_vertex_fetch_one write (PORT_VERTEX_VERIFY=1 in
 * rsx_engine_bridge.c checks it). */
int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 n, u8* out)
{
    if (!plan || (!refs && n))
        return 0;
    const u32 stride = plan->layout.stride;
    if (!out && stride)
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        u8* dst = out + slot * 16u;
        if (!fetch->desc.type || !fetch->desc.size) {
            for (u32 i = 0; i < n; i++) memcpy(dst + (u64)i * stride, fetch->default_value, 16);
            continue;
        }
        const u32 type = fetch->desc.type, size = fetch->desc.size;
        const u32 divider = (plan->divider_mask >> attr) & 1u;
        const u32 kind = rsx_vertex_decode_kind(type, size);
        for (u32 i = 0; i < n; i++) {
            float* value = (float*)(dst + (u64)i * stride);
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index, fetch->desc.frequency, divider);
            if (fetch->stable_base &&
                element >= fetch->stable_first && element <= fetch->stable_last) {
                const u8* source = fetch->stable_base +
                    (u64)(element - fetch->stable_first) * fetch->stride;
                if (kind != VTX_KIND_CALL) {
                    rsx_vertex_decode_inline(kind, size, source, value);
                    continue;
                }
                if (rsx_vertex_decode_element(type, size, source, value))
                    continue;
                if (attr == 0)
                    return 0;
                memcpy(value, fetch->default_value, 16);
                continue;
            }
            /* Not in the stable range: exactly rsx_vertex_fetch_one's path. */
            const u8* source = NULL;
            if (plan->inline_data) {
                source = NULL;
            } else if (plan->guest_ptr) {
                const u32 offset = plan->base_offset + fetch->desc.offset + element * fetch->stride;
                source = plan->guest_ptr(plan->guest_user, fetch->desc.location, offset, fetch->elem_size);
            }
            if (source && rsx_vertex_decode_element(type, size, source, value))
                continue;
            if (attr == 0)
                return 0;
            memcpy(value, fetch->default_value, 16);
        }
    }
    return 1;
}

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    /* A mesh's references usually share one base index and span a compact
     * range of vertex ids: number them through a table indexed by the id
     * instead of hashing. Same result, first-occurrence order included.
     * Only the FIFO walker's thread builds remaps. */
    {
        static __thread u32* stamp;
        static __thread u32* value;
        static __thread u32 cap, gen;
        const u32 base = refs[0].base_index;
        u32 lo = refs[0].vertex_id, hi = lo, same = 1;
        for (u32 i = 1; i < count; i++) {
            const u32 v = refs[i].vertex_id;
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
            same &= refs[i].base_index == base;
        }
        const u64 span = (u64)hi - lo + 1u;
        if (same && span <= (u64)count * 4u + 4096u) {
            if (span > cap) {
                u32* ns = (u32*)realloc(stamp, (size_t)span * sizeof(u32));
                if (ns) stamp = ns;
                u32* nv = ns ? (u32*)realloc(value, (size_t)span * sizeof(u32)) : NULL;
                if (nv) value = nv;
                if (ns && nv) {
                    memset(stamp, 0, (size_t)span * sizeof(u32));
                    cap = (u32)span;
                    gen = 0;
                }
            }
            if (span <= cap) {
                if (++gen == 0) { memset(stamp, 0, (size_t)cap * sizeof(u32)); gen = 1; }
                u32 unique = 0;
                for (u32 occurrence = 0; occurrence < count; occurrence++) {
                    const rsx_vertex_ref ref = refs[occurrence];
                    const u32 k = ref.vertex_id - lo;
                    if (stamp[k] != gen) {
                        stamp[k] = gen;
                        value[k] = unique;
                        refs[unique] = ref;
                        remap->occurrence_to_unique[occurrence] = unique++;
                    } else {
                        remap->occurrence_to_unique[occurrence] = value[k];
                    }
                }
                *unique_count = unique;
                return 1;
            }
        }
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
