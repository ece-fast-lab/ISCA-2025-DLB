#ifndef COMMON_H
#define COMMON_H

#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>

#if defined(__x86_64__)
#include "utils/x86/cycles.h"
#elif defined(__arm__)
#include "utils/arm/cycles_32.h"
#elif defined(__aarch64__)
#include "utils/arm/cycles_64.h"
#endif


#define USE_DLB 1
#define IS_SERVER 1
#define IS_CLIENT 0
#define IS_SNIC 0

#define DEBUG 0

#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096

#define DEFAULT_IB_PORT 1
#define DEFAULT_MSG_SIZE 4096
#define DEFAULT_ITERS 100
#define DEFAULT_SERVER_PORT 8080
#define DEFAULT_CONTROLLER_PORT 8080
#define DEFAULT_NUM_QPS 1
#define DEFAULT_TX_DEPTH 1
#define DEFAULT_RX_DEPTH 50
#define DEFAULT_MTU 1024

#define COMPLETE_MSG "Complete"
#define READY_MSG "Ready"
#define MSG_TO_WRITE "Hi This is a 64-byte string in C This is a 64-byte string in C!"

#define MAX_GID_COUNT 128
#define WC_BATCH 100

#define PORT 8080
#define PORT_STR "8080"

#define PORT_DLB 8090
#define PORT_DLB_STR "8090"

#define bufs_num 200

#define PI 3.14159265358979323846

#define BATCH_SIZE 4

// ==== RDMA variables
extern int num_qps;
extern int msg_size;
extern int mr_size;
extern int ib_port;
extern char *server_ip;
extern char *client_ip;
extern char *snic_ip;
extern int host_type;
// =============================
extern uint64_t num_events;
extern int meta_size;
extern uint64_t metadata_mr_size;

extern int num_dlb_pp;
extern int num_meta_conns;
extern int num_client_conns;
extern int num_dlb_workers;

extern int test_finished;

enum {
    SERVER = 0,
    SNIC = 1,
    CLIENT = 2,
};

enum {
    RECV_WRID = 1,
    SEND_WRID = 2,
    WRITE_WRID = 3,
    READ_WRID = 4,
    ATOMIC_WRID = 5,
};

#if IS_CLIENT || IS_SNIC
typedef struct {
    uint8_t qe_cmd:4;
    uint8_t int_arm:1;
    uint8_t error:1;
    uint8_t rsvd:2;
} __attribute__((packed)) dlb_enqueue_cmd_info_t;

typedef struct dlb_enqueue_qe {
    uint64_t data;
    uint16_t opaque;
    uint8_t qid;
    uint8_t sched_byte;
    union {
        uint16_t flow_id;
        uint16_t num_tokens_minus_one;
    };
    union {
        struct {
            uint8_t meas_lat:1;
            uint8_t weight:2;
            uint8_t no_dec:1;
            uint8_t cmp_id:4;
        };
        uint8_t misc_byte;
    };
    union {
        dlb_enqueue_cmd_info_t cmd_info;
        uint8_t cmd_byte;
    };
} __attribute__((packed)) __attribute__ ((aligned (sizeof(long long)))) dlb_enqueue_qe_t;
#endif

struct dev_context
{
    // IB device name
    char                    *ib_dev_name;
    // IB device port
    int                     dev_port;

    // Global identifier
    int                     gid_index_list[MAX_GID_COUNT];
    union ibv_gid           gid_list[MAX_GID_COUNT];
    size_t                  gid_count;

    // GUID
    uint64_t                guid;

    // IB device context
    struct ibv_context      *ctx;
    // IB device attribute
    struct ibv_device_attr  dev_attr;
    // IB port attribute
    struct ibv_port_attr    port_attr;

    // Completion channel
    struct ibv_comp_channel *channel;
    // Protection domain
    struct ibv_pd           *pd;
    // Completion queue
    struct ibv_cq           *cq;
    // If use completion channel (event driven)
    bool                    use_event;
};

// Connection destination information
struct conn_dest {
    // Local identifier
    uint16_t        lid;
    // Queue pair number
    uint32_t        qpn;
    // Packet sequence number
    uint32_t        psn;
    // Global identifier
    union ibv_gid   gid;
    // GUID
    uint64_t        guid;
};

// Memory information
struct conn_mem {
    uint64_t    addr;
    uint32_t    rkey;
} __attribute__((packed));

// RDMA metadata
struct rdma_metadata {
    uint64_t    addr;
    uint16_t    size;
    uint8_t     wr_id;
    uint8_t     valid;
} __attribute__((packed));

struct conn_context {
    unsigned int            id;
    struct dev_context      *dev_ctx;
    // Queue pair
    struct ibv_qp           *qp;
    int                     qp_access_flags;   // queue pair access flags
    // Completion queue
    struct ibv_cq           *cq;

    // Memory region for data
    struct ibv_mr           *data_mr;

    // Memory for data
    unsigned char           *data_buf;
    size_t                  data_buf_size;
    bool                    validate_buf;

    // Work request send flags
    bool                    inline_msg;
    int                     send_flags;

    // Destination information
    struct conn_dest        local_dest;
    // Remote Destination information
    struct conn_dest        remote_dest;

    // Remote memory information
    struct conn_mem         remote_mr;

    // Local memory information
    struct conn_mem         local_mr;
    int                     mr_access_flags;   // memory region access flags

    // local gid index
    unsigned int            gid_index;

    // Statistics
    unsigned int            post_reqs;
    unsigned int            complete_reqs;
    struct timeval          end;

    // QP flags
    int                     dlb_data_qp;        // use to communicate dlb qe
    int                     dlb_credit_qp;      // use to communicate dlb credit
    int                     rdma_data_qp;       // use as regular RDMA queue pairs

    // Buffer
    char *                  buf_recv[bufs_num];
	char *                  buf_send[bufs_num];

    /* New fields for batching */
    struct ibv_send_wr batch_wrs[BATCH_SIZE];
    struct ibv_sge    batch_sges[BATCH_SIZE];
    int batch_count;  // number of WRs accumulated so far
};

// Initialize device context
int init_device_ctx(struct dev_context *dev_ctx);

// Destory device context
void destroy_device_ctx(struct dev_context *dev_ctx);

// Initialize connection context
int init_connection_ctx(struct conn_context *cnn_ctx);

// Destroy connection context
void destroy_connection_ctx(struct conn_context *cnn_ctx);

int get_server_dest(struct conn_context *conn_ctx, char *server_ip, char *port, int num_qps);
int get_server_dlb_dest(struct conn_context *conn_ctx, char *server_ip, char *port, int num_qps);

int get_client_dest(struct conn_context *conn_ctx, int num_qps, uint16_t port);
int get_client_dlb_dest(struct conn_context *conn_ctx, int num_qps, uint16_t port);

int connect_qps(struct conn_context *conn_ctx);

// helper functions for post send/recv
int post_recv(struct conn_context *conn_ctx, uint64_t offset, int size);
int post_send(struct conn_context *conn_ctx, uint64_t offset, int size);
int post_send_read(struct conn_context *conn_ctx, uint64_t offset, int size);
int post_send_write(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size);
int post_send_write_no_signal(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size);
int post_send_atomic_cmp_and_swap(struct conn_context *conn_ctx, uint64_t offset, int size, uint64_t compare_add, uint64_t swap);
int post_send_atomic_fetch_and_add(struct conn_context *conn_ctx, uint64_t offset, int size, uint64_t compare_add);
// int wait_completions(struct dev_context *dev_ctx, int wr_id, int cnt);
int wait_completions(struct conn_context *conn_ctx, int wr_id, int cnt);

int prepare_send_write_batch(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size, int signal);
int post_send_write_batch(struct conn_context *conn_ctx);


// credit management
uint32_t acquire_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size, uint64_t global_credits, uint64_t new_global_credits);
uint32_t query_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size);

void print_conn_ctx(struct conn_context *conn_ctx);
void print_dest(struct conn_dest *dest);

char* generate_message(size_t n);

int parse_args(int argc, char **argv);

// Compute latency stats
static inline int cmpfunc (const void * a, const void * b)
{
    if (*(double*)a > *(double*)b) return 1;
    else if (*(double*)a < *(double*)b) return -1;
    else return 0;
}


static uint64_t dummy_process_delay = 0;
// Generate normal distribution random numbers using the Box-Muller transform, converting to uint64_t 
static uint64_t generateNormalDist(uint64_t mean, uint64_t variance) {
    double u1 = rand() / (double)RAND_MAX;
    double u2 = rand() / (double)RAND_MAX;
    double z0 = sqrt(-2.0 * log(u1)) * cos(2 * PI * u2);
    double result = mean + z0 * sqrt(variance);
    return result > 0 ? (uint64_t)result : 0;
}

// Generate a uniform random number based on mean and variance
static uint64_t generateUniformDist(double mean, double variance) {
    double a = mean - sqrt(3 * variance);
    double b = mean + sqrt(3 * variance);

    // Handling potential underflow or overflow in uint64_t
    if (a < 0) a = 0;
    if (b > UINT64_MAX) b = UINT64_MAX;

    // Generate random number in the range [a, b]
    double range = b - a;
    double fraction = range * rand() / (RAND_MAX + 1.0);
    uint64_t result = (uint64_t)(fraction + a);

    return result;
}

// Generate a log-normal distribution based on mean and variance
static uint64_t generateLogNormalDist(uint64_t mean, uint64_t variance) {
    double u1 = rand() / (double)RAND_MAX;
    double u2 = rand() / (double)RAND_MAX;
    double z0 = sqrt(-2.0 * log(u1)) * cos(2 * PI * u2);
    double result = exp(mean + z0 * sqrt(variance));
    return result > 0 ? (uint64_t)result : 0;
}

// Generate a exponential distribution based on lambda
static uint64_t generateExponentialDist(uint64_t lambda) {
    double u = rand() / (double)RAND_MAX;
    double result = -log(1 - u) / lambda;
    return (uint64_t)result + dummy_process_delay;
}

// Generate a Weibull distribution based on lambda and k
static uint64_t generateWeibullDist(double lambda, double k) {
    double u = rand() / (double)RAND_MAX;
    double result = lambda * pow(-log(1 - u), 1 / k);
    return (uint64_t)result;
}

#endif
