/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2018 Intel Corporation
 */

#include "dlb_microbench_common.h"


static void *loopback_traffic(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[enqueue_depth];
    int64_t num_loops, i, num_tx = 0, num_rx = 0, num_mismatch = 0;
    int ret, nfds = 0, epoll_fd;
    struct epoll_event epoll_events;
    int epoll_retry = EPOLL_RETRY;

    uint64_t test_start, test_end, test_count = 0;
    uint64_t tx_start_time, tx_end_time;
    uint64_t rx_start_time, rx_end_time;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0) {
        perror("pthread_setaffinity_np");
    }
    printf("core_mask is %d\n", args->core_mask);

    /* Seed random number generator */
    srand(time(NULL));

    num_loops = (num_events == 0) ? -1 : num_events / enqueue_depth / num_txs;

    enqueue_depth = 1;
    dequeue_depth = 1;

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

    
    /* TX starts */
    tx_start_time = __rdtsc();
    test_start = tx_start_time;
    int tx_prio = 0;
    for (i = 0; (num_tx < num_events / num_txs) || (num_loops == -1); i++) {
        int j, num;

        /* Initialize the dynamic fields in the send events */
        if (packet_prio_en) {
            tx_prio = rand() % 8;   // enable packet priority
        } else {
            tx_prio = 0;            // disable packet priority
        }
        uint64_t temp_time = __rdtsc();
        for (j = 0; j < enqueue_depth; j++) {
            events[j].adv_send.priority = tx_prio;
            events[j].adv_send.udata64 = temp_time;
            // events[j].adv_send.udata64 = num_tx + j;
            events[j].adv_send.udata16 = (num_tx + j) % UINT16_MAX;
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

        /* Receive the events */
        for (j = 0; (num != enqueue_depth && j < RETRY_LIMIT); j++)
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



    }
    tx_end_time = __rdtsc();
    double tx_latency = (double)(tx_end_time - tx_start_time) / (double)freq / (double)num_tx * 1E6;
    printf("[Tx %d] per event tx latency is %lf us\n", args->index, tx_latency);
    printf("[Tx %d] throughput is %lf Mpps\n", args->index, 1 / tx_latency);
    printf("[%s()] Sent %ld events\n",
           __func__, num_tx);

    if (epoll_enabled)
        epoll_fd = setup_epoll(args);
end:
    // rx_end_time = rdtscp();
    // double rx_latency = (double)(rx_end_time - rx_start_time) / (double)freq / (double)num_rx * 1E6;

    // printf("per event rx latency is %lf us\n", rx_latency);
    // printf("[RX] throughput is %lf Mpps\n", 1.0/rx_latency);
    // printf("[%s()] Received %ld events, num_mismatch: %ld\n",
    //        __func__, num_rx, num_mismatch);
    worker_done = true;
    printf("rx_thread done\n");
    if (epoll_enabled) {
        close(epoll_fd);
				close(args->efd);
    }

    return NULL;
}






static void *loopback_traffic_old(void *__args)
{
    thread_args_t *args = (thread_args_t *) __args;
    dlb_event_t events[enqueue_depth];
    int64_t num_loops, i, num_tx = 0, num_rx = 0, num_mismatch = 0;
    int ret, nfds = 0, epoll_fd;
    struct epoll_event epoll_events;
    int epoll_retry = EPOLL_RETRY;

    uint64_t test_start, test_end, test_count = 0;
    uint64_t tx_start_time, tx_end_time;
    uint64_t rx_start_time, rx_end_time;

    /* bind process to processor */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_mask, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0) {
        perror("pthread_setaffinity_np");
    }
    printf("core_mask is %d\n", args->core_mask);

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

    
    /* TX starts */
    tx_start_time = __rdtsc();
    test_start = tx_start_time;
    int tx_prio = 0;
    for (i = 0; (num_tx < num_events / num_txs) || (num_loops == -1); i++) {
        int j, num;

        /* Initialize the dynamic fields in the send events */
        if (packet_prio_en) {
            tx_prio = rand() % 8;   // enable packet priority
        } else {
            tx_prio = 0;            // disable packet priority
        }
        uint64_t temp_time = __rdtsc();
        for (j = 0; j < enqueue_depth; j++) {
            events[j].adv_send.priority = tx_prio;
            events[j].adv_send.udata64 = temp_time;
            // events[j].adv_send.udata64 = num_tx + j;
            events[j].adv_send.udata16 = (num_tx + j) % UINT16_MAX;
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

    }
    tx_end_time = __rdtsc();
    double tx_latency = (double)(tx_end_time - tx_start_time) / (double)freq / (double)num_tx * 1E6;
    printf("[Tx %d] per event tx latency is %lf us\n", args->index, tx_latency);
    printf("[Tx %d] throughput is %lf Mpps\n", args->index, 1 / tx_latency);
    printf("[%s()] Sent %ld events\n",
           __func__, num_tx);


    
    /* RX starts */
    if (epoll_enabled)
        epoll_fd = setup_epoll(args);
    num_loops = (num_events == 0) ? -1 : num_events / dequeue_depth;
    int last_verified_udata64 = 0;

    rx_start_time = __rdtsc();
    test_start = rx_start_time;
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

        if (dlb_release(args->port, num) != num) {
            printf("[%s()] FAILED: Release of all %d events (iter %ld)!\n",
                __func__, num, i);
            exit(-1);
        }
    }

end:
    rx_end_time = rdtscp();
    double rx_latency = (double)(rx_end_time - rx_start_time) / (double)freq / (double)num_rx * 1E6;

    printf("per event rx latency is %lf us\n", rx_latency);
    printf("[RX] throughput is %lf Mpps\n", 1.0/rx_latency);
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
    pthread_t loopback_thread, monitor_thread;
    thread_args_t loopback_args, monitor_args;
    int ldb_pool_id, dir_pool_id;
    int rx_port_id, tx_port_id;
    dlb_domain_hdl_t domain;
    int num_seq_numbers = 0;
    int worker_queue_id = -1;
    int domain_id, i, j;
    dlb_hdl_t dlb;

    unsigned int core_idx;

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

    if (parse_args(argc, argv))
        return -1;

    freq = get_tsc_freq_arch();
    printf("TSC Frequency is %ld\n", freq);
    isTSC();

    core_idx = core_mask;

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

    // Create port
    int port_id = -1;
    loopback_args.queue_id = create_ldb_queue(domain, num_seq_numbers);
    port_id = create_ldb_port(domain, ldb_pool_id, dir_pool_id);
    if (port_id == -1)
        error(1, errno, "dlb_create_ldb_port");
    loopback_args.port = dlb_attach_ldb_port(domain, port_id, NULL);
    if (loopback_args.port == NULL)
        error(1, errno, "dlb_attach_ldb_port");
    loopback_args.core_mask = core_idx;
    core_idx += 1;
    if (dlb_link_queue(loopback_args.port, loopback_args.queue_id, 0) == -1)
        error(1, errno, "dlb_link_queue");




    if (dlb_launch_domain_alert_thread(domain, NULL, NULL))
        error(1, errno, "dlb_launch_domain_alert_thread");

    if (dlb_start_sched_domain(domain))
        error(1, errno, "dlb_start_sched_domain");


    monitor_args.core_mask = core_idx;
    core_idx += 1;
    // pthread_create(&monitor_thread, NULL, monitor_traffic, &monitor_args);
    /* Add sleep here to make sure the rx_thread is staretd before tx_thread */
    // usleep(1000);

    pthread_create(&loopback_thread, NULL, loopback_traffic, &loopback_args);
    pthread_join(loopback_thread, NULL);

    // pthread_join(monitor_thread, NULL);


    /* Clean up */
    if (dlb_detach_port(loopback_args.port) == -1)
        error(1, errno, "dlb_detach_port");

    if (dlb_detach_sched_domain(domain) == -1)
        error(1, errno, "dlb_detach_sched_domain");

    if (dlb_reset_sched_domain(dlb, domain_id) == -1)
        error(1, errno, "dlb_reset_sched_domain");

    if (dlb_close(dlb) == -1)
        error(1, errno, "dlb_close");

    if (dlb_pp_addr != NULL) free(dlb_pp_addr);

    return 0;
}
