#include "min_delay.h"
#include <string.h>
#include <stdint.h>

void min_delay_init(min_delay_t *f)
{
    min_delay_reset(f);
}

void min_delay_reset(min_delay_t *f)
{
    memset(f->window, 0, sizeof(f->window));
    f->count       = 0;
    f->head        = 0;
    f->current_min = INT64_MAX;
}

int min_delay_update(min_delay_t *f, int64_t rtt_ns)
{
    /* Always insert into the ring (affects future min) */
    f->window[f->head] = rtt_ns;
    f->head = (f->head + 1) % DRS_MIN_DELAY_WINDOW;
    if (f->count < DRS_MIN_DELAY_WINDOW)
        f->count++;

    /* Recompute minimum across all valid slots */
    int64_t mn = INT64_MAX;
    for (int i = 0; i < f->count; i++) {
        int idx = (f->head - f->count + i + DRS_MIN_DELAY_WINDOW) % DRS_MIN_DELAY_WINDOW;
        if (f->window[idx] < mn)
            mn = f->window[idx];
    }
    f->current_min = mn;

    /* Accept if within tolerance of the minimum */
    return (rtt_ns <= mn + DRS_MIN_DELAY_TOLERANCE_NS) ? 1 : 0;
}

int64_t min_delay_min(const min_delay_t *f)
{
    return f->current_min;
}
