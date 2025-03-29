/* Modified from DPDK */

#ifndef CYCLES_ARM_64_H
#define CYCLES_ARM_64_H

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

/** Read generic counter frequency */
static inline uint64_t __arm64_cntfrq(void) {
    uint64_t freq;

    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

/** Read generic counter */
static inline uint64_t __arm64_cntvct(void) {
    uint64_t tsc;

    asm volatile("mrs %0, cntvct_el0" : "=r"(tsc));
    return tsc;
}

static inline uint64_t __arm64_cntvct_precise(void) {
    asm volatile("isb" : : : "memory");
    return __arm64_cntvct();
}

/* Define rdtsc function for arm */
#if USE_PMU
/**
 * This is an alternative method to enable rdtsc() with high resolution
 * PMU cycles counter.The cycle counter runs at cpu frequency and this scheme
 * uses ARMv8 PMU subsystem to get the cycle counter at userspace, However,
 * access to PMU cycle counter from user space is not enabled by default in
 * arm64 linux kernel.
 * It is possible to enable cycle counter at user space access by configuring
 * the PMU from the privileged mode (kernel space).
 *
 * asm volatile("msr pmintenset_el1, %0" : : "r" ((u64)(0 << 31)));
 * asm volatile("msr pmcntenset_el0, %0" :: "r" BIT(31));
 * asm volatile("msr pmuserenr_el0, %0" : : "r"(BIT(0) | BIT(2)));
 * asm volatile("mrs %0, pmcr_el0" : "=r" (val));
 * val |= (BIT(0) | BIT(2));
 * isb();
 * asm volatile("msr pmcr_el0, %0" : : "r" (val));
 *
 */

/** Read PMU cycle counter */
static inline uint64_t __arm64_pmccntr(void) {
    uint64_t tsc;

    asm volatile("mrs %0, pmccntr_el0" : "=r"(tsc));
    return tsc;
}

static inline uint64_t rdtsc(void) {
    return __arm64_pmccntr();
}
#else
/**
 * This call is portable to any ARMv8 architecture, however, typically
 * cntvct_el0 runs at <= 100MHz and it may be imprecise for some tasks.
 */
static inline uint64_t rdtsc(void) {
    return __arm64_cntvct();
}
#endif


static inline uint64_t rdtscp(void) {
    asm volatile("isb" : : : "memory");
    return rdtsc();
}

static inline uint64_t get_tsc_freq_arch(void) {
#if defined(__aarch64__) && !USE_PMU
    return __arm64_cntfrq();
#elif defined(__aarch64__) && USE_PMU
#define CYC_PER_1MHZ 1E6
    /* Use the generic counter ticks to calculate the PMU
     * cycle frequency.
     */
    uint64_t ticks;
    uint64_t start_ticks, cur_ticks;
    uint64_t start_pmu_cycles, end_pmu_cycles;

    /* Number of ticks for 1/10 second */
    ticks = __arm64_cntfrq() / 10;

    start_ticks = __arm64_cntvct_precise();
    start_pmu_cycles = rdtscp();
    do {
        cur_ticks = __arm64_cntvct();
    } while ((cur_ticks - start_ticks) < ticks);
    end_pmu_cycles = rdtscp();

    /* Adjust the cycles to next 1Mhz */
    return RDTSC_ALIGN_MUL_CEIL(end_pmu_cycles - start_pmu_cycles, CYC_PER_1MHZ) * 10;
#else
    return 0;
#endif
}

#endif