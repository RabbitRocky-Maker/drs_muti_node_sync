#ifndef CALIBRATE_H
#define CALIBRATE_H

#include <stdint.h>
#include "vclock.h"

/*
 * Measure loopback latency via UDP on 127.0.0.1:DRS_CALIB_LOOPBACK_PORT.
 * Collects DRS_CALIB_SAMPLES round-trips, rejects outliers >min+20 µs,
 * result = min / 2 (one-way latency).
 *
 * On success, writes lat_corr_ns into `vc` and returns 0.
 * On failure (too few valid samples after max retries), returns -1.
 */
int calibrate_loopback(vclock_t *vc);

#endif /* CALIBRATE_H */
