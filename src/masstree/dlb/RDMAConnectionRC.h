/*
 * Copyright (c) 2021 Georgia Institute of Technology.  All rights reserved.
 */

#pragma once

#ifndef _RDMACONNRCH_
#define _RDMACONNRCH_

#include <infiniband/verbs.h>
#include <linux/types.h>  //for __be32 type
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <string>
#include <unistd.h>
#include <malloc.h>

#if defined(__x86_64__)
#include "utils/x86/cycles.h"
#elif defined(__arm__)
#include "utils/arm/cycles_32.h"
#elif defined(__aarch64__)
#include "utils/arm/cycles_64.h"
#endif

#define USE_DLB 1
#define IS_SERVER 0
#define IS_CLIENT 1
#define IS_SNIC 0

#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096
#define BUF_SIZE 512

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

using namespace std;

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


// #if IS_CLIENT || IS_SNIC
// typedef struct {
//     uint8_t qe_cmd:4;
//     uint8_t int_arm:1;
//     uint8_t error:1;
//     uint8_t rsvd:2;
// } __attribute__((packed)) dlb_enqueue_cmd_info_t;

// typedef struct dlb_enqueue_qe {
//     uint64_t data;
//     uint16_t opaque;
//     uint8_t qid;
//     uint8_t sched_byte;
//     union {
//         uint16_t flow_id;
//         uint16_t num_tokens_minus_one;
//     };
//     union {
//         struct {
//             uint8_t meas_lat:1;
//             uint8_t weight:2;
//             uint8_t no_dec:1;
//             uint8_t cmp_id:4;
//         };
//         uint8_t misc_byte;
//     };
//     union {
//         dlb_enqueue_cmd_info_t cmd_info;
//         uint8_t cmd_byte;
//     };
// } __attribute__((packed)) __attribute__ ((aligned (sizeof(long long)))) dlb_enqueue_qe_t;
// #endif

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
    uint16_t    src_qpn;
    uint8_t     wr_id;
    uint8_t     valid;
} __attribute__((packed));

alignas(64) struct conn_context {
    unsigned int            id;
    struct dev_context      *dev_ctx;
    // Queue pair
    struct ibv_qp           *qp;
    unsigned int                     qp_access_flags;   // queue pair access flags
    // Completion queue
    struct ibv_cq           *cq;
    // Memory region for data
    struct ibv_mr           *data_mr;
    // struct ibv_mr           *recv_mr;

    // Memory for data
    unsigned char           *data_buf;
    // unsigned char           *recv_buf;
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
    unsigned int                     mr_access_flags;   // memory region access flags

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

    // masstree kv store
#if IS_SERVER
    static const unsigned int bufs_num = 1024;
#elif IS_CLIENT
    static const unsigned int bufs_num = 1024;
#endif
    char *                  buf_recv [bufs_num];
	char *                  buf_send [bufs_num];
    unsigned int            routs, souts = 0;
	unsigned int            rcnt, scnt = 0;
    // client
    int                     wr_index = 0;
    uint32_t 				key_offset = 0;
	uint32_t 				key_number = 0;//9000000000000000;
    uint32_t                *random_keys;
	uint8_t                 *op_type;
    double                  *measured_latency;
};

static int cmpfunc(const void * a, const void * b) {
   return (*(int*)a - *(int*)b);
}

// Check if the gid referenced by gid_index is a ipv4-gid
bool is_ipv4_gid(struct dev_context *ctx, int gid_index) {
    char file_name[384] = {0};
    static const char ipv4_gid_prefix[] = "0000:0000:0000:0000:0000:ffff:";
    FILE * fp = NULL;
    ssize_t read;
    char * line = NULL;
    size_t len = 0;
    snprintf(file_name, sizeof(file_name), "/sys/class/infiniband/%s/ports/%d/gids/%d",
                     ctx->ib_dev_name, ctx->dev_port, gid_index);
    fp = fopen(file_name, "r");
    if (!fp) {
        return false;
    }
    read = getline(&line, &len, fp);
    fclose(fp);
    if (!read) {
        return false;
    }
    return strncmp(line, ipv4_gid_prefix, strlen(ipv4_gid_prefix)) == 0;
}

namespace MyNamespace {
#include <dirent.h>
}
// Get the index of all the GIDs whose types are RoCE v2
// Refer to https://docs.mellanox.com/pages/viewpage.action?pageId=12013422#RDMAoverConvergedEthernet(RoCE)-RoCEv2 for more details
void get_rocev2_gid_index(struct dev_context *ctx)
{
    const size_t max_gid_count = sizeof(ctx->gid_index_list) / sizeof(ctx->gid_index_list[0]);
    int gid_index_list[max_gid_count];
    int gid_count = 0;

    ctx->gid_count = 0;

    char dir_name[128] = {0};
    snprintf(dir_name, sizeof(dir_name),
             "/sys/class/infiniband/%s/ports/%d/gid_attrs/types",
             ctx->ib_dev_name, ctx->dev_port);
    MyNamespace::DIR *dir = MyNamespace::opendir(dir_name);
    if (!dir) {
        fprintf(stderr, "Fail to open folder %s\n", dir_name);
        return;
    }

    struct MyNamespace::dirent *dp = NULL;
    char file_name[384] = {0};
    FILE * fp = NULL;
    ssize_t read;
    char * line = NULL;
    size_t len = 0;
    int gid_index;

    while ((dp = MyNamespace::readdir(dir)) && gid_count < max_gid_count) {
        gid_index = atoi(dp->d_name);


        snprintf(file_name, sizeof(file_name), "%s/%s", dir_name, dp->d_name);
        fp = fopen(file_name, "r");
        if (!fp) {
            continue;
        }

        read = getline(&line, &len, fp);
        fclose(fp);
        if (read <= 0) {
            continue;
        }

        if (strncmp(line, "RoCE v2", strlen("RoCE v2")) != 0) {
            continue;
        }

        if (!is_ipv4_gid(ctx, gid_index)) {
            continue;
        }

        gid_index_list[gid_count++] = gid_index;
    }

    MyNamespace::closedir(dir);

    qsort(gid_index_list, gid_count, sizeof(int), cmpfunc);
    ctx->gid_count = gid_count;
    for (int i = 0; i < gid_count; i++) {
        ctx->gid_index_list[i] = gid_index_list[i];
        printf("Get RoCE V2 GID index is %d\n", gid_index_list[i]);
    }
    //Debug
    printf("Get %lu RoCE V2 GIDs\n", ctx->gid_count);

}


#if !IS_CLIENT && !IS_SNIC
#include "../../dlb_8.9.0/libdlb/dlb.h"
#include "../../dlb_8.9.0/libdlb/dlb_priv.h"
#include "../../dlb_8.9.0/libdlb/dlb2_ioctl.h"
#include "../../dlb_8.9.0/libdlb/dlb_common.h"
#endif

class RDMAConnectionRC {
public:

#if IS_SERVER
	RDMAConnectionRC(int id, int num_qps_in, uint64_t*** dlb_pp_addr, dlb_shared_domain_t** dlb_shared_domain_ptr, int num_dlb_pp_in, int num_client_conns_in);
#endif

#if IS_CLIENT
	RDMAConnectionRC(int id, int num_qps_in, int num_dlb_pp_in, int num_client_conns_in);
#endif
    int ib_dev_id_by_name(char *ib_dev_name, struct ibv_device **dev_list, int num_devices);
    // static int cmpfunc(const void * a, const void * b);
    // bool is_ipv4_gid(struct dev_context *ctx, int gid_index);
    // void get_rocev2_gid_index(struct dev_context *ctx);

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

    // credit management
    uint32_t acquire_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size, uint64_t global_credits, uint64_t new_global_credits);
    uint32_t query_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size);

    void print_conn_ctx(struct conn_context *conn_ctx);
    void print_dest(struct conn_dest *dest);

    char* generate_message(size_t n);

    int parse_args(int argc, char **argv);

    // Client Function
    int pp_post_recv(struct conn_context *ctx, int wr_id, bool sync);
    int pp_post_send(struct conn_context *ctx, uint32_t qpn, unsigned int length, int wr_id);
    int pp_post_send_write_no_signal(struct conn_context *conn_ctx, uint64_t offset_remote, int size, int wr_id);
    int pp_post_send_write(struct conn_context *conn_ctx, uint64_t offset_remote, int size, int wr_id);

    struct dev_context      dev_ctx;
    struct conn_context     *conn_ctxs;
    int                     num_qps = 3; 
    int                     num_qps_init = 0;
    char                    *server_ip = "192.168.200.10";
    char                    *client_ip = "192.168.200.30";
    char                    *snic_ip = "192.168.200.20";
#if IS_SERVER
    int                     host_type = SERVER;
#elif IS_CLIENT
    int                     host_type = CLIENT;
#endif
    int                     server_sockfd = -1;
    int                     client_sockfd = -1;
    int                     snic_sockfd = -1;

    int                     qp_id;

    // Client parameter
	unsigned int            size = 512;
	int                     use_event = 0;
	struct timeval          start, end;
    long long int   		iters = 1000000;
	long long int   		sync_iters = 1000000;

    // unsigned int             routs, souts;
	// unsigned int             rcnt, scnt = 0;

#if IS_SERVER
    static const unsigned int bufs_num = 1024; //768+1;
	static const unsigned int sync_bufs_num = 1;
#elif IS_CLIENT
    static const unsigned int bufs_num = 1024;
	static const unsigned int sync_bufs_num = 1;
#endif

    // char * buf_recv [bufs_num];
	// char * buf_send [bufs_num];

	// struct ibv_mr* mr_recv [bufs_num];
	// struct ibv_mr* mr_send [bufs_num];

	int wr_index = 0;

    int num_dlb_pp = 1;
    int num_client_conns = 1;

};

#endif
