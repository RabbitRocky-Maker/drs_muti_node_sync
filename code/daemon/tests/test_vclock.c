#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/vclock.h"
#include "../include/drs_sync_config.h"

int main(void)
{
    vclock_t vc;
    int64_t t0 = INT64_C(1000000000);   /* arbitrary start */
    vclock_init(&vc, t0);

    /* 1. At t0, vclock_now() should return 0 (base_global=0, offset=0) */
    int64_t g0 = vclock_now(&vc, t0);
    if (g0 != 0) {
        printf("FAIL test_vclock: initial vclock_now != 0, got %lld\n", (long long)g0);
        return 1;
    }

    /* 2. After 1 s of real time at nominal rate, virtual time should advance ~1 s */
    int64_t t1 = t0 + INT64_C(1000000000);
    int64_t g1 = vclock_now(&vc, t1);
    int64_t drift = g1 - INT64_C(1000000000);
    if (drift < -1000 || drift > 1000) {
        /* Allow ±1 µs rounding */
        printf("FAIL test_vclock: after 1s, drift = %lld ns (expected ~0)\n",
               (long long)drift);
        return 1;
    }

    /* 3. Rate scaling: 1000 ppm fast → after 1s virtual should be +1ms ahead */
    vclock_init(&vc, t0);
    uint64_t fast_rate = DRS_RATE_NOMINAL_Q32 + (uint64_t)(DRS_RATE_MAX_PPM * DRS_RATE_PPM_Q32);
    vclock_set_rate(&vc, fast_rate, t0);
    int64_t g_fast = vclock_now(&vc, t1);
    int64_t expected_fast = INT64_C(1000000000) + INT64_C(1000000); /* +1 ms */
    int64_t err_fast = g_fast - expected_fast;
    if (err_fast < -2000 || err_fast > 2000) {
        printf("FAIL test_vclock: 1000ppm fast rate wrong, err=%lld ns\n",
               (long long)err_fast);
        return 1;
    }

    /* 4. Step offset: +500µs step should be immediately visible */
    vclock_init(&vc, t0);
    vclock_step_offset(&vc, INT64_C(500000), t0);
    int64_t g_step = vclock_now(&vc, t0);
    /* After step, base_global = old_g0 + 500000 = 500000 */
    if (g_step != INT64_C(500000)) {
        printf("FAIL test_vclock: after step, vclock_now = %lld, expected 500000\n",
               (long long)g_step);
        return 1;
    }

    /* 5. Inverse mapping: vclock_local_for_global should round-trip */
    vclock_init(&vc, t0);
    vclock_set_lat_corr(&vc, INT64_C(4000));
    int64_t target_global = INT64_C(500000000);
    int64_t t_local = vclock_local_for_global(&vc, target_global);
    int64_t g_check = vclock_now(&vc, t_local);
    /* g_check should equal target_global within a few ns (integer arithmetic) */
    int64_t inv_err = g_check - target_global;
    if (inv_err < -100 || inv_err > 100) {
        printf("FAIL test_vclock: inverse error = %lld ns\n", (long long)inv_err);
        return 1;
    }

    printf("PASS test_vclock\n");
    return 0;
}
