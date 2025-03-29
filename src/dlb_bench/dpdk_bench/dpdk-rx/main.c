/*
    Running Traffic Receiver with applications
*/

#include "main.h"

// RX packet processing main loop function
static int lcore_rx_worker(void *arg)
{
    unsigned int lcore_id = rte_lcore_id();
    unsigned int lcore_index = rte_lcore_index(lcore_id);
    unsigned int rx_index = lcore_index;

    if (call_main != 1 && lcore_id > rte_get_main_lcore()) {
        rx_index -= 1;
    }

    // void *state;
    // switch(app_id) {
    //     case 1:
    //         state = (struct _matchinfo *)malloc(sizeof(struct _matchinfo) * TOTAL_MATCH);
    //         bm25_init(state, app_arg1);
    //         break;
    //     case 2:
    //         state = malloc(sizeof(struct _bayes_state));
    //         bayes_init(state, app_arg1);
    //         break;
    //     case 3:
    //         state = malloc(sizeof(struct _knn_node) * app_arg1);
    //         knn_init(state, app_arg1);
    //         break;
    //     default:
    //         break;
    // }


    printf("lcore %2u (lcore index %2u, RX index %2u) starts to receive packets\n",
           lcore_id, lcore_index, rx_index);

    int dev_socket_id = rte_eth_dev_socket_id(port_id);
    if (dev_socket_id != (int)rte_socket_id()) {
        fprintf(stderr, "WARNING, port %u is on remote NUMA node to lcore %u\n", port_id, lcore_id);
    }

    struct rte_mbuf *bufs[BURST_SIZE];

    // MODIFY HERE: TEST LATENCY PERIOD
    uint64_t interval_cycles = (uint64_t) (interval * hz / 1000.0);
    uint64_t prev_tsc, cur_tsc, diff_tsc;
    prev_tsc = rte_get_timer_cycles();

    uint16_t nb_tx = 0;
    // test pps
    uint16_t pps_index = 0;
    uint64_t pps_prev_tsc, pps_cur_tsc, pps_diff_tsc;
    pps_prev_tsc = rte_get_timer_cycles();
    uint64_t last_lcore_rx_pkts = 0;

    size_t offset = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
    uint64_t dummy_process_delay = 0;  // unit of nanoseconds

    while (likely(keep_receiving)) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, rx_index, bufs, BURST_SIZE);

        if (unlikely(nb_rx == 0)) {
            continue;
        }
        lcore_rx_pkts[rx_index] += nb_rx;

        for (int i = 0; i < nb_rx; i++) {
            cur_tsc = rte_get_timer_cycles();
            // =========== Perform dummy process
            dummy_process_delay = *(uint64_t *)(rte_pktmbuf_mtod_offset(bufs[i], char *, offset));
            // rte_delay_us_sleep(1);
            // printf("dummy process delay is %lu\n", dummy_process_delay);
            if (dummy_process_delay > 0) {
                delay_cycles(dummy_process_delay);
                // printf("dummy process delay is %lu\n", dummy_process_delay);
            } else {
                dummy_process_delay = dummy_delay;
                if (dummy_process_delay > 0)
                    delay_cycles(dummy_process_delay);
            }
            // ================================

            /***** application start *****/ 
            // char *payload;
            // payload = rte_pktmbuf_mtod_offset(bufs[i], char *, offset);
            // switch(app_id) {
            //     case 0:
            //         delay_nanoseconds(dummy_delay);
            //         break;
            //     case 1:
            //         bm25_exec(state);
            //         break;
            //     case 2:
            //         bayes_exec(state, app_arg1);
            //         break;
            //     case 3:
            //         knn_exec(state, app_arg1, app_arg2);
            //         break;
            //     default:
            //         break;
            // }
            /***** application end *****/ 
        
            if (latency_size != 0) {
                // latency test, send back packets
                diff_tsc = cur_tsc - prev_tsc;
                nb_tx = 0;
                if (diff_tsc > interval_cycles) {
                    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
                    // assert(rte_is_same_ether_addr(&eth_hdr->d_addr, &my_ether_addr));
                    rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
                    rte_ether_addr_copy(&my_ether_addr, &eth_hdr->src_addr);
                    nb_tx = rte_eth_tx_burst(port_id, rx_index, &bufs[i], 1);
                    lcore_tx_pkts[rx_index] += nb_tx;
                    prev_tsc = cur_tsc;
                } else {
                    rte_pktmbuf_free(bufs[i]);
                }
                
                // if (likely(nb_tx < nb_rx)) {
                //     uint16_t buf;
                //     for (buf = nb_tx; buf < nb_rx; buf++)
                //         rte_pktmbuf_free(bufs[buf]);
                // }
            } else {
                rte_pktmbuf_free(bufs[i]);
            }
        }

        if (test_pps == 1 && call_main == 1) {
            // will require call_main == 1
            // used to capture last MAX_TIME pps stats
            pps_cur_tsc = rte_get_timer_cycles();
            pps_diff_tsc = pps_cur_tsc - pps_prev_tsc;
            if (pps_diff_tsc > hz) {
                if (pps_index >= MAX_TIME) pps_index = 0;
                uint64_t total_rx_pkts_per_sec = (lcore_rx_pkts[lcore_index] - last_lcore_rx_pkts);
                last_lcore_rx_pkts = lcore_rx_pkts[lcore_index];
                pps_arr[lcore_index].data[pps_index] = total_rx_pkts_per_sec / 1000000.0 / pps_diff_tsc * hz;
                pps_prev_tsc = pps_cur_tsc;
                pps_index++;
                if (lcore_index == 0) {
                    pps_size = pps_index;
                }
            }
        }

    }

    // switch(app_id) {
    //     case 1:
    //         bm25_free(state);
    //         break;
    //     case 2:
    //         bayes_free(state, app_arg1);
    //         break;
    //     case 3:
    //         knn_free(state);
    //         break;
    //     default:
    //         break;
    // }

    return 0;
}


int main(int argc, char **argv)
{
    // Initialize Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    }

    printf("%d EAL arguments used\n", ret);

    argc -= ret;
    argv += ret;

    // Parse application arguments
    ret = parse_args(argc, argv);
    printf("%d application arguments used\n", ret);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Invalid application arguments\n");
    }

    print_config();

    // Init RTE timer library
    rte_timer_subsystem_init();

    // Get the number of RX lcores
    unsigned int rx_lcore_count = rte_lcore_count();
    printf("RX lcore count: %u\n", rx_lcore_count);
    if (call_main == 0) {   // determine whther main core will be used as a worker
        rx_lcore_count-=1;
    }
    if (rx_lcore_count > MAX_RX_CORES) {
        rte_exit(EXIT_FAILURE, "We only support up to %u lcores\n", MAX_RX_CORES);
    }

    // Initialize all lcore_params_array
    for (size_t i = 0; i < rx_lcore_count; i++) {
        last_lcore_rx_pkts[i] = 0;
        lcore_rx_pkts[i] = 0;
    }

    // Create a memory pool
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                                                            NUM_MBUFS * rx_lcore_count,
		                                                    MBUF_CACHE_SIZE,
                                                            0,
                                                            RTE_MBUF_DEFAULT_BUF_SIZE,
                                                            rte_socket_id());
    if (!mbuf_pool) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }

    // Initialize port
    if (port_init(port_id, mbuf_pool, rx_lcore_count, rx_lcore_count, TX_DESC_DEFAULT, RX_DESC_DEFAULT) != 0) {
        rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", port_id);
    }

    // Ctrl+C handler
    keep_receiving = 1;
    signal(SIGINT, stop_rx);

    hz = rte_get_timer_hz();
    uint64_t interval_cycles = interval * hz;
    uint64_t start_tsc, end_tsc, diff_tsc;
    uint64_t tot_start_tsc;
    uint64_t total_rx_pkts = 0;
    uint64_t total_tx_pkts = 0;
    start_tsc = rte_get_timer_cycles();
    tot_start_tsc = rte_get_timer_cycles();
    double pkt_rate = 0;


    if (call_main == 1) {
        rte_eal_mp_remote_launch(lcore_rx_worker, NULL, CALL_MAIN);
    } else {
        rte_eal_mp_remote_launch(lcore_rx_worker, NULL, SKIP_MAIN);

        // use the main core to print stats every 1 second
        while (likely(keep_receiving)) {
            end_tsc = rte_get_timer_cycles();
            diff_tsc = end_tsc - start_tsc;
            if (diff_tsc < interval_cycles) {
                continue;
            }

            start_tsc = end_tsc;
            total_rx_pkts = 0;
            for (size_t i = 0; i < rx_lcore_count; i++) {
                uint64_t tmp = lcore_rx_pkts[i];
                total_rx_pkts += (tmp - last_lcore_rx_pkts[i]);
                last_lcore_rx_pkts[i] = tmp;
            }

            // Print packet rate (millions packets per second)
            pkt_rate = total_rx_pkts / 1000000.0 / diff_tsc * interval_cycles;
            printf("Total RX packet per second: %0.4f M\n", pkt_rate);
        }
    }


    // Finishing program and print statistics
    total_rx_pkts = 0;
    for (size_t i = 0; i < rx_lcore_count; i++) {
        total_rx_pkts += lcore_rx_pkts[i];
    }
    end_tsc = rte_get_timer_cycles();
    diff_tsc = end_tsc - tot_start_tsc;
    pkt_rate = (double) total_rx_pkts / diff_tsc * hz / 1000000.0;
    printf("Average RX packet per second: %0.4f M ", pkt_rate);

    printf("Receive %" PRId64 " packets in total\n", total_rx_pkts);
    for (size_t i = 0; i < rx_lcore_count; i++) {
        printf("RX ring %2lu receives %" PRId64 " packets (%0.3f%%)\n",
               i, lcore_rx_pkts[i], total_rx_pkts ? lcore_rx_pkts[i] * 100.0 / total_rx_pkts : 0.0);
    }

    // Print pps stats every second for the latest MAX_TIME data, it is only enabled when call_main == 1
    if (test_pps == 1 && call_main == 1) {
        printf("==== test pps is %d ====\n", test_pps);
        for(uint16_t i = 0; i < pps_size; i++) {
            double pps = 0;
            for (uint16_t j = 0; j < rx_lcore_count; j++) {
                pps += pps_arr[j].data[i];
            }
            printf("Receiving packets at %u s is %f Mpps\n", i, pps);
        }
    } 

    // Stop and cleanup
    ret = rte_eth_dev_stop(port_id);
    if (ret != 0) {
        printf("rte_eth_dev_stop: err=%d, port=%d\n", ret, port_id);
    }
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();

    return EXIT_SUCCESS;
}