#include <stdio.h>
#include <stdint.h>
#include "../src/min_delay.h"
#include "../include/drs_sync_config.h"

int main(void)
{
    min_delay_t f;
    min_delay_init(&f);

    /* Feed 10 identical samples of 100 µs */
    for (int i = 0; i < 10; i++) {
        int accepted = min_delay_update(&f, 100000);
        if (!accepted) {
            printf("FAIL test_min_delay: first 10 samples should be accepted\n");
            return 1;
        }
    }
    if (min_delay_min(&f) != 100000) {
        printf("FAIL test_min_delay: min should be 100000, got %lld\n",
               (long long)min_delay_min(&f));
        return 1;
    }

    /* An outlier 300 µs above min should be rejected */
    int r = min_delay_update(&f, 100000 + 300000);
    if (r != 0) {
        printf("FAIL test_min_delay: outlier (300µs above) should be rejected\n");
        return 1;
    }

    /* A sample exactly at min + tolerance should be accepted */
    r = min_delay_update(&f, 100000 + DRS_MIN_DELAY_TOLERANCE_NS);
    if (r != 1) {
        printf("FAIL test_min_delay: sample at min+tolerance should be accepted\n");
        return 1;
    }

    /* Reset test */
    min_delay_reset(&f);
    if (min_delay_min(&f) != INT64_MAX) {
        printf("FAIL test_min_delay: min after reset should be INT64_MAX\n");
        return 1;
    }

    printf("PASS test_min_delay\n");
    return 0;
}
