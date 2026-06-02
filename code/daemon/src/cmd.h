#ifndef CMD_H
#define CMD_H

#include <stdint.h>
#include "../include/drs_sync_config.h"

typedef struct {
    volatile uint32_t seq;       /* monotone; atomic compare-and-swap */
    volatile uint32_t code;      /* drs_cmd_* constant */
    volatile uint64_t unix_ns;   /* diagnostic timestamp */
    uint8_t           payload[48];
} drs_cmd_channel_t;

/*
 * Open (create if needed) /dev/shm/drs-sync.cmd and mmap it.
 * Returns 0 on success, -1 on failure.
 */
int cmd_init(void);
void cmd_close(void);

/*
 * Poll for a new command.  Returns command code (DRS_CMD_*) if a new command
 * has arrived since the last call, DRS_CMD_NONE otherwise.
 * Thread-safe (reads atomic seq).
 */
uint32_t cmd_poll(void);

/*
 * Send a command (used by agent via the same mmap region).
 * Returns 0 on success.
 */
int cmd_send(uint32_t code);

#endif /* CMD_H */
