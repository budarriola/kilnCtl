#ifndef KILNLINK_BYTES_H
#define KILNLINK_BYTES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small by-hand little-endian byte packing helpers shared by
 * kilnlink_context.c / kilnlink_status.c. Deliberately NOT used by
 * kilnlink_frame.c, which predates this file and inlines its own -- not
 * worth touching working, tested code to save a few lines.
 *
 * CommonFW/README.md rule 5: "no memcpy of a struct... Serialize field by
 * field, little endian, by hand." A float is not a struct and there is no
 * struct-shaped memcpy here -- each helper moves exactly one scalar field,
 * by its own bytes, at a caller-chosen offset. Every caller is responsible
 * for the bounds check; these are unconditional writes/reads into a
 * caller-supplied buffer, same convention as kilnlink_frame.c. */

static inline void kilnlink_put_u16le(uint8_t *out, size_t off, uint16_t v)
{
    out[off] = (uint8_t)(v & 0xFFu);
    out[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t kilnlink_get_u16le(const uint8_t *in, size_t off)
{
    return (uint16_t)((uint16_t)in[off] | ((uint16_t)in[off + 1] << 8));
}

static inline void kilnlink_put_u32le(uint8_t *out, size_t off, uint32_t v)
{
    out[off] = (uint8_t)(v & 0xFFu);
    out[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    out[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    out[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t kilnlink_get_u32le(const uint8_t *in, size_t off)
{
    return (uint32_t)in[off] | ((uint32_t)in[off + 1] << 8) | ((uint32_t)in[off + 2] << 16) |
           ((uint32_t)in[off + 3] << 24);
}

static inline void kilnlink_put_f32le(uint8_t *out, size_t off, float v)
{
    union {
        float f;
        uint32_t u;
    } conv;
    conv.f = v;
    kilnlink_put_u32le(out, off, conv.u);
}

static inline float kilnlink_get_f32le(const uint8_t *in, size_t off)
{
    union {
        float f;
        uint32_t u;
    } conv;
    conv.u = kilnlink_get_u32le(in, off);
    return conv.f;
}

static inline void kilnlink_put_u64le(uint8_t *out, size_t off, uint64_t v)
{
    for (size_t i = 0; i < 8; ++i) {
        out[off + i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static inline uint64_t kilnlink_get_u64le(const uint8_t *in, size_t off)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v |= ((uint64_t)in[off + i]) << (8u * i);
    }
    return v;
}

static inline void kilnlink_put_f64le(uint8_t *out, size_t off, double v)
{
    union {
        double d;
        uint64_t u;
    } conv;
    conv.d = v;
    kilnlink_put_u64le(out, off, conv.u);
}

static inline double kilnlink_get_f64le(const uint8_t *in, size_t off)
{
    union {
        double d;
        uint64_t u;
    } conv;
    conv.u = kilnlink_get_u64le(in, off);
    return conv.d;
}

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_BYTES_H */
