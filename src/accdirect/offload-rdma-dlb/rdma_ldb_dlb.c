/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2018 Intel Corporation
 */
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <cpuid.h>
#include <sched.h>
#include <math.h>
#include <x86intrin.h>

#include "../../dlb_8.9.0/libdlb/dlb.h"
#include "../../dlb_8.9.0/libdlb/dlb_priv.h"
#include "../../dlb_8.9.0/libdlb/dlb2_ioctl.h"
#include "common.h"

// ==== DLB variables
uint64_t** dlb_pp_addr[64];
dlb_shared_domain_t** dlb_shared_domain_ptr;
// =============================

uint64_t start_time, end_time;
double difference;

uint64_t hz;

static dlb_dev_cap_t cap;
static int dev_id;
// static uint64_t num_events = 4*128;
static uint64_t num_flows = 1;
static int num_workers = 8;
static int num_queues = 1;
static int num_txs = 1;
static int num_rxs = 0;
static int num_credit_combined;
static int num_credit_ldb;
static int num_credit_dir;
static bool use_max_credit_combined = true;
static bool use_max_credit_ldb = true;
static bool use_max_credit_dir = true;
static int partial_resources = 100;
static enum dlb_event_sched_t sched_type = 1; /* Parallel by default */
// static enum dlb_event_sched_t sched_type = 2; /* Ordered */
static dlb_resources_t rsrcs;

#define CQ_DEPTH 16
// Performance Tuning
static uint64_t enqueue_depth = 4;
static uint64_t dequeue_depth = CQ_DEPTH;
static uint64_t cq_depth = CQ_DEPTH;   /* Must be power of 2 in [8, 1024] */

// Priority statistics
static volatile uint64_t priority_latency[8][8][8];     /* worker index, queue priority, packet priority -> latency */
static volatile uint64_t packet_priority_count[8][8];   /* tx index, packet priority -> packet count */
static int queue_priority[32];                          /* queue id -> tx index mapping */
static volatile uint64_t worker_pkt_count[16];           /* worker index -> worker packet count */
static volatile double worker_tput[8];                  /* worker index -> worker tput */

// Flow statistics
static volatile uint32_t worker_flows_served[8][2048] = {0};                  /* worker index -> flows served by the workers */


// Control signals
static int report_latency = 0;
static int queue_prio_en = 0;
static int packet_prio_en = 0;

// Multi-stage
static int num_stages = 1;


#define NUM_EVENTS_PER_LOOP 4
#define RETRY_LIMIT 100000

#define EPOLL_SIZE 256
#define EPOLL_RETRY 10

static bool epoll_enabled = false;
static uint64_t ticks = 2000; /* 2 sec*/

enum wait_mode_t {
    POLL,
    INTERRUPT,
} wait_mode = INTERRUPT;


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

static void print_sched_type(enum dlb_event_sched_t sched_type)
{
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

static int print_resources(
    dlb_resources_t *rsrcs)
{
    printf("DLB's available resources:\n");
    printf("\tDomains:           %d\n", rsrcs->num_sched_domains);
    printf("\tLDB queues:        %d\n", rsrcs->num_ldb_queues);
    printf("\tLDB ports:         %d\n", rsrcs->num_ldb_ports);
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

static int create_sched_domain(
    dlb_hdl_t dlb)
{
    int p_rsrsc = partial_resources;
    dlb_create_sched_domain_t args = {0};

    args.num_ldb_queues = num_queues;
    args.num_ldb_ports = num_workers;
    args.num_dir_ports = num_txs;
    args.num_ldb_event_state_entries = 2 * args.num_ldb_ports * CQ_DEPTH;
    if (!cap.combined_credits) {
        args.num_ldb_credits = rsrcs.max_contiguous_ldb_credits * p_rsrsc / 100;
        args.num_dir_credits = rsrcs.max_contiguous_dir_credits * p_rsrsc / 100;
        printf("Give num_ldb_credits to be %d, num_dir_credits to be %d\n", args.num_ldb_credits, args.num_dir_credits);
        args.num_ldb_credit_pools = 1;
        args.num_dir_credit_pools = 1;
    } else {
        args.num_credits = rsrcs.num_credits * p_rsrsc / 100;
        printf("Give num_credits to be %d\n", args.num_credits);
        args.num_credit_pools = 1;
    }

    args.num_sn_slots[0] = rsrcs.num_sn_slots[0] * p_rsrsc / 100;
    args.num_sn_slots[1] = rsrcs.num_sn_slots[1] * p_rsrsc / 100;

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
    args.cq_depth = CQ_DEPTH;
    args.num_ldb_event_state_entries = CQ_DEPTH*2;
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

    args.cq_depth = CQ_DEPTH;

    return dlb_create_dir_port(domain, &args, queue_id);
}

typedef struct {
	int id;
    dlb_port_hdl_t port;
    int queue_id;
    int efd;
    unsigned int core_mask;
    int index;
    struct conn_context* conn_ctxs;
    struct dev_context dev_ctx;
} thread_args_t;

static void *tx_traffic(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[NUM_EVENTS_PER_LOOP];
    int64_t num_loops, i, num_tx = 0;
    int ret;

    num_loops = (num_events == 0) ? -1 : num_events / NUM_EVENTS_PER_LOOP;

    /* Initialize the static fields in the send events */
    for (i = 0; i < NUM_EVENTS_PER_LOOP; i++) {
        events[i].send.queue_id = args->queue_id;
        events[i].send.sched_type = sched_type;
        events[i].send.priority = 0;
	    if (!sched_type)
		    events[i].send.flow_id = 0xABCD;
    }

    for (i = 0; (num_tx < num_events) || (num_loops == -1); i++) {
        int j, num;

        /* Initialize the dynamic fields in the send events */
        for (j = 0; j < NUM_EVENTS_PER_LOOP; j++) {
            events[j].adv_send.udata64 = num_tx + j;
            events[j].adv_send.udata16 = (num_tx + j) % UINT16_MAX;
        }

        /* Send the events */
        ret = 0;
        num = 0;
        for (j = 0; num != NUM_EVENTS_PER_LOOP && j < RETRY_LIMIT; j++) {
            ret = dlb_send(args->port, NUM_EVENTS_PER_LOOP-num, &events[num]);

            if (ret == -1)
                break;

            num += ret;
        }
	    num_tx += num;
	    if (num_tx % 1000000 == 0)
		    printf("[%s] Sent events : %ld\n", __func__, num_tx);

#if 0
        if (num != NUM_EVENTS_PER_LOOP) {
            printf("[%s()] FAILED: Sent %d/%d events on iteration %ld!\n",
                   __func__, num, NUM_EVENTS_PER_LOOP, i);
            exit(-1);
        }
#endif
    }

    printf("[%s()] Sent %ld events\n",
           __func__, num_tx);

    return NULL;
}

static volatile bool worker_done = false;

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

static void *rx_traffic(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[dequeue_depth];
    int ret, nfds = 0, epoll_fd;
    struct epoll_event epoll_events;
    int epoll_retry = EPOLL_RETRY;
    int64_t num_loops, i, num_mismatch = 0, num_rx = 0;

    printf("Start RX thread\n");
    printf("%ld events\n", num_events);

    if (epoll_enabled)
        epoll_fd = setup_epoll(args);

    num_loops = (num_events == 0) ? -1 : num_events / dequeue_depth;

    int last_verified_udata64 = 0;
    for (i = 0; num_rx < num_events && num_loops != -1; i++)
    {
        // printf("keep trying...\n");
        int j, num = 0;
        epoll_retry = EPOLL_RETRY;
        nfds = 0;
        if (epoll_enabled) {
            while (nfds == 0 && --epoll_retry > 0) {
                nfds = epoll_wait(epoll_fd, &epoll_events, 1, ticks);
                if (nfds < 0) {
                    printf("[%s()] FAILED: epoll_wait", __func__);
                    goto end;
                }
            }
            if (nfds == 0 && epoll_retry == 0) {
                printf("[%s()] TIMEOUT: No eventfd ready in %ld msec. Exiting.\n",
                            __func__, ticks * EPOLL_RETRY);
                goto end;
            }
            num = dlb_recv(args->port, dequeue_depth,
                    (wait_mode == INTERRUPT), &events[0]);
            if (num == -1) {
                printf("[%s()] ERROR: dlb_recv failure in epoll mode\n", __func__);
                num = 0; /* Received 0 events */
            } else {
                for (int e = 0; e < num; e++) {
                    uint64_t recv_data = events[num].recv.udata64;
                    printf("[%s()] received packet %d has data %lx\n", __func__, num_rx+e, recv_data);
                }
            }
        }
        else
        {
            if (num_rx == dequeue_depth) {
                // start test latency
                start_time = rdtscp();
            }
            /* Receive the events */
            for (j = 0; (num != dequeue_depth && j < RETRY_LIMIT); j++) {
                ret = dlb_recv(args->port, dequeue_depth-num,
                            (wait_mode == INTERRUPT), &events[num]);

                if (ret == -1) {
                    printf("[%s()] ERROR: dlb_recv failure at iterations %d\n", __func__, j);
                    break;
                }

                num += ret;
                if ((j != 0) && (j % RETRY_LIMIT == 0)) {
                    printf("[%s()] TIMEOUT: Rx blocked for %d iterations\n", __func__, j);
                    return NULL;
                }
            }

            if (num != dequeue_depth) {
                printf("[%s()] FAILED: Recv'ed %d events (iter %ld)!\n", __func__, num, i);
                // exit(-1);
                return NULL;
            }
        }

        /* Validate the events */
        for (j = 0; j < num; j++)
        {
            if (events[j].recv.sched_type == SCHED_UNORDERED)
            {
                if (events[j].recv.error)
                    printf("[%s()] FAILED: Bug in received event [PARALLEL]", __func__);
            }
            else if ((events[j].recv.udata16 != (num_rx + j) % UINT16_MAX)) 
            {
                printf("[%s()] FAILED: Bug in received event num_rx + j:%ld "
                "(num_rx: %ld, j: %d), events[j].recv.udata64: %ld  "
                "events[j].recv.udata16 : %d\n",
                __func__, num_rx + j, num_rx, j, events[j].recv.udata64,
                events[j].recv.udata16);
                num_mismatch++;
                if (num_mismatch > 100)
                // exit(-1);
                return NULL;
            }
            else {
                last_verified_udata64 = events[j].recv.udata64;
                // printf("[%s()] SUCCESS: received event num_rx + j:%ld "
                // "(num_rx: %ld, j: %d), events[j].recv.udata64: %ld  "
                // "events[j].recv.udata16 : %d\n",
                // __func__, num_rx + j, num_rx, j, events[j].recv.udata64,
                // events[j].recv.udata16);
            }
        }
        num_rx += num;

        // printf("[%s] Received events : %ld\n", __func__, num_rx);
        // if (num_rx % 1000000 == 0)
            // printf("[%s] Received events : %ld\n", __func__, num_rx);
        if (dlb_release(args->port, num) != num)
        {
            printf("[%s()] FAILED: Release of all %d events (iter %ld)!\n",
                __func__, num, i);
            // exit(-1);
            return NULL;
        }
    }
    // end test latency
    end_time = rdtscp();
    uint64_t freq = get_tsc_freq_arch();
    printf("TSC Freq is %ld Hz\n", freq);
    difference = (double)(end_time - start_time) / (double)freq * 1e6;
    printf("RDMA running time is %lf us\n", difference);
    printf("RDMA request per sec is %lf mrps\n", (double)num_rx / difference);
    printf("rx_thread done\n");

end:
    printf("[%s()] Received %ld events, num_mismatch: %ld\n",
           __func__, num_rx, num_mismatch);
    worker_done = true;
    if (epoll_enabled) {
        close(epoll_fd);
		close(args->efd);
    }

    return NULL;
}

static void *worker_fn(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    int epoll_fd, epoll_retry = EPOLL_RETRY;
    struct epoll_event epoll_events;
    int i, j, ret, nfds = 0;
    int64_t total = 0;
    int64_t total_latency_test = 0;
	struct conn_context *conn_ctxs = args->conn_ctxs;
    int tid = args->index;

    uint64_t worker_start, worker_end;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) <0) {
        perror("pthread_setaffinity_np");
    }

    dlb_event_t events[dequeue_depth];
    int64_t num_tx, num_rx;

    uint64_t timestamp = 0;

    printf("Worker %d started\n", args->index);

    if (epoll_enabled)
        epoll_fd = setup_epoll(args);

    for (i = 0; !worker_done; i++) {
        if (epoll_enabled)
        {
            while (nfds == 0 && --epoll_retry > 0) {
                nfds = epoll_wait(epoll_fd, &epoll_events, 1, ticks);
                if (worker_done) {
                    goto end;
                } else if (nfds < 0) {
                    printf("[%s()] FAILED: epoll_wait", __func__);
                    goto end;
                }
            }
            if (nfds == 0 && epoll_retry == 0) {
                printf("[%s()] TIMEOUT: No eventfd ready in %ld msec. Exiting.\n",
                        __func__, ticks * EPOLL_RETRY);
                goto end;
            }
            num_rx = dlb_recv(args->port, dequeue_depth,
                            (wait_mode == INTERRUPT), events);
        }
        else
        {
            /* Receive the events */
            for (j = 0, num_rx = 0; num_rx == 0; j++) {
                if (worker_done) goto end;
                num_rx = dlb_recv(args->port,
                                dequeue_depth,
                                (wait_mode == INTERRUPT),
                                events);
            }
        }
        /* The port was disabled, indicating the thread should return */
        if (num_rx == -1 && errno == EACCES)
            break;

        total += num_rx;
        // printf("Dequeued %ld events\n", num_rx);
        // printf("[%s] Received events : %ld\n", __func__, total);

        /* Validate the events, send back to client */
        for (j = 0; j < num_rx; j++) {
            int remote_tid = events[j].recv.udata16;
            // ((char *)events[j].recv.udata64)[2] = 'a';
            timestamp = ((uint64_t *)(events[j].recv.udata64))[0];
            // delay_cycles(timestamp);

            delay_cycles(1000);

            if (remote_tid == num_client_conns - 1) {
                post_send(&(conn_ctxs[num_client_conns + num_dlb_pp]), bufs_num*PAGE_SIZE, 16);
                total_latency_test++;
                wait_completions(&(conn_ctxs[num_client_conns + num_dlb_pp]), SEND_WRID, 1);
            }
        }
        worker_pkt_count[args->index] = total_latency_test;

        if (dlb_release(args->port, num_rx) != num_rx) {
            printf("[%s()] FAILED: Release of all %ld events (iter %d)!\n",
                __func__, num_rx, i);
            exit(-1);
        }
    }
end:
    worker_pkt_count[args->index] = total;
    printf("[%s()] Received %ld events\n",
           __func__, total);

    if (epoll_enabled) {
        close(epoll_fd);
        close(args->efd);
    }
    return NULL;
}


static void *monitor_traffic(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    int64_t i;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0) {
        perror("pthread_setaffinity_np");
    }

    if (num_stages == 1 && num_events != 0) {
        uint64_t count = 0;
        while (count < num_events) {
            count = 0;
            for (i = 0; i < num_workers; i++) {
                count += worker_pkt_count[i];
            }
        }
        printf("count is %ld!\n", count);
        goto end;
    }

end:
    worker_done = true;
    return NULL;
}


static void dlb_movdir64b(struct dlb_enqueue_qe *qe4, uint64_t *pp_addr)
{
    /* TODO: Change to proper assembly when compiler support available */
    asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02"
                 :
                 : "a" (pp_addr), "d" (qe4));
}

int ldb_traffic(int argc, char **argv)
{
    pthread_t rx_thread, tx_thread, *worker_threads, monitor_thread;
    thread_args_t rx_args, tx_args, *worker_args, monitor_args;
    int ldb_pool_id, dir_pool_id;
    int rx_port_id, tx_port_id;
    dlb_domain_hdl_t domain;
    int num_seq_numbers = 0;
    int worker_queue_id;
    int domain_id, i;
    dlb_hdl_t dlb;
    unsigned long core_idx = 2;

    num_txs = num_client_conns;
    num_queues = (num_txs > 8) ? 8 : num_txs;
    num_workers = num_dlb_workers;
    hz = get_tsc_freq_arch();

/************************************************/
/****************** Configure DLB ***************/
/************************************************/
    worker_threads = (pthread_t *)calloc(num_workers, sizeof(pthread_t));
    if (!worker_threads)
        return -ENOMEM;

    worker_args = (thread_args_t *)calloc(num_workers, sizeof(thread_args_t));
    if (!worker_args) {
        free(worker_threads);
        return -ENOMEM;
    }

    if (dlb_open(dev_id, &dlb) == -1)
        error(1, errno, "dlb_open");

    if (dlb_get_dev_capabilities(dlb, &cap))
        error(1, errno, "dlb_get_dev_capabilities");

    if (dlb_get_num_resources(dlb, &rsrcs))
        error(1, errno, "dlb_get_num_resources");

    if (print_resources(&rsrcs))
        error(1, errno, "print_resources");

    if (sched_type == 2) { /* Ordered */
    if (dlb_get_ldb_sequence_number_allocation(dlb, 0, &num_seq_numbers))
        error(1, errno, "dlb_get_ldb_sequence_number_allocation");
    }

    domain_id = create_sched_domain(dlb);
    if (domain_id == -1)
        error(1, errno, "dlb_create_sched_domain");

    dlb_shared_domain_ptr = malloc(sizeof(dlb_shared_domain_t*));
    domain = dlb_attach_sched_domain(dlb, domain_id, dlb_shared_domain_ptr);
    if (domain == NULL)
        error(1, errno, "dlb_attach_sched_domain");

    if (!cap.combined_credits) {
        int max_ldb_credits = rsrcs.num_ldb_credits * partial_resources / 100;
        int max_dir_credits = rsrcs.num_dir_credits * partial_resources / 100;
        printf("max_ldb_credits is %d, max_dir_credits is %d\n", max_ldb_credits, max_dir_credits);

        if (use_max_credit_ldb == true)
            ldb_pool_id = dlb_create_ldb_credit_pool(domain, max_ldb_credits);
        else if (num_credit_ldb <= max_ldb_credits)
            ldb_pool_id = dlb_create_ldb_credit_pool(domain,
                                                        num_credit_ldb);
        else
            error(1, EINVAL, "Requested ldb credits are unavailable!");

        if (ldb_pool_id == -1)
            error(1, errno, "dlb_create_ldb_credit_pool");

        if (use_max_credit_dir == true)
            dir_pool_id = dlb_create_dir_credit_pool(domain, max_dir_credits);
        else if (num_credit_dir <= max_dir_credits)
            dir_pool_id = dlb_create_dir_credit_pool(domain,
                                                        num_credit_dir);
        else
            error(1, EINVAL, "Requested dir credits are unavailable!");

        if (dir_pool_id == -1)
            error(1, errno, "dlb_create_dir_credit_pool");
    } else {
	    int max_credits = rsrcs.num_credits * partial_resources / 100;
        printf("max_credits %d\n", max_credits);

        if (use_max_credit_combined == true)
            ldb_pool_id = dlb_create_credit_pool(domain, max_credits);
        else if (num_credit_combined <= max_credits)
            ldb_pool_id = dlb_create_credit_pool(domain,
                                                    num_credit_combined);
        else
            error(1, EINVAL, "Requested combined credits are unavailable!");

        if (ldb_pool_id == -1)
            error(1, errno, "dlb_create_credit_pool");
    }

    /* Create TX ports */
    dlb_port_hdl_t tx_ports[num_client_conns];
    int tx_queues[num_client_conns];
    for (int i = 0; i < num_txs; i++) {
        tx_port_id = create_dir_port(domain, ldb_pool_id, dir_pool_id, -1);
        if (tx_port_id == -1)
            error(1, errno, "dlb_create_dir_port");

        dlb_pp_addr[i] = (uint64_t**)malloc(sizeof(uint64_t*));
        tx_ports[i] = dlb_attach_dir_port(domain, tx_port_id, dlb_pp_addr[i]);
        if (dlb_pp_addr[i] == NULL) {
            error(1, errno, "dlb_attach_dir_port");
        } else {
            printf("[%s()] RDMA sees DLB dlb_pp_addr: %p\n", __func__, *(dlb_pp_addr[i]));
        }

        if (tx_ports[i] == NULL)
            error(1, errno, "dlb_attach_dir_port");

        // printf("tx %d, queue_id = %d, prio_idx = %d\n", i, tx_queues[i], (queue_prio_en)? i : 0);
    }

    /* Create dlb queues */
    for (int i = 0; i < num_queues; i++) {
        tx_queues[i] = create_ldb_queue(domain, num_seq_numbers);
        printf("queue id is %d\n", tx_queues[i]);
        if (tx_queues[i] == -1)
            error(1, errno, "dlb_create_ldb_queue");
        if (queue_prio_en != 0) {
            queue_priority[tx_queues[i]] = i;
        }  else {
            queue_priority[tx_queues[i]] = 0;
        }
        printf("queue_id = %d, prio_idx = %d\n", tx_queues[i], (queue_prio_en)? i : 0);
    }

    // rx_port_id = create_ldb_port(domain, ldb_pool_id, dir_pool_id);
    // if (rx_port_id == -1)
    //     error(1, errno, "dlb_create_ldb_port");

    // rx_args.port = dlb_attach_ldb_port(domain, rx_port_id, NULL);
    // if (rx_args.port == NULL)
    //     error(1, errno, "dlb_attach_ldb_port");

    /* Create worker queue */
    worker_queue_id = 0;
    // if (num_workers) {
    //     worker_queue_id = create_ldb_queue(domain, 0);
    //     if (worker_queue_id == -1)
    //         error(1, errno, "dlb_create_ldb_queue");
    // }

    /* Create worker ports */
    for (i = 0; i < num_workers; i++) {
        int port_id;
        port_id = create_ldb_port(domain, ldb_pool_id, dir_pool_id);
        if (port_id == -1)
            error(1, errno, "dlb_create_ldb_port");
        worker_args[i].port = dlb_attach_ldb_port(domain, port_id, NULL);
        if (worker_args[i].port == NULL)
            error(1, errno, "dlb_attach_ldb_port");

        worker_args[i].queue_id = worker_queue_id;

        if (queue_prio_en != 0) {
            // enable queue priority
            for (int j = 0; j < num_queues; j++) {
                if (dlb_link_queue(worker_args[i].port, tx_queues[j], queue_priority[tx_queues[j]]) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        } else {
            // disable queue priority
            for (int j = 0; j < num_queues; j++) {
                if (dlb_link_queue(worker_args[i].port, tx_queues[j], 0) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        }
            
        worker_args[i].core_mask = core_idx;
        core_idx += 1;
        worker_args[i].index = i;
        printf("worker %d, prio_idx = %d\n", i, (queue_prio_en)? i : 0);
    }

    if (dlb_launch_domain_alert_thread(domain, NULL, NULL))
        error(1, errno, "dlb_launch_domain_alert_thread");

    if (dlb_start_sched_domain(domain))
        error(1, errno, "dlb_start_sched_domain");

    /* Launch monitor thread */
    monitor_args.core_mask = core_idx;
    core_idx += 1;
    pthread_create(&monitor_thread, NULL, monitor_traffic, &monitor_args);

    /* Add sleep here to make sure the rx_thread is staretd before tx_thread */
    usleep(1000);


/************************************************/
/***************** Configure RDMA ***************/
/************************************************/
    struct dev_context dev_ctx = {
        .ib_dev_name = "mlx5_0",
        .dev_port = 1, // port id = 1
        .ctx = NULL,
        .channel = NULL,
        .pd = NULL,
        .cq = NULL,
        .use_event = false
    };
    if (init_device_ctx(&dev_ctx) != 0) {
        fprintf(stderr, "Failed to initialize device context.\n");
        return EXIT_FAILURE;
    }

    num_dlb_pp = num_txs;
    num_meta_conns = num_txs;
    // 1 for server credit communication, num_dlb_pp for SNIC to enqueue DLB, num_meta_conns for client to RDMA write data to server
    int num_qps = 1 + num_dlb_pp + num_meta_conns;
    struct conn_context *conn_ctxs = (struct conn_context*)malloc(num_qps * sizeof(struct conn_context));
    if (conn_ctxs == NULL) goto destroy_device;

    int num_qps_init = 0;
    if (*(dlb_pp_addr[0]) != (void *) -1 && *dlb_shared_domain_ptr != (void *) -1) {
        for (int i = 0; i < num_qps; i++) {
            conn_ctxs[i].id = i;
            conn_ctxs[i].dev_ctx = &dev_ctx;
            conn_ctxs[i].validate_buf = false;
            conn_ctxs[i].inline_msg = false;
            conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
            conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            conn_ctxs[i].qp = NULL;
            conn_ctxs[i].cq = NULL;
            conn_ctxs[i].data_mr = NULL;
            conn_ctxs[i].dlb_data_qp = -1;
            conn_ctxs[i].dlb_credit_qp = -1;
            conn_ctxs[i].rdma_data_qp = -1;
            
            if (i == 0) {
                // for SNIC and server DLB credit communication
                conn_ctxs[i].data_buf = (unsigned char *)&((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits);
                printf("Server credit address: %p\n", conn_ctxs[i].data_buf);
                printf("Server credit address: %p\n", &((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[1].avail_credits));
                printf("credit pool: %u %u\n", ((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits), ((*dlb_shared_domain_ptr)->sw_credits.dir_pools[0].avail_credits));
                conn_ctxs[i].data_buf_size = 64;
                conn_ctxs[i].dlb_credit_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
            } else if (i > 0 && i <= num_dlb_pp) {
                // for SNIC to enqueue DLB
                conn_ctxs[i].data_buf = (unsigned char *)(*(dlb_pp_addr[i-1]));
                conn_ctxs[i].data_buf_size = PAGE_SIZE;
                conn_ctxs[i].dlb_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
                printf("DLB payload address: %p\n", *(dlb_pp_addr[i-1]));
            } else {
                // for client to RDMA write data to server
                conn_ctxs[i].data_buf = NULL;
                conn_ctxs[i].data_buf_size = bufs_num*PAGE_SIZE*2;
                conn_ctxs[i].rdma_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
                // printf("Client payload address: %%hhn\n", conn_ctxs[i].data_buf);
            }

            if (init_connection_ctx(&conn_ctxs[i]) != 0) {
                fprintf(stderr, "Failed to initialize connection %d.\n", i);
            }
            num_qps_init++;
        }
    } else {
        printf("[%s()] Faield to allocate MMAP DLB MMIO Space addr: %p\n", __func__, dlb_pp_addr);
    }
    printf("Initialize %d connection success\n", num_qps_init);

    // sleep(600);

    // start connection
    char *server_ip = NULL;
    int host_type = SERVER;
    int server_sockfd = -1;
    int client_sockfd = -1;
    int snic_sockfd = -1;
    if (host_type == CLIENT) {
        printf("Connecting server for data transter %s ...\n", server_ip);
        if ((server_sockfd = get_server_dest(&conn_ctxs[0], server_ip, "12340", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
        }
        printf("Connecting snic for metadata %s ...\n", snic_ip);
        if ((snic_sockfd = get_server_dlb_dest(&conn_ctxs[0], snic_ip, "12341", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
        }
    } else if (host_type == SNIC) {
        printf("Connecting server for metadata %s ...\n", server_ip);
        if ((server_sockfd = get_server_dlb_dest(&conn_ctxs[0], server_ip, "12342", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12341) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
        }
    } else if (host_type == SERVER) {
        printf("Waiting for snic's connections ...\n");
        if ((snic_sockfd = get_client_dlb_dest(&conn_ctxs[0], num_qps_init, 12342) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12340) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
        }
    }
    printf("Connection success\n");

    // connect QPs
    for (int i = 0; i < num_qps_init; i++) {
        printf("==== Connect QP %d ====\n", i);
        print_conn_ctx(&conn_ctxs[i]);
        print_dest(&(conn_ctxs[i].local_dest));
        print_dest(&(conn_ctxs[i].remote_dest));
        if (connect_qps(&conn_ctxs[i]) != 0) {
            fprintf(stderr, "Failed to connect QPs\n");
            goto destroy_server_socket;
        }
        printf("Connection QP success\n");
    }

    /* Launch threads */
    for (i = 0; i < num_workers; i++) {
        worker_args[i].conn_ctxs = conn_ctxs;
        pthread_create(&worker_threads[i], NULL, worker_fn, &worker_args[i]);
    }

/************************************************/
/****************** Start Testing ***************/
/************************************************/
    pthread_join(monitor_thread, NULL);
    // pthread_join(rx_thread, NULL);
    // for (int i = 0; i < num_workers; i++)
    //     pthread_join(worker_threads[i], NULL);

    for (i = 0; i < num_workers; i++) {
        if (dlb_disable_port(worker_args[i].port))
            error(1, errno, "dlb_disable_port");
        pthread_join(worker_threads[i], NULL);
    }
    /* The worker threads may be blocked on the CQ interrupt wait queue, so
     * disable their ports in order to wake them before joining the thread.
     */

    printf("Finishing...\n");
    memcpy(conn_ctxs[num_qps_init-1].data_buf, COMPLETE_MSG, sizeof(COMPLETE_MSG));
    post_send(&conn_ctxs[num_qps_init-1], 0, sizeof(COMPLETE_MSG));
    wait_completions(&conn_ctxs[num_qps_init-1], SEND_WRID, 1);
    // printf("credit pool: %u\n", *(uint32_t *)(conn_ctxs[1].data_buf));
    // printf("credit pool: %lu\n", *(uint64_t *)(conn_ctxs[1].data_buf));
    printf("credit pool: %u %u\n", ((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits), ((*dlb_shared_domain_ptr)->sw_credits.dir_pools[0].avail_credits));
    printf("credit pool: %lu %u\n", (uint64_t)((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits), ((*dlb_shared_domain_ptr)->sw_credits.dir_pools[0].avail_credits));
    printf("dlb_sw_credits_t: %lu %u\n", (uint64_t)((*dlb_shared_domain_ptr)->sw_credits.total_credits[0]), ((*dlb_shared_domain_ptr)->sw_credits.avail_credits[0]));
    

#if USE_DLB
destroy_client_socket:
    if (host_type == CLIENT) {
        close(client_sockfd);
    }

destroy_server_socket:
    if (host_type == SERVER) {
        close(server_sockfd);
    }
    
destroy_connections:
    for (unsigned int i = 0; i < num_qps_init; i++) {
        destroy_connection_ctx(&conn_ctxs[i]);
    }
    free(conn_ctxs);
destroy_device:
    destroy_device_ctx(&dev_ctx);
#endif

    for (i = 0; i < num_workers; i++) {
        if (dlb_detach_port(worker_args[i].port) == -1)
            error(1, errno, "dlb_detach_port");
    }

    // clean up the tx ports
    for (i = 0; i < num_txs; i++) {
        if (dlb_detach_port(tx_ports[i]) == -1)
            error(1, errno, "dlb_detach_port");
    }

    if (dlb_detach_sched_domain(domain) == -1)
        error(1, errno, "dlb_detach_sched_domain");

    if (dlb_reset_sched_domain(dlb, domain_id) == -1)
        error(1, errno, "dlb_reset_sched_domain");

    if (dlb_close(dlb) == -1)
        error(1, errno, "dlb_close");

    free(worker_threads);
    free(worker_args);

    for (i = 0; i < 64; i++) {
        if (dlb_pp_addr[i] != NULL) free(dlb_pp_addr[i]);
    }

    return 0;
}
