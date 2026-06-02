#ifndef PROTO_V2_H
#define PROTO_V2_H

#include <stdint.h>
#include "../include/drs_sync_config.h"

/* In-memory representation of a DRS packet */
typedef struct {
    uint8_t  msg_type;
    uint8_t  flags;
    uint16_t seq;
    uint32_t node_id;
    uint32_t election_term;
    int64_t  t1;
    int64_t  t2;
    int64_t  t3;
    int64_t  t4;
} drs_packet_t;

/*
 * Serialise `pkt` into `buf` (exactly DRS_PAYLOAD_BYTES).
 * Returns 0 on success, -1 on error.
 */
int proto_encode(uint8_t buf[DRS_PAYLOAD_BYTES], const drs_packet_t *pkt);

/*
 * Deserialise `buf` into `pkt`.
 * Validates magic, version, size and CRC32.
 * Returns 0 on success, -1 on validation failure.
 */
int proto_decode(drs_packet_t *pkt, const uint8_t buf[DRS_PAYLOAD_BYTES]);

#endif /* PROTO_V2_H */
