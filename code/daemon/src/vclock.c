#include "vclock.h"

#include <stdatomic.h>
#include <string.h>

/* ── Seqlock helpers ─────────────────────────────────────────────────── */

static inline void seqlock_write_begin(vclock_t *vc)
{
    uint32_t s = atomic_load_explicit(&vc->seq, memory_order_relaxed);
    atomic_store_explicit(&vc->seq, s + 1, memory_order_release);
}

static inline void seqlock_write_end(vclock_t *vc)
{
    uint32_t s = atomic_load_explicit(&vc->seq, memory_order_relaxed);
    atomic_store_explicit(&vc->seq, s + 1, memory_order_release);
}

/* ── Internal time calculation (no seqlock held) ─────────────────────── */

static int64_t calc_now(int64_t base_local, int64_t base_global,
                        uint64_t rate_q32, int64_t offset_ns,
                        int64_t lat_corr_ns, int64_t t_local_raw)
{
    /*
     * T_global = (T_local_raw - base_local) * Rate >> 32
     *            + base_global + offset_ns - lat_corr_ns
     *
     * Use __int128 for the multiply to avoid overflow.
     */
    int64_t delta = t_local_raw - base_local;
    __extension__ __int128 scaled = (__int128)delta * (int64_t)rate_q32;
    int64_t t_global = (int64_t)(scaled >> 32) + base_global + offset_ns - lat_corr_ns;
    return t_global;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void vclock_init(vclock_t *vc, int64_t t_local_raw)
{
    memset(vc, 0, sizeof(*vc));
    atomic_store_explicit(&vc->seq, 0, memory_order_relaxed);
    vc->base_local  = t_local_raw;
    vc->base_global = 0;
    vc->rate_q32    = DRS_RATE_NOMINAL_Q32;
    vc->offset_ns   = 0;
    vc->lat_corr_ns = 0;
}

int64_t vclock_now(const vclock_t *vc, int64_t t_local_raw)
{
    uint32_t s1, s2 = 0;
    int64_t result = 0;

    do {
        s1 = atomic_load_explicit(&vc->seq, memory_order_acquire);
        if (s1 & 1)
            continue; /* writer active, spin */

        result = calc_now(vc->base_local, vc->base_global,
                          vc->rate_q32, vc->offset_ns,
                          vc->lat_corr_ns, t_local_raw);

        s2 = atomic_load_explicit(&vc->seq, memory_order_acquire);
    } while (s1 != s2);

    return result;
}

void vclock_step_offset(vclock_t *vc, int64_t delta_ns, int64_t t_now)
{
    seqlock_write_begin(vc);

    /* Latch current virtual time as new base_global so there is no discontinuity */
    int64_t t_global_now = calc_now(vc->base_local, vc->base_global,
                                    vc->rate_q32, vc->offset_ns,
                                    vc->lat_corr_ns, t_now);
    vc->base_local  = t_now;
    vc->base_global = t_global_now + delta_ns;
    vc->offset_ns   = 0;

    seqlock_write_end(vc);
}

void vclock_set_rate(vclock_t *vc, uint64_t new_rate_q32, int64_t t_now)
{
    seqlock_write_begin(vc);

    /* Latch current virtual time before changing rate */
    int64_t t_global_now = calc_now(vc->base_local, vc->base_global,
                                    vc->rate_q32, vc->offset_ns,
                                    vc->lat_corr_ns, t_now);
    vc->base_local  = t_now;
    vc->base_global = t_global_now;
    vc->offset_ns   = 0;
    vc->rate_q32    = new_rate_q32;

    seqlock_write_end(vc);
}

int64_t vclock_local_for_global(const vclock_t *vc, int64_t t_global_ns)
{
    /*
     * Inverse of calc_now (ignoring lat_corr which is already baked into
     * the caller's target):
     *   y = t_global_ns - base_global - offset_ns + lat_corr_ns
     *   T_local_raw = base_local + (y << 32) / rate_q32
     */
    uint32_t s1, s2 = 0;
    int64_t result = 0;

    do {
        s1 = atomic_load_explicit(&vc->seq, memory_order_acquire);
        if (s1 & 1)
            continue;

        int64_t y = t_global_ns - vc->base_global - vc->offset_ns + vc->lat_corr_ns;
        __extension__ __int128 num = (__int128)y << 32;
        result = vc->base_local + (int64_t)(num / (int64_t)vc->rate_q32);

        s2 = atomic_load_explicit(&vc->seq, memory_order_acquire);
    } while (s1 != s2);

    return result;
}

void vclock_set_lat_corr(vclock_t *vc, int64_t lat_corr_ns)
{
    seqlock_write_begin(vc);
    vc->lat_corr_ns = lat_corr_ns;
    seqlock_write_end(vc);
}

uint64_t vclock_rate(const vclock_t *vc)
{
    return atomic_load_explicit((_Atomic uint64_t *)&vc->rate_q32, memory_order_relaxed);
}
