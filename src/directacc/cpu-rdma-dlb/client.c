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
#include <mm_malloc.h>

// #include <cpuid.h>
#include <sched.h>
#include <math.h>
#include <x86intrin.h>

#include "common.h"

double * latency;
uint64_t total_sent[32] = {0};

typedef struct {
    int tid;
    int dlb_conn_id;
    int data_conn_id;
    struct conn_context* conn_ctxs;
    struct dev_context dev_ctx;
} thread_args_t;

static void *latency_client_thread(void *__args)
{
    thread_args_t *tdata = __args;
    int tid = tdata->tid;
    int data_conn_id = tdata->data_conn_id;
    int dlb_conn_id = tdata->dlb_conn_id;
    struct conn_context* conn_ctxs = tdata->conn_ctxs;
    struct dev_context dev_ctx = tdata->dev_ctx;

    // Makeup rdma metadata
    for (int i = 0; i < meta_size; i++) {
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].addr = conn_ctxs[data_conn_id].remote_mr.addr;
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].size = msg_size;
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].valid = 0x1111;
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].wr_id = tid;
    }

    int cnt = 0;
    uint64_t metadata_offset = 0;
    post_recv(&conn_ctxs[tid], metadata_mr_size, sizeof(READY_MSG));
    post_recv(&conn_ctxs[data_conn_id], bufs_num*PAGE_SIZE, 16);
    uint64_t start_time, end_time;
    uint64_t test_start, test_end;

    uint64_t remote_offset = 0;
    uint64_t latency_idx = 0;

    uint64_t freq = get_tsc_freq_arch();
    test_start = __rdtsc();
    // Send payload and QE
    for (int i = 0; i < num_events; i++) {
        post_send_write(&conn_ctxs[data_conn_id], 0, remote_offset, msg_size);
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[metadata_offset].addr = conn_ctxs[data_conn_id].remote_mr.addr + remote_offset;
        post_send_write(&conn_ctxs[tid], metadata_offset*sizeof(struct rdma_metadata), metadata_offset*sizeof(struct rdma_metadata), 1*sizeof(struct rdma_metadata));
        remote_offset = (remote_offset + msg_size) % (bufs_num*PAGE_SIZE);
        metadata_offset = (metadata_offset + 1) % meta_size;
        wait_completions(&conn_ctxs[tid], WRITE_WRID, 1);

        start_time = __rdtsc();
        struct ibv_wc wc[20];
        int ne = 0;
        while (ne < 2) {
            ne += ibv_poll_cq(conn_ctxs[data_conn_id].cq, 2, wc);
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
            }
        }
        end_time = __rdtsc();
        for (int k = 0; k < ne; k++) {
            if (wc[k].wr_id == RECV_WRID) {
                latency[latency_idx] = (double)(end_time - start_time) / (double)freq * 1e6;
                latency_idx = (latency_idx + 1) % num_events;
                post_recv(&conn_ctxs[data_conn_id], bufs_num*PAGE_SIZE+(i%128)*msg_size, msg_size);
            }
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n", ibv_wc_status_str(wc[k].status), wc[k].status, (int)wc[k].wr_id);
            }
        }
    }
    test_end = __rdtsc();
    printf("Request rate is %lf mrps\n", (double)(num_events) / (double)(test_end - test_start) * (double)freq / 1e6);

    // Cleanup
    printf("Leaving T%d...\n", tid);
    test_finished = 1;
}


static void *client_thread(void *__args)
{
    thread_args_t *tdata = __args;
    int tid = tdata->tid;
    int data_conn_id = tdata->data_conn_id;
    int dlb_conn_id = tdata->dlb_conn_id;
    struct conn_context* conn_ctxs = tdata->conn_ctxs;
    struct dev_context dev_ctx = tdata->dev_ctx;

    // Makeup rdma metadata
    for (int i = 0; i < meta_size; i++) {
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].addr = conn_ctxs[data_conn_id].remote_mr.addr;
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].size = msg_size;
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].valid = 0x1111;
        ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[i].wr_id = tid;
    }

    int cnt = 0;
    uint64_t metadata_offset = 0;
    uint64_t remote_offset = 0;
    printf("rdma_metadata size is %d\n", sizeof(struct rdma_metadata));
    post_recv(&conn_ctxs[tid], metadata_mr_size, sizeof(READY_MSG));
    uint64_t start_time, end_time, timestamp;

    // Send payload and QE
    start_time = rdtsc();
    for (int i = 0; test_finished == 0; i++) {
        // Send request
        for (int j = 0; j < 3; j++) {
            cnt++;
            ((uint64_t *)(conn_ctxs[data_conn_id].data_buf+(cnt)*msg_size))[0] = 0;
            post_send_write_no_signal(&conn_ctxs[data_conn_id], cnt*msg_size, cnt*msg_size, msg_size);
        }
        cnt++;
        ((uint64_t *)(conn_ctxs[data_conn_id].data_buf+(cnt)*msg_size))[0] = 0;

        if (cnt % 4 == 0 && cnt < meta_size) {
            post_send_write_no_signal(&conn_ctxs[data_conn_id], cnt*msg_size, cnt*msg_size, msg_size);
        }
        else if (cnt == meta_size) {
            post_send_write(&conn_ctxs[data_conn_id], cnt*msg_size, cnt*msg_size, msg_size);
            wait_completions(&conn_ctxs[data_conn_id], WRITE_WRID, 1);
        }

        // Prepare and send metadata
        for (int j = 0; j < 4; j++) {      
            ((struct rdma_metadata *)(conn_ctxs[tid].data_buf))[metadata_offset+j].addr = conn_ctxs[data_conn_id].remote_mr.addr + remote_offset;
            remote_offset = (remote_offset + msg_size) % (bufs_num*PAGE_SIZE);
        }
        if (i != 0 && metadata_offset == meta_size-4) {
            post_send_write(&conn_ctxs[tid], metadata_offset*sizeof(struct rdma_metadata), metadata_offset*sizeof(struct rdma_metadata), 4*sizeof(struct rdma_metadata));
        } else {
            post_send_write_no_signal(&conn_ctxs[tid], metadata_offset*sizeof(struct rdma_metadata), metadata_offset*sizeof(struct rdma_metadata), 4*sizeof(struct rdma_metadata));
        }
        metadata_offset = (metadata_offset + 4) % meta_size;

        if (cnt == meta_size) {
            wait_completions(&conn_ctxs[tid], RECV_WRID, 1);
            post_recv(&conn_ctxs[tid], metadata_mr_size, sizeof(READY_MSG));
            cnt = 0;
        }

        total_sent[tid] += 4;
        // printf("T%d metadata_offset is %d, cnt = %d, i = %d\n", tid, metadata_offset, cnt, i);
    }

    end_time = rdtsc();
    uint64_t freq = get_tsc_freq_arch();
    printf("Request rate is %lf mrps\n", (double)total_sent[tid] / (double)(end_time - start_time) * (double)freq / 1e6);

    // Cleanup
    printf("Leaving T%d...\n", tid);
}



int main (int argc, char *argv[]) {

    int server_sockfd = -1;
    int snic_sockfd = -1;
    int client_sockfd = -1;
    int ret;

    ret = parse_args(argc, argv);
    printf("%d application arguments used\n", ret);
    if (ret < 0) {
        printf("Invalid application arguments\n");
        goto destroy_client_socket;
    }
    server_ip = "192.168.200.10";
    snic_ip = "192.168.200.20";
    host_type = CLIENT;

    // initialize device
    struct dev_context dev_ctx = {
        .ib_dev_name = "mlx5_0",
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

    // initialize connection
    num_qps = num_dlb_pp + num_client_conns;
    struct conn_context *conn_ctxs = (struct conn_context*)malloc(num_qps * sizeof(struct conn_context));
    if (conn_ctxs == NULL) goto destroy_device;

    int num_qps_init = 0;
    for (int i = 0; i < num_qps; i++) {
        conn_ctxs[i].id = i;
        conn_ctxs[i].dev_ctx = &dev_ctx;
        conn_ctxs[i].validate_buf = false;
        conn_ctxs[i].inline_msg = false;
        conn_ctxs[i].qp = NULL;
        conn_ctxs[i].cq = NULL;
        conn_ctxs[i].data_mr = NULL;
        conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
        conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
        conn_ctxs[i].dlb_data_qp = -1;
        conn_ctxs[i].dlb_credit_qp = -1;
        conn_ctxs[i].rdma_data_qp = -1;
        if (i < num_dlb_pp) {
            // use to send rdma metadata to SNIC
            conn_ctxs[i].data_buf = NULL;
            conn_ctxs[i].data_buf_size = metadata_mr_size + PAGE_SIZE;
            conn_ctxs[i].rdma_data_qp = i;
            conn_ctxs[i].dlb_data_qp = i;
            conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE;
            conn_ctxs[i].qp_access_flags = 0;
        } else {
            // use to send rdma data to server
            conn_ctxs[i].data_buf = NULL;
            conn_ctxs[i].data_buf_size = bufs_num*PAGE_SIZE*2;
            conn_ctxs[i].rdma_data_qp = i;
            conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE;
            conn_ctxs[i].qp_access_flags = 0;
        }

        if (init_connection_ctx(&conn_ctxs[i]) != 0) {
            fprintf(stderr, "Failed to initialize connection %d.\n", i);
        }
        num_qps_init++;
    }


    // start connection
    if (host_type == CLIENT) {
        printf("Connecting server for data transter %s ...\n", server_ip);
        if ((server_sockfd = get_server_dest(&conn_ctxs[0], server_ip, "12340", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
        }
    } else if (host_type == SERVER) {
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12340) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
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
            goto destroy_connections;
        }
        printf("Connection QP success\n");
    }

    latency = (double *)malloc(num_events * sizeof(double));
    // Create client child threads
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
    for (int i = 0; i < num_meta_conns - 1; i++) {
        worker_args[i].tid = i;
        worker_args[i].dlb_conn_id = i;
        worker_args[i].data_conn_id = num_dlb_pp + i;
        worker_args[i].conn_ctxs = &conn_ctxs[0];
        worker_args[i].dev_ctx = dev_ctx;
        pthread_create(&worker_threads[i], NULL, client_thread, &worker_args[i]);
    }

    // last thread as latency measurement
    worker_args[num_meta_conns - 1].tid = num_meta_conns - 1;
    worker_args[num_meta_conns - 1].dlb_conn_id = num_meta_conns - 1;
    worker_args[num_meta_conns - 1].data_conn_id = num_dlb_pp + num_meta_conns - 1;
    worker_args[num_meta_conns - 1].conn_ctxs = &conn_ctxs[0];
    worker_args[num_meta_conns - 1].dev_ctx = dev_ctx;
    pthread_create(&worker_threads[num_meta_conns - 1], NULL, latency_client_thread, &worker_args[num_meta_conns - 1]);

    // Latency tests
    uint64_t start_time, end_time;
    double difference;
    start_time = rdtscp();

    for (int i = 0; i < num_meta_conns; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    // Complete, get latency
    post_recv(&conn_ctxs[num_qps_init-1], 0, sizeof(COMPLETE_MSG));
    wait_completions(&conn_ctxs[num_qps_init-1], RECV_WRID, 1);


    end_time = rdtsc();
    uint64_t freq = get_tsc_freq_arch();
    printf("TSC Freq is %ld Hz\n", freq);
    difference = (double)(end_time - start_time) / (double)freq * 1e6;
    uint64_t total_events = num_events;
    for (int i = 0; i < num_client_conns; i++) {
        total_events += total_sent[i];
    }
    printf("RDMA running time is %lf us\n", difference);
    printf("Avg RDMA running time is %lf us\n", difference/total_events);
    printf("Throughput is %lf MB/s\n", (double)total_events * (double)(msg_size + 16) / difference);
    printf("Throughput is %lf Gbps\n", (double)total_events * (double)(msg_size + 16) * 8.0 / difference / 1000.0);
    printf("Request rate is %lf mrps\n", (double)total_events / difference);

    double total_latency = 0;
    for (int i = 0; i < num_events; i++) {
        total_latency += latency[i];
    }
    qsort(latency, num_events, sizeof(double), cmpfunc);
    printf("==== E2E Latency ====\n");
    printf("Total latency size: %d\n", num_events);
    printf("Total latency: %.3f us\n", total_latency);
    printf("Average latency: %.3f us\n", total_latency / (double)num_events);
    printf("min%% tail latency: %.3f us\n", latency[(int) 0]);
    printf("25%% tail latency: %.3f us\n", latency[(int) (0.25 * num_events)]);
    printf("50%% tail latency: %.3f us\n", latency[(int) (0.50 * num_events)]);
    printf("75%% tail latency: %.3f us\n", latency[(int) (0.75 * num_events)]);
    printf("90%% tail latency: %.3f us\n", latency[(int) (0.9 * num_events)]);
    printf("95%% tail latency: %.3f us\n", latency[(int) (0.95 * num_events)]);
    printf("99%% tail latency: %.3f us\n", latency[(int) (0.99 * num_events)]);
    printf("max%% tail latency: %.3f us\n", latency[(int) (num_events - 1)]);

    free(latency);

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