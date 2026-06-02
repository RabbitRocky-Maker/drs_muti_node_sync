#include "proto_v2.h"
#include "crc32.h"

#include <string.h>
#include <arpa/inet.h>

/* Compile-time guarantee: wire format size matches selected protocol version */
_Static_assert(
    (DRS_PROTO_VER == 1 && DRS_PAYLOAD_BYTES == 66) ||
    (DRS_PROTO_VER == 2 && DRS_PAYLOAD_BYTES == 64),
    "DRS_PAYLOAD_BYTES must be 66 for proto v1 or 64 for proto v2"
);

/* Byte-order helpers for int64 (not in POSIX, but available via endian.h on Linux) */
#include <endian.h>
#define hton64(x) htobe64((uint64_t)(x))
#define ntoh64(x) ((int64_t)be64toh((uint64_t)(x)))

int proto_encode(uint8_t buf[DRS_PAYLOAD_BYTES], const drs_packet_t *pkt)
{
    memset(buf, 0, DRS_PAYLOAD_BYTES);

    /* magic */
    uint32_t magic = htonl(DRS_MAGIC_V2);
    memcpy(buf + 0, &magic, 4);

    buf[4] = DRS_VERSION;
    buf[5] = pkt->msg_type;
    buf[6] = pkt->flags;
    buf[7] = 0; /* reserved */

    uint16_t seq = htons(pkt->seq);
    memcpy(buf + 8, &seq, 2);

    uint32_t node_id = htonl(pkt->node_id);
    memcpy(buf + 10, &node_id, 4);

    uint32_t term = htonl(pkt->election_term);
    memcpy(buf + 14, &term, 4);

    uint64_t t1 = hton64(pkt->t1);
    memcpy(buf + 18, &t1, 8);
    uint64_t t2 = hton64(pkt->t2);
    memcpy(buf + 26, &t2, 8);
    uint64_t t3 = hton64(pkt->t3);
    memcpy(buf + 34, &t3, 8);
    uint64_t t4 = hton64(pkt->t4);
    memcpy(buf + 42, &t4, 8);

    /* CRC field = 0 during calculation, then written */
    uint32_t crc = htonl(crc32_ieee(buf, DRS_PAYLOAD_BYTES));
    memcpy(buf + 50, &crc, 4);
    /* bytes 54..(DRS_PAYLOAD_BYTES-1) remain zero (padding, zeroed by memset above) */

    return 0;
}

int proto_decode(drs_packet_t *pkt, const uint8_t buf[DRS_PAYLOAD_BYTES])
{
    /* Validate magic */
    uint32_t magic;
    memcpy(&magic, buf + 0, 4);
    if (ntohl(magic) != DRS_MAGIC_V2)
        return -1;

    /* Validate version */
    if (buf[4] != DRS_VERSION)
        return -1;

    /* Validate CRC: zero the CRC field in a scratch copy and recompute */
    uint8_t scratch[DRS_PAYLOAD_BYTES];
    memcpy(scratch, buf, DRS_PAYLOAD_BYTES);
    memset(scratch + 50, 0, 4);
    uint32_t computed = crc32_ieee(scratch, DRS_PAYLOAD_BYTES);
    uint32_t stored;
    memcpy(&stored, buf + 50, 4);
    if (ntohl(stored) != computed)
        return -1;

    pkt->msg_type     = buf[5];
    pkt->flags        = buf[6];
    /* buf[7] reserved, ignored */

    uint16_t seq;
    memcpy(&seq, buf + 8, 2);
    pkt->seq = ntohs(seq);

    uint32_t node_id;
    memcpy(&node_id, buf + 10, 4);
    pkt->node_id = ntohl(node_id);

    uint32_t term;
    memcpy(&term, buf + 14, 4);
    pkt->election_term = ntohl(term);

    uint64_t tmp;
    memcpy(&tmp, buf + 18, 8); pkt->t1 = ntoh64(tmp);
    memcpy(&tmp, buf + 26, 8); pkt->t2 = ntoh64(tmp);
    memcpy(&tmp, buf + 34, 8); pkt->t3 = ntoh64(tmp);
    memcpy(&tmp, buf + 42, 8); pkt->t4 = ntoh64(tmp);

    return 0;
}
