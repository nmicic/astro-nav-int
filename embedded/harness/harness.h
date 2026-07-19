/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Nenad Micic <nenad@micic.be>
 *
 * harness.h -- the profile contract and shared test-schedule helpers.
 *
 * Each profile translation unit defines profile_name and profile_run().
 * profile_run() executes a fixed, deterministic schedule of library
 * calls and folds every output value into one FNV-1a hash. The same
 * translation unit links on the host (harness/host_main.c) and on every
 * embedded target (harness/start_*.c + harness/harness_main.c), and the
 * printed hash must be identical everywhere: the host runs the native
 * __int128 backend, the 32-bit targets run the portable two-limb
 * backend, so hash equality is a cross-ISA, cross-backend bit-exactness
 * proof over the profile's whole output surface.
 */

#ifndef EMB_HARNESS_H
#define EMB_HARNESS_H

#include <stdint.h>

extern const char profile_name[];
uint64_t profile_run(void);

/* FNV-1a over little-endian value bytes, same construction as the
 * repo's golden battery. Signed values go through their unsigned
 * counterpart of the SAME width first, so sign extension never depends
 * on the caller's cast habits. */
static inline uint64_t fnv1a_init(void)
{
    return 0xcbf29ce484222325ULL;
}

static inline uint64_t fnv1a_u64(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static inline uint64_t fnv1a_i64(uint64_t h, int64_t v)
{
    return fnv1a_u64(h, (uint64_t)v);
}

static inline uint64_t fnv1a_i32(uint64_t h, int32_t v)
{
    return fnv1a_u64(h, (uint32_t)v);
}

/* Deterministic schedule generator: Knuth's MMIX LCG, top bits only
 * (the low bits of any power-of-two LCG are weak). Every profile seeds
 * it with its own constant, so schedules never depend on link order or
 * on each other. */
static inline uint64_t emb_rng_next(uint64_t *s)
{
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s >> 11;
}

/* Uniform-ish integer in [lo, hi], span < 2^32. Modulo bias is
 * irrelevant here: the schedule needs determinism and domain coverage,
 * not statistical uniformity. */
static inline int32_t emb_rng_range(uint64_t *s, int32_t lo, int32_t hi)
{
    uint32_t span = (uint32_t)(hi - lo) + 1u;
    return lo + (int32_t)(emb_rng_next(s) % span);
}

/* Uniform-ish int64 in [-abs_max, abs_max]; 2*abs_max + 1 must fit the
 * generator's 53 output bits, which every library time domain does. */
static inline int64_t emb_rng_ms(uint64_t *s, int64_t abs_max)
{
    uint64_t span = (uint64_t)abs_max * 2u + 1u;
    return (int64_t)(emb_rng_next(s) % span) - abs_max;
}

static inline int32_t emb_clip(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

#endif /* EMB_HARNESS_H */
