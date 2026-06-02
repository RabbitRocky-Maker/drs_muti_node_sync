#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../src/telemetry.h"
#include "../src/errors.h"
#include "../include/drs_sync_config.h"

static const char *state_name(uint32_t s)
{
    switch (s) {
    case 0: return "GROUND";
    case 1: return "CALIBRATION";
    case 2: return "LISTEN";
    case 3: return "CANDIDATE";
    case 4: return "FOLLOWER";
    case 5: return "LEADER";
    case 6: return "HOLDOVER";
    default: return "UNKNOWN";
    }
}

static const char *lock_name(uint32_t m)
{
    switch (m) {
    case DRS_LOCK_AUTO:          return "AUTO";
    case DRS_LOCK_AS_LEADER:     return "LEADER";
    case DRS_LOCK_AS_FOLLOWER:   return "FOLLOWER";
    default: return "?";
    }
}

int main(void)
{
    int fd = open(DRS_SHM_STATE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s (is drs_syncd running?)\n",
                DRS_SHM_STATE_PATH);
        return 1;
    }

    drs_telemetry_t *tel = mmap(NULL, sizeof(drs_telemetry_t),
                                PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (tel == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Seqlock read */
    uint32_t s1, s2 = 0;
    drs_telemetry_t snap;
    do {
        s1 = atomic_load_explicit(&tel->seqlock, memory_order_acquire);
        if (s1 & 1) continue;
        snap = *tel;
        s2 = atomic_load_explicit(&tel->seqlock, memory_order_acquire);
    } while (s1 != s2);

    munmap(tel, sizeof(drs_telemetry_t));

    double offset_us = (double)snap.offset_ns / 1000.0;
    double rtt_us    = (double)snap.last_rtt_ns / 1000.0;
    double lat_us    = (double)snap.latency_corr_ns / 1000.0;
    double rate_ppm  = ((double)(int64_t)(snap.rate_q32_32
                        - (int64_t)DRS_RATE_NOMINAL_Q32)) / DRS_RATE_PPM_Q32;

    printf("=== drs_syncd telemetry ===\n");
    printf("  PID          : %u\n",   snap.pid);
    printf("  State        : %s\n",   state_name(snap.state));
    printf("  Flags        : 0x%02X%s%s%s%s\n",
           snap.flags,
           (snap.flags & DRS_FLAG_LEADER)     ? " LEADER"     : "",
           (snap.flags & DRS_FLAG_HOLDOVER)   ? " HOLDOVER"   : "",
           (snap.flags & DRS_FLAG_CALIBRATED) ? " CALIBRATED" : "",
           (snap.flags & DRS_FLAG_FAULT)      ? " FAULT"      : "");
    printf("  Leader NodeID: %u\n",   snap.leader_node_id);
    printf("  Election Term: %u\n",   snap.election_term);
    printf("  Offset       : %.3f µs\n", offset_us);
    printf("  Rate         : %.3f ppm\n", rate_ppm);
    printf("  RTT (last)   : %.3f µs\n", rtt_us);
    printf("  Lat corr     : %.3f µs\n", lat_us);
    printf("  Pulse count  : %u\n",   snap.pulse_count);
    printf("  Lock mode    : %s\n",   lock_name(snap.lock_mode));
    if (snap.state == 6)
        printf("  Holdover rem : %u ms\n", snap.holdover_remaining_ms);
    if (snap.convergence_unix_ns)
        printf("  Converged at : %llu ns\n",
               (unsigned long long)snap.convergence_unix_ns);
    else
        printf("  Converged    : NO\n");

    return 0;
}
