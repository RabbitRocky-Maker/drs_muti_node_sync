#ifndef PULSE_ENGINE_H
#define PULSE_ENGINE_H

#include <stdint.h>
#include <pthread.h>
#include "vclock.h"

typedef struct {
    int     timer_fd;       /* timerfd (CLOCK_MONOTONIC_RAW) for rising edge */
    int     pulse_low_fd;   /* timerfd (CLOCK_MONOTONIC_RAW) for falling edge */
    uint32_t pulse_count;   /* monotone counter, read by telemetry */

    /* GPIO 23 health state */
    int      stable;
    int64_t  stable_since_ns;

    /* Dedicated pulse thread */
    vclock_t        *vc;                /* non-owning pointer, set by pulse_engine_start */
    pthread_t        thread;
    int              thread_started;
    volatile int     stop;
    volatile int64_t rise_target_raw_ns; /* CLOCK_MONOTONIC_RAW deadline for next rising edge */
} pulse_engine_t;

/* Create timerfds.  Returns 0 on success. */
int  pulse_engine_init(pulse_engine_t *pe);

/*
 * Connect vclock and start the dedicated pulse thread.
 * Must be called after pulse_engine_init and after the vclock is ready.
 * Returns 0 on success.
 */
int  pulse_engine_start(pulse_engine_t *pe, vclock_t *vc);

void pulse_engine_close(pulse_engine_t *pe);

/*
 * Schedule the next rising edge.
 * Must be called once after calibration completes; the thread self-replenishes afterward.
 */
void pulse_engine_arm(pulse_engine_t *pe, vclock_t *vc);

/*
 * Update GPIO 23 health indicator based on current |offset_ns| and time.
 * Call every 50 ms tick.
 */
void pulse_engine_update_health(pulse_engine_t *pe, int64_t offset_ns,
                                int in_holdover);

#endif /* PULSE_ENGINE_H */
