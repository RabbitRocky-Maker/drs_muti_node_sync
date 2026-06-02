#include "calibrate.h"
#include "net_io.h"
#include "../include/drs_sync_config.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#define LOOPBACK_TIMEOUT_US 50000   /* 50 ms per sample */
#define MIN_VALID_SAMPLES   10      /* need at least this many after rejection */

static int64_t timespec_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * INT64_C(1000000000) + ts->tv_nsec;
}

int calibrate_loopback(vclock_t *vc)
{
    int srv = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (srv < 0)
        return -1;

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DRS_CALIB_LOOPBACK_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv);
        return -1;
    }

    int cli = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (cli < 0) {
        close(srv);
        return -1;
    }

    /* Set receive timeout on server socket */
    struct timeval tv = { .tv_sec = 0, .tv_usec = LOOPBACK_TIMEOUT_US };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int64_t samples[DRS_CALIB_SAMPLES];
    int     n_samples = 0;
    uint8_t buf[8];
    struct sockaddr_in from_addr;

    for (int i = 0; i < DRS_CALIB_SAMPLES && n_samples < DRS_CALIB_SAMPLES; i++) {
        struct timespec t_send, t_recv;

        clock_gettime(CLOCK_MONOTONIC_RAW, &t_send);

        ssize_t sent = sendto(cli, buf, sizeof(buf), 0,
                              (struct sockaddr *)&addr, sizeof(addr));
        if (sent < 0)
            continue;

        socklen_t alen = sizeof(from_addr);
        ssize_t r = recvfrom(srv, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from_addr, &alen);
        clock_gettime(CLOCK_MONOTONIC_RAW, &t_recv);

        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }

        samples[n_samples++] = timespec_ns(&t_recv) - timespec_ns(&t_send);
    }

    close(cli);
    close(srv);

    if (n_samples < MIN_VALID_SAMPLES)
        return -1;

    /* Find minimum */
    int64_t mn = samples[0];
    for (int i = 1; i < n_samples; i++)
        if (samples[i] < mn)
            mn = samples[i];

    /* Reject outliers > min + 20 µs, compute filtered min */
    int64_t valid_min = INT64_MAX;
    int     valid_count = 0;
    for (int i = 0; i < n_samples; i++) {
        if (samples[i] <= mn + DRS_CALIB_OUTLIER_NS) {
            if (samples[i] < valid_min)
                valid_min = samples[i];
            valid_count++;
        }
    }

    if (valid_count < MIN_VALID_SAMPLES)
        return -1;

    /* One-way latency = round-trip / 2 */
    int64_t lat_corr = valid_min / 2;
    vclock_set_lat_corr(vc, lat_corr);

    return 0;
}
