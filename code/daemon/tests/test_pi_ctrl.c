#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/pi_ctrl.h"
#include "../include/drs_sync_config.h"

int main(void)
{
    pi_ctrl_t pi;
    pi_ctrl_init(&pi);

    /* 1. Step trigger: 3 consecutive samples with |e| > 1 ms */
    uint64_t rate = 0;
    for (int i = 0; i < DRS_STEP_CONFIRM_SAMPLES - 1; i++) {
        int step = pi_ctrl_update(&pi, INT64_C(2000000), DRS_PI_UPDATE_PERIOD_NS, &rate);
        if (step) {
            printf("FAIL test_pi_ctrl: step triggered too early (i=%d)\n", i);
            return 1;
        }
    }
    int step = pi_ctrl_update(&pi, INT64_C(2000000), DRS_PI_UPDATE_PERIOD_NS, &rate);
    if (!step) {
        printf("FAIL test_pi_ctrl: step not triggered after %d samples\n",
               DRS_STEP_CONFIRM_SAMPLES);
        return 1;
    }

    /* 2. After PI reset, slew mode should produce a rate != nominal for non-zero e */
    pi_ctrl_init(&pi);
    step = pi_ctrl_update(&pi, INT64_C(500000), DRS_PI_UPDATE_PERIOD_NS, &rate);
    if (step) {
        printf("FAIL test_pi_ctrl: step triggered for |e| < 1ms\n");
        return 1;
    }
    if (rate == DRS_RATE_NOMINAL_Q32) {
        printf("FAIL test_pi_ctrl: rate unchanged for non-zero error\n");
        return 1;
    }

    /* 3. Rate clamp: very large error should clamp to max */
    pi_ctrl_init(&pi);
    step = pi_ctrl_update(&pi, INT64_C(999999), DRS_PI_UPDATE_PERIOD_NS, &rate);
    if (rate > DRS_RATE_MAX_Q32) {
        printf("FAIL test_pi_ctrl: rate exceeds max\n");
        return 1;
    }
    if (rate < DRS_RATE_MIN_Q32) {
        printf("FAIL test_pi_ctrl: rate below min\n");
        return 1;
    }

    /* 4. Zero error should produce nominal rate */
    pi_ctrl_init(&pi);
    step = pi_ctrl_update(&pi, 0, DRS_PI_UPDATE_PERIOD_NS, &rate);
    if (step) {
        printf("FAIL test_pi_ctrl: step on zero error\n");
        return 1;
    }
    if (rate != DRS_RATE_NOMINAL_Q32) {
        printf("FAIL test_pi_ctrl: rate != nominal for zero error, got %llu\n",
               (unsigned long long)rate);
        return 1;
    }

    printf("PASS test_pi_ctrl\n");
    return 0;
}
