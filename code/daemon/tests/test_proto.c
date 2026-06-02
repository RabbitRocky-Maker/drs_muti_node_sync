#include <stdio.h>
#include <string.h>
#include "../src/proto_v2.h"

int main(void)
{
    drs_packet_t orig = {
        .msg_type      = DRS_MSG_SYNC_RESP,
        .flags         = DRS_FLAG_LEADER | DRS_FLAG_CALIBRATED,
        .seq           = 0xABCD,
        .node_id       = 42,
        .election_term = 7,
        .t1 = INT64_C(1000000001),
        .t2 = INT64_C(1000000050),
        .t3 = INT64_C(1000000055),
        .t4 = INT64_C(1000000100),
    };

    uint8_t buf[DRS_PAYLOAD_BYTES];
    if (proto_encode(buf, &orig) != 0) {
        printf("FAIL test_proto: encode returned error\n");
        return 1;
    }

    drs_packet_t dec;
    if (proto_decode(&dec, buf) != 0) {
        printf("FAIL test_proto: decode returned error\n");
        return 1;
    }

    if (dec.msg_type      != orig.msg_type ||
        dec.flags         != orig.flags ||
        dec.seq           != orig.seq ||
        dec.node_id       != orig.node_id ||
        dec.election_term != orig.election_term ||
        dec.t1 != orig.t1 || dec.t2 != orig.t2 ||
        dec.t3 != orig.t3 || dec.t4 != orig.t4) {
        printf("FAIL test_proto: roundtrip mismatch\n");
        return 1;
    }

    /* Corrupt one byte and expect decode failure */
    buf[6] ^= 0xFF;
    if (proto_decode(&dec, buf) == 0) {
        printf("FAIL test_proto: corrupt packet not rejected\n");
        return 1;
    }

    /* Wrong magic */
    uint8_t bad[DRS_PAYLOAD_BYTES];
    memset(bad, 0, sizeof(bad));
    if (proto_decode(&dec, bad) == 0) {
        printf("FAIL test_proto: zero buf not rejected\n");
        return 1;
    }

    printf("PASS test_proto\n");
    return 0;
}
