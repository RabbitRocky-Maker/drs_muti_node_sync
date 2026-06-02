#include "pi_ctrl.h"

#include <stdint.h>

void pi_ctrl_init(pi_ctrl_t *pi)
{
    pi->integrator_ns = 0;
    pi->step_counter  = 0;
    /* step_accum_ns is a running sum — preserved across PI resets */
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int pi_ctrl_update(pi_ctrl_t *pi, int64_t e_ns, int64_t period_ns,
                   uint64_t *new_rate_q32)
{
    /* Step-mode check (F-29: 3 consecutive samples) */
    if (e_ns > DRS_STEP_THRESHOLD_NS || e_ns < -DRS_STEP_THRESHOLD_NS) {
        pi->step_counter++;
        if (pi->step_counter >= DRS_STEP_CONFIRM_SAMPLES) {
            /* Caller handles the step and MUST call pi_ctrl_init() afterward */
            *new_rate_q32 = DRS_RATE_NOMINAL_Q32;
            return 1; /* step triggered */
        }
    } else {
        pi->step_counter = 0;
    }

    /*
     * Slew mode — F-35 sign-symmetric arithmetic:
     *   /65536 (not >>16) prevents sign-extension on negative int64
     *   ×1000000LL gives dimensional ppm (not dimensionless fraction)
     */
    int64_t kp_term_ppm = (int64_t)DRS_PI_KP_Q16 * e_ns * INT64_C(1000000)
                          / period_ns / 65536;

    /*
     * Integral accumulation at full ns precision.
     * The old formula ki_term_ppm = Ki * e * 1e6 / T / 2^16 truncates
     * to 0 for |e| < ~10 µs, creating a dead zone.  Instead we accumulate
     * Ki * e / 2^16 (in ns) and only convert to ppm on output.
     */
    int64_t ki_delta_ns = (int64_t)DRS_PI_KI_Q16 * e_ns / 65536;
    pi->integrator_ns += ki_delta_ns;

    /* Anti-windup: clamp integral so output stays within ±DRS_RATE_MAX_PPM */
    int64_t max_integral_ns = DRS_RATE_MAX_PPM * period_ns
                              * INT64_C(65536)
                              / ((int64_t)DRS_PI_KI_Q16 * INT64_C(1000000));
    pi->integrator_ns = clamp64(pi->integrator_ns,
                                -max_integral_ns, max_integral_ns);

    int64_t ki_term_ppm = pi->integrator_ns * INT64_C(1000000) / period_ns;
    ki_term_ppm = clamp64(ki_term_ppm, -DRS_RATE_MAX_PPM, DRS_RATE_MAX_PPM);

    int64_t total_ppm = clamp64(kp_term_ppm + ki_term_ppm,
                                -DRS_RATE_MAX_PPM, DRS_RATE_MAX_PPM);

    int64_t rate_delta = total_ppm * DRS_RATE_PPM_Q32;
    int64_t new_rate   = (int64_t)DRS_RATE_NOMINAL_Q32 + rate_delta;

    if (new_rate < (int64_t)DRS_RATE_MIN_Q32) new_rate = (int64_t)DRS_RATE_MIN_Q32;
    if (new_rate > (int64_t)DRS_RATE_MAX_Q32) new_rate = (int64_t)DRS_RATE_MAX_Q32;

    *new_rate_q32 = (uint64_t)new_rate;
    return 0;
}
