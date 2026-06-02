#ifndef TELEMETRY_UDP_H
#define TELEMETRY_UDP_H

#include <stdint.h>

/* 40-byte telemetry record — little-endian, no framing */
typedef struct {
    int64_t  timestamp_ns;   /* 0: CLOCK_MONOTONIC_RAW */
    int32_t  state;          /* 8: drs_state_t */
    int64_t  offset_ns;      /* 12: clock offset from leader */
    int64_t  rtt_ns;         /* 20: round-trip time */
    int64_t  rate_q32;       /* 28: Q32.32 rate */
    uint32_t node_id;        /* 36: sender node ID */
} __attribute__((packed)) telem_udp_record_t;

_Static_assert(sizeof(telem_udp_record_t) == 40,
               "telem_udp_record_t must be 40 bytes");

/*
 * Initialise UDP telemetry sender.
 * dest_ip – destination IP string (NULL → "127.0.0.1")
 * node_id – this node's ID (placed in every record)
 * Returns 0 on success, -1 on failure.
 */
int telem_udp_init(const char *dest_ip, uint32_t node_id);

/* Shutdown sender thread and close socket */
void telem_udp_close(void);

/*
 * Emit one record from the RT sync thread (non-blocking).
 * Drops silently if the ring buffer is full.
 */
void telem_udp_emit(int64_t  timestamp_ns,
                    int32_t  state,
                    int64_t  offset_ns,
                    int64_t  rtt_ns,
                    int64_t  rate_q32);

#endif /* TELEMETRY_UDP_H */
