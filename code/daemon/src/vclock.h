#ifndef VCLOCK_H
#define VCLOCK_H

#include <stdint.h>
#include <stdatomic.h>
#include "../include/drs_sync_config.h"

/*
 * Virtual clock state protected by a seqlock.
 *
 * Writer (sync thread): increment seq to odd → write fields → increment to even.
 * Reader (agent, pulse): spin while seq is odd or changes between two reads.
 *
 * All time values are nanoseconds.
 * Rate is Q32.32 fixed-point: nominal = 0x100000000 (= 1.0).
 */
typedef struct {
    _Atomic uint32_t seq;       /* seqlock counter */
    uint32_t         _pad;

    /* Protected by seqlock */
    int64_t  base_local;        /* CLOCK_MONOTONIC_RAW reference point */
    int64_t  base_global;       /* virtual time at last rate/step change */
    uint64_t rate_q32;          /* Q32.32 rate, nominal = DRS_RATE_NOMINAL_Q32 */
    int64_t  offset_ns;         /* cumulative phase offset applied to vClock */
    int64_t  lat_corr_ns;       /* static loopback latency correction */
} vclock_t;

/* Initialise with current CLOCK_MONOTONIC_RAW as base, rate = nominal, offset = 0 */
void vclock_init(vclock_t *vc, int64_t t_local_raw);

/* Read current virtual time.  t_local_raw = clock_gettime(CLOCK_MONOTONIC_RAW). */
int64_t vclock_now(const vclock_t *vc, int64_t t_local_raw);

/*
 * Apply a phase step:  offset_ns += delta_ns.
 * base_local and base_global are updated simultaneously so T_global is continuous.
 * t_now = current CLOCK_MONOTONIC_RAW at the moment of the step.
 */
void vclock_step_offset(vclock_t *vc, int64_t delta_ns, int64_t t_now);

/*
 * Update rate (Q32.32).  base_local / base_global are latched so there is no
 * discontinuity in the virtual time reading.
 */
void vclock_set_rate(vclock_t *vc, uint64_t new_rate_q32, int64_t t_now);

/*
 * Inverse mapping: given a target global time, return the CLOCK_MONOTONIC_RAW
 * instant at which vclock_now() will equal t_global_ns.
 * Used by pulse_engine to schedule timerfd wakeups.
 */
int64_t vclock_local_for_global(const vclock_t *vc, int64_t t_global_ns);

/* Set latency correction (updated after each calibration). */
void vclock_set_lat_corr(vclock_t *vc, int64_t lat_corr_ns);

/* Read the current rate (Q32.32) without holding any lock. */
uint64_t vclock_rate(const vclock_t *vc);

#endif /* VCLOCK_H */
