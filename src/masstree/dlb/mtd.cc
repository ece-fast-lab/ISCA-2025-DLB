/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2014 President and Fellows of Harvard College
 * Copyright (c) 2012-2014 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
// -*- mode: c++ -*-
// mtd: key/value server
//

#include "ServerRDMAConnection.h"
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#if HAVE_SYS_EPOLL_H
#include <sys/epoll.h>
#endif
#if __linux__
#include <asm-generic/mman.h>
#endif
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#ifdef __linux__
#include <malloc.h>
#endif
#include "nodeversion.hh"
#include "kvstats.hh"
#include "json.hh"
#include "kvtest.hh"
#include "kvrandom.hh"
#include "clp.h"
#include "log.hh"
#include "checkpoint.hh"
#include "file.hh"
#include "kvproto.hh"
#include "query_masstree.hh"
#include "masstree_tcursor.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "msgpack.hh"
#include <algorithm>
#include <deque>

////////////////////////added by Hamed for RDMA/////////////////////
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <string>
#include <vector>
#include <sched.h>

#include <infiniband/verbs.h>
#include <linux/types.h>  //for __be32 type
#include "ServerRDMAConnection.cc"

/********************** DLB RDMA headers **********************/
#include "RDMAConnectionRC.cc"
#include "../../dlb_8.9.0/libdlb/dlb.h"
#include "../../dlb_8.9.0/libdlb/dlb_priv.h"
#include "../../dlb_8.9.0/libdlb/dlb2_ioctl.h"
#include "../../dlb_8.9.0/libdlb/dlb_common.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <error.h>
#include <x86intrin.h>
#include <xmmintrin.h>

#if defined(__x86_64__)
#include "utils/x86/cycles.h"
#elif defined(__arm__)
#include "utils/arm/cycles_32.h"
#elif defined(__aarch64__)
#include "utils/arm/cycles_64.h"
#endif

// #include "dlb.h"
// #include "dlb_priv.h"
// #include "dlb2_ioctl.h"
/********************** DLB RDMA headers **********************/


#ifndef debug 
#define debug 0
#endif

typedef struct{
	RDMAConnectionRC *conn;
	ServerRDMAConnection *ud_conn;
	threadinfo *ti;
	int id;
    // DLB variables
    dlb_port_hdl_t port;
    int queue_id;
    int efd;
    unsigned int core_mask;
    int index;
} thread_data;

/********************** DLB variables **********************/
// ==== DLB variables
uint64_t** dlb_pp_addr[64];
dlb_shared_domain_t** dlb_shared_domain_ptr;
// =============================
uint64_t start_time, end_time;
double difference;

int num_dlb_pp = 1;
int num_client_conns = 1;
int num_queues = 8;

uint64_t hz;


static dlb_dev_cap_t cap;
static int dev_id;

static uint64_t num_events = 40;
static uint64_t num_flows = 1;
static int num_workers = 2;
static int num_txs = 1;
static int num_rxs = 0;
static int num_credit_combined;
static int num_credit_ldb;
static int num_credit_dir;
static bool use_max_credit_combined = true;
static bool use_max_credit_ldb = true;
static bool use_max_credit_dir = true;
static int partial_resources = 100;
static enum dlb_event_sched_t sched_type = SCHED_UNORDERED; /* Parallel by default */
// static enum dlb_event_sched_t sched_type = 2; /* Ordered */
static dlb_resources_t rsrcs;

#define CQ_DEPTH 8
// Performance Tuning
static uint64_t enqueue_depth = 4;
static uint64_t dequeue_depth = CQ_DEPTH;
static uint64_t cq_depth = CQ_DEPTH;   /* Must be power of 2 in [8, 1024] */

// Priority statistics
static volatile uint64_t priority_latency[8][8][8];     /* worker index, queue priority, packet priority -> latency */
static volatile uint64_t packet_priority_count[8][8];   /* tx index, packet priority -> packet count */
static int queue_priority[32];                          /* queue id -> tx index mapping */
static volatile uint64_t worker_pkt_count[8];           /* worker index -> worker packet count */
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
#define EPOLL_RETRY 100

static bool epoll_enabled = false;
static uint64_t ticks = 2000; /* 2 sec*/

static volatile bool worker_done;

enum wait_mode_t {
    POLL,
    INTERRUPT,
} wait_mode = POLL;

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
    // args.num_ldb_queues = 1 + (num_workers > 0);
    args.num_ldb_ports = num_workers;
    args.num_dir_ports = num_txs;
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

/* Create eventfd per port and map the eventfd to the port
 * using dlb_enable_cq_epoll() API. Create epoll instance
 * and register the eventfd to monitor for events.
 */
static int setup_epoll(thread_data *args) {
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
/********************** DLB variables **********************/


//static int wr_id [96];
//0-47 is for send, 48-95 is for recv

//extern struct pingpong_dest    *rem_dest;


using namespace std;

// Enable ctrl c to quit the program
// volatile std::atomic<bool> running = true;

/*
inline double tv_to_double(struct timeval *tv) {
  return tv->tv_sec + (double) tv->tv_usec / 1000000;
}

inline double get_time() {
  //#if USE_CLOCK_GETTIME
  //  struct timespec ts;
  //  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  //  //  clock_gettime(CLOCK_REALTIME, &ts);
  //  return ts.tv_sec + (double) ts.tv_nsec / 1000000000;
  //#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv_to_double(&tv);
  //#endif
}
*/
/////////////////////////added by Hamed for getting time ////////

using lcdf::StringAccum;

#include <chrono> //added for service time measurement
#define MEAS_SERV_TIME 0

#if MEAS_SERV_TIME
    int isGet = 0;
    int isPut = 0;
    int isScan = 0;
    int isRemove = 0;
    int isReplace = 0;

    int getCount = 0;
    int putCount = 0;
    int scanCount = 0;
    int removeCount = 0;
    int replaceCount = 0;
#endif

enum { CKState_Quit, CKState_Uninit, CKState_Ready, CKState_Go };

volatile bool timeout[2] = {false, false};
double duration[2] = {10, 0};

Masstree::default_table *tree;

// all default to the number of cores
static int udpthreads = 0;
static int client_threads = 0;
static int tcpthreads = 0;
static int nckthreads = 0;
static int testthreads = 0;
static int nlogger = 0;
static std::vector<int> cores;

static bool logging = true;
static bool pinthreads = false;
static bool recovery_only = false;
volatile uint64_t globalepoch = 1;     // global epoch, updated by main thread regularly
volatile uint64_t active_epoch = 1;
static int port = 2117;
static uint64_t test_limit = ~uint64_t(0);
static int doprint = 0;
int kvtest_first_seed = 31949;

static volatile sig_atomic_t go_quit = 0;
static int quit_pipe[2];

static std::vector<const char*> logdirs;
static std::vector<const char*> ckpdirs;

static logset* logs;
volatile bool recovering = false; // so don't add log entries, and free old value immediately

static double checkpoint_interval = 1000000;
static kvepoch_t ckp_gen = 0; // recover from checkpoint
static ckstate *cks = NULL; // checkpoint status of all checkpointing threads
static pthread_cond_t rec_cond;
pthread_mutex_t rec_mu;
static int rec_nactive;
static int rec_state = REC_NONE;

kvtimestamp_t initial_timestamp;

static pthread_cond_t checkpoint_cond;
static pthread_mutex_t checkpoint_mu;

static void prepare_thread(threadinfo *ti);
static int* tcp_thread_pipes;
static void* tcp_threadfunc(void* ti);
static void* udp_threadfunc(void* ti);

static void log_init();
static void recover(threadinfo*);
static kvepoch_t read_checkpoint(threadinfo*, const char *path);

static void* conc_checkpointer(void* ti);
static void recovercheckpoint(threadinfo* ti);

static void *canceling(void *);
static void catchint(int);
static void epochinc(int);

/* running local tests */
void test_timeout(int) {
    size_t n;
    for (n = 0; n < arraysize(timeout) && timeout[n]; ++n)
        /* do nothing */;
    if (n < arraysize(timeout)) {
        timeout[n] = true;
        if (n + 1 < arraysize(timeout) && duration[n + 1])
            xalarm(duration[n + 1]);
    }
}

struct kvtest_client {
    kvtest_client()
        : checks_(0), kvo_() {
    }
    kvtest_client(const char *testname)
        : testname_(testname), checks_(0), kvo_() {
    }

    int id() const {
        return ti_->index();
    }
    int nthreads() const {
        return testthreads;
    }
    void set_thread(threadinfo *ti) {
        ti_ = ti;
    }
    void register_timeouts(int n) {
        always_assert(n <= (int) arraysize(::timeout));
        for (int i = 1; i < n; ++i)
            if (duration[i] == 0)
                duration[i] = 0;//duration[i - 1];
    }
    bool timeout(int which) const {
        return ::timeout[which];
    }
    uint64_t limit() const {
        return test_limit;
    }
    Json param(const String&) const {
        return Json();
    }
    double now() const {
        return ::now();
    }

    void get(long ikey, Str *value);
    void get(const Str &key);
    void get(long ikey) {
        quick_istr key(ikey);
        get(key.string());
    }
    void get_check(const Str &key, const Str &expected);
    void get_check(const char *key, const char *expected) {
        get_check(Str(key, strlen(key)), Str(expected, strlen(expected)));
    }
    void get_check(long ikey, long iexpected) {
        quick_istr key(ikey), expected(iexpected);
        get_check(key.string(), expected.string());
    }
    void get_check_key8(long ikey, long iexpected) {
        quick_istr key(ikey, 8), expected(iexpected);
        get_check(key.string(), expected.string());
    }
    void get_check_key10(long ikey, long iexpected) {
        quick_istr key(ikey, 10), expected(iexpected);
        get_check(key.string(), expected.string());
    }
    void get_col_check(const Str &key, int col, const Str &expected);
    void get_col_check_key10(long ikey, int col, long iexpected) {
        quick_istr key(ikey, 10), expected(iexpected);
        get_col_check(key.string(), col, expected.string());
    }
    bool get_sync(long ikey);

    void put(const Str &key, const Str &value);
    void put(const char *key, const char *val) {
        put(Str(key, strlen(key)), Str(val, strlen(val)));
    }
    void put(long ikey, long ivalue) {
        quick_istr key(ikey), value(ivalue);
        put(key.string(), value.string());
    }
    void put_key8(long ikey, long ivalue) {
        quick_istr key(ikey, 8), value(ivalue);
        put(key.string(), value.string());
    }
    void put_key10(long ikey, long ivalue) {
        quick_istr key(ikey, 10), value(ivalue);
        put(key.string(), value.string());
    }

    int put_async() {return -1;}
    int get_async(int32_t *a, unsigned n) {return -1;}

    void put_col(const Str &key, int col, const Str &value);
    void put_col_key10(long ikey, int col, long ivalue) {
        quick_istr key(ikey, 10), value(ivalue);
        put_col(key.string(), col, value.string());
    }

    bool remove_sync(long ikey);

    void puts_done() {
    }
    void wait_all() {
    }
    void rcu_quiesce() {
    }
    String make_message(StringAccum &sa) const;
    void notice(const char *fmt, ...);
    void fail(const char *fmt, ...);
    const Json& report(const Json& x) {
        return report_.merge(x);
    }
    void finish() {
        fprintf(stderr, "%d: %s\n", ti_->index(), report_.unparse().c_str());
    }
    threadinfo *ti_;
    query<row_type> q_[10];
    const char *testname_;
    kvrandom_lcg_nr rand;
    int checks_;
    Json report_;
    struct kvout *kvo_;
    static volatile int failing;
};

volatile int kvtest_client::failing;

void kvtest_client::get(long ikey, Str *value)
{
    quick_istr key(ikey);
    if (!q_[0].run_get1(tree->table(), key.string(), 0, *value, *ti_))
        *value = Str();
}

void kvtest_client::get(const Str &key)
{
    Str val;
    (void) q_[0].run_get1(tree->table(), key, 0, val, *ti_);
}

void kvtest_client::get_check(const Str &key, const Str &expected)
{
    Str val;
    if (!q_[0].run_get1(tree->table(), key, 0, val, *ti_)) {
        fail("get(%.*s) failed (expected %.*s)\n", key.len, key.s, expected.len, expected.s);
        return;
    }
    if (val.len != expected.len || memcmp(val.s, expected.s, val.len) != 0)
        fail("get(%.*s) returned unexpected value %.*s (expected %.*s)\n", key.len, key.s,
             std::min(val.len, 40), val.s, std::min(expected.len, 40), expected.s);
    else
        ++checks_;
}

void kvtest_client::get_col_check(const Str &key, int col, const Str &expected)
{
    Str val;
    if (!q_[0].run_get1(tree->table(), key, col, val, *ti_)) {
        fail("get.%d(%.*s) failed (expected %.*s)\n", col, key.len, key.s,
             expected.len, expected.s);
        return;
    }
    if (val.len != expected.len || memcmp(val.s, expected.s, val.len) != 0)
        fail("get.%d(%.*s) returned unexpected value %.*s (expected %.*s)\n",
             col, key.len, key.s, std::min(val.len, 40), val.s,
             std::min(expected.len, 40), expected.s);
    else
        ++checks_;
}

bool kvtest_client::get_sync(long ikey) {
    quick_istr key(ikey);
    Str val;
    return q_[0].run_get1(tree->table(), key.string(), 0, val, *ti_);
}

void kvtest_client::put(const Str &key, const Str &value) {
    while (failing)
        /* do nothing */;
    q_[0].run_replace(tree->table(), key, value, *ti_);
    if (ti_->logger()) // NB may block
        ti_->logger()->record(logcmd_replace, q_[0].query_times(), key, value);
}

void kvtest_client::put_col(const Str &key, int col, const Str &value) {
    while (failing)
        /* do nothing */;
#if !MASSTREE_ROW_TYPE_STR
    if (!kvo_)
        kvo_ = new_kvout(-1, 2048);
    Json req[2] = {Json(col), Json(String::make_stable(value))};
    (void) q_[0].run_put(tree->table(), key, &req[0], &req[2], *ti_);
    if (ti_->logger()) // NB may block
        ti_->logger()->record(logcmd_put, q_[0].query_times(), key,
                              &req[0], &req[2]);
#else
    (void) key, (void) col, (void) value;
    assert(0);
#endif
}

bool kvtest_client::remove_sync(long ikey) {
    quick_istr key(ikey);
    bool removed = q_[0].run_remove(tree->table(), key.string(), *ti_);
    if (removed && ti_->logger()) // NB may block
        ti_->logger()->record(logcmd_remove, q_[0].query_times(), key.string(), Str());
    return removed;
}

String kvtest_client::make_message(StringAccum &sa) const {
    const char *begin = sa.begin();
    while (begin != sa.end() && isspace((unsigned char) *begin))
        ++begin;
    String s = String(begin, sa.end());
    if (!s.empty() && s.back() != '\n')
        s += '\n';
    return s;
}

void kvtest_client::notice(const char *fmt, ...) {
    va_list val;
    va_start(val, fmt);
    String m = make_message(StringAccum().vsnprintf(500, fmt, val));
    va_end(val);
    if (m)
        fprintf(stderr, "%d: %s", ti_->index(), m.c_str());
}

void kvtest_client::fail(const char *fmt, ...) {
    static nodeversion32 failing_lock(false);
    static nodeversion32 fail_message_lock(false);
    static String fail_message;
    failing = 1;

    va_list val;
    va_start(val, fmt);
    String m = make_message(StringAccum().vsnprintf(500, fmt, val));
    va_end(val);
    if (!m)
        m = "unknown failure";

    fail_message_lock.lock();
    if (fail_message != m) {
        fail_message = m;
        fprintf(stderr, "%d: %s", ti_->index(), m.c_str());
    }
    fail_message_lock.unlock();

    if (doprint) {
        failing_lock.lock();
        fprintf(stdout, "%d: %s", ti_->index(), m.c_str());
        tree->print(stdout);
        fflush(stdout);
    }

    always_assert(0);
}

static void* testgo(void* x) {
    kvtest_client *kc = reinterpret_cast<kvtest_client*>(x);
    kc->ti_->pthread() = pthread_self();
    prepare_thread(kc->ti_);

    if (strcmp(kc->testname_, "rw1") == 0)
        kvtest_rw1(*kc);
    else if (strcmp(kc->testname_, "rw2") == 0)
        kvtest_rw2(*kc);
    else if (strcmp(kc->testname_, "rw3") == 0)
        kvtest_rw3(*kc);
    else if (strcmp(kc->testname_, "rw4") == 0)
        kvtest_rw4(*kc);
    else if (strcmp(kc->testname_, "rwsmall24") == 0)
        kvtest_rwsmall24(*kc);
    else if (strcmp(kc->testname_, "rwsep24") == 0)
        kvtest_rwsep24(*kc);
    else if (strcmp(kc->testname_, "palma") == 0)
        kvtest_palma(*kc);
    else if (strcmp(kc->testname_, "palmb") == 0)
        kvtest_palmb(*kc);
    else if (strcmp(kc->testname_, "rw16") == 0)
        kvtest_rw16(*kc);
    else if (strcmp(kc->testname_, "rw5") == 0
             || strcmp(kc->testname_, "rw1fixed") == 0)
        kvtest_rw1fixed(*kc);
    else if (strcmp(kc->testname_, "ycsbk") == 0)
        kvtest_ycsbk(*kc);
    else if (strcmp(kc->testname_, "wd1") == 0)
        kvtest_wd1(10000000, 1, *kc);
    else if (strcmp(kc->testname_, "wd1check") == 0)
        kvtest_wd1_check(10000000, 1, *kc);
    else if (strcmp(kc->testname_, "w1") == 0)
        kvtest_w1_seed(*kc, kvtest_first_seed + kc->id());
    else if (strcmp(kc->testname_, "r1") == 0)
        kvtest_r1_seed(*kc, kvtest_first_seed + kc->id());
    else if (strcmp(kc->testname_, "wcol1") == 0)
        kvtest_wcol1at(*kc, kc->id() % 24, kvtest_first_seed + kc->id() % 48, 5000000);
    else if (strcmp(kc->testname_, "rcol1") == 0)
        kvtest_rcol1at(*kc, kc->id() % 24, kvtest_first_seed + kc->id() % 48, 5000000);
    else
        kc->fail("unknown test '%s'", kc->testname_);
    return 0;
}

static const char * const kvstats_name[] = {
    "ops", "ops_per_sec", "puts", "gets", "scans", "puts_per_sec", "gets_per_sec", "scans_per_sec"
};

void runtest(const char *testname, int nthreads) {
    std::vector<kvtest_client> clients(nthreads, kvtest_client(testname));
    ::testthreads = nthreads;
    for (int i = 0; i < nthreads; ++i)
        clients[i].set_thread(threadinfo::make(threadinfo::TI_PROCESS, i));
    bzero((void *)timeout, sizeof(timeout));
    signal(SIGALRM, test_timeout);
    if (duration[0])
        xalarm(duration[0]);
    for (int i = 0; i < nthreads; ++i) {
        int r = pthread_create(&clients[i].ti_->pthread(), 0, testgo, &clients[i]);
        always_assert(r == 0);
    }
    for (int i = 0; i < nthreads; ++i)
        pthread_join(clients[i].ti_->pthread(), 0);

    kvstats kvs[arraysize(kvstats_name)];
    for (int i = 0; i < nthreads; ++i)
        for (int j = 0; j < (int) arraysize(kvstats_name); ++j)
            if (double x = clients[i].report_.get_d(kvstats_name[j]))
                kvs[j].add(x);
    for (int j = 0; j < (int) arraysize(kvstats_name); ++j)
        kvs[j].print_report(kvstats_name[j]);
}


struct conn {
    int fd;
    enum { inbufsz = 20 * 1024, inbufrefill = 16 * 1024 };

    conn(int s)
        : fd(s), inbuf_(new char[inbufsz]),
          inbufpos_(0), inbuflen_(0), kvout(new_kvout(s, 20 * 1024)),
          inbuftotal_(0) {
    }
    ~conn() {
        close(fd);
        free_kvout(kvout);
        delete[] inbuf_;
        for (char* x : oldinbuf_)
            delete[] x;
    }

    Json& receive() {
        while (!parser_.done() && check(2))
            inbufpos_ += parser_.consume(inbuf_ + inbufpos_,
                                         inbuflen_ - inbufpos_,
                                         String::make_stable(inbuf_, inbufsz));
        if (parser_.success() && parser_.result().is_a())
            parser_.reset();
        else
            parser_.result() = Json();
        return parser_.result();
    }

    int check(int tryhard) {
        if (inbufpos_ == inbuflen_ && tryhard)
            hard_check(tryhard);
        return inbuflen_ - inbufpos_;
    }

    uint64_t xposition() const {
        return inbuftotal_ + inbufpos_;
    }
    Str recent_string(uint64_t xposition) const {
        if (xposition - inbuftotal_ <= unsigned(inbufpos_))
            return Str(inbuf_ + (xposition - inbuftotal_),
                       inbuf_ + inbufpos_);
        else
            return Str();
    }

  private:
    char* inbuf_;
    int inbufpos_;
    int inbuflen_;
    std::vector<char*> oldinbuf_;
    msgpack::streaming_parser parser_;
  public:
    struct kvout *kvout;
  private:
    uint64_t inbuftotal_;

    void hard_check(int tryhard);
};

void conn::hard_check(int tryhard) {
    masstree_precondition(inbufpos_ == inbuflen_);
    if (parser_.empty()) {
        inbuftotal_ += inbufpos_;
        inbufpos_ = inbuflen_ = 0;
        for (auto x : oldinbuf_)
            delete[] x;
        oldinbuf_.clear();
    } else if (inbufpos_ == inbufsz) {
        oldinbuf_.push_back(inbuf_);
        inbuf_ = new char[inbufsz];
        inbuftotal_ += inbufpos_;
        inbufpos_ = inbuflen_ = 0;
    }
    if (tryhard == 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {0, 0};
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            return;
    } else
        kvflush(kvout);

    ssize_t r = read(fd, inbuf_ + inbufpos_, inbufsz - inbufpos_);
    if (r != -1)
        inbuflen_ += r;
}

struct conninfo {
    int s;
    Json handshake;
};


/* main loop */
enum { clp_val_suffixdouble = Clp_ValFirstUser };
enum { opt_nolog = 1, opt_pin, opt_logdir, opt_port, opt_ckpdir, opt_duration,
       opt_test, opt_test_name, opt_threads, opt_cores,
       opt_print, opt_norun, opt_checkpoint, opt_limit, opt_epoch_interval, opt_client_threads};
static const Clp_Option options[] = {
    { "no-log", 0, opt_nolog, 0, 0 },
    { 0, 'n', opt_nolog, 0, 0 },
    { "no-run", 0, opt_norun, 0, 0 },
    { "pin", 'p', opt_pin, 0, Clp_Negate },
    { "logdir", 0, opt_logdir, Clp_ValString, 0 },
    { "ld", 0, opt_logdir, Clp_ValString, 0 },
    { "checkpoint", 'c', opt_checkpoint, Clp_ValDouble, Clp_Optional | Clp_Negate },
    { "ckp", 0, opt_checkpoint, Clp_ValDouble, Clp_Optional | Clp_Negate },
    { "ckpdir", 0, opt_ckpdir, Clp_ValString, 0 },
    { "ckdir", 0, opt_ckpdir, Clp_ValString, 0 },
    { "cd", 0, opt_ckpdir, Clp_ValString, 0 },
    { "port", 0, opt_port, Clp_ValInt, 0 },
    { "duration", 'd', opt_duration, Clp_ValDouble, 0 },
    { "limit", 'l', opt_limit, clp_val_suffixdouble, 0 },
    { "test", 0, opt_test, Clp_ValString, 0 },
    { "test-rw1", 0, opt_test_name, 0, 0 },
    { "test-rw2", 0, opt_test_name, 0, 0 },
    { "test-rw3", 0, opt_test_name, 0, 0 },
    { "test-rw4", 0, opt_test_name, 0, 0 },
    { "test-rw5", 0, opt_test_name, 0, 0 },
    { "test-rw16", 0, opt_test_name, 0, 0 },
    { "test-palm", 0, opt_test_name, 0, 0 },
    { "test-ycsbk", 0, opt_test_name, 0, 0 },
    { "test-rw1fixed", 0, opt_test_name, 0, 0 },
    { "threads", 'j', opt_threads, Clp_ValInt, 0 },
    { "client_threads", 'k', opt_client_threads, Clp_ValInt, 0 },
    { "cores", 0, opt_cores, Clp_ValString, 0 },
    { "print", 0, opt_print, 0, Clp_Negate },
    { "epoch-interval", 0, opt_epoch_interval, Clp_ValDouble, 0 }
};

// uint64_t rdtscp(){
//     uint32_t low, high;
//     __asm__ volatile ("rdtscp": "=a" (low), "=d" (high) :: "ecx");
//     uint64_t timestamp = (((uint64_t)high) << 32) | low;
//     return timestamp;
// }

int main(int argc, char *argv[])
{
    using std::swap;
    int s, ret, yes = 1, i = 1, j = 0, firstcore = -1, corestride = 1;
    const char *dotest = 0;
    nlogger = tcpthreads = udpthreads = nckthreads = sysconf(_SC_NPROCESSORS_ONLN);
    Clp_Parser *clp = Clp_NewParser(argc, argv, (int) arraysize(options), options);
    Clp_AddType(clp, clp_val_suffixdouble, Clp_DisallowOptions, clp_parse_suffixdouble, 0);
    int opt;
    double epoch_interval_ms = 1000;



    while ((opt = Clp_Next(clp)) >= 0) {
        switch (opt) {
        case opt_nolog:
            logging = false;
            break;
        case opt_pin:
            pinthreads = !clp->negated;
            break;
        case opt_threads:
            nlogger = tcpthreads = udpthreads = nckthreads = clp->val.i;
            break;
        case opt_client_threads:
            client_threads = clp->val.i;
            break;
        case opt_logdir: {
            const char *s = strtok((char *) clp->vstr, ",");
            for (; s; s = strtok(NULL, ","))
                logdirs.push_back(s);
            break;
        }
        case opt_ckpdir: {
            const char *s = strtok((char *) clp->vstr, ",");
            for (; s; s = strtok(NULL, ","))
                ckpdirs.push_back(s);
            break;
        }
        case opt_checkpoint:
            if (clp->negated || (clp->have_val && clp->val.d <= 0))
                checkpoint_interval = -1;
            else if (clp->have_val)
                checkpoint_interval = clp->val.d;
            else
                checkpoint_interval = 30;
            break;
        case opt_port:
            port = clp->val.i;
            break;
        case opt_duration:
            duration[0] = clp->val.d;
            break;
        case opt_limit:
            test_limit = (uint64_t) clp->val.d;
            break;
        case opt_test:
            dotest = clp->vstr;
            break;
        case opt_test_name:
            dotest = clp->option->long_name + 5;
            break;
        case opt_print:
            doprint = !clp->negated;
            break;
        case opt_cores:
            if (firstcore >= 0 || cores.size() > 0) {
                Clp_OptionError(clp, "%<%O%> already given");
                exit(EXIT_FAILURE);
            } else {
                const char *plus = strchr(clp->vstr, '+');
                Json ij = Json::parse(clp->vstr),
                    aj = Json::parse(String("[") + String(clp->vstr) + String("]")),
                    pj1 = Json::parse(plus ? String(clp->vstr, plus) : "x"),
                    pj2 = Json::parse(plus ? String(plus + 1) : "x");
                for (int i = 0; aj && i < aj.size(); ++i)
                    if (!aj[i].is_int() || aj[i].to_i() < 0)
                        aj = Json();
                if (ij && ij.is_int() && ij.to_i() >= 0)
                    firstcore = ij.to_i(), corestride = 1;
                else if (pj1 && pj2 && pj1.is_int() && pj1.to_i() >= 0 && pj2.is_int())
                    firstcore = pj1.to_i(), corestride = pj2.to_i();
                else if (aj) {
                    for (int i = 0; i < aj.size(); ++i)
                        cores.push_back(aj[i].to_i());
                } else {
                    Clp_OptionError(clp, "bad %<%O%>, expected %<CORE1%>, %<CORE1+STRIDE%>, or %<CORE1,CORE2,...%>");
                    exit(EXIT_FAILURE);
                }
            }
            break;
        case opt_norun:
            recovery_only = true;
            break;
        case opt_epoch_interval:
        epoch_interval_ms = clp->val.d;
        break;
        default:
            fprintf(stderr, "Usage: mtd [-np] [--ld dir1[,dir2,...]] [--cd dir1[,dir2,...]]\n");
            exit(EXIT_FAILURE);
        }
    }
    Clp_DeleteParser(clp);
    if (logdirs.empty())
        logdirs.push_back(".");
    if (ckpdirs.empty())
        ckpdirs.push_back(".");
    if (firstcore < 0)
        firstcore = cores.size() ? cores.back() + 1 : 0;
    for (; (int) cores.size() < udpthreads; firstcore += corestride)
        cores.push_back(firstcore);

    // for -pg profiling
    signal(SIGINT, catchint);

    // log epoch starts at 1
    global_log_epoch = 1;
    global_wake_epoch = 0;
    log_epoch_interval.tv_sec = 0;
    log_epoch_interval.tv_usec = 200000;

    // set a timer for incrementing the global epoch
    if (!dotest) {
        if (!epoch_interval_ms) {
        printf("WARNING: epoch interval is 0, it means no GC is executed\n");
        } else {
        signal(SIGALRM, epochinc);
        struct itimerval etimer;
        etimer.it_interval.tv_sec = epoch_interval_ms / 1000;
        etimer.it_interval.tv_usec = fmod(epoch_interval_ms, 1000) * 1000;
        etimer.it_value.tv_sec = epoch_interval_ms / 1000;
        etimer.it_value.tv_usec = fmod(epoch_interval_ms, 1000) * 1000;
        ret = setitimer(ITIMER_REAL, &etimer, NULL);
        always_assert(ret == 0);
        }
    }

    // for parallel recovery
    ret = pthread_cond_init(&rec_cond, 0);
    always_assert(ret == 0);
    ret = pthread_mutex_init(&rec_mu, 0);
    always_assert(ret == 0);

    // for waking up the checkpoint thread
    ret = pthread_cond_init(&checkpoint_cond, 0);
    always_assert(ret == 0);
    ret = pthread_mutex_init(&checkpoint_mu, 0);
    always_assert(ret == 0);

    threadinfo *main_ti = threadinfo::make(threadinfo::TI_MAIN, -1);
    main_ti->pthread() = pthread_self();

    initial_timestamp = timestamp();
    tree = new Masstree::default_table;
    tree->initialize(*main_ti);
    printf("%s, %s, pin-threads %s, ", tree->name(), row_type::name(),
            pinthreads ? "enabled" : "disabled");
    if(logging){
        printf("logging enabled\n");
        log_init();
        recover(main_ti);
    } else {
        printf("logging disabled\n");
    }

    /////////////////RDMA control path initialization////////////

    // UDP threads, each with its own port.
    if (udpthreads == 0)
        printf("0 udp threads\n");
    else if (udpthreads == 1)
        printf("1 udp thread (port %d)\n", port);
    else
        printf("%d udp threads (ports %d-%d)\n", udpthreads, port, port + udpthreads - 1);


    /********************** DLB Configuration**********************/
    num_workers = udpthreads;
    // num_txs = udpthreads;
    // num_dlb_pp = udpthreads;
    // num_client_conns = udpthreads;
    num_txs = client_threads;
    num_dlb_pp = client_threads;
    num_client_conns = client_threads;
    num_queues = (num_txs < 8) ? num_txs : 8;

    hz = get_tsc_freq_arch();

    // Prepare the worker threads
    thread_data tdata[num_workers];
    pthread_t pt[num_workers];

    int ldb_pool_id, dir_pool_id;
    int rx_port_id, tx_port_id;
    dlb_domain_hdl_t domain;
    unsigned int num_seq_numbers = 0;
    int worker_queue_id = -1;
    int domain_id;
    dlb_hdl_t dlb;
    unsigned long core_idx = 2;

    dlb_port_hdl_t tx_ports[num_txs];
    int tx_queues[num_queues];

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

    dlb_shared_domain_ptr = (dlb_shared_domain_t**)malloc(sizeof(dlb_shared_domain_t*));
    domain = dlb_attach_sched_domain(dlb, domain_id, (void**)dlb_shared_domain_ptr);
    if (domain == NULL)
        error(1, errno, "dlb_attach_sched_domain");
    if (dlb_shared_domain_ptr != NULL) {
        printf("[%s()] RDMA sees DLB dlb_shared_domain_ptr: %p\n", __func__, *dlb_shared_domain_ptr);
    }
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
    }

    /* Create TX ports */
    for (int i = 0; i < num_txs; i++) {
        // tx_queues[i] = create_ldb_queue(domain, num_seq_numbers);
        // printf("queue id is %d\n", tx_queues[i]);
        // if (tx_queues[i] == -1)
        //     error(1, errno, "dlb_create_ldb_queue");
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

    /* Create worker ports */
    for (i = 0; i < num_workers; i++) {
        int port_id;
        port_id = create_ldb_port(domain, ldb_pool_id, dir_pool_id);
        if (port_id == -1)
            error(1, errno, "dlb_create_ldb_port");

        tdata[i].port = dlb_attach_ldb_port(domain, port_id, NULL);
        if (tdata[i].port == NULL)
            error(1, errno, "dlb_attach_ldb_port");

        tdata[i].queue_id = worker_queue_id;

        if (queue_prio_en != 0) {
            // enable queue priority
            for (j = 0; j < num_queues; j++) {
                if (dlb_link_queue(tdata[i].port, tx_queues[j], queue_priority[tx_queues[j]]) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        } else {
            // disable queue priority
            for (j = 0; j < num_queues; j++) {
                if (dlb_link_queue(tdata[i].port, tx_queues[j], 0) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        }
            
        tdata[i].core_mask = core_idx;
        core_idx += 1;
        tdata[i].index = i;
        printf("worker %d, prio_idx = %d\n", i, (queue_prio_en)? i : 0);
    }

    if (dlb_launch_domain_alert_thread(domain, NULL, NULL))
        error(1, errno, "dlb_launch_domain_alert_thread");

    if (dlb_start_sched_domain(domain))
        error(1, errno, "dlb_start_sched_domain");

/****************************** DLB Configuration **********************************/

    /***************************** RDMA Initialization *****************************/
    // UD QPs
    vector<ServerRDMAConnection*> ud_connections;
    for (int i = 0; i < num_workers; i++) {
      ud_connections.push_back(new ServerRDMAConnection(i));
    }
    vector<RDMAConnectionRC *> connections;
    for (int i = 0; i < 1; i++) {
        RDMAConnectionRC *conn;// = new RDMAConnectionRC(i);
        #if IS_CLIENT
        // Only for compilation purpose
        conn = new RDMAConnectionRC(i, num_dlb_pp + num_client_conns, num_dlb_pp, num_client_conns);
        #elif IS_SERVER
        conn = new RDMAConnectionRC(i, 1 + num_dlb_pp + num_client_conns, dlb_pp_addr, dlb_shared_domain_ptr, num_dlb_pp, num_client_conns);
        #endif
        connections.push_back(conn);
    }

    for(i = 0; i < num_workers; i++){
        threadinfo *ti = threadinfo::make(threadinfo::TI_PROCESS, i);
        tdata[i].ti = ti;
        tdata[i].conn = connections[0];  
        tdata[i].ud_conn = ud_connections[i];  
        tdata[i].id = i;
        ret = pthread_create(&pt[i], NULL, udp_threadfunc, &tdata[i]);
        //printf("exited udp_threadfunc \n");
        always_assert(ret == 0);
        if(i == 0) { 
            printf("inside sleep \n");
            // sleep(1);
        }
    }

    for(i = 0; i < num_workers; i++){
        ret = pthread_join(pt[i], NULL);
        assert(!ret);
    }

    /***************************** Clean Up RDMA and DLB *****************************/
    printf("Finishing...\n");
    memcpy(connections[0]->conn_ctxs[2].data_buf, COMPLETE_MSG, sizeof(COMPLETE_MSG));
    connections[0]->post_send(&connections[0]->conn_ctxs[2], 0, sizeof(COMPLETE_MSG));
    // connections[0]->wait_completions(&connections[0]->dev_ctx, SEND_WRID, 1);
    // printf("credit pool: %u\n", *(uint32_t *)(conn_ctxs[1].data_buf));
    // printf("credit pool: %lu\n", *(uint64_t *)(conn_ctxs[1].data_buf));
    printf("credit pool: %u %u\n", ((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits), ((*dlb_shared_domain_ptr)->sw_credits.dir_pools[0].avail_credits));
    printf("credit pool: %lu %u\n", (uint64_t)((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits), ((*dlb_shared_domain_ptr)->sw_credits.dir_pools[0].avail_credits));
    printf("dlb_sw_credits_t: %lu %u\n", (uint64_t)((*dlb_shared_domain_ptr)->sw_credits.total_credits[0]), ((*dlb_shared_domain_ptr)->sw_credits.avail_credits[0]));
    
destroy_client_socket:
    if (connections[0]->host_type == CLIENT) {
        close(connections[0]->client_sockfd);
    }

destroy_server_socket:
    if (connections[0]->host_type == SERVER) {
        close(connections[0]->server_sockfd);
    }
    
destroy_connections:
    for (unsigned int i = 0; i < connections[0]->num_qps_init; i++) {
        connections[0]->destroy_connection_ctx(&connections[0]->conn_ctxs[i]);
    }
    free(connections[0]->conn_ctxs);

destroy_device:
    connections[0]->destroy_device_ctx(&(connections[0]->dev_ctx));

    /* The worker threads may be blocked on the CQ interrupt wait queue, so
     * disable their ports in order to wake them before joining the thread.
     */
    /* Clean up */
    for (i = 0; i < num_workers; i++) {
        // clean up the worker ports
        if (dlb_detach_port(tdata[i].port) == -1)
            error(1, errno, "dlb_detach_port");
    }

    for (i = 0; i < num_txs; i++) {
        // clean up the tx ports
        if (dlb_detach_port(tx_ports[i]) == -1)
            error(1, errno, "dlb_detach_port");
    }

    if (dlb_detach_sched_domain(domain) == -1)
        error(1, errno, "dlb_detach_sched_domain");

    if (dlb_reset_sched_domain(dlb, domain_id) == -1)
        error(1, errno, "dlb_reset_sched_domain");

    if (dlb_close(dlb) == -1)
        error(1, errno, "dlb_close");

    for (i = 0; i < 64; i++) {
        if (dlb_pp_addr[i] != NULL) free(dlb_pp_addr[i]);
    }
    /***************************** Clean Up DLB *****************************/

    if (dotest) {
        if (strcmp(dotest, "palm") == 0) {
            runtest("palma", 1);
            runtest("palmb", tcpthreads);
        } else
            runtest(dotest, tcpthreads);
        tree->stats(stderr);
        if (doprint)
            tree->print(stdout);
        exit(0);
    }

    /*
    // TCP socket and threads

    s = socket(AF_INET, SOCK_STREAM, 0);
    always_assert(s >= 0);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);
    ret = bind(s, (struct sockaddr *) &sin, sizeof(sin));
    if (ret < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    ret = listen(s, 100);
    if (ret < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    threadinfo **tcpti = new threadinfo *[tcpthreads];
    tcp_thread_pipes = new int[tcpthreads * 2];
    printf("%d tcp threads (port %d)\n", tcpthreads, port);
    for(i = 0; i < tcpthreads; i++){
        threadinfo *ti = threadinfo::make(threadinfo::TI_PROCESS, i);
        ret = pipe(&tcp_thread_pipes[i * 2]);
        always_assert(ret == 0);
        ret = pthread_create(&ti->pthread(), 0, tcp_threadfunc, ti);
        always_assert(ret == 0);
        tcpti[i] = ti;
    }
    */
    // Create a canceling thread.
    //   ret = pipe(quit_pipe);
    //   always_assert(ret == 0);
    //   pthread_t canceling_tid;
    //   ret = pthread_create(&canceling_tid, NULL, canceling, NULL);
    //   always_assert(ret == 0);

    //   static int next = 0;
    //   while(1){
    //     int s1;
    //     struct sockaddr_in sin1;
    //     socklen_t sinlen = sizeof(sin1);

        
    //     bzero(&sin1, sizeof(sin1));
    //     s1 = accept(s, (struct sockaddr *) &sin1, &sinlen);
    //     always_assert(s1 >= 0);
    //     setsockopt(s1, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        

    //     // Complete handshake.
    //     char buf[BUFSIZ];
        
    //     ssize_t nr = -1;//read(s1, buf, BUFSIZ);
    //     if (nr == -1) {
    //         perror("read");
        
    //     kill_connection:
    //         close(s1);
    //         //continue;
    //     }
        
    //     msgpack::streaming_parser sp;
    //     if (nr == 0 || sp.consume(buf, nr) != (size_t) nr
    //         || !sp.result().is_a() || sp.result().size() < 2
    //         || !sp.result()[1].is_i() || sp.result()[1].as_i() != Cmd_Handshake) {
    //         fprintf(stderr, "failed handshake\n");
    //         goto kill_connection;
    //     }
        

    //     int target_core = -1;
    //     if (sp.result().size() >= 3 && sp.result()[2].is_o()
    //         && sp.result()[2]["core"].is_i())
    //         target_core = sp.result()[2]["core"].as_i();
    //     if (target_core < 0 || target_core >= tcpthreads) {
    //         target_core = next % tcpthreads;
    //         ++next;
    //     }

    //     conninfo* ci = new conninfo;
    //     ci->s = s1;
    //     swap(ci->handshake, sp.result());

    //     ssize_t w = write(tcp_thread_pipes[2*target_core + 1], &ci, sizeof(ci));
    //     always_assert((size_t) w == sizeof(ci));
    //   }
}

void
catchint(int)
{
    go_quit = 1;
    char cmd = 0;
    // Does not matter if the write fails (when the pipe is full)
    int r = write(quit_pipe[1], &cmd, sizeof(cmd));
    (void)r;
}

inline const char *threadtype(int type) {
  switch (type) {
    case threadinfo::TI_MAIN:
      return "main";
    case threadinfo::TI_PROCESS:
      return "process";
    case threadinfo::TI_LOG:
      return "log";
    case threadinfo::TI_CHECKPOINT:
      return "checkpoint";
    default:
      always_assert(0 && "Unknown threadtype");
      break;
  };
}

void *
canceling(void *)
{
    /*
    char cmd;
    int r = read(quit_pipe[0], &cmd, sizeof(cmd));
    (void) r;
    assert(r == sizeof(cmd) && cmd == 0);
    // Cancel wake up checkpointing threads
    pthread_mutex_lock(&checkpoint_mu);
    pthread_cond_signal(&checkpoint_cond);
    pthread_mutex_unlock(&checkpoint_mu);

    fprintf(stderr, "\n");
    // cancel outstanding threads. Checkpointing threads will exit safely
    // when the checkpointing thread 0 sees go_quit, and don't need cancel
    for (threadinfo *ti = threadinfo::allthreads; ti; ti = ti->next())
        if (ti->purpose() != threadinfo::TI_MAIN
            && ti->purpose() != threadinfo::TI_CHECKPOINT) {
            int r = pthread_cancel(ti->pthread());
            always_assert(r == 0);
        }
    */

    // join canceled threads
    for (threadinfo *ti = threadinfo::allthreads; ti; ti = ti->next())
        if (ti->purpose() != threadinfo::TI_MAIN) {
            fprintf(stderr, "joining thread %s:%d\n",
                    threadtype(ti->purpose()), ti->index());
            int r = pthread_join(ti->pthread(), 0);
            always_assert(r == 0);
        }
    tree->stats(stderr);
    exit(0);
}

void
epochinc(int)
{
    globalepoch += 2;
    active_epoch = threadinfo::min_active_epoch();
}

// Return 1 if success, -1 if I/O error or protocol unmatch
int handshake(Json& request, threadinfo& ti) {
    always_assert(request.is_a() && request.size() >= 2
                  && request[1].is_i() && request[1].as_i() == Cmd_Handshake
                  && (request.size() == 2 || request[2].is_o()));
    if (request.size() >= 2
        && request[2]["maxkeylen"].is_i()
        && request[2]["maxkeylen"].as_i() > MASSTREE_MAXKEYLEN) {
        request[2] = false;
        request[3] = "bad maxkeylen";
        request.resize(4);
    } else {
        request[2] = true;
        request[3] = ti.index();
        request[4] = row_type::name();
        request.resize(5);
    }
    request[1] = Cmd_Handshake + 1;
    return request[2].as_b() ? 1 : -1;
}

// execute command, return result.
int onego(query<row_type>& q, Json& request, Str request_str, threadinfo& ti) {
    
    int command = request[1].as_i();
    // if (command != 8) {
	//     printf("command = %d \n", command);
    // }
    if (command == Cmd_Checkpoint) {
        #if debug 
            printf("Cmd_Checkpoint \n");
        #endif
        // force checkpoint
        pthread_mutex_lock(&checkpoint_mu);
        pthread_cond_broadcast(&checkpoint_cond);
        pthread_mutex_unlock(&checkpoint_mu);
        request.resize(2);
    } else if (command == Cmd_Get) {
        #if debug 
            printf("Cmd_Get \n");
        #endif
        #if MEAS_SERV_TIME 
            //printf("get \n");
            isGet = 1;
            getCount++;
        #endif
            // printf("Cmd_Get \n");
        q.run_get(tree->table(), request, ti);
    } else if (command == Cmd_Put && request.size() > 3 && (request.size() % 2) == 1) { // insert or update
	    // printf("command = %d \n", command);
        #if debug 
            printf("Cmd_Put \n");
        #endif
        #if MEAS_SERV_TIME
            //printf("put \n");
            isPut = 1;
            putCount++;
        #endif
        Str key(request[2].as_s());
        const Json* req = request.array_data() + 3;
        const Json* end_req = request.end_array_data();
        request[2] = q.run_put(tree->table(), request[2].as_s(),
                               req, end_req, ti);
        if (ti.logger() && request_str) {
            // use the client's parsed version of the request
            msgpack::parser mp(request_str.data());
            mp.skip_array_size().skip_primitives(3);
            ti.logger()->record(logcmd_put, q.query_times(), key, Str(mp.position(), request_str.end()));
        } else if (ti.logger())
            ti.logger()->record(logcmd_put, q.query_times(), key, req, end_req);
        request.resize(3);
    } else if (command == Cmd_Replace) { // insert or update
	    // printf("command = %d \n", command);
        #if debug 
            printf("Cmd_Replace \n");
        #endif
        #if MEAS_SERV_TIME
            //printf("replace \n");
            isReplace = 1;
            replaceCount++;
        #endif
            // printf("Cmd_Replace \n");
        Str key(request[2].as_s()), value(request[3].as_s());
        request[2] = q.run_replace(tree->table(), key, value, ti);
        if (ti.logger()) // NB may block
            ti.logger()->record(logcmd_replace, q.query_times(), key, value);
        request.resize(3);
    } else if (command == Cmd_Remove) { // remove
        #if debug 
            printf("Cmd_Remove \n");
        #endif
        #if MEAS_SERV_TIME
            isRemove = 1;
            removeCount++;
            //printf("remove \n");
        #endif
            // printf("Cmd_Remove \n");
        Str key(request[2].as_s());
        bool removed = q.run_remove(tree->table(), key, ti);
        if (removed && ti.logger()) // NB may block
            ti.logger()->record(logcmd_remove, q.query_times(), key, Str());
        request[2] = removed;
        request.resize(3);
    } else if (command == Cmd_Scan) {
        #if debug
            printf("Cmd_Scan \n");
        #endif
        #if MEAS_SERV_TIME
            //printf("scan \n");
            isScan = 1;
            scanCount++;
		    //printf("request[3].as_i() = %d \n",request[3].as_i());
        #endif
            // printf("Cmd_Scan \n");
        q.run_scan(tree->table(), request, ti);
    } else {
		#if debug 
            printf("didn't recognize command = %d \n", command);
        #endif
        request[1] = -1;
        request.resize(2);
        return -1;
    }
    request[1] = command + 1;
    return 1;
}

#if HAVE_SYS_EPOLL_H
struct tcpfds {
    int epollfd;

    tcpfds(int pipefd) {
        epollfd = epoll_create(10);
        if (epollfd < 0) {
            perror("epoll_create");
            exit(EXIT_FAILURE);
        }
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = (void *) 1;
        int r = epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd, &ev);
        always_assert(r == 0);
    }

    enum { max_events = 100 };
    typedef struct epoll_event eventset[max_events];
    int wait(eventset &es) {
        return epoll_wait(epollfd, es, max_events, -1);
    }

    conn *event_conn(eventset &es, int i) const {
        return (conn *) es[i].data.ptr;
    }

    void add(int fd, conn *c) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        int r = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
        always_assert(r == 0);
    }

    void remove(int fd) {
        int r = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
        always_assert(r == 0);
    }
};
#else
class tcpfds {
    int nfds_;
    fd_set rfds_;
    std::vector<conn *> conns_;

  public:
    tcpfds(int pipefd)
        : nfds_(pipefd + 1) {
        always_assert(pipefd < FD_SETSIZE);
        FD_ZERO(&rfds_);
        FD_SET(pipefd, &rfds_);
        conns_.resize(nfds_, 0);
        conns_[pipefd] = (conn *) 1;
    }

    typedef fd_set eventset;
    int wait(eventset &es) {
        es = rfds_;
        int r = select(nfds_, &es, 0, 0, 0);
        return r > 0 ? nfds_ : r;
    }

    conn *event_conn(eventset &es, int i) const {
        return FD_ISSET(i, &es) ? conns_[i] : 0;
    }

    void add(int fd, conn *c) {
        always_assert(fd < FD_SETSIZE);
        FD_SET(fd, &rfds_);
        if (fd >= nfds_) {
            nfds_ = fd + 1;
            conns_.resize(nfds_, 0);
        }
        conns_[fd] = c;
    }

    void remove(int fd) {
        always_assert(fd < FD_SETSIZE);
        FD_CLR(fd, &rfds_);
        if (fd == nfds_ - 1) {
            while (nfds_ > 0 && !FD_ISSET(nfds_ - 1, &rfds_))
                --nfds_;
        }
    }
};
#endif

void prepare_thread(threadinfo *ti) {
#if __linux__
    if (pinthreads) {
        cpu_set_t cs;
        CPU_ZERO(&cs);
        //CPU_SET(cores[ti->index()], &cs);
        CPU_SET(ti->index(), &cs);
		//printf("ti->index() = %d, cores[ti->index()] = %d \n",ti->index(), cores[ti->index()]);
        always_assert(sched_setaffinity(0, sizeof(cs), &cs) == 0);
    }
#else
    always_assert(!pinthreads && "pinthreads not supported\n");
#endif
    if (logging)
        ti->set_logger(&logs->log(ti->index() % nlogger));
}

void* tcp_threadfunc(void* x) {
    threadinfo* ti = reinterpret_cast<threadinfo*>(x);
    ti->pthread() = pthread_self();
    prepare_thread(ti);

    int myfd = tcp_thread_pipes[2 * ti->index()];
    tcpfds sloop(myfd);
    tcpfds::eventset events;
    std::deque<conn*> ready;
    query<row_type> q;

    while (1) {
        int nev = sloop.wait(events);
        for (int i = 0; i < nev; i++)
            if (conn *c = sloop.event_conn(events, i))
                ready.push_back(c);

        while (!ready.empty()) {
            conn* c = ready.front();
            ready.pop_front();

            if (c == (conn *) 1) {
                // new connections
#define MAX_NEWCONN 100
                conninfo* ci[MAX_NEWCONN];
                ssize_t len = read(myfd, ci, sizeof(ci));
                always_assert(len > 0 && len % sizeof(int) == 0);
                for (int j = 0; j * sizeof(*ci) < (size_t) len; ++j) {
                    struct conn *c = new conn(ci[j]->s);
                    sloop.add(c->fd, c);
                    int ret = handshake(ci[j]->handshake, *ti);
                    msgpack::unparse(*c->kvout, ci[j]->handshake);
                    kvflush(c->kvout);
                    if (ret < 0) {
                        sloop.remove(c->fd);
                        delete c;
                    }
                    delete ci[j];
                }
            } else if (c) {
                // Should not block as suggested by epoll
                uint64_t xposition = c->xposition();
                Json& request = c->receive();
                int ret;
                if (unlikely(!request))
                    goto closed;
                ti->rcu_start();
                ret = onego(q, request, c->recent_string(xposition), *ti);
                ti->rcu_stop();
                msgpack::unparse(*c->kvout, request);
                request.clear();
                if (likely(ret >= 0)) {
                    if (c->check(0))
                        ready.push_back(c);
                    else
                        kvflush(c->kvout);
                    continue;
                }
                printf("socket read error\n");
            closed:
                kvflush(c->kvout);
                sloop.remove(c->fd);
                delete c;
            }
        }
    }
    return 0;
}

inline double tv_to_double(struct timeval *tv) {
  return tv->tv_sec + (double) tv->tv_usec / 1000000;
}

inline double get_time() {
  //#if USE_CLOCK_GETTIME
  //  struct timespec ts;
  //  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  //  //  clock_gettime(CLOCK_REALTIME, &ts);
  //  return ts.tv_sec + (double) ts.tv_nsec / 1000000000;
  //#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv_to_double(&tv);
  //#endif
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


// serve a client udp socket, in a dedicated thread
void* udp_threadfunc(void* x) 
{
	printf("inside udpthreadfunc \n");
    //printf("1: %d  |  2: %d \n", thread_ptr->ti, thread_ptr->id);
       
    #if MEAS_SERV_TIME
        FILE *fput;
        FILE *fget;
        FILE *fscan;
        FILE *fremove;
        FILE *freplace;

        fput = fopen("put.txt", "w");
        fget = fopen("get.txt", "w");
        fscan = fopen("scan.txt", "w");
        fremove = fopen("remove.txt", "w");
        freplace = fopen("replace.txt", "w");

    #endif

	thread_data *tdata = (thread_data*) x;

    if(tdata->id != 0) sleep(2);

    int tid = tdata->id;


	RDMAConnectionRC *conn = tdata->conn;
	ServerRDMAConnection *ud_conn = tdata->ud_conn;
    // conn = new RDMAConnectionRC(tdata->id);
	// struct conn_context *conn = tdata->conn;

    // printf("1: %d  |  2: %d \n", tdata->ti, tdata->id);
	threadinfo* ti = reinterpret_cast<threadinfo*>(tdata->ti);
	ti->pthread() = pthread_self();
	prepare_thread(ti);

	//printf("thread id = %d \n",tdata->id);
    //printf("tid = %d, buf_recv[0]: %x \n",tdata->id,&conn->buf_recv[0]);

	int cpu = tdata->id;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);       //clears the cpuset
    CPU_SET( cpu , &cpuset); //set CPU 2 on cpuset
	sched_setaffinity(0, sizeof(cpuset), &cpuset);
	//tdata->conn->runRDMAServer();

	struct kvout *kvout = new_bufkvout();
	msgpack::streaming_parser parser;
	StringAccum sa;
	query<row_type> q;

    const int num_bufs = conn->bufs_num;

    const int tx_depth = 1;

        /*************************** DLB ***************************/
        dlb_event_t events[CQ_DEPTH];
        int ret, nfds = 0, epoll_fd;
        struct epoll_event epoll_events;
        int epoll_retry = EPOLL_RETRY;
        int64_t i, num_mismatch = 0, num_rx = 0;

        int num_recved = 0;
        int total_num_recved = 0;

        if (epoll_enabled)
            epoll_fd = setup_epoll(tdata);

        int last_verified_udata64 = 0;
        printf("T%d starts running\n", tid);


        while (!go_quit) {
            // DLB Receive
            int j, num = 0;
            epoll_retry = EPOLL_RETRY;
            nfds = 0;
            if (epoll_enabled) {
                while (nfds == 0) {
                    nfds = epoll_wait(epoll_fd, &epoll_events, 8, -1);
                    if (nfds < 0) {
                        printf("[%s()] FAILED: epoll_wait", __func__);
                        goto end;
                    }
                }
                num = dlb_recv(tdata->port, CQ_DEPTH,
                        (wait_mode == INTERRUPT), &events[0]);
                if (num == -1) {
                    printf("[%s()] ERROR: dlb_recv failure in epoll mode\n", __func__);
                    num = 0; /* Received 0 events */
                }
            }
            else
            {
                // if (num_rx == dequeue_depth) {
                //     // start test latency
                //     start_time = rdtscp();
                // }
                /* Receive the events */
                // int retry = 0;
                num = 0;
                for (j = 0; (num < 1 && !go_quit); j++) {
                    #if debug
                        printf("T%d - num = %d, dequeue_depth = %d, j = %d, RETRY_LIMIT = %d\n", tid, num, dequeue_depth, j, RETRY_LIMIT);
                    #endif
                    ret = dlb_recv(tdata->port, CQ_DEPTH-num, (wait_mode == INTERRUPT), events);
                    #if debug
                        printf("T%d dlb_recv ret = %d\n", tid, ret);
                    #endif
                    if (ret == -1) {
                        printf("[%s()] ERROR: dlb_recv failure at iterations %d\n", __func__, j);
                        break;
                    }
                    num += ret;
                }

                // if (num != dequeue_depth) {
                //     printf("[%s()] FAILED: Recv'ed %d events (iter %ld)!\n", __func__, num, i);
                //     // exit(-1);
                //     return NULL;
                // }
            }

            #if debug
            printf("T%d - dequeue element data is %p:\n", tid, events[0].recv.udata64);
            #endif

            // poll UD CQ
            struct ibv_wc wc[num_bufs*2];
            int ne = 0;
            ne += ibv_poll_cq(ud_conn->ctx->cq, num_bufs, wc);

            // Process the received events
            for (j = 0; j < num; j++) {
                if (events[j].recv.error)
                    printf("[%s()] FAILED: Bug in received event [PARALLEL]", __func__);
                // int a = events[j].recv.udata16;
                int remote_qpn = events[j].recv.udata16;
                // __AUTO_GENERATED_PRINT_VAR_START__
                int send_index = num_recved;

                /* dummy test */
                // delay_cycles(400);
                // int success;
                // success = ud_conn->pp_post_send(ud_conn->ctx, remote_qpn, sa.length()+2, send_index);
                // if (success == EINVAL) printf("Invalid value provided in wr \n");
                // else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation T%d, remote_qpn %d\n", tid, remote_qpn);
                // else if (success == EFAULT) printf("Invalid value provided in qp \n");
                // else if (success != 0) {
                //     printf("success = %d, \n",success);
                //     fprintf(stderr, "Couldn't post send 2 \n");
                //     //return 1;
                // }
                // else {
                //     // ++conn->conn_ctxs[1+num_dlb_pp+tid].souts;
                //     num_recved = (num_recved + 1) % (num_bufs/num_workers);
                //     total_num_recved++;
                //     #if debug 
                //         printf("T%d - total_num_recved = %d \n", tid, total_num_recved);
                //         printf("T%d - send posted... souts = %d, \n", tid, conn->conn_ctxs[1+num_dlb_pp+tid].souts);
                //     #endif
                // }


                #if debug
                printf("T%d - ");
                for(int p = 0; p < 30; p++) {	
                    printf("%x",((const char *)(events[j].recv.udata64))[p]);
                }
                printf("\n");
                #endif

                kvout_reset(kvout);
                //printf("Buffer length after = %d \n", buf.length());
                parser.reset();
                //if(debug) printf("parse_state 1 = %d \n", parser.state_);

                #if MEAS_SERV_TIME
                    //start timestamp
                    //auto start = std::chrono::high_resolution_clock::now();

                    uint64_t start = rdtscp();
                #endif

                // if (events[j].recv.udata64 & 511 != 0) printf("T%d - events[%d].recv.udata64 is %lx\n", tid, num_rx+j, events[j].recv.udata64);
                // _mm_mfence();
                
                unsigned consumed = parser.consume((const char *)(events[j].recv.udata64+2), 256, String((const char *)(events[j].recv.udata64+2)));
                // printf("T%d - consumed = %d\n", tid, consumed);
                #if debug
                if (num_rx > conn->iters / num_workers) {
                    printf("tid = %d, remote_qpn = %d, cnt = %d, addr = %lx\n", tid, remote_qpn, num_rx+j, events[j].recv.udata64);
                    for(int p = 0; p < 30; p++)
                    {	
                        printf("%x",(((uint8_t *)(events[j].recv.udata64))[p]));
                    }
                    printf("\n");

                    int command = parser.result()[1].as_i();
                    printf("command is %d \n", command);

                    // printf("key is %s \n", parser.result()[2].as_s().c_str());
                    // printf("value is %s \n", parser.result()[3].as_s().c_str());
                    // printf("request type is %d %d\n", ((const char *)events[j].recv.udata64)[0], ((const char *)events[j].recv.udata64)[1]);
                }
                #endif
                // int command = (parser.result())[1].as_i();


                // Fail if we received a partial request
                if (parser.success() && parser.result().is_a()) 
                {
                    ti->rcu_start();
                    //if(debug) printf("parse_state 4 = %d \n", parser.state_);

                    if (onego(q, parser.result(), Str((const char *)(events[j].recv.udata64+2), consumed), *ti) >= 0) 
                    {
                        for(int p = 0; p < sa.length(); p++)
                        {	
                            sa.data()[p] = 0x00;
                        }
                        sa.clear();
                        
                        msgpack::unparser<StringAccum> cu(sa);
                        cu << parser.result();

                        #if MEAS_SERV_TIME
                            //end timestamp
                            /*
                            auto end = std::chrono::high_resolution_clock::now();
                            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                            printf("start %llu, end = %llu, elapsed = %llu \n",start,end,elapsed);
                            */

                            uint64_t end = rdtscp();
                            uint64_t elapsed = (end-start);///1000;
                            //printf("start %llu, end = %llu, elapsed = %llu \n",start,end,(end-start));
                            
                            //fprintf(fp, "This is testing for fprintf...\n");
                            std::string tmp = std::to_string(elapsed);
                            char const *num_char = tmp.c_str();
                            
                            if(isGet){
                                if(getCount > 10000) {
                                    fputs(num_char, fget);
                                    fputs("\n", fget);
                                }
                                isGet = 0;
                            }
                            else if(isReplace){
                                if(replaceCount > 10000) {
                                    fputs(num_char, freplace);
                                    fputs("\n", freplace);
                                }
                                isReplace = 0;
                            }
                            else if(isPut){
                                if(putCount > 10000) {
                                    fputs(num_char, fput);
                                    fputs("\n", fput);
                                }
                                isPut = 0;
                            }
                            else if(isScan){
                                if(scanCount > 10000) {
                                    fputs(num_char, fscan);
                                    fputs("\n", fscan);
                                }
                                isScan = 0;
                            }
                            else if(isRemove){
                                if(removeCount > 10000) {
                                    fputs(num_char, fremove);
                                    fputs("\n", fremove);
                                }
                                isRemove = 0;
                            }
                        #endif
                        
                        
                        #if debug
                        if (num_rx > conn->iters) {
                            printf("printing send_buf, length =  %d \n",sa.length());
                            //printf("printing sent message, qp_offset = %d \n", qp_offset);
                            // hameds
                            for(int p = 0; p < sa.length(); p++)
                            {	
                                printf("%x",(sa.data()[p]));
                            }
                            printf("\n");
                            printf("tid = %d, remote_qpn = %d, cnt = %d, length = %d \n", tid, remote_qpn, num_rx+j, sa.length());
                        }
                        #endif

                        for(int p = 0; p <= 2; p++) {
                            ud_conn->buf_send[send_index][p] = (uint)((const char *)(events[j].recv.udata64))[p];
                        }

                        memcpy(ud_conn->buf_send[send_index]+2, sa.data(), sa.length());

                        #if debug
                        if (num_rx > (conn->iters / num_workers)) {
                            printf("tid = %d, remote_qpn = %d, cnt = %d, length = %d \n", tid, remote_qpn, num_rx+j, sa.length());
                            for(int p = 0; p < 30; p++)
                            {	
                                printf("%x",(((uint8_t *)(events[j].recv.udata64))[p]));
                            }
                            printf("\n");

                            int command = parser.result()[1].as_i();
                            printf("command is %d \n", command);
                        }
                        #endif
                        

                        // Try to send back to original QP
                        int success;
                        success = ud_conn->pp_post_send(ud_conn->ctx, remote_qpn, sa.length()+2, send_index);
                        // success = ud_conn->pp_post_send(ud_conn->ctx, remote_qpn, 16, send_index);
                        if (success == EINVAL) printf("Invalid value provided in wr \n");
                        else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation T%d \n", (remote_qpn));
                        else if (success == EFAULT) printf("Invalid value provided in qp \n");
                        else if (success != 0) {
                            printf("success = %d, \n",success);
                            fprintf(stderr, "Couldn't post send 2 \n");
                            //return 1;
                        }
                        else {
                            // ++conn->conn_ctxs[1+num_dlb_pp+tid].souts;
                            num_recved = (num_recved + 1) % (num_bufs);
                            total_num_recved++;
                            #if debug 
                                printf("T%d - total_num_recved = %d \n", tid, total_num_recved);
                                printf("T%d - send posted... souts = %d, \n", tid, conn->conn_ctxs[1+num_dlb_pp+tid].souts);
                            #endif
                        }
                    }
                    else {
                        printf("onego failed 0 \n");
                        int success = ud_conn->pp_post_send(ud_conn->ctx, remote_qpn, 16, send_index);
                        if (success == EINVAL) printf("Invalid value provided in wr \n");
                        else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                        else if (success == EFAULT) printf("Invalid value provided in qp \n");
                        else if (success != 0) {
                            printf("success = %d, \n",success);
                            fprintf(stderr, "Couldn't post send 2 \n");
                            //return 1;
                        }
                        else {
                            // ++conn->conn_ctxs[1+num_dlb_pp+tid].souts;
                            num_recved = (num_recved + 1) % (num_bufs);
                            total_num_recved++;
                            #if debug 
                                printf("T%d - total_num_recved = %d \n", tid, total_num_recved);
                                printf("T%d - send posted... souts = %d, \n", tid, conn->conn_ctxs[1+num_dlb_pp+tid].souts);
                            #endif
                        }
                    }
                    ti->rcu_stop();
                } 
                else {
                    printf("T%d - parser failed remote_qpn=%d\n", tid, remote_qpn);
                    int success = ud_conn->pp_post_send(ud_conn->ctx, remote_qpn, 16, send_index);
                    if (success == EINVAL) printf("Invalid value provided in wr \n");
                    else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                    else if (success == EFAULT) printf("Invalid value provided in qp \n");
                    else if (success != 0) {
                        printf("success = %d, \n",success);
                        fprintf(stderr, "Couldn't post send 2 \n");
                        //return 1;
                    }
                    else {
                        // ++conn->conn_ctxs[1+num_dlb_pp+tid].souts;
                        num_recved = (num_recved + 1) % (num_bufs);
                        total_num_recved++;
                        #if debug 
                            printf("T%d - total_num_recved = %d \n", tid, total_num_recved);
                            printf("T%d - send posted... souts = %d, \n", tid, conn->conn_ctxs[1+num_dlb_pp+tid].souts);
                        #endif
                    }
                }
            }
            // Poll UD CQ
            ne += ibv_poll_cq(ud_conn->ctx->cq, num_bufs, wc);

            num_rx += num;

            // if (num_rx % 100000 == 0)
            // if (num_rx > 1000000)
                // printf("T%d [%s] Received events : %ld\n", tid, __func__, num_rx);
            if (dlb_release(tdata->port, num) != num)
            {
                printf("[%s()] FAILED: Release of all %d events (iter %ld)!\n",
                    __func__, num, i);
                // exit(-1);
                return NULL;
            }
        }


end:
    if (epoll_enabled) {
        close(epoll_fd);
        close(tdata->efd);
    }
	//if (pp_close_ctx(ctx)) printf("DIDN'T CLOSE SUCCESSFULLY \n");
	printf("tid %d exited while loop in udpthreadfunc \n",tdata->id);
	// printf("tid %d, rcnt = %d, scnt = %d tid = %d \n",tdata->id, conn->conn_ctxs[1+num_dlb_pp+tid].rcnt,conn->conn_ctxs[1+num_dlb_pp+tid].scnt,tdata->id);
	return 0;
}

static String log_filename(const char* logdir, int logindex) {
    struct stat sb;
    int r = stat(logdir, &sb);
    if (r < 0 && errno == ENOENT) {
        r = mkdir(logdir, 0777);
        if (r < 0) {
            fprintf(stderr, "%s: %s\n", logdir, strerror(errno));
            always_assert(0);
        }
    }

    StringAccum sa;
    sa.snprintf(strlen(logdir) + 24, "%s/kvd-log-%d", logdir, logindex);
    return sa.take_string();
}

void log_init() {
  int ret, i;

  logs = logset::make(nlogger);
  for (i = 0; i < nlogger; i++)
      logs->log(i).initialize(log_filename(logdirs[i % logdirs.size()], i));

  cks = (ckstate *)malloc(sizeof(ckstate) * nckthreads);
  for (i = 0; i < nckthreads; i++) {
    threadinfo *ti = threadinfo::make(threadinfo::TI_CHECKPOINT, i);
    cks[i].state = CKState_Uninit;
    cks[i].ti = ti;
    ret = pthread_create(&ti->pthread(), 0, conc_checkpointer, ti);
    always_assert(ret == 0);
  }
}

// read a checkpoint, insert key/value pairs into tree.
// must be followed by a read of the log!
// since checkpoint is not consistent
// with any one point in time.
// returns the timestamp of the first log record that needs
// to come from the log.
kvepoch_t read_checkpoint(threadinfo *ti, const char *path) {
    double t0 = now();

    int fd = open(path, 0);
    if(fd < 0){
        printf("no %s\n", path);
        return 0;
    }
    struct stat sb;
    int ret = fstat(fd, &sb);
    always_assert(ret == 0);
    char *p = (char *) mmap(0, sb.st_size, PROT_READ, MAP_FILE|MAP_PRIVATE, fd, 0);
    always_assert(p != MAP_FAILED);
    close(fd);

    msgpack::parser par(String::make_stable(p, sb.st_size));
    Json j;
    par >> j;
    std::cerr << j << "\n";
    always_assert(j["generation"].is_i() && j["size"].is_i());
    uint64_t gen = j["generation"].as_i();
    uint64_t n = j["size"].as_i();
    printf("reading checkpoint with %" PRIu64 " nodes\n", n);

    // read data
    for (uint64_t i = 0; i != n; ++i)
        ckstate::insert(tree->table(), par, *ti);

    munmap(p, sb.st_size);
    double t1 = now();
    printf("%.1f MB, %.2f sec, %.1f MB/sec\n",
           sb.st_size / 1000000.0,
           t1 - t0,
           (sb.st_size / 1000000.0) / (t1 - t0));
    return gen;
}

void
waituntilphase(int phase)
{
  always_assert(pthread_mutex_lock(&rec_mu) == 0);
  while (rec_state != phase)
    always_assert(pthread_cond_wait(&rec_cond, &rec_mu) == 0);
  always_assert(pthread_mutex_unlock(&rec_mu) == 0);
}

void
inactive(void)
{
  always_assert(pthread_mutex_lock(&rec_mu) == 0);
  rec_nactive --;
  always_assert(pthread_cond_broadcast(&rec_cond) == 0);
  always_assert(pthread_mutex_unlock(&rec_mu) == 0);
}

void recovercheckpoint(threadinfo *ti) {
    waituntilphase(REC_CKP);
    char path[256];
    sprintf(path, "%s/kvd-ckp-%" PRId64 "-%d",
            ckpdirs[ti->index() % ckpdirs.size()],
            ckp_gen.value(), ti->index());
    kvepoch_t gen = read_checkpoint(ti, path);
    always_assert(ckp_gen == gen);
    inactive();
}

void
recphase(int nactive, int state)
{
  rec_nactive = nactive;
  rec_state = state;
  always_assert(pthread_cond_broadcast(&rec_cond) == 0);
  while (rec_nactive)
    always_assert(pthread_cond_wait(&rec_cond, &rec_mu) == 0);
}

// read the checkpoint file.
// read each log file.
// insert will ignore attempts to update with timestamps
// less than what was in the entry from the checkpoint file.
// so we don't have to do an explicit merge by time of the log files.
void
recover(threadinfo *)
{
  recovering = true;
  // XXX: discard temporary checkpoint and ckp-gen files generated before crash

  // get the generation of the checkpoint from ckp-gen, if any
  char path[256];
  sprintf(path, "%s/kvd-ckp-gen", ckpdirs[0]);
  ckp_gen = 0;
  rec_ckp_min_epoch = rec_ckp_max_epoch = 0;
  int fd = open(path, O_RDONLY);
  if (fd >= 0) {
      Json ckpj = Json::parse(read_file_contents(fd));
      close(fd);
      if (ckpj && ckpj["kvdb_checkpoint"] && ckpj["generation"].is_number()) {
          ckp_gen = ckpj["generation"].to_u64();
          rec_ckp_min_epoch = ckpj["min_epoch"].to_u64();
          rec_ckp_max_epoch = ckpj["max_epoch"].to_u64();
          printf("recover from checkpoint %" PRIu64 " [%" PRIu64 ", %" PRIu64 "]\n", ckp_gen.value(), rec_ckp_min_epoch.value(), rec_ckp_max_epoch.value());
      }
  } else {
    printf("no %s\n", path);
  }
  always_assert(pthread_mutex_lock(&rec_mu) == 0);

  // recover from checkpoint, and set timestamp of the checkpoint
  recphase(nckthreads, REC_CKP);

  // find minimum maximum timestamp of entries in each log
  rec_log_infos = new logreplay::info_type[nlogger];
  recphase(nlogger, REC_LOG_TS);

  // replay log entries, remove inconsistent entries, and append
  // an empty log entry with minimum timestamp

  // calculate log range

  // Maximum epoch seen in the union of the logs and the checkpoint. (We
  // don't commit a checkpoint until all logs are flushed past the
  // checkpoint's max_epoch.)
  kvepoch_t max_epoch = rec_ckp_max_epoch;
  if (max_epoch)
      max_epoch = max_epoch.next_nonzero();
  for (logreplay::info_type *it = rec_log_infos;
       it != rec_log_infos + nlogger; ++it)
      if (it->last_epoch
          && (!max_epoch || max_epoch < it->last_epoch))
          max_epoch = it->last_epoch;

  // Maximum first_epoch seen in the logs. Full log information is not
  // available for epochs before max_first_epoch.
  kvepoch_t max_first_epoch = 0;
  for (logreplay::info_type *it = rec_log_infos;
       it != rec_log_infos + nlogger; ++it)
      if (it->first_epoch
          && (!max_first_epoch || max_first_epoch < it->first_epoch))
          max_first_epoch = it->first_epoch;

  // Maximum epoch of all logged wake commands.
  kvepoch_t max_wake_epoch = 0;
  for (logreplay::info_type *it = rec_log_infos;
       it != rec_log_infos + nlogger; ++it)
      if (it->wake_epoch
          && (!max_wake_epoch || max_wake_epoch < it->wake_epoch))
          max_wake_epoch = it->wake_epoch;

  // Minimum last_epoch seen in QUIESCENT logs.
  kvepoch_t min_quiescent_last_epoch = 0;
  for (logreplay::info_type *it = rec_log_infos;
       it != rec_log_infos + nlogger; ++it)
      if (it->quiescent
          && (!min_quiescent_last_epoch || min_quiescent_last_epoch > it->last_epoch))
          min_quiescent_last_epoch = it->last_epoch;

  // If max_wake_epoch && min_quiescent_last_epoch <= max_wake_epoch, then a
  // wake command was missed by at least one quiescent log. We can't replay
  // anything at or beyond the minimum missed wake epoch. So record, for
  // each log, the minimum wake command that at least one quiescent thread
  // missed.
  if (max_wake_epoch && min_quiescent_last_epoch <= max_wake_epoch)
      rec_replay_min_quiescent_last_epoch = min_quiescent_last_epoch;
  else
      rec_replay_min_quiescent_last_epoch = 0;
  recphase(nlogger, REC_LOG_ANALYZE_WAKE);

  // Calculate upper bound of epochs to replay.
  // This is the minimum of min_post_quiescent_wake_epoch (if any) and the
  // last_epoch of all non-quiescent logs.
  rec_replay_max_epoch = max_epoch;
  for (logreplay::info_type *it = rec_log_infos;
       it != rec_log_infos + nlogger; ++it) {
      if (!it->quiescent
          && it->last_epoch
          && it->last_epoch < rec_replay_max_epoch)
          rec_replay_max_epoch = it->last_epoch;
      if (it->min_post_quiescent_wake_epoch
          && it->min_post_quiescent_wake_epoch < rec_replay_max_epoch)
          rec_replay_max_epoch = it->min_post_quiescent_wake_epoch;
  }

  // Calculate lower bound of epochs to replay.
  rec_replay_min_epoch = rec_ckp_min_epoch;
  // XXX what about max_first_epoch?

  // Checks.
  if (rec_ckp_min_epoch) {
      always_assert(rec_ckp_min_epoch > max_first_epoch);
      always_assert(rec_ckp_min_epoch < rec_replay_max_epoch);
      always_assert(rec_ckp_max_epoch < rec_replay_max_epoch);
      fprintf(stderr, "replay [%" PRIu64 ",%" PRIu64 ") from [%" PRIu64 ",%" PRIu64 ") into ckp [%" PRIu64 ",%" PRIu64 "]\n",
              rec_replay_min_epoch.value(), rec_replay_max_epoch.value(),
              max_first_epoch.value(), max_epoch.value(),
              rec_ckp_min_epoch.value(), rec_ckp_max_epoch.value());
  }

  // Actually replay.
  delete[] rec_log_infos;
  rec_log_infos = 0;
  recphase(nlogger, REC_LOG_REPLAY);

  // done recovering
  recphase(0, REC_DONE);
#if !NDEBUG
  // check that all delta markers have been recycled (leaving only remove
  // markers and real values)
  uint64_t deltas_created = 0, deltas_removed = 0;
  for (threadinfo *ti = threadinfo::allthreads; ti; ti = ti->next()) {
      deltas_created += ti->counter(tc_replay_create_delta);
      deltas_removed += ti->counter(tc_replay_remove_delta);
  }
  if (deltas_created)
      fprintf(stderr, "deltas created: %" PRIu64 ", removed: %" PRIu64 "\n", deltas_created, deltas_removed);
  always_assert(deltas_created == deltas_removed);
#endif

  global_log_epoch = rec_replay_max_epoch.next_nonzero();

  always_assert(pthread_mutex_unlock(&rec_mu) == 0);
  recovering = false;
  if (recovery_only)
      exit(0);
}

void
writecheckpoint(const char *path, ckstate *c, double t0)
{
  double t1 = now();
  printf("memory phase: %" PRIu64 " nodes, %.2f sec\n", c->count, t1 - t0);

  int fd = creat(path, 0666);
  always_assert(fd >= 0);

  // checkpoint file format, all msgpack:
  //   {"generation": generation, "size": size, ...}
  //   then `size` triples of key (string), timestmap (int), value (whatever)
  Json j = Json().set("generation", ckp_gen.value())
      .set("size", c->count)
      .set("firstkey", c->startkey);
  StringAccum sa;
  msgpack::unparse(sa, j);
  checked_write(fd, sa.data(), sa.length());
  checked_write(fd, c->vals->buf, c->vals->n);

  int ret = fsync(fd);
  always_assert(ret == 0);
  ret = close(fd);
  always_assert(ret == 0);

  double t2 = now();
  c->bytes = c->vals->n;
  printf("file phase (%s): %" PRIu64 " bytes, %.2f sec, %.1f MB/sec\n",
         path,
         c->bytes,
         t2 - t1,
         (c->bytes / 1000000.0) / (t2 - t1));
}

void
conc_filecheckpoint(threadinfo *ti)
{
    ckstate *c = &cks[ti->index()];
    c->vals = new_bufkvout();
    double t0 = now();
    tree->table().scan(c->startkey, true, *c, *ti);
    char path[256];
    sprintf(path, "%s/kvd-ckp-%" PRId64 "-%d",
            ckpdirs[ti->index() % ckpdirs.size()],
            ckp_gen.value(), ti->index());
    writecheckpoint(path, c, t0);
    c->count = 0;
    free(c->vals);
}

static Json
prepare_checkpoint(kvepoch_t min_epoch, int nckthreads, const Str *pv)
{
    Json j;
    j.set("kvdb_checkpoint", true)
        .set("min_epoch", min_epoch.value())
        .set("max_epoch", global_log_epoch.value())
        .set("generation", ckp_gen.value())
        .set("nckthreads", nckthreads);

    Json pvj;
    for (int i = 1; i < nckthreads; ++i)
        pvj.push_back(Json::make_string(pv[i].s, pv[i].len));
    j.set("pivots", pvj);

    return j;
}

static void
commit_checkpoint(Json ckpj)
{
    // atomically commit a set of checkpoint files by incrementing
    // the checkpoint generation on disk
    char path[256];
    sprintf(path, "%s/kvd-ckp-gen", ckpdirs[0]);
    int r = atomic_write_file_contents(path, ckpj.unparse());
    always_assert(r == 0);
    fprintf(stderr, "kvd-ckp-%" PRIu64 " [%s,%s]: committed\n",
            ckp_gen.value(), ckpj["min_epoch"].to_s().c_str(),
            ckpj["max_epoch"].to_s().c_str());

    // delete old checkpoint files
    for (int i = 0; i < nckthreads; i++) {
        char path[256];
        sprintf(path, "%s/kvd-ckp-%" PRId64 "-%d",
                ckpdirs[i % ckpdirs.size()],
                ckp_gen.value() - 1, i);
        unlink(path);
    }
}

static kvepoch_t
max_flushed_epoch()
{
    kvepoch_t mfe = 0, ge = global_log_epoch;
    for (int i = 0; i < nlogger; ++i) {
        loginfo& log = logs->log(i);
        kvepoch_t fe = log.quiescent() ? ge : log.flushed_epoch();
        if (!mfe || fe < mfe)
            mfe = fe;
    }
    return mfe;
}

// concurrent periodic checkpoint
void* conc_checkpointer(void* x) {
  threadinfo* ti = reinterpret_cast<threadinfo*>(x);
  ti->pthread() = pthread_self();
  recovercheckpoint(ti);
  ckstate *c = &cks[ti->index()];
  c->count = 0;
  pthread_cond_init(&c->state_cond, NULL);
  c->state = CKState_Ready;
  while (recovering)
    sleep(1);
  if (checkpoint_interval <= 0)
      return 0;
  if (ti->index() == 0) {
    for (int i = 1; i < nckthreads; i++)
      while (cks[i].state != CKState_Ready)
        ;
    Str *pv = new Str[nckthreads + 1];
    Json uncommitted_ckp;

    while (1) {
      struct timespec ts;
      set_timespec(ts, now() + (uncommitted_ckp ? 0.25 : checkpoint_interval));

      pthread_mutex_lock(&checkpoint_mu);
      if (!go_quit)
        pthread_cond_timedwait(&checkpoint_cond, &checkpoint_mu, &ts);
      if (go_quit) {
          for (int i = 0; i < nckthreads; i++) {
              cks[i].state = CKState_Quit;
              pthread_cond_signal(&cks[i].state_cond);
          }
          pthread_mutex_unlock(&checkpoint_mu);
          break;
      }
      pthread_mutex_unlock(&checkpoint_mu);

      if (uncommitted_ckp) {
          kvepoch_t mfe = max_flushed_epoch();
          if (!mfe || mfe > uncommitted_ckp["max_epoch"].to_u64()) {
              commit_checkpoint(uncommitted_ckp);
              uncommitted_ckp = Json();
          }
          continue;
      }

      double t0 = now();
      ti->rcu_start();
      for (int i = 0; i < nckthreads + 1; i++)
        pv[i].assign(NULL, 0);
      tree->findpivots(pv, nckthreads + 1);
      ti->rcu_stop();

      kvepoch_t min_epoch = global_log_epoch;
      pthread_mutex_lock(&checkpoint_mu);
      ckp_gen = ckp_gen.next_nonzero();
      for (int i = 0; i < nckthreads; i++) {
          cks[i].startkey = pv[i];
          cks[i].endkey = (i == nckthreads - 1 ? Str() : pv[i + 1]);
          cks[i].state = CKState_Go;
          pthread_cond_signal(&cks[i].state_cond);
      }
      pthread_mutex_unlock(&checkpoint_mu);

      ti->rcu_start();
      conc_filecheckpoint(ti);
      ti->rcu_stop();

      cks[0].state = CKState_Ready;
      uint64_t bytes = cks[0].bytes;
      pthread_mutex_lock(&checkpoint_mu);
      for (int i = 1; i < nckthreads; i++) {
        while (cks[i].state != CKState_Ready)
          pthread_cond_wait(&cks[i].state_cond, &checkpoint_mu);
        bytes += cks[i].bytes;
      }
      pthread_mutex_unlock(&checkpoint_mu);

      uncommitted_ckp = prepare_checkpoint(min_epoch, nckthreads, pv);

      for (int i = 0; i < nckthreads + 1; i++)
        if (pv[i].s)
          free((void *)pv[i].s);
      double t = now() - t0;
      fprintf(stderr, "kvd-ckp-%" PRIu64 " [%s,%s]: prepared (%.2f sec, %" PRIu64 " MB, %" PRIu64 " MB/sec)\n",
              ckp_gen.value(), uncommitted_ckp["min_epoch"].to_s().c_str(),
              uncommitted_ckp["max_epoch"].to_s().c_str(),
              t, bytes / (1 << 20), (uint64_t)(bytes / t) >> 20);
    }
  } else {
    while(1) {
      pthread_mutex_lock(&checkpoint_mu);
      while (c->state != CKState_Go && c->state != CKState_Quit)
        pthread_cond_wait(&c->state_cond, &checkpoint_mu);
      if (c->state == CKState_Quit) {
        pthread_mutex_unlock(&checkpoint_mu);
        break;
      }
      pthread_mutex_unlock(&checkpoint_mu);

      ti->rcu_start();
      conc_filecheckpoint(ti);
      ti->rcu_stop();

      pthread_mutex_lock(&checkpoint_mu);
      c->state = CKState_Ready;
      pthread_cond_signal(&c->state_cond);
      pthread_mutex_unlock(&checkpoint_mu);
    }
  }
  return 0;
}
