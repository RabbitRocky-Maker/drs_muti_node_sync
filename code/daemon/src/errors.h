#ifndef ERRORS_H
#define ERRORS_H

#include <stdint.h>
#include <stddef.h>
#include "../include/drs_sync_config.h"

typedef struct {
    uint32_t code;
    uint32_t count;
    int64_t  timestamp_ns;
    char     msg[48];
} drs_error_entry_t;

typedef struct {
    _Atomic uint32_t  head;   /* next write index (monotone) */
    uint32_t          _pad;
    drs_error_entry_t slots[DRS_ERROR_RING_SLOTS];
} drs_error_ring_t;

/*
 * Open (create if needed) /dev/shm/drs-sync.errors and mmap it.
 * Returns 0 on success, -1 on failure.
 */
int errors_init(void);
void errors_close(void);

/* Atomically push one error entry (O(1), no lock). */
void errors_push(uint32_t code, const char *msg);

/*
 * Drain up to `max` entries into `out`, starting from *offset.
 * Updates *offset to the next unread position.
 * Returns number of entries written.
 */
int errors_drain(drs_error_entry_t *out, int max, uint32_t *offset);

#endif /* ERRORS_H */
