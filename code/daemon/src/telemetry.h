#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdatomic.h>
#include "../include/drs_sync_config.h"

/*
 * 128-byte shared-memory layout (Seqlock, single writer = drs_syncd).
 *
 * Memory map (all offsets relative to struct start):
 *   0   seqlock(4) pid(4) version(4)
 *  12   state(4) flags(4) leader_id(4) term(4) _pad(4)
 *  32   offset_ns(8) rate(8) lat_corr(8) last_rtt(8)
 *  64   convergence(8)
 *  72   holdover_ms(4) err_last(4) err_count(4) pulse(4) lock_mode(4)
 *  92   [padding 4 bytes — reserved field keeping int64 alignment]
 *  96   sync_accepted(4) sync_rejected(4) step_count(4) holdover_count(4)
 * 112   reserved[16]
 * 128   END
 *
 * _pad_align is an explicit padding field to fix the 4-byte alignment gap
 * that the compiler would otherwise insert silently between election_term
 * and offset_ns (uint32 → int64 boundary).
 */
typedef struct {
    _Atomic uint32_t seqlock;              /* 0  — even=ok, odd=writer active */
    uint32_t         pid;                  /* 4  */
    uint32_t         version;              /* 8  — DRS_TELEMETRY_VERSION */

    volatile uint32_t state;              /* 12 — drs_state_t */
    volatile uint32_t flags;              /* 16 — DRS_FLAG_* bitmask */
    volatile uint32_t leader_node_id;     /* 20 */
    volatile uint32_t election_term;      /* 24 */
    uint32_t          _pad_align;         /* 28 — explicit pad for int64 alignment */

    volatile int64_t  offset_ns;          /* 32 — θ_residual (not raw θ) */
    volatile int64_t  rate_q32_32;        /* 40 */
    volatile int64_t  latency_corr_ns;    /* 48 */
    volatile int64_t  last_rtt_ns;        /* 56 */

    volatile uint64_t convergence_unix_ns;/* 64 — set when |offset|<100µs for 10s */
    volatile uint32_t holdover_remaining_ms; /* 72 */
    volatile uint32_t error_code_last;    /* 76 */
    volatile uint32_t error_count;        /* 80 */
    volatile uint32_t pulse_count;        /* 84 */
    volatile uint32_t lock_mode;          /* 88 */
    uint32_t          _pad2;              /* 92 — reserved, keeps counters aligned */

    /* Counters — populated by statemachine, displayed by drs_mon */
    volatile uint32_t sync_accepted;      /* 96  — samples accepted by min-delay filter */
    volatile uint32_t sync_rejected;      /* 100 — outliers rejected */
    volatile uint32_t step_count;         /* 104 — phase steps applied (large jumps) */
    volatile uint32_t holdover_count;     /* 108 — how many times HOLDOVER was entered */

    uint8_t  reserved[16];               /* 112 — future use */
} drs_telemetry_t;                       /* 128 bytes total */

_Static_assert(sizeof(drs_telemetry_t) == 128, "telemetry struct must be 128 bytes");

/*
 * Create/open /dev/shm/drs-sync.state and mmap it.
 * Returns 0 on success, -1 on failure.
 */
int telemetry_init(void);
void telemetry_close(void);

/* Get a pointer to the live telemetry region (valid after telemetry_init). */
drs_telemetry_t *telemetry_get(void);

/*
 * Write-lock helpers (seqlock writer protocol):
 *   telemetry_begin_write() → modify fields → telemetry_end_write()
 */
void telemetry_begin_write(void);
void telemetry_end_write(void);

#endif /* TELEMETRY_H */
