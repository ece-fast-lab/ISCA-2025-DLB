/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2018 Intel Corporation
 */

#include "dlb_microbench_common.h"

static void *tx_traffic(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[enqueue_depth];
    int64_t num_loops, i, num_tx = 0;
    int ret;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0) {
        perror("pthread_setaffinity_np");
    }

    /* Seed random number generator */
    srand(time(NULL));

    num_loops = (num_events == 0) ? -1 : num_events / enqueue_depth / num_txs;

    /* Initialize the static fields in the send events */
    for (i = 0; i < enqueue_depth; i++) {
        events[i].adv_send.queue_id = args->queue_id;
        events[i].adv_send.sched_type = sched_type;
        events[i].adv_send.priority = 0;
        // events[i].send.priority = (args->queue_id) % 8;
        // events[i].send.weight = (args->queue_id) % 4;
	    if (!sched_type)
		    events[i].adv_send.flow_id = args->core_mask;
		    // events[i].send.flow_id = 0xABCD;
    }

    uint64_t test_start, test_end, test_count = 0;
    uint64_t local_start_time = rdtscp();
    test_start = local_start_time;
    bool cnt = 0;
    int tx_prio = 0;
    for (i = 0; (num_tx < num_events / num_txs) || (num_loops == -1); i++) {
        int j, num;

        /* Initialize the dynamic fields in the send events */
        if (packet_prio_en) {
            tx_prio = rand() % 8;   // enable packet priority
        } else {
            tx_prio = 0;            // disable packet priority
        }
        for (j = 0; j < enqueue_depth; j++) {
            events[j].adv_send.priority = tx_prio;
        uint64_t temp_time = __rdtsc();
        events[j].adv_send.udata64 = temp_time;
            // events[j].adv_send.udata64 = num_tx + j;
            // events[j].adv_send.udata16 = (num_tx + j) % UINT16_MAX;
            // events[j].adv_send.udata16 = UINT16_MAX;
        }

        /* Send the events */
        ret = 0;
        num = 0;
        for (j = 0; num != enqueue_depth && j < RETRY_LIMIT; j++) {
            ret = dlb_send(args->port, enqueue_depth-num, &events[num]);

            if (ret == -1)
                continue;

            num += ret;
        }
        packet_priority_count[args->index][tx_prio] += num;
	    num_tx += num;

        if (report_latency > 0) {
            test_end = rdtsc();
            uint64_t test_diff = test_end - test_start;
            test_count += num;
            if (test_diff > freq) {
                printf("[Tx %d] Avg thourghput is %lf Mpps\n", args->index, (double)test_count / ((double)test_diff / (double)freq * 1E6));
                test_start = test_end;
                test_count = 0;
            }
        }

	    // if (num_tx % 1000000 == 0)
		//     printf("[%s] Sent events : %ld\n", __func__, num_tx);

#if 0
        if (num != enqueue_depth) {
            printf("[%s()] FAILED: Sent %d/%d events on iteration %ld!\n",
                   __func__, num, enqueue_depth, i);
            exit(-1);
        }
#endif
    }
    uint64_t tx_local_end_time = rdtscp();
    double tx_latency = (double)(tx_local_end_time - local_start_time) / (double)freq / (double)num_tx * 1E6;
    printf("[Tx %d] per event tx latency is %lf us\n", args->index, tx_latency);
    printf("[Tx %d] throughput is %lf Mpps\n", args->index, 1 / tx_latency);
    printf("[%s()] Sent %ld events\n",
           __func__, num_tx);
    return NULL;
}

static void *tx_traffic_rate_limiting(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[enqueue_depth];
    int64_t num_loops, i, num_tx = 0;
    int ret;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0) {
        perror("pthread_setaffinity_np");
    }

    /* Seed random number generator */
    srand(time(NULL));

    num_loops = (num_events == 0) ? -1 : num_events / enqueue_depth / num_txs;

    /* Initialize the static fields in the send events */
    for (i = 0; i < enqueue_depth; i++) {
        events[i].adv_send.queue_id = args->queue_id;
        events[i].adv_send.sched_type = sched_type;
        events[i].adv_send.priority = 0;
        // events[i].send.priority = (args->queue_id) % 8;
        // events[i].send.weight = (args->queue_id) % 4;
	    if (!sched_type)
		    events[i].adv_send.flow_id = args->core_mask;
		    // events[i].send.flow_id = 0xABCD;
    }

    uint64_t test_start, test_end, test_count = 0;
    uint64_t local_start_time = __rdtsc();
    test_start = local_start_time;
    bool cnt = 0;
    int tx_prio = 0;

    uint64_t cur_ticks = 0;
    uint64_t prev_ticks = __rdtsc();
    for (i = 0; (num_tx < num_events / num_txs) || (num_loops == -1); i++) {
        int j, num;

        /* Initialize the dynamic fields in the send events */
        if (packet_prio_en) {
            tx_prio = rand() % 8;   // enable packet priority
        } else {
            tx_prio = 0;            // disable packet priority
        }

        cur_ticks = __rdtsc();
        if (cur_ticks - prev_ticks > freq / 1000000) {
            for (j = 0; j < enqueue_depth; j++) {
                events[j].adv_send.priority = tx_prio;
            uint64_t temp_time = __rdtsc();
            events[j].adv_send.udata64 = temp_time;
                // events[j].adv_send.udata64 = num_tx + j;
                // events[j].adv_send.udata16 = (num_tx + j) % UINT16_MAX;
                // events[j].adv_send.udata16 = UINT16_MAX;
            }

            /* Send the events */
            ret = 0;
            num = 0;
            for (j = 0; num != enqueue_depth && j < RETRY_LIMIT; j++) {
                ret = dlb_send(args->port, enqueue_depth-num, &events[num]);

                if (ret == -1)
                    break;

                num += ret;
            }
            packet_priority_count[args->index][tx_prio] += num;
            num_tx += num;

            if (report_latency > 0) {
                test_end = __rdtsc();
                uint64_t test_diff = test_end - test_start;
                test_count += num;
                if (test_diff > freq) {
                    printf("[Tx %d] Avg thourghput is %lf Mpps\n", args->index, (double)test_count / ((double)test_diff / (double)freq * 1E6));
                    test_start = test_end;
                    test_count = 0;
                }
            }

            prev_ticks = cur_ticks;

        }
	    // if (num_tx % 1000000 == 0)
		//     printf("[%s] Sent events : %ld\n", __func__, num_tx);

#if 0
        if (num != enqueue_depth) {
            printf("[%s()] FAILED: Sent %d/%d events on iteration %ld!\n",
                   __func__, num, enqueue_depth, i);
            exit(-1);
        }
#endif
    }
    uint64_t tx_local_end_time = __rdtsc();
    double tx_latency = (double)(tx_local_end_time - local_start_time) / (double)freq / (double)num_tx * 1E6;
    printf("[Tx %d] per event tx latency is %lf us\n", args->index, tx_latency);
    printf("[Tx %d] throughput is %lf Mpps\n", args->index, 1 / tx_latency);
    printf("[%s()] Sent %ld events\n",
           __func__, num_tx);
    return NULL;
}

static void *rx_traffic(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[dequeue_depth];
    int ret, nfds = 0, epoll_fd;
    struct epoll_event epoll_events;
    int epoll_retry = EPOLL_RETRY;
    int64_t num_loops, i, num_mismatch = 0, num_rx = 0;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) <0) {
        perror("pthread_setaffinity_np");
    }

    if (num_stages == 1 && num_events != 0) {
        uint64_t count = 0;
        while (count < num_events) {
            count = 0;
            for (i = 0; i < 8; i++) {
                count += worker_pkt_count[i];
            }
        }
        printf("count is %ld!!!!\n", count);
        goto end;
    }

    if (epoll_enabled)
        epoll_fd = setup_epoll(args);

    num_loops = (num_events == 0) ? -1 : num_events / dequeue_depth;

    int last_verified_udata64 = 0;
    bool cnt = 0;

    uint64_t test_start, test_end, test_count = 0;
    uint64_t rx_latency = 0;
    uint64_t local_start_time = rdtscp();
    test_start = local_start_time;
    struct timespec clock_start_time, clock_end_time;
    clock_gettime(CLOCK_REALTIME, &clock_start_time);

    for (i = 0; num_rx < num_events || num_loops == -1; i++) {
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
            } 
        }
        else{
            /* Receive the events */
            for (j = 0; (num != dequeue_depth && j < RETRY_LIMIT); j++)
            {
                ret = dlb_recv(args->port, dequeue_depth-num,
                            (wait_mode == INTERRUPT), &events[num]);

                if (ret == -1) {
                    printf("[%s()] ERROR: dlb_recv failure at iterations %d\n", __func__, j);
                    break;
                }

                num += ret;
                if ((j != 0) && (j % 10000000 == 0))
                    printf("[%s()] TIMEOUT: Rx blocked for %d iterations\n", __func__, j);
            }            

            if (num != dequeue_depth)
            {
                printf("[%s()] FAILED: Recv'ed %d events (iter %ld)!\n", __func__, num, i);
                exit(-1);
            }
        }

        /* Validate the events */
        uint64_t recv_lat = rdtscp();
        for (j = 0; j < num; j++) {
            if (events[j].recv.sched_type == SCHED_UNORDERED) {
                if (events[j].recv.error)
                    printf("[%s()] FAILED: Bug in received event [PARALLEL]", __func__);
                
                // if (events[j].recv.udata16 == UINT16_MAX) {
                    rx_latency += (recv_lat - events[j].recv.udata64);
                    // printf("Rx: pakcet consumes %ld cycles in DLB\n", recv_lat - events[j].recv.udata64);
                // }
            }
            else if ((events[j].recv.udata64 != num_rx + j && (events[j].recv.udata16 != (num_rx + j) % UINT16_MAX)) ||
                    (events[j].recv.queue_id != args->queue_id && (num_txs == 1 || num_workers > 0)) ||
                    (events[j].recv.sched_type == SCHED_ATOMIC &&
                    events[j].recv.flow_id != 0xABCD) ||
                    events[j].recv.error) {
                printf("[%s()] FAILED: Bug in received event num_rx + j:%ld "
                        "(num_rx: %ld, j: %d), events[j].recv.udata64: %ld  "
                        "events[j].recv.udata16 : %d\n",
                        __func__, num_rx + j, num_rx, j, events[j].recv.udata64,
                        events[j].recv.udata16);
                num_mismatch++;
                if (num_mismatch > 100)
                    exit(-1);
            }
            else {
                    last_verified_udata64 = events[j].recv.udata64;
            }
        }
        num_rx += num;

        if (num_rx == 4) {
            local_start_time = recv_lat;
        }

        if (report_latency > 0) {
            test_end = rdtsc();
            uint64_t test_diff = test_end - test_start;
            test_count += num;
            if (test_diff > freq) {
                printf("[Rx] Avg thourghput is %lf Mpps\n", (double)test_count / ((double)test_diff / (double)freq * 1E6));
                test_start = test_end;
                test_count = 0;
            }
        }

        // if (num_rx % 1000000 == 0)
        //     printf("[%s] Received events : %ld\n", __func__, num_rx);

        if (dlb_release(args->port, num) != num) {
            printf("[%s()] FAILED: Release of all %d events (iter %ld)!\n",
                __func__, num, i);
            exit(-1);
        }
    }

end:
    uint64_t local_end_time = rdtscp();
    clock_gettime(CLOCK_REALTIME, &clock_end_time);
    printf("per event rx latency is %lf us\n", (double)(local_end_time - local_start_time) / (double)freq / (double)num_rx * 1E6);
    printf("clock_gettime per event rx latency is %lf us\n", ((double)(clock_end_time.tv_sec - clock_start_time.tv_sec) * 1e6 + (double)(clock_end_time.tv_nsec - clock_start_time.tv_nsec) / 1e3) / (double)num_rx);
    printf("one-by-one calculation: per event rx latency is %lf us\n", (double)(rx_latency) / (double)freq / (double)num_rx * 1E6);
    printf("[RX] throughput is %lf Mpps\n", (double)num_rx / (double)(local_end_time - local_start_time) * (double)freq / 1E6);
    printf("[%s()] Received %ld events, num_mismatch: %ld\n",
           __func__, num_rx, num_mismatch);
    worker_done = true;
    printf("rx_thread done\n");
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

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) <0) {
        perror("pthread_setaffinity_np");
    }

    printf("Worker %d started\n", args->index);

    if (epoll_enabled)
        epoll_fd = setup_epoll(args);

    uint64_t lat_arr[10000];
    int latency_idx = 0;

    uint64_t recv_lat;

    uint64_t test_start, test_end, test_count = 0;
    uint64_t local_start_time = rdtscp();
    test_start = local_start_time;
    for (i = 0; !worker_done; i++) {
        dlb_event_t events[dequeue_depth];
	    int64_t num_tx, num_rx;
        int j;

        nfds = 0;
        epoll_retry = EPOLL_RETRY;
        if (epoll_enabled)
        {
            while (nfds == 0 && --epoll_retry > 0) {
                    nfds = epoll_wait(epoll_fd, &epoll_events, 1, ticks);
                if (worker_done)
                        goto end;
                    else if (nfds < 0) {
                        printf("[%s()] FAILED: epoll_wait", __func__);
                        goto end;
                    }
                }
                if (nfds == 0 && epoll_retry == 0) {
                        printf("[%s()] TIMEOUT: No eventfd ready in %ld msec. Exiting.\n",
                            __func__, ticks * EPOLL_RETRY);
                    goto end;
                        }
                num_rx = dlb_recv(args->port, 
                                dequeue_depth,
                                (wait_mode == INTERRUPT), 
                                events);
        }
        else
        {
            /* Receive the events */
            for (j = 0, num_rx = 0; num_rx == 0 && j < RETRY_LIMIT; j++) {
                num_rx = dlb_recv(args->port,
                                dequeue_depth,
                                (wait_mode == INTERRUPT),
                                events);
                // if(num_rx == 0) usleep(0); /* Worker should release CPU if queue is empty */
                // if ((j != 0) && (j % 10000000 == 0))
                //     printf("[%s()] TIMEOUT: Worker blocked for %d iterations\n", __func__, j);
            }
        }

        /* The port was disabled, indicating the thread should return */
        if (num_rx == -1 && errno == EACCES)
            break;

        /* Validate the events */
        for (j = 0; j < num_rx; j++) {
            if (events[j].recv.error) {
                printf("[%s()] FAILED: Bug in received event %d,%d\n", __func__, i, j);
                exit(-1);
            } 
#if PER_PACKET_MEASUREMENT
    // if (events[j].recv.udata16 == UINT16_MAX) {
                // get priority latency data
                // double temp_lat = (double) (recv_lat - events[j].recv.udata64) / (double) freq;
                // recv_lat = __rdtsc();
        recv_lat = __rdtsc();
        priority_latency[args->index][queue_priority[events[j].recv.queue_id]][events[j].recv.priority] += (recv_lat - events[j].recv.udata64);
                // printf("Worker: pakcet consumes %ld cycles in DLB\n", recv_lat - events[j].recv.udata64);
            // }
            // if (!sched_type)
		    //     printf("[Worker %d] event flow id is %d\n", args->index, events[j].recv.flow_id);
            // delay_cycles(200);
#endif
            if (total != 0 && (total + j) % 1000 == 0) {
                recv_lat = __rdtsc();
                lat_arr[latency_idx] = recv_lat - events[j].recv.udata64;
                latency_idx = (latency_idx + 1) % 10000;
            }

            // if (events[j].recv.sched_type == SCHED_UNORDERED) {
            //     if (events[j].recv.error)
            //         printf("[%s()] FAILED: Bug in received event [PARALLEL]", __func__);
            // }
        }

        total += num_rx;
        if (total == 4) {
            local_start_time = __rdtsc();
        }


        if (num_stages > 1) {
            /* Forward events */
            for (j = 0; j < num_rx; j++) {
                events[j].send.queue_id = args->queue_id;
                if (events[j].recv.sched_type == SCHED_UNORDERED)
                    events[j].send.sched_type = SCHED_UNORDERED;
                else {
                    events[j].send.sched_type = SCHED_ATOMIC;
                    events[j].send.flow_id = 0xABCD;
                }
            }

            for (j = 0, num_tx = 0; num_tx < num_rx && j < RETRY_LIMIT; j++) {
                ret = dlb_forward(args->port, num_rx-num_tx, &events[num_tx]);

                if (ret == -1)
                    break;

                num_tx += ret;
            }

            if (num_tx != num_rx) {
                printf("[%s()] Forwarded %ld/%ld events on iteration %d!\n",
                    __func__, num_tx, num_rx, i);
                exit(-1);
            }
        }
        
        worker_pkt_count[args->index] = total;
        // printf("received %d\n", total);

        if (report_latency > 0) {
            test_end = rdtsc();
            uint64_t test_diff = test_end - test_start;
            test_count += num_rx;
            if (test_diff > freq) {
                printf("[Worker %d] Avg thourghput is %lf Mpps\n", args->index, (double)test_count / ((double)test_diff / (double)freq * 1E6));
                test_start = test_end;
                test_count = 0;
            }
        }

        if (num_stages == 1) {
            if (dlb_release(args->port, num_rx) != num_rx) {
                printf("[%s()] FAILED: Release of all %ld events (iter %d)!\n",
                    __func__, num_rx, i);
                exit(-1);
            }
        }
        // if (total % 1000000 == 0)
        //     printf("[%s] Received and forwarded events : %ld\n", __func__, total);
    }



end:
    double worker_latency = (double)(rdtscp() - local_start_time) / (double)freq * 1E6;
    double worker_latency_packet = 0;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            worker_latency_packet += priority_latency[args->index][i][j] / (double)freq * 1E6;
        }
    }
    worker_pkt_count[args->index] = total;
    worker_tput[args->index] = (double)total / worker_latency;
    printf("Total worker %d time is %lf us\n", args->index, worker_latency);
    printf("Worker %d per packet time is %lf us\n", args->index, worker_latency / (double)total);
    printf("Woker %d throughput is %lf Mpps\n", args->index, (double)total / worker_latency);
    printf("Woker %d throughput based on packet is %lf Mpps\n", args->index, (double)total / worker_latency_packet);
    printf("[%s()] Received %ld events\n",
           __func__, total);


    double c2c_latency[10000];
    int valid_size = 0;
    double total_latency = 0;
    for (int i = 0; i < 10000; i++) {
        c2c_latency[i] = (double) lat_arr[i] / (double)freq * 1000000.0;
        total_latency += c2c_latency[i];
        if (c2c_latency[i] > 0) valid_size++;
    }

    qsort(c2c_latency, valid_size, sizeof(double), cmpfunc);
    printf("==== E2E Latency ====\n");
    printf("Valid latency size: %u\n", valid_size);
    printf("Average latency: %.3f us\n", total_latency/valid_size);
    printf("min%% tail latency: %.3f us\n", c2c_latency[(int) 0]);
    printf("25%% tail latency: %.3f us\n", c2c_latency[(int) (0.25 * valid_size)]);
    printf("50%% tail latency: %.3f us\n", c2c_latency[(int) (0.50 * valid_size)]);
    printf("75%% tail latency: %.3f us\n", c2c_latency[(int) (0.75 * valid_size)]);
    printf("90%% tail latency: %.3f us\n", c2c_latency[(int) (0.9 * valid_size)]);
    printf("95%% tail latency: %.3f us\n", c2c_latency[(int) (0.95 * valid_size)]);
    printf("99%% tail latency: %.3f us\n", c2c_latency[(int) (0.99 * valid_size)]);
    printf("max%% tail latency: %.3f us\n", c2c_latency[(int) (valid_size - 1)]);


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
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) <0) {
        perror("pthread_setaffinity_np");
    }

    if (num_stages == 1 && num_events != 0) {
        uint64_t count = 0;
        while (count < num_events) {
            count = 0;
            for (i = 0; i < 8; i++) {
                count += worker_pkt_count[i];
            }
        }
        printf("count is %ld!!!!\n", count);
        goto end;
    }

end:
    worker_done = true;
    return NULL;
}


int main(int argc, char **argv)
{
    pthread_t rx_thread, tx_thread, *worker_threads, *tx_threads, monitor_thread;
    thread_args_t rx_args, tx_args, *worker_args, *tx_argses, monitor_args;
    int ldb_pool_id, dir_pool_id;
    int rx_port_id, tx_port_id;
    dlb_domain_hdl_t domain;
    int num_seq_numbers = 0;
    int worker_queue_id = -1;
    int domain_id, i, j;
    dlb_hdl_t dlb;

    if (parse_args(argc, argv))
        return -1;
        
    unsigned long core_idx = core_mask;
    printf("core_idx is %ld\n", core_idx);
    printf("enqueue_depth is %ld\n", enqueue_depth);
    hz = get_tsc_freq_arch();

    dlb_pp_addr = malloc(sizeof(uint64_t*));
    for (i = 0; i < 8; i++) {
        worker_pkt_count[i] = 0;
        worker_tput[i] = 0;
        for (j = 0; j < 8; j++) {
            packet_priority_count[i][j] = 0;
            for (int k = 0; k < 8; k++) {
                priority_latency[i][j][k] = 0;
            }
        }
    }

    freq = get_tsc_freq_arch();
    printf("TSC Frequency is %ld\n", freq);
    isTSC();

    worker_threads = (pthread_t *)calloc(num_workers, sizeof(pthread_t));
    if (!worker_threads)
        return -ENOMEM;

    worker_args = (thread_args_t *)calloc(num_workers, sizeof(thread_args_t));
    if (!worker_args) {
        free(worker_threads);
        return -ENOMEM;
    }

    tx_threads = (pthread_t *)calloc(num_txs, sizeof(pthread_t));
    if (!tx_threads)
        return -ENOMEM;

    tx_argses = (thread_args_t *)calloc(num_txs, sizeof(thread_args_t));
    if (!tx_argses) {
        free(tx_threads);
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

    domain_id = create_sched_domain(dlb, num_rxs + num_workers, num_txs);
    if (domain_id == -1)
        error(1, errno, "dlb_create_sched_domain");

    domain = dlb_attach_sched_domain(dlb, domain_id, NULL);
    if (domain == NULL)
        error(1, errno, "dlb_attach_sched_domain");

    if (!cap.combined_credits) {
        int max_ldb_credits = rsrcs.num_ldb_credits * partial_resources / 100;
        int max_dir_credits = rsrcs.num_dir_credits * partial_resources / 100;
        if (use_max_credit_ldb == true)
            ldb_pool_id = dlb_create_ldb_credit_pool(domain, max_ldb_credits);
        else if (num_credit_ldb <= max_ldb_credits)
            ldb_pool_id = dlb_create_ldb_credit_pool(domain, num_credit_ldb);
        else
            error(1, EINVAL, "Requested ldb credits are unavailable!");

        if (ldb_pool_id == -1)
            error(1, errno, "dlb_create_ldb_credit_pool");

        if (use_max_credit_dir == true)
            dir_pool_id = dlb_create_dir_credit_pool(domain, max_dir_credits);
        else if (num_credit_dir <= max_dir_credits)
            dir_pool_id = dlb_create_dir_credit_pool(domain, num_credit_dir);
        else
            error(1, EINVAL, "Requested dir credits are unavailable!");

        if (dir_pool_id == -1)
            error(1, errno, "dlb_create_dir_credit_pool");
    } else {
        int max_credits = rsrcs.num_credits * partial_resources / 100;

        if (use_max_credit_combined == true)
            ldb_pool_id = dlb_create_credit_pool(domain, max_credits);
        else
            if (num_credit_combined <= max_credits)
                ldb_pool_id = dlb_create_credit_pool(domain, num_credit_combined);
            else
                error(1, EINVAL, "Requested combined credits are unavailable!");

        if (ldb_pool_id == -1)
            error(1, errno, "dlb_create_credit_pool");
    }


    /* Create TX ports */
    for (int i = 0; i < num_txs; i++) {
        tx_argses[i].queue_id = create_ldb_queue(domain, num_seq_numbers);
        if (tx_argses[i].queue_id == -1)
            error(1, errno, "dlb_create_ldb_queue");
        tx_port_id = create_dir_port(domain, ldb_pool_id, dir_pool_id, -1);
        if (tx_port_id == -1)
            error(1, errno, "dlb_create_dir_port");

        // uint64_t** dlb_pp_addr = (uint64_t**)malloc(sizeof(uint64_t*));
        tx_argses[i].port = dlb_attach_dir_port(domain, tx_port_id, NULL);
        // printf("[%s()] RDMA sees DLB dlb_pp_addr: %p\n", __func__, *(dlb_pp_addr));
        // free(dlb_pp_addr);
        if (tx_argses[i].port == NULL)
            error(1, errno, "dlb_attach_dir_port");

        tx_argses[i].core_mask = core_idx;
        core_idx += 1;

        if (queue_prio_en) {
            queue_priority[tx_argses[i].queue_id] = i;
        }  else {
            queue_priority[tx_argses[i].queue_id] = 0;
        }
        tx_argses[i].index = i;
        printf("tx %d, queue_id = %d, prio_idx = %d\n", i, tx_argses[i].queue_id, (queue_prio_en)? i : 0);
        // if (dlb_enable_cq_weight(tx_argses[i].port) == 0) {
        //     printf("set weight success!!! \n");
        // } else {
        //     perror("cannot set weight!!! \n");
        // }
    }

    if (num_stages > 1) {
        /* Create RX ports */
        rx_port_id = create_ldb_port(domain, ldb_pool_id, dir_pool_id);
        if (rx_port_id == -1)
            error(1, errno, "dlb_create_ldb_port");

        rx_args.port = dlb_attach_ldb_port(domain, rx_port_id, dlb_pp_addr);
        if (rx_args.port == NULL)
            error(1, errno, "dlb_attach_ldb_port");

        if (dlb_pp_addr != NULL)
            printf("[%s()] RDMA sees DLB pp_addr: %p\n", __func__, *dlb_pp_addr);

        rx_args.core_mask = core_idx;
        core_idx += 1;

        /* Create worker queue */
        if (num_workers) {
            worker_queue_id = create_ldb_queue(domain, 0);
            if (worker_queue_id == -1)
                error(1, errno, "dlb_create_ldb_queue");
            printf("worker queue_id = %d\n", worker_queue_id);
        }
    }

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

        if (queue_prio_en) {
            // enable queue priority
            for (j = 0; j < num_txs; j++) {
                if (dlb_link_queue(worker_args[i].port, tx_argses[j].queue_id, queue_priority[tx_argses[j].queue_id]) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        } else {
            // disable queue priority
            for (j = 0; j < num_txs; j++) {
                if (dlb_link_queue(worker_args[i].port, tx_argses[j].queue_id, 0) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        }
            
        worker_args[i].core_mask = core_idx;
        core_idx += 1;
        worker_args[i].index = i;
    }

    if (num_stages > 1) {
        /* Link the worker queue if there any workers, else link the tx queue. */
        rx_args.queue_id = (num_workers > 0) ? worker_queue_id : tx_argses[0].queue_id;
        if (num_workers > 0 || num_txs == 1) {
            if (dlb_link_queue(rx_args.port, rx_args.queue_id, 0) == -1)
                error(1, errno, "dlb_link_queue");
        } else {
            for (i = 0; i < num_txs; i++) {
                if (dlb_link_queue(rx_args.port, tx_argses[i].queue_id, 0) == -1)
                    error(1, errno, "dlb_link_queue");
            }
        }
    }

    // sleep(600);
    

    if (dlb_launch_domain_alert_thread(domain, NULL, NULL))
        error(1, errno, "dlb_launch_domain_alert_thread");

    if (dlb_start_sched_domain(domain))
        error(1, errno, "dlb_start_sched_domain");

    /* Launch threads */
    for (i = 0; i < num_workers; i++)
        pthread_create(&worker_threads[i], NULL, worker_fn, &worker_args[i]);

    if (num_stages > 1)
        pthread_create(&rx_thread, NULL, rx_traffic, &rx_args);

    monitor_args.core_mask = core_idx;
    core_idx += 1;
    pthread_create(&monitor_thread, NULL, monitor_traffic, &monitor_args);
    /* Add sleep here to make sure the rx_thread is staretd before tx_thread */
    // usleep(1000);

    // pthread_create(&tx_thread, NULL, tx_traffic, &tx_args);
    for (i = 0; i < num_txs; i++) {
        // pthread_create(&tx_threads[i], NULL, tx_traffic, &tx_argses[i]);
        pthread_create(&tx_threads[i], NULL, tx_traffic_rate_limiting, &tx_argses[i]);
    }

    /* Wait for threads to complete */
    for (i = 0; i < num_txs; i++) {
        pthread_join(tx_threads[i], NULL);
    }

    if (num_stages > 1)
        pthread_join(rx_thread, NULL);

    pthread_join(monitor_thread, NULL);
    /* The worker threads may be blocked on the CQ interrupt wait queue, so
     * disable their ports in order to wake them before joining the thread.
     */
    for (i = 0; i < num_workers; i++) {
        if (dlb_disable_port(worker_args[i].port))
            error(1, errno, "dlb_disable_port");
        pthread_join(worker_threads[i], NULL);
    }

    /* Clean up */
    for (i = 0; i < num_workers; i++) {
        if (dlb_detach_port(worker_args[i].port) == -1)
            error(1, errno, "dlb_detach_port");
    }

    if (num_stages > 1 && dlb_detach_port(rx_args.port) == -1)
        error(1, errno, "dlb_detach_port");

    for (i = 0; i < num_txs; i++) {
        if (dlb_detach_port(tx_argses[i].port) == -1)
            error(1, errno, "dlb_detach_port");
    }

    if (dlb_detach_sched_domain(domain) == -1)
        error(1, errno, "dlb_detach_sched_domain");

    if (dlb_reset_sched_domain(dlb, domain_id) == -1)
        error(1, errno, "dlb_reset_sched_domain");

    if (dlb_close(dlb) == -1)
        error(1, errno, "dlb_close");

    /* Get performance data */
    double total_tput = 0;
    for (i = 0; i < num_workers; i++) {
        total_tput += worker_tput[i];
    }

#if PER_PACKET_MEASUREMENT
    double packet_prio_lat[8];
    uint64_t packet_prio_count[8];
    double queue_prio_lat[8];
    double worker_pkt_lat[8];
    double total_latency = 0;
    uint64_t total_recv_counts = 0;
    for (i = 0; i < 8; i++) {
        packet_prio_lat[i] = 0;
        packet_prio_count[i] = 0;
        queue_prio_lat[i] = 0;
        worker_pkt_lat[i] = 0;
    }
    for (i = 0; i < num_workers; i++) {
        // total_tput += worker_tput[i];
        for (j = 0; j < 8; j++) {
            for (int k = 0; k < 8; k++) {
                queue_prio_lat[j] += priority_latency[i][j][k] / (double)freq * 1E6;
                packet_prio_lat[k] += priority_latency[i][j][k] / (double)freq * 1E6;
                worker_pkt_lat[i] += priority_latency[i][j][k] / (double)freq * 1E6;
                if (priority_latency[i][j][k] != 0) {
                    printf("Worker %d, Queue priority %d, packet priority %d has %f us\n", i, j, k, (double)priority_latency[i][j][k] / (double)freq * 1E6);
                }
            }
        }
    }
    for (i = 0; i < num_txs; i++) {
        for (j = 0; j < 8; j++) {
            packet_prio_count[j] += packet_priority_count[i][j];
        }
    }

    for (i = 0; i < 8; i++) {
        printf("Queue priority %d has %f us latency\n", i, queue_prio_lat[i] / ((double)num_events / (double)num_txs));
        total_latency += queue_prio_lat[i];
    }
    for (i = 0; i < 8; i++) {
        printf("Packet priority %d has %ld counts, %f us latency\n", i, packet_prio_count[i], packet_prio_lat[i] / (double)packet_prio_count[i]);
    }
    for (i = 0; i < 8; i++) {
        printf("Worker %d has %ld counts, %f us latency\n", i, worker_pkt_count[i], worker_pkt_lat[i] / (double)worker_pkt_count[i]);
        total_recv_counts += worker_pkt_count[i];
    }
    printf("Average worker latency is %lf us\n", total_latency / (double)total_recv_counts);
#endif
    printf("Total worker throughput is %lf Mpps\n", total_tput);

    free(worker_threads);
    free(worker_args);

    free(tx_threads);
    free(tx_argses);

    if (dlb_pp_addr != NULL) free(dlb_pp_addr);

    return 0;
}
