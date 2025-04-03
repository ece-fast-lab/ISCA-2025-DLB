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

    // DLB credit control
    uint32_t local_credits = 0;
    uint32_t global_credits = 0;
    uint32_t credit_batch = 128;
    uint64_t global_credits_64 = 0;

    ret = parse_args(argc, argv);
    printf("%d application arguments used\n", ret);
    if (ret < 0) {
        printf("Invalid application arguments\n");
        goto destroy_client_socket;
    }
    server_ip = "192.168.200.10";
    host_type = SNIC;

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

    void* map_base = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), 4*PAGE_SIZE);
    if (map_base == (void *) -1) {
        fprintf(stderr, "Fail to allocate memory\n");
        goto destroy_device;
    }

    void* map_base_1 = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), PAGE_SIZE);
    if (map_base_1 == (void *) -1) {
        fprintf(stderr, "Fail to allocate memory\n");
        goto destroy_device;
    }

    // void* map_base_2 = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), 4*PAGE_SIZE);
    // if (map_base_2 == (void *) -1) {
    //     fprintf(stderr, "Fail to allocate memory\n");
    //     goto destroy_device;
    // }

    // initialize connection
    struct conn_context *conn_ctxs = (struct conn_context*)malloc(num_qps * sizeof(struct conn_context));
    if (conn_ctxs == NULL) goto destroy_device;

    int num_qps_init = 0;
    if (map_base != (void *) -1) {
        for (int i = 0; i < num_qps; i++) {
            conn_ctxs[i].id = i;
            conn_ctxs[i].dev_ctx = &dev_ctx;
            conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
            conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            conn_ctxs[i].qp = NULL;
            conn_ctxs[i].data_mr = NULL;
            conn_ctxs[i].dlb_data_qp = -1;
            conn_ctxs[i].dlb_credit_qp = -1;
            conn_ctxs[i].rdma_data_qp = -1;
            if (i == 0) {
                // use to enqueue QE to DLB remotely
                conn_ctxs[i].data_buf = (unsigned char *)(map_base);
                conn_ctxs[i].data_buf_size = 4*PAGE_SIZE;
                conn_ctxs[i].dlb_data_qp = i;
            } else if (i == 1) {
                // use to manage DLB credits
                conn_ctxs[i].data_buf = (unsigned char *)(map_base_1);
                conn_ctxs[i].data_buf_size = mr_size;
                conn_ctxs[i].dlb_credit_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
            } else {
                // use to get client's rdma request metadata
                conn_ctxs[i].data_buf = NULL;
                conn_ctxs[i].data_buf_size = metadata_mr_size + PAGE_SIZE;
                conn_ctxs[i].rdma_data_qp = i;
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
        printf("[%s()] Faield to allocate MMAP DLB MMIO Space addr: %p\n", __func__, map_base);
    }

    // start connection
    if (host_type == CLIENT) {
        printf("Connecting server for data transter %s ...\n", server_ip);
        if ((server_sockfd = get_server_dest(&conn_ctxs[0], server_ip, "8080", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
            goto destroy_server_socket;
        }
        printf("Connecting snic for metadata %s ...\n", snic_ip);
        if ((snic_sockfd = get_server_dlb_dest(&conn_ctxs[0], snic_ip, "8081", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
            goto destroy_server_socket;
        }
    } else if (host_type == SNIC) {
        printf("Connecting server for metadata %s ...\n", server_ip);
        if ((server_sockfd = get_server_dlb_dest(&conn_ctxs[0], server_ip, "8082", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
            goto destroy_server_socket;
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 8081) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
            goto destroy_client_socket;
        }
    } else if (host_type == SERVER) {
        printf("Waiting for snic's connections ...\n");
        if ((snic_sockfd = get_client_dlb_dest(&conn_ctxs[0], num_qps_init, 8082) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
            goto destroy_client_socket;
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 8080) < 0)) {
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

    // Makeup queuing element
    dlb_enqueue_qe_t *enqueue_qe;
    int err = posix_memalign((void **) &enqueue_qe,
                         CACHE_LINE_SIZE,
                         CACHE_LINE_SIZE);
    if (err == 1)
        printf("Allocation failed\n");

    for (int i = 0; i < 4; i++) {
        enqueue_qe[i].data = conn_ctxs[1].remote_mr.addr + i*msg_size;
        enqueue_qe[i].opaque = i;
        enqueue_qe[i].qid = 31;
        enqueue_qe[i].sched_byte = 1;
        enqueue_qe[i].flow_id = 0;
        enqueue_qe[i].misc_byte = 0;
        enqueue_qe[i].cmd_byte = 8;
    }
    memcpy(conn_ctxs[0].data_buf, (char *)(enqueue_qe), 4*sizeof(dlb_enqueue_qe_t));

    // Obtain initial batch credits
    global_credits = query_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t));
    global_credits_64 = *(uint64_t *)(conn_ctxs[1].data_buf);
    local_credits = acquire_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t), global_credits_64, (global_credits_64 - (uint64_t)credit_batch));
    printf("global credits is %d, local_credits is %d\n", global_credits, local_credits);
    // for (int i = 0; i < local_credits; i++) {
    //     post_recv(&conn_ctxs[2], 4*sizeof(struct rdma_metadata));
    // }


    // Start RDMA
    uint64_t ecnt = 0;
    int buf_idx = 0;
    int credit_acquire_cnt = 0;
    uint64_t metadata_offset = 0;
    uint64_t start_time, end_time;
    double difference;
    start_time = rdtsc();
    printf("start_time is %lu\n", start_time);
    printf("rdma_metadata size is %d\n", sizeof(struct rdma_metadata));
    memcpy(conn_ctxs[2].data_buf + metadata_mr_size, READY_MSG, sizeof(READY_MSG));

    while (ecnt < num_events) {
        // Acquire credits before sending
        if (local_credits < (uint32_t)4) {
            global_credits = query_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t));
            while (global_credits < credit_batch) {
                // usleep(1);
                // nanosleep(&request, &remaining);
                global_credits = query_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t));
                // printf("has %d, sent %d\n", global_credits, i*4);
            }
            while (local_credits < (uint32_t)4) {
                global_credits = query_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t));
                global_credits_64 = *(uint64_t *)(conn_ctxs[1].data_buf);
                local_credits += acquire_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t), global_credits_64, (global_credits_64 - (uint64_t)credit_batch));
                // global_credits_64 = *(uint64_t *)(conn_ctxs[1].data_buf + 8);
                // global_credits = query_credits(&dev_ctx, &conn_ctxs[1], sizeof(uint32_t));
                // printf("new global credits is %d, local credits is %d\n", *(uint32_t *)(conn_ctxs[1].data_buf), local_credits);
                credit_acquire_cnt++;
            }
            // printf("local credits is %d\n", local_credits);
        }

        // Check new RDMA request
        // wait_completions(&dev_ctx, RECV_WRID, 1);
        // printf("here %d\n", ecnt);
        // post_recv(&conn_ctxs[2], 4*sizeof(struct rdma_metadata));

        // Send QE to DLB remotely from SNIC
        int valid_qe = 0;
        while (valid_qe != 4) {
            // printf("valid_qe is %d\n", valid_qe);
            if (metadata_offset + 4 >= 1024) {
                metadata_offset = 0;
            }

            if (buf_idx + 4 >= 1024) {
                buf_idx = 0;
            }
            // printf("metadata_offset is %d\n", metadata_offset);
            // printf("valid 0 is %d\n", ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset].valid);
            // printf("valid 1 is %d\n", ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+1].valid);
            // printf("valid 2 is %d\n", ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+2].valid);
            // printf("valid 3 is %d\n", ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+3].valid);
            if (((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset].valid == 0
                    || ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+1].valid == 0
                    || ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+2].valid == 0
                    || ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+3].valid == 0) 
                continue;

            for (int j = 0; j < 4; j++) {
                if (((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+j].valid != 0) {
                    // printf("addr is %lu\n", ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+j].addr);
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].data = ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+j].addr;
                    // ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[j].opaque = ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+j].size;
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].opaque = (ecnt+j) % UINT16_MAX;
                    // printf("opaque is %d\n", ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].opaque);
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].qid = 31;
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].sched_byte = 1;
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].flow_id = 0;
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].misc_byte = 0;
                    ((dlb_enqueue_qe_t *)(conn_ctxs[0].data_buf))[buf_idx+j].cmd_byte = 8;

                    ((struct rdma_metadata *)(conn_ctxs[2].data_buf))[metadata_offset+j].valid = 0;
                    valid_qe++;
                }
            }
        }
        metadata_offset += 4;
        ecnt += 4;
        // memcpy(conn_ctxs[0].data_buf, (char *)(enqueue_qe), 4*sizeof(dlb_enqueue_qe_t));

        if (ecnt % 512 != 0) {
            post_send_write_no_signal(&conn_ctxs[0], buf_idx*sizeof(dlb_enqueue_qe_t), 0, 4*sizeof(dlb_enqueue_qe_t));
        } else {
            post_send_write(&conn_ctxs[0], buf_idx*sizeof(dlb_enqueue_qe_t), 0, 4*sizeof(dlb_enqueue_qe_t));
            wait_completions(&dev_ctx, WRITE_WRID, 1);
        }
        buf_idx += 4;

        local_credits -= 4;
        // printf("metadata_offset is %d\n", metadata_offset);
        // printf("valid_qe is %d\n", valid_qe);
        // printf("local credits is %d\n", local_credits);
        // printf("ecnt is %d\n", ecnt);
        if (ecnt % 1024 == 0) {
            post_send(&conn_ctxs[2], metadata_mr_size, sizeof(READY_MSG));
            wait_completions(&dev_ctx, SEND_WRID, 1);
            // printf("here\n");
            usleep(1);
        }
    }

    post_send_read(&conn_ctxs[1], 0, sizeof(uint32_t));
    wait_completions(&dev_ctx, READ_WRID, 1);

    // Complete, get latency
    end_time = rdtsc();
    uint64_t freq = get_tsc_freq_arch();
    printf("TSC Freq is %ld Hz\n", freq);
    difference = (double)(end_time - start_time) / (double)freq * 1e6;
    printf("RDMA running time is %lf us\n", difference);
    printf("Avg RDMA running time is %lf us\n", difference/num_events);
    printf("Throughput is %lf MB/s\n", (double)num_events * (double)(msg_size + 16) / difference);
    printf("Throughput is %lf Gbps\n", (double)num_events * (double)(msg_size + 16) * 8.0 / difference / 1000.0);
    printf("Request rate is %lf mrps\n", (double)num_events / difference);

    // Complete, get latency
    post_recv(&conn_ctxs[2], 0, sizeof(COMPLETE_MSG));
    wait_completions(&dev_ctx, RECV_WRID, 1);

    printf("Message received '%s'\n", conn_ctxs[2].data_buf);
    printf("Global credit is '%u'\n", *(uint32_t *)(conn_ctxs[1].data_buf));
    printf("Local credit is '%u'\n", local_credits);

    // memcpy(conn_ctxs[2].data_buf, COMPLETE_MSG, sizeof(COMPLETE_MSG));
    // post_send(&conn_ctxs[2], 0, sizeof(COMPLETE_MSG));
    // wait_completions(&dev_ctx, SEND_WRID, 1);

    free(enqueue_qe);

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