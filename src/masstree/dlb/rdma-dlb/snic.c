#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <error.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdatomic.h>

#include "common.h"

// ==== RDMA variables
// int num_qps = 2;
// int msg_size = 256;
// int mr_size = 4096;
// int ib_port = 1;
// char *server_ip = NULL;
// char *client_ip = NULL;
// char *snic_ip = NULL;
// int host_type = -100;
// =============================
// uint64_t num_events = 4*128;

// int num_dlb_pp = 2;
// int num_meta_conns = 2;

typedef struct {
    int tid;
    int meta_conn_id;
    int dlb_conn_id;
    struct conn_context* conn_ctxs;
    struct dev_context dev_ctx;
} thread_args_t;

// DLB credit control
uint32_t local_credits[16] = {0};
uint32_t global_credits = 0;
uint32_t snic_global_credits = 0;
uint32_t credit_batch = 800;
uint64_t global_credits_64 = 0;

int retry_thresh = 100;
int num_dlb_queues = 8;

static void *snic_dlb_representative(void *__args)
{
    thread_args_t *tdata = __args;
    int tid = tdata->tid;
    int meta_conn_id = tdata->meta_conn_id;
    int dlb_conn_id = tdata->dlb_conn_id;
    struct conn_context* conn_ctxs = tdata->conn_ctxs;
    struct dev_context dev_ctx = tdata->dev_ctx;

    // Start RDMA
    uint64_t ecnt = 0;
    int buf_idx = 0;
    int credit_acquire_cnt = 0;
    uint64_t metadata_offset = 0;
    uint64_t start_time, end_time;
    double difference;
    int success;
    start_time = rdtsc();
    // printf("start_time is %lu\n", start_time);
    // printf("rdma_metadata size is %d\n", sizeof(struct rdma_metadata));
    memcpy(conn_ctxs[meta_conn_id].data_buf + metadata_mr_size, READY_MSG, sizeof(READY_MSG));

    srand48(meta_conn_id * time(NULL));

    // Pre-fill the DLB QE
    for (int j = 0; j < meta_size; j++) {
        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[j].qid = 31 - (num_dlb_pp-1-tid)%8;
        // ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[j].qid = 31 - (lrand48() % num_dlb_queues);
        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[j].sched_byte = 1;
        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[j].flow_id = 0;
        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[j].misc_byte = 0;
        // ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[j].opaque = tid;
    }

    int iter = 0;

    while (1) {
        // Acquire credits before sending
        // while (local_credits[tid] < (uint32_t)4) {
        //     // usleep(1);
        // }

        #if DEBUG
            printf("T%d local credits is %d\n", tid, local_credits);
            printf("T%d local address is %p\n", tid, conn_ctxs[meta_conn_id].local_mr.addr);
        #endif

        // Send QE to DLB remotely from SNIC
        int valid_qe = 0;
        int retry = 0;
        while ((valid_qe == 0)) {
            if (metadata_offset + 4 >= meta_size && metadata_offset < meta_size) {
                for (int j = 0; j < (meta_size-metadata_offset); j++) {
                    if (((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].valid != 0x00) {
                        if (((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].addr == 0) printf("tid %d, addr is NULL\n", tid);
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].data = ((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].addr;
                        // ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].qid = 31 - (lrand48() % num_dlb_queues);
                        // ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].qid = 31 - ((buf_idx+j) % num_dlb_queues);
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].cmd_byte = 8;
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].opaque = ((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].src_qpn;
                        ((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].valid = 0x00;
                        valid_qe++;
                    } else {
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].cmd_byte = 0;
                        break;
                    }
                }
            }
            else {
                if (metadata_offset == meta_size) {
                    metadata_offset = 0;
                    buf_idx = 0;
                }

                // if (buf_idx + 1 > meta_size) {
                //     buf_idx = 0;
                // }

                for (int j = 0; j < 4; j++) {
                    if (((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].valid != 0x00) {
                        if (((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].addr == 0) printf("tid %d, addr is NULL\n", tid);
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].data = ((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].addr;
                        // ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].qid = 31 - (lrand48() % num_dlb_queues);
                        // ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].qid = 31 - ((buf_idx+j) % num_dlb_queues);
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].cmd_byte = 8;
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].opaque = ((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].src_qpn;
                        ((struct rdma_metadata *)(conn_ctxs[meta_conn_id].data_buf))[metadata_offset+j].valid = 0x00;
                        valid_qe++;
                    } else {
                        ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+j].cmd_byte = 0;
                        break;
                    }
                }
            }
            
        }
        // printf("T%d qid is %d\n", tid, ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx].qid);

        // for (int k = valid_qe; k < 4; k++) {
        //     ((dlb_enqueue_qe_t *)(conn_ctxs[dlb_conn_id].data_buf))[buf_idx+k].cmd_byte = 0;
        // }

        metadata_offset += valid_qe;
        ecnt += valid_qe;

        if (iter < 16) {
            success = post_send_write_no_signal(&conn_ctxs[dlb_conn_id], buf_idx*sizeof(dlb_enqueue_qe_t), 0, valid_qe*sizeof(dlb_enqueue_qe_t));
        } else {
            success = post_send_write(&conn_ctxs[dlb_conn_id], buf_idx*sizeof(dlb_enqueue_qe_t), 0, valid_qe*sizeof(dlb_enqueue_qe_t));
            wait_completions(&conn_ctxs[dlb_conn_id], WRITE_WRID, 1);
            iter = 0;
        }
        iter += 1;

        buf_idx += valid_qe;
        local_credits[tid] -= valid_qe;


        // if (ecnt != 0 && ecnt % meta_size == 0) {
        //     // memcpy(conn_ctxs[meta_conn_id].data_buf+metadata_mr_size, READY_MSG, sizeof(READY_MSG));
        //     post_send(&conn_ctxs[meta_conn_id], metadata_mr_size, sizeof(READY_MSG));
        //     wait_completions(&conn_ctxs[meta_conn_id], SEND_WRID, 1);
        //     // post_send_write(&conn_ctxs[meta_conn_id], metadata_mr_size, metadata_mr_size, sizeof(READY_MSG));
        //     // wait_completions(&conn_ctxs[meta_conn_id], WRITE_WRID, 1);
        //     // printf("here\n");
        //     // usleep(1);
        // }

        // if (tid == 0 && ecnt > 1000000) printf("T%d metadata_offset is %d, ecnt = %d, valid_qe=%d, global_credits=%d\n", tid, metadata_offset, ecnt, valid_qe, global_credits);
    }    
}

static void *credit_manager(void *__args) {
    thread_args_t *tdata = __args;
    struct conn_context* conn_ctxs = tdata->conn_ctxs;
    struct dev_context dev_ctx = tdata->dev_ctx;

    // Obtain initial batch credits
    // global_credits = query_credits(&dev_ctx, &conn_ctxs[0], sizeof(uint32_t));
    // global_credits_64 = *(uint64_t *)(conn_ctxs[0].data_buf);
    for (int i = 0; i < num_dlb_pp-1; i++) {
        global_credits = query_credits(&dev_ctx, &conn_ctxs[0], sizeof(uint32_t));
        global_credits_64 = *(uint64_t *)(conn_ctxs[0].data_buf);
        local_credits[i] = acquire_credits(&dev_ctx, &(conn_ctxs[0]), sizeof(uint32_t), global_credits_64, (global_credits_64 - (uint64_t)credit_batch));
        printf("local_credits is %d\n", local_credits[i]);
    }

    global_credits = query_credits(&dev_ctx, &conn_ctxs[0], sizeof(uint32_t));
    global_credits_64 = *(uint64_t *)(conn_ctxs[0].data_buf);
    local_credits[num_dlb_pp-1] = acquire_credits(&dev_ctx, &(conn_ctxs[0]), sizeof(uint32_t), global_credits_64, (global_credits_64 - (uint64_t)64));

    snic_global_credits = 0;

    // Obtain credits at runtime
    // while (1) {
    //     // if (snic_global_credits < 1024) {
    //         global_credits = query_credits(&dev_ctx, &conn_ctxs[0], sizeof(uint32_t));
    //         while (global_credits < credit_batch) {
    //             // printf("global credit = %d\n", global_credits);
    //             global_credits = query_credits(&dev_ctx, &conn_ctxs[0], sizeof(uint32_t));
    //         }
    //         // global_credits = query_credits(&dev_ctx, &conn_ctxs[0], sizeof(uint32_t));
    //         global_credits_64 = *(uint64_t *)(conn_ctxs[0].data_buf);
    //         snic_global_credits += acquire_credits(&dev_ctx, &(conn_ctxs[0]), sizeof(uint32_t), global_credits_64, (uint64_t)(128));
    //     // }
    //     uint64_t new_local_credit = snic_global_credits / num_dlb_pp;
    //     for (int i = 0; i < num_dlb_pp; i++) {
    //         if (local_credits[i] < (uint32_t)16) {
    //             local_credits[i] += new_local_credit;
    //             snic_global_credits -= new_local_credit;
    //             // printf("local_credits is %d\n", local_credits[i]);
    //         }
    //     }
    //     // for (int i = 0; i < num_dlb_pp; i++) {
    //     //     if (local_credits[i] < (uint32_t)4) {
    //     //         local_credits[i] += snic_global_credits / num_dlb_pp;
    //     //     }
    //     // }
    // }
}



int main (int argc, char *argv[]) {
#if defined(__x86_64__)
    printf("Compiled for x86_64 architecture.\n");
#elif defined(__i386__)
    printf("Compiled for x86 (32-bit) architecture.\n");
#elif defined(__aarch64__)
    printf("Compiled for ARM 64-bit architecture.\n");
#elif defined(__arm__)
    printf("Compiled for ARM (32-bit) architecture.\n");
#else
    printf("Unknown architecture.\n");
#endif

    int server_sockfd = -1;
    int client_sockfd = -1;
    int snic_sockfd = -1;
    int ret;

    ret = parse_args(argc, argv);
    printf("%d application arguments used\n", ret);
    if (ret < 0) {
        printf("Invalid application arguments\n");
        goto destroy_client_socket;
    }
    server_ip = "192.168.200.10";
    host_type = SNIC;

    credit_batch = (8192-1024) / num_dlb_pp;

    // initialize device
    struct dev_context dev_ctx = {
        .ib_dev_name = "mlx5_2",
        .dev_port = ib_port,
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

    void* map_base_1 = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), PAGE_SIZE);
    if (map_base_1 == (void *) -1) {
        fprintf(stderr, "Fail to allocate memory\n");
        goto destroy_device;
    }

    num_dlb_queues = (num_dlb_pp < 8) ? num_dlb_pp : 8;
    printf("num_dlb_queues = %d\n", num_dlb_queues);

    // initialize connection
    num_qps = 1 + num_dlb_pp + num_meta_conns;
    struct conn_context *conn_ctxs = (struct conn_context*)malloc(num_qps * sizeof(struct conn_context));
    if (conn_ctxs == NULL) goto destroy_device;

    int num_qps_init = 0;
    if (map_base_1 != (void *) -1) {
        for (int i = 0; i < num_qps; i++) {
            conn_ctxs[i].id = i;
            conn_ctxs[i].dev_ctx = &dev_ctx;
            conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
            conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            conn_ctxs[i].qp = NULL;
            conn_ctxs[i].cq = NULL;
            conn_ctxs[i].data_mr = NULL;
            conn_ctxs[i].dlb_data_qp = -1;
            conn_ctxs[i].dlb_credit_qp = -1;
            conn_ctxs[i].rdma_data_qp = -1;
            if (i == 0) {
                // use to manage DLB credits
                conn_ctxs[i].data_buf = (unsigned char *)(map_base_1);
                conn_ctxs[i].data_buf_size = PAGE_SIZE;
                conn_ctxs[i].dlb_credit_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
            } else if (i > 0 && i <= num_dlb_pp) {
                // use to enqueue QE to DLB remotely
                conn_ctxs[i].data_buf = NULL;
                conn_ctxs[i].data_buf_size = meta_size * sizeof(struct dlb_enqueue_qe) + PAGE_SIZE;
                conn_ctxs[i].dlb_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
            } else {
                // use to get client's rdma request metadata
                conn_ctxs[i].data_buf = NULL;
                conn_ctxs[i].data_buf_size = metadata_mr_size + PAGE_SIZE;
                conn_ctxs[i].rdma_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
            }
            conn_ctxs[i].validate_buf = false;
            conn_ctxs[i].inline_msg = false;

            if (init_connection_ctx(&conn_ctxs[i]) != 0) {
                fprintf(stderr, "Failed to initialize connection %d.\n", i);
                goto destroy_connections;
            }
            num_qps_init++;
        }
    } else {
        printf("[%s()] Faield to allocate MMAP DLB MMIO Space addr: %p\n", __func__, map_base_1);
    }

    // start connection
    if (host_type == CLIENT) {
        printf("Connecting server for data transter %s ...\n", server_ip);
        if ((server_sockfd = get_server_dest(&conn_ctxs[0], server_ip, "12340", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
            goto destroy_server_socket;
        }
        printf("Connecting snic for metadata %s ...\n", snic_ip);
        if ((snic_sockfd = get_server_dlb_dest(&conn_ctxs[0], snic_ip, "12341", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
            goto destroy_server_socket;
        }
    } else if (host_type == SNIC) {
        printf("Connecting server for metadata %s ...\n", server_ip);
        if ((server_sockfd = get_server_dlb_dest(&conn_ctxs[0], server_ip, "12342", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
            goto destroy_server_socket;
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12341) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
            goto destroy_client_socket;
        }
    } else if (host_type == SERVER) {
        printf("Waiting for snic's connections ...\n");
        if ((snic_sockfd = get_client_dlb_dest(&conn_ctxs[0], num_qps_init, 12342) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
            goto destroy_client_socket;
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12340) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
            goto destroy_client_socket;
        }
    }
    printf("Connection success\n");

    // connect QPs
    for (int i = 0; i < num_qps_init; i++) {
        print_conn_ctx(&conn_ctxs[i]);
        print_dest(&(conn_ctxs[i].local_dest));
        print_dest(&(conn_ctxs[i].remote_dest));
        if (connect_qps(&conn_ctxs[i]) != 0) {
            fprintf(stderr, "Failed to connect QPs\n");
            goto destroy_client_socket;
        }
        printf("Connection QP success\n");
    }

    // Create credit manager
    pthread_t credit_thread;
    thread_args_t credit_args;
    credit_args.conn_ctxs = &conn_ctxs[0];
    credit_args.dev_ctx = dev_ctx;
    pthread_create(&credit_args, NULL, credit_manager, &credit_args);

    // Create child threads
    pthread_t *worker_threads;
    thread_args_t *worker_args;
    worker_threads = (pthread_t *)calloc(num_meta_conns, sizeof(pthread_t));
    if (!worker_threads)
        return -ENOMEM;

    worker_args = (thread_args_t *)calloc(num_meta_conns, sizeof(thread_args_t));
    if (!worker_args) {
        free(worker_threads);
        return -ENOMEM;
    }
    for (int i = 0; i < num_meta_conns; i++) {
        worker_args[i].tid = i;
        worker_args[i].dlb_conn_id = 1 + i;
        worker_args[i].meta_conn_id = 1 + num_dlb_pp + i;
        worker_args[i].conn_ctxs = &conn_ctxs[0];
        worker_args[i].dev_ctx = dev_ctx;
        pthread_create(&worker_threads[i], NULL, snic_dlb_representative, &worker_args[i]);
    }

    pthread_join(credit_thread, NULL);

    for (int i = 0; i < num_meta_conns; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    free(worker_threads);
    free(worker_args);

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
    return EXIT_FAILURE;
}
