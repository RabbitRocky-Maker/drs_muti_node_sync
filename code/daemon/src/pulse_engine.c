#include "pulse_engine.h"
#include "gpio_bcm2711.h"
#include "net_io.h"   /* for mono_raw_ns() */
#include "../include/drs_sync_config.h"

#include <sys/timerfd.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* Wake up this many ns before the RAW target, then busy-wait the remainder */
#define BUSY_WAIT_NS INT64_C(300000)

static void arm_timerfd_abs(int fd, int64_t abs_mono_ns)
{
    struct itimerspec its = {0};
    its.it_value.tv_sec  = abs_mono_ns / INT64_C(1000000000);
    its.it_value.tv_nsec = abs_mono_ns % INT64_C(1000000000);
    timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL);
}

/* Current CLOCK_MONOTONIC in ns */
static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

/*
 * Convert a CLOCK_MONOTONIC_RAW timestamp to CLOCK_MONOTONIC.
 * Samples both clocks back-to-back to get the instantaneous offset.
 * The offset is stable when NTP is disabled (same hardware oscillator).
 */
static int64_t raw_to_mono(int64_t raw_ns)
{
    struct timespec ts_m, ts_r;
    clock_gettime(CLOCK_MONOTONIC,     &ts_m);
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_r);
    int64_t mono_now = (int64_t)ts_m.tv_sec * INT64_C(1000000000) + ts_m.tv_nsec;
    int64_t raw_now  = (int64_t)ts_r.tv_sec * INT64_C(1000000000) + ts_r.tv_nsec;
    return raw_ns + (mono_now - raw_now);
}

/* Compute the CLOCK_MONOTONIC_RAW time of the next 1 Hz boundary */
static int64_t next_rise_raw(vclock_t *vc)
{
    int64_t t_raw_now    = mono_raw_ns();
    int64_t t_global_now = vclock_now(vc, t_raw_now);

    int64_t t_next_global = ((t_global_now + INT64_C(1000000)) / DRS_PULSE_PERIOD_NS + 1)
                            * DRS_PULSE_PERIOD_NS;

    return vclock_local_for_global(vc, t_next_global);
}

static void *pulse_thread_fn(void *arg)
{
    pulse_engine_t *pe = (pulse_engine_t *)arg;

    /* Higher RT priority than main thread (85) so we preempt it during busy-wait */
    struct sched_param sp = { .sched_priority = 90 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* Same CPU core as main thread */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(DRS_RT_CPU, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    struct pollfd pfds[2] = {
        { .fd = pe->timer_fd,     .events = POLLIN },
        { .fd = pe->pulse_low_fd, .events = POLLIN },
    };

    while (!pe->stop) {
        int r = poll(pfds, 2, 1000);
        if (pe->stop)
            break;
        if (r <= 0)
            continue;

        if (pfds[0].revents & POLLIN) {
            uint64_t exp;
            if (read(pe->timer_fd, &exp, 8) < 0) { /* ignore */ }

            /*
             * Busy-wait using CLOCK_MONOTONIC_RAW for sub-µs accuracy.
             * The timerfd (CLOCK_MONOTONIC) woke us up ~300 µs early.
             */
            int64_t target = pe->rise_target_raw_ns;
            while (mono_raw_ns() < target)
                ; /* spin — mono_raw_ns() is a syscall, loop won't be optimized away */

            gpio_set(DRS_GPIO_PULSE_PIN);
            pe->pulse_count++;

            /* Arm falling edge 10 ms from now — use CLOCK_MONOTONIC directly */
            arm_timerfd_abs(pe->pulse_low_fd, mono_ns() + DRS_PULSE_HIGH_DURATION_NS);
        }

        if (pfds[1].revents & POLLIN) {
            uint64_t exp;
            if (read(pe->pulse_low_fd, &exp, 8) < 0) { /* ignore */ }

            gpio_clear(DRS_GPIO_PULSE_PIN);

            /* Self-replenish: compute and arm next rising edge */
            if (pe->vc) {
                int64_t t_next_raw = next_rise_raw(pe->vc);
                pe->rise_target_raw_ns = t_next_raw;
                /* Convert RAW deadline to CLOCK_MONOTONIC for timerfd */
                arm_timerfd_abs(pe->timer_fd, raw_to_mono(t_next_raw - BUSY_WAIT_NS));
            }
        }
    }

    return NULL;
}

int pulse_engine_init(pulse_engine_t *pe)
{
    memset(pe, 0, sizeof(*pe));

    /* timerfd_create does not support CLOCK_MONOTONIC_RAW — must use CLOCK_MONOTONIC */
    pe->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (pe->timer_fd < 0)
        return -1;

    pe->pulse_low_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (pe->pulse_low_fd < 0) {
        close(pe->timer_fd);
        pe->timer_fd = -1;
        return -1;
    }

    return 0;
}

int pulse_engine_start(pulse_engine_t *pe, vclock_t *vc)
{
    pe->vc   = vc;
    pe->stop = 0;
    if (pthread_create(&pe->thread, NULL, pulse_thread_fn, pe) != 0)
        return -1;
    pe->thread_started = 1;
    return 0;
}

void pulse_engine_close(pulse_engine_t *pe)
{
    if (pe->thread_started) {
        pe->stop = 1;
        /* Closing the fds unblocks poll() inside the thread */
        if (pe->timer_fd >= 0)     { close(pe->timer_fd);     pe->timer_fd = -1; }
        if (pe->pulse_low_fd >= 0) { close(pe->pulse_low_fd); pe->pulse_low_fd = -1; }
        pthread_join(pe->thread, NULL);
        pe->thread_started = 0;
    } else {
        if (pe->timer_fd >= 0)     { close(pe->timer_fd);     pe->timer_fd = -1; }
        if (pe->pulse_low_fd >= 0) { close(pe->pulse_low_fd); pe->pulse_low_fd = -1; }
    }
}

void pulse_engine_arm(pulse_engine_t *pe, vclock_t *vc)
{
    int64_t t_next_raw = next_rise_raw(vc);
    pe->rise_target_raw_ns = t_next_raw;
    arm_timerfd_abs(pe->timer_fd, raw_to_mono(t_next_raw - BUSY_WAIT_NS));
}

void pulse_engine_update_health(pulse_engine_t *pe, int64_t offset_ns,
                                int in_holdover)
{
    if (in_holdover) {
        gpio_set(DRS_GPIO_HEALTH_PIN);
        pe->stable          = 0;
        pe->stable_since_ns = 0;
        return;
    }

    int64_t abs_offset = offset_ns < 0 ? -offset_ns : offset_ns;

    if (abs_offset < DRS_STABLE_OFFSET_THRESH_NS) {
        if (!pe->stable) {
            pe->stable          = 1;
            pe->stable_since_ns = mono_raw_ns();
        }
        int64_t stable_duration = mono_raw_ns() - pe->stable_since_ns;
        if (stable_duration >= DRS_STABLE_WINDOW_NS) {
            gpio_clear(DRS_GPIO_HEALTH_PIN);
        }
    } else {
        pe->stable          = 0;
        pe->stable_since_ns = 0;
        gpio_set(DRS_GPIO_HEALTH_PIN);
    }
}
