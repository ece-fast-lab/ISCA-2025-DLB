/* Modified from DPDK */

#ifndef CYCLES_ARM_32_H
#define CYCLES_ARM_32_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>

#define __ARM_PMU_USER_EN__ 0
#define USE_PMU ((__ARM_ARCH >= 6) && __ARM_PMU_USER_EN__)

#define RDTSC_ALIGN_MUL_CEIL(v, mul)                                             \
    ((((v) + (typeof(v))(mul)-1) / ((typeof(v))(mul))) * (typeof(v))(mul))


/* Define rdtsc function for arm */
#if USE_PMU
/**
 * This function requires to configure the PMCCNTR and enable
 * userspace access to it:
 *
 *      asm volatile("mcr p15, 0, %0, c9, c14, 0" : : "r"(1));
 *      asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(29));
 *      asm volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(0x8000000f));
 *
 * which is possible only from the privileged mode (kernel space).
 */
static inline uint64_t rdtsc(void) {
    unsigned tsc;
    uint64_t final_tsc;

    /* Read PMCCNTR */
    asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(tsc));
    /* 1 tick = 64 clocks */
    final_tsc = ((uint64_t)tsc) << 6;

    return (uint64_t)final_tsc;
}
#else 
/**
 * This call is easily portable to any architecture, however,
 * it may require a system call and imprecise for some tasks.
 */
static inline uint64_t rdtsc(void) {
    struct timespec val;
    uint64_t v;

    while (clock_gettime(CLOCK_MONOTONIC_RAW, &val) != 0)
        /* no body */;

    v = (uint64_t)val.tv_sec * 1000000000LL;
    v += (uint64_t)val.tv_nsec;
    return v;
}
#endif


static inline uint64_t rdtscp(void) {
    // Add a barrier
    return rdtsc();
}

static inline uint64_t get_tsc_freq_arch(void) {
    // TODO
}

#endif