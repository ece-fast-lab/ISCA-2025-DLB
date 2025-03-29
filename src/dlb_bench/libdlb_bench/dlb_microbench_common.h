#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <error.h>
#include <pthread.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include "dlb.h"
#include "dlb_priv.h"
#include "dlb2_ioctl.h"

#include <fcntl.h>
#include <cpuid.h>
#include <sched.h>
#include <math.h>
#include <x86intrin.h>

#define NUM_EVENTS_PER_LOOP 4
#define RETRY_LIMIT 1000000000

#define EPOLL_SIZE 256
#define EPOLL_RETRY 10

#define CQ_DEPTH_DEFAULT 8

#define PER_PACKET_MEASUREMENT 1

// ==== DLB variables
uint64_t **dlb_pp_addr;
uint64_t freq = 0;
// =============================

typedef struct {
    dlb_port_hdl_t port;
    int queue_id;
    int efd;
    unsigned int core_mask;
    int index;
    uint16_t flow_id_start;
    uint64_t num_flows_local;
    // uint64_t *flow_speed_array;
} thread_args_t;

enum wait_mode_t {
    POLL,
    INTERRUPT,
} wait_mode = POLL;

static uint64_t ticks = 2000;       /* 2 sec*/
static uint64_t hz;

static volatile bool worker_done;   /* Notify workers to stop */
static bool epoll_enabled = false;

static dlb_dev_cap_t cap;
static int dev_id;
static uint64_t num_events;
static uint64_t num_flows = 1;
static uint64_t num_flows_per_worker;
static int num_workers;
static int num_txs = 1;
static int num_rxs = 0;
static int num_credit_combined;
static int num_credit_ldb;
static int num_credit_dir;
static bool use_max_credit_combined = true;
static bool use_max_credit_ldb = true;
static bool use_max_credit_dir = true;
static int partial_resources = 100;
static enum dlb_event_sched_t sched_type = 1;           /* Parallel by default */
static dlb_resources_t rsrcs;

// Performance Tuning
static uint64_t enqueue_depth = 4;
static uint64_t dequeue_depth = 4;
static uint64_t cq_depth = CQ_DEPTH_DEFAULT;   /* Must be power of 2 in [8, 1024] */
static unsigned int core_mask = 2;

// Priority statistics
static volatile uint64_t priority_latency[16][16][16];     /* worker index, queue priority, packet priority -> latency */
static volatile uint64_t packet_priority_count[16][16];   /* tx index, packet priority -> packet count */
static int queue_priority[32];                          /* queue id -> tx index mapping */
static volatile uint64_t worker_pkt_count[16];           /* worker index -> worker packet count */
static volatile double worker_tput[16];                  /* worker index -> worker tput */

// Flow statistics
static volatile uint32_t worker_flows_served[16][2048] = {0};                  /* worker index -> flows served by the workers */


// Control signals
static int report_latency = 0;
static int queue_prio_en = 0;
static int packet_prio_en = 0;

// Multi-stage
static int num_stages = 1;

// Compute latency stats
static int cmpfunc (const void * a, const void * b)
{
    if (*(double*)a > *(double*)b) return 1;
    else if (*(double*)a < *(double*)b) return -1;
    else return 0;
}

static inline void isTSC(void)
{
    uint32_t a=0x1, b, c, d;
    asm volatile ("cpuid"
         : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
         : "a" (a), "b" (b), "c" (c), "d" (d)
         );
    if ((d & (1 << 4))) {
        printf("TSC exist!\n");
    }

    a=0x80000007;
    asm volatile ("cpuid"
         : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
         : "a" (a), "b" (b), "c" (c), "d" (d)
         );
    if ((d & (1 << 8))) {
        printf("Invariant TSC available!\n");
    }
}

static inline uint64_t rdtsc(void)
{
    return __rdtsc();
    unsigned int a, d; 
    asm volatile("cpuid" ::: "%rax", "%rbx", "%rcx", "%rdx");
    asm volatile("rdtsc" : "=a" (a), "=d" (d)); 
    return a | (((uint64_t)d) << 32); 
}

static inline uint64_t rdtscp(void)
{
    unsigned int junk;
    return __rdtscp(&junk);
    unsigned int a, d; 
    asm volatile("cpuid" ::: "%rax", "%rbx", "%rcx", "%rdx");
    asm volatile("rdtscp" : "=a" (a), "=d" (d)); 
    return a | (((uint64_t)d) << 32); 
}

static unsigned int get_cpu_model(uint32_t fam_mod_step) {
	uint32_t family, model, ext_model;

	family = (fam_mod_step >> 8) & 0xf;
	model = (fam_mod_step >> 4) & 0xf;

	if (family == 6 || family == 15) {
		ext_model = (fam_mod_step >> 16) & 0xf;
		model += (ext_model << 4);
	}

	return model;
}

static int32_t rdmsr(int msr, uint64_t *val) {
	int fd;
	int ret;

	fd = open("/dev/cpu/0/msr", O_RDONLY);
	if (fd < 0)
		return fd;

	ret = pread(fd, val, sizeof(uint64_t), msr);

	close(fd);

	return ret;
}

static uint32_t check_model_wsm_nhm(uint8_t model) {
	switch (model) {
	/* Westmere */
	case 0x25:
	case 0x2C:
	case 0x2F:
	/* Nehalem */
	case 0x1E:
	case 0x1F:
	case 0x1A:
	case 0x2E:
		return 1;
	}

	return 0;
}

static uint32_t check_model_gdm_dnv(uint8_t model) {
	switch (model) {
	/* Goldmont */
	case 0x5C:
	/* Denverton */
	case 0x5F:
		return 1;
	}

	return 0;
}

static inline uint64_t get_tsc_freq_arch(void) {
	uint64_t tsc_hz = 0;
	uint32_t a, b, c, d, maxleaf;
	uint8_t mult, model;
	int32_t ret;

	/*
	 * Time Stamp Counter and Nominal Core Crystal Clock
	 * Information Leaf
	 */
	maxleaf = __get_cpuid_max(0, NULL);

	if (maxleaf >= 0x15) {
		__cpuid(0x15, a, b, c, d);

		/* EBX : TSC/Crystal ratio, ECX : Crystal Hz */
		if (b && c)
			return c * (b / a);
	}

	__cpuid(0x1, a, b, c, d);
	model = get_cpu_model(a);

	if (check_model_wsm_nhm(model))
		mult = 133;
	else if ((c & bit_AVX) || check_model_gdm_dnv(model))
		mult = 100;
	else
		return 0;

	ret = rdmsr(0xCE, &tsc_hz);
	if (ret < 0)
		return 0;

	return ((tsc_hz >> 8) & 0xff) * mult * 1E6;
}


static inline void delay_nanoseconds(uint64_t nanoseconds) {
    uint64_t start_time, end_time;
    uint64_t elapsed_nanoseconds;
    uint64_t cycles = (hz * nanoseconds) / 1E9;
    // printf("hz: %lu, cycles: %lu\n", hz, cycles);

    start_time = __rdtsc();
    do {
        end_time = __rdtsc();
        elapsed_nanoseconds = end_time - start_time;
    } while (elapsed_nanoseconds < cycles);
}

static inline void delay_cycles(uint64_t cycles) {
    uint64_t start_time, end_time;
    uint64_t elapsed_nanoseconds;
    // uint64_t cycles = (hz * nanoseconds) / 1E9;
    // printf("hz: %lu, cycles: %lu\n", hz, cycles);

    start_time = __rdtsc();
    do {
        end_time = __rdtsc();
        elapsed_nanoseconds = end_time - start_time;
    } while (elapsed_nanoseconds < cycles);
}


/********************************/
/****** DLB Setup Functions ******/
/********************************/
static int create_sched_domain(
    dlb_hdl_t dlb, int num_ldb_ports, int num_dir_ports)
{
    int p_rsrsc = partial_resources;
    dlb_create_sched_domain_t args = {0};

    args.num_ldb_queues = (num_workers > 0) ? num_rxs + num_txs : num_txs;
    // args.num_ldb_queues = 1 + (num_workers > 0);
    args.num_ldb_ports = num_ldb_ports;
    args.num_dir_ports = num_dir_ports;
    // args.num_ldb_ports = num_rxs + num_workers;
    // args.num_dir_ports = num_txs;
    args.num_ldb_event_state_entries = 2 * args.num_ldb_ports * cq_depth;
    if (!cap.combined_credits) {
        args.num_ldb_credits = rsrcs.max_contiguous_ldb_credits * p_rsrsc / 100;
        args.num_dir_credits = rsrcs.max_contiguous_dir_credits * p_rsrsc / 100;
        args.num_ldb_credit_pools = 1;
        args.num_dir_credit_pools = 1;
    } else {
        args.num_credits = rsrcs.num_credits * p_rsrsc / 100;
        args.num_credit_pools = 1;
    }

    args.num_sn_slots[0] = rsrcs.num_sn_slots[0] * p_rsrsc / 100;
    args.num_sn_slots[1] = rsrcs.num_sn_slots[1] * p_rsrsc / 100;

    // for (int i = 0; i < num_txs; i++) {
    //     // args.producer_coremask[0] |= (0x1 << (core_mask + i));
    //     args.producer_coremask[0] |= (0x800000000000000 >> (core_mask + i));
    //     args.producer_coremask[1] |= (0x800000000000000 >> (core_mask + i));
    // }

    return dlb_create_sched_domain(dlb, &args);
}

static int create_ldb_queue(
    dlb_domain_hdl_t domain,
    int num_seq_numbers)
{
    dlb_create_ldb_queue_t args = {0};

    args.num_sequence_numbers = num_seq_numbers;

    return dlb_create_ldb_queue(domain, &args);
}

static int create_ldb_port(
    dlb_domain_hdl_t domain,
    int ldb_pool,
    int dir_pool)
{
    dlb_create_port_t args;

    if (!cap.combined_credits) {
        args.ldb_credit_pool_id = ldb_pool;
        args.dir_credit_pool_id = dir_pool;
    } else {
        args.credit_pool_id = ldb_pool;
    }
    args.cq_depth = cq_depth;
    args.num_ldb_event_state_entries = cq_depth*2;
    // args.cos_id = DLB_PORT_COS_ID_0;
    args.cos_id = DLB_PORT_COS_ID_ANY;

    return dlb_create_ldb_port(domain, &args);
}

static int create_dir_queue(
    dlb_domain_hdl_t domain,
    int port_id)
{
    return dlb_create_dir_queue(domain, port_id);
}

static int create_dir_port(
    dlb_domain_hdl_t domain,
    int ldb_pool,
    int dir_pool,
    int queue_id)
{
    dlb_create_port_t args;

    if (!cap.combined_credits) {
        args.ldb_credit_pool_id = ldb_pool;
        args.dir_credit_pool_id = dir_pool;
    } else {
        args.credit_pool_id = ldb_pool;
    }

    args.cq_depth = cq_depth;

    return dlb_create_dir_port(domain, &args, queue_id);
}

/* Create eventfd per port and map the eventfd to the port
 * using dlb_enable_cq_epoll() API. Create epoll instance
 * and register the eventfd to monitor for events.
 */
static int setup_epoll(thread_args_t *args) {
    struct epoll_event ev;
    int epoll_fd;

    args->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (args->efd < 0)
        error(1, errno, "eventfd error");
    if (dlb_enable_cq_epoll(args->port, true, args->efd))
        error(1, errno, "dlb_enable_cq_epoll");

    epoll_fd = epoll_create(EPOLL_SIZE);
    if( epoll_fd < 0 )
        error(1, errno, "epoll_create failed");

    ev.data.fd = args->efd;
    ev.events  = EPOLLIN;
    if( epoll_ctl(epoll_fd, EPOLL_CTL_ADD, args->efd, &ev ) ) {
        close(epoll_fd);
        error(1, errno, "Failed to add file descriptor to epoll");
    }
    return epoll_fd;
}

/********************************/
/****** Print Functions ******/
/********************************/
static void print_sched_type(enum dlb_event_sched_t sched_type) {
    switch(sched_type) {
	case 0: printf("Using Atomic Queue\n");
	        break;
	case 1: printf("Using Unordered Queue\n");
	        break;
	case 2: printf("Using Ordered Queue\n");
	        break;
	default: break;
    }
}

static int print_resources(dlb_resources_t *rsrcs) {
    printf("DLB's available resources:\n");
    printf("\tDomains:           %d\n", rsrcs->num_sched_domains);
    printf("\tLDB queues:        %d\n", rsrcs->num_ldb_queues);
    printf("\tLDB ports:         %d\n", rsrcs->num_ldb_ports);
    printf("\tLDB CoS:           %d, %d, %d, %d\n", rsrcs->num_ldb_ports_per_cos[0], rsrcs->num_ldb_ports_per_cos[1], rsrcs->num_ldb_ports_per_cos[2], rsrcs->num_ldb_ports_per_cos[3]);
    printf("\tDIR ports:         %d\n", rsrcs->num_dir_ports);
    printf("\tSN slots:          %d,%d\n", rsrcs->num_sn_slots[0],
	   rsrcs->num_sn_slots[1]);
    printf("\tES entries:        %d\n", rsrcs->num_ldb_event_state_entries);
    printf("\tContig ES entries: %d\n",
           rsrcs->max_contiguous_ldb_event_state_entries);
    if (!cap.combined_credits) {
        printf("\tLDB credits:       %d\n", rsrcs->num_ldb_credits);
        printf("\tContig LDB cred:   %d\n", rsrcs->max_contiguous_ldb_credits);
        printf("\tDIR credits:       %d\n", rsrcs->num_dir_credits);
        printf("\tContig DIR cred:   %d\n", rsrcs->max_contiguous_dir_credits);
        printf("\tLDB credit pls:    %d\n", rsrcs->num_ldb_credit_pools);
        printf("\tDIR credit pls:    %d\n", rsrcs->num_dir_credit_pools);
    } else {
        printf("\tCredits:           %d\n", rsrcs->num_credits);
        printf("\tCredit pools:      %d\n", rsrcs->num_credit_pools);
    }
    printf("\n");

    return 0;
}

/********************************/
/****** Program parameters ******/
/********************************/
static struct option long_options[] = {
    {"help",                no_argument, 0, 'h'},	
    {"num-events",          required_argument, 0, 'n'},
    {"dev-id",              required_argument, 0, 'd'},
    {"wait-mode",           required_argument, 0, 'w'},
    {"num-workers",         required_argument, 0, 'f'},
    {"num-tx",              required_argument, 0, 't'},
    {"partial_resources",   required_argument, 0, 'p'},
    {"num-credit-combined", required_argument, 0, 'c'},
    {"num-credit-ldb",      required_argument, 0, 'l'},
    {"num-credit-dir",      required_argument, 0, 'e'},
    {"sched_type",          required_argument, 0, 's'},
    {"queue-prio",          no_argument, 0, 'Q'},
    {"packet-prio",         no_argument, 0, 'P'},
    {"report-latecy",       no_argument, 0, 'R'},
    {"num-flows",           required_argument, 0, 'F'},
    {"enqueue-depth",       required_argument, 0, 'E'},
    {"denqueue-depth",      required_argument, 0, 'D'},
    {"cq-depth",            required_argument, 0, 'C'},
    {"core-mask",           required_argument, 0, 'm'},
    {0, 0, 0, 0}
};

static void usage(void) {
    const char *usage_str =
        "  Usage: traffic [options]\n"
        "  Options:\n"
	    "  -h, --help             Prints all the available options\n"
        "  -n, --num-events=N     Number of looped events (0: infinite) (default: 0)\n"
        "  -d, --dev-id=N         Device ID (default: 0)\n"
        "  -w, --wait-mode=<str>  Options: 'poll', 'interrupt', 'epoll' (default: interrupt)\n"
        "  -f, --num-workers=N    Number of 'worker' threads that forward events (default: 0)\n"
        "  -t, --num-tx=N           Number of 'tx' threads that generate and send events (default: 1)\n"
        "  -p, --partial_resources=N    Partial HW resources in percentage (default: 100)\n"
        "  -c, --num-credit-combined=N   Number of combined SW credits (default: combined HW credits\n"
        "  -l, --num-credit-ldb=N    Number of ldb SW credits (default: HW ldb credits)\n"
        "  -e, --num-credit-dir=N    Number of dir SW credits (default: HW dir credits)\n"
	    "  -s,  --sched-type=N    N = 0 (Atomic)\n"
	    "			N = 1 (Unordered)\n"
	    "			N = 2 (Ordered)\n"
	    "  -Q,  --queue-prio-en    enable queue priority (default: disabled)\n"
	    "  -P,  --packet-prio-en   enable packet priority (default: disabled)\n"
	    "  -R,  --report-latency   enable latency report every one second (default: disabled)\n"
	    "  -F,  --num-flows        Set number of flows, used for atomic queue (default: 1)\n"
	    "  -E,  --enqueue-depth    Set batch size for enqueuing QEs in batches (default: 4)\n"
	    "  -D,  --dequeue-depth   Set batch size for dequeuing QEs in batches (default: 4)\n"
	    "  -C,  --cq-depth         Set completion queue depth, must be power of 2, otherwise ceil up (default: 8, max CQ depth: 1024)\n"
	    "  -m,  --core-mask        The core start index for tx, worker, rx threads (default: 2)\n"
        "\n";
    fprintf(stderr, "%s", usage_str);
    exit(1);
}

int parse_args(int argc, char **argv) {
    int option_index, c;
    const char short_options[] = "n:d:w:f:t:c:l:e:p:s:Q:P:R:F:E:D:C:m:h";

    for (;;) {
        c = getopt_long(argc, argv, short_options, long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'n':
                num_events = (uint64_t) atol(optarg);
                break;
            case 'd':
                dev_id = atoi(optarg);
                break;
            case 'w':
                if (strncmp(optarg, "poll", sizeof("poll")) == 0)
                    wait_mode = POLL;
                else if (strncmp(optarg, "interrupt", sizeof("interrupt")) == 0)
                    wait_mode = INTERRUPT;
                else if (strncmp(optarg, "epoll", sizeof("epoll")) == 0) {
                    epoll_enabled = true;
                    wait_mode = POLL;
                }
                else
                    usage();
                break;
            case 'f':
                num_workers = atoi(optarg);
                break;
            case 't':
                num_txs = atoi(optarg);
                break;
            case 'c':
                num_credit_combined = atoi(optarg);
                use_max_credit_combined = false;
                break;
            case 'p':
		        partial_resources = atoi(optarg);
                break;
            case 'l':
                num_credit_ldb = atoi(optarg);
                use_max_credit_ldb = false;
                break;
            case 'e':
                num_credit_dir = atoi(optarg);
                use_max_credit_dir = false;
                break;
	        case 's':
                sched_type = atoi(optarg);
                if (sched_type < 0 || sched_type > 3) {
                    printf("\nIncorrect event scheduling type passed.\n\n");
                    usage();
                }
                print_sched_type(sched_type);
                break;
            case 'Q':
                queue_prio_en = atoi(optarg);
                break;
            case 'P':
                packet_prio_en = atoi(optarg);
                break;
            case 'R':
                report_latency = atoi(optarg);
                break;
            case 'F':
                num_flows = atoi(optarg);
                if (num_flows <= 0) {
                    printf("\nNumber of flow must be positive.\n\n");
                    usage();
                }
                if (num_flows < num_txs || num_flows % num_txs != 0) {
                    printf("\nNumber of flow cannot be distributed to multi cores.\n\n");
                    usage();
                }
                break;
            case 'E':
                enqueue_depth = atoi(optarg);
                if (enqueue_depth < 4) {
                    enqueue_depth = 4;
                }
                break;
            case 'D':
                dequeue_depth = atoi(optarg);
                if (dequeue_depth < 4) {
                    dequeue_depth = 4;
                }
                break;
            case 'C':
                cq_depth = atoi(optarg);
                int power_tmp = ceil(log2(cq_depth));
                cq_depth = pow(2, power_tmp);
                if (cq_depth < 8) {
                    cq_depth = 8;
                }
                printf("CQ depth is set to %ld\n", cq_depth);
                break;
            case 'm':
                core_mask = atoi(optarg);
                if (core_mask < 0) {
                    printf("\nNumber of core mask must be positive.\n\n");
                    usage();
                }
                break;
            case 'h':
                usage();
                break;
            default:
                usage();
        }
    }
    return 0;
}