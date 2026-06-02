#ifndef MIN_DELAY_H
#define MIN_DELAY_H

#include <stdint.h>
#include "../include/drs_sync_config.h"

typedef struct {
    int64_t  window[DRS_MIN_DELAY_WINDOW];
    int      count;
    int      head;     /* next write position (ring) */
    int64_t  current_min;
} min_delay_t;

void min_delay_init(min_delay_t *f);
void min_delay_reset(min_delay_t *f);

/*
 * Offer a new RTT sample.
 * Returns 1 if accepted (rtt <= current_min + DRS_MIN_DELAY_TOLERANCE_NS),
 *         0 if rejected as outlier.
 * Always adds the sample to the ring for future min computation.
 */
int min_delay_update(min_delay_t *f, int64_t rtt_ns);

/* Current minimum RTT in the window */
int64_t min_delay_min(const min_delay_t *f);

#endif /* MIN_DELAY_H */
