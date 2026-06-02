#ifndef PI_CTRL_H
#define PI_CTRL_H

#include <stdint.h>
#include "../include/drs_sync_config.h"

typedef struct {
    int64_t integrator_ns;   /* accumulated Ki·e (ns, no per-step truncation) */
    int     step_counter;    /* consecutive samples with |e| > threshold */
    int64_t step_accum_ns;   /* running sum of applied steps (F-33) */
} pi_ctrl_t;

void pi_ctrl_init(pi_ctrl_t *pi);

/*
 * Run one PI update.
 *
 * e_ns        = θ_residual (= θ + step_accum_ns).  Positive = follower behind.
 * period_ns   = actual elapsed time since last call (nominally 50 ms).
 * new_rate_q32 = output: updated Q32.32 rate.
 *
 * Returns 1 if a phase step was triggered (caller must apply it), 0 otherwise.
 *
 * When returning 1, the caller MUST call:
 *   vclock_step_offset(vc, +theta_residual, t_now);
 *   pi->step_accum_ns += -theta_residual;
 *   vclock_set_rate(vc, DRS_RATE_NOMINAL_Q32, t_now);
 *   pi_ctrl_init(pi);
 */
int pi_ctrl_update(pi_ctrl_t *pi, int64_t e_ns, int64_t period_ns,
                   uint64_t *new_rate_q32);

#endif /* PI_CTRL_H */
