#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdio.h>
#include <stdlib.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_timer.h>
#include <rte_mbuf.h>
#include <getopt.h>
#include <sys/shm.h>
#include <sys/ipc.h>

// Configurable number of RX/TX ring descriptors
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024

#define NUM_MBUFS 1024
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 8

#define MAX_RX_CORES 128
#define MAX_TIME 1000

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_RSS,
	    .max_lro_pkt_size = RTE_ETHER_MAX_LEN,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP,
        }
    },
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE
    }
};

struct my_pps { 
    double data[MAX_TIME]; 
}; 
static uint16_t test_pps = 0;
static uint16_t call_main = 1;
volatile struct my_pps pps_arr[MAX_RX_CORES];
static int pps_size = 0;

uint64_t hz;

static struct rte_ether_addr my_ether_addr;
static uint16_t port_id = 0;
static unsigned int interval = 1;
static uint32_t pkts_per_sec = 0;
static uint32_t latency_size = 0;
static unsigned long dummy_delay = 0;

// used for applications
static int app_id = 0;
static int app_arg1;
static int app_arg2;

static uint64_t last_lcore_rx_pkts[MAX_RX_CORES];
static volatile uint64_t lcore_rx_pkts[MAX_RX_CORES];
static uint64_t last_lcore_tx_pkts[MAX_RX_CORES];
static volatile uint64_t lcore_tx_pkts[MAX_RX_CORES];

// Initialzie a port with the number of tx/rx rings and the ring size
static int port_init(uint16_t port, struct rte_mempool *mbuf_pool,
                    uint16_t nb_tx_rings, uint16_t nb_rx_rings,
                    uint16_t tx_desc_size, uint16_t rx_desc_size);

// RX processing main loop function
static int lcore_rx_worker(void *arg);

// Keep receiving packet until set to 0
static volatile int keep_receiving = 1;
static void stop_rx(int sig) { keep_receiving = 0; }

// Print functions
void print_mac(uint16_t port, struct rte_ether_addr mac_addr) {
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port, mac_addr.addr_bytes[0], mac_addr.addr_bytes[1], mac_addr.addr_bytes[2],
            mac_addr.addr_bytes[3], mac_addr.addr_bytes[4], mac_addr.addr_bytes[5]);
}

void print_ip(rte_be32_t ip) {
    struct in_addr addr = {.s_addr = ip};
    printf("%s\n", inet_ntoa(addr));
}

static void print_config() {
    printf("================ Configuration ================\n");
    printf("Port:           %u\n", port_id);
    printf("Interval:       %u sec\n", interval);
    printf("===============================================\n");
}

// Initialzie a port with the number of tx/rx rings and the ring size
static int port_init(uint16_t port, struct rte_mempool *mbuf_pool,
                    uint16_t nb_tx_rings, uint16_t nb_rx_rings,
                    uint16_t tx_desc_size, uint16_t rx_desc_size)
{                                                
    int retval;
    struct rte_eth_conf local_port_conf = port_conf_default;
    uint16_t nb_txd = tx_desc_size;
    uint16_t nb_rxd = rx_desc_size;
    struct rte_eth_dev_info dev_info;

    printf("Initalizing port %hu ...\n", port);

    if (!rte_eth_dev_is_valid_port(port)) {
        return -1;
    }

    int socket_id = rte_eth_dev_socket_id(port);

    // Get device information
    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        fprintf(stderr, "rte_eth_dev_info_get failed  to get port %u info: %s\n", port, strerror(-retval));
        return retval;
    }

    // if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE) {
	// 	local_port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
    // }

    // Configure RSS
    local_port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
    if (local_port_conf.rx_adv_conf.rss_conf.rss_hf != port_conf_default.rx_adv_conf.rss_conf.rss_hf) {
        printf("Port %u modified RSS hash function "
                "based on hardware support,"
                "requested:%#"PRIx64" configured:%#"PRIx64"\n",
                port, 
                port_conf_default.rx_adv_conf.rss_conf.rss_hf,
                local_port_conf.rx_adv_conf.rss_conf.rss_hf);
    }

    // Configure the Ethernet device
    retval = rte_eth_dev_configure(port, nb_rx_rings, nb_tx_rings, &local_port_conf);
    if (retval != 0) {
        fprintf(stderr, "rte_eth_dev_configure\n");
        return retval;
    }

    // Adjust number of descriptors
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0) {
        fprintf(stderr, "rte_eth_dev_adjust_nb_rx_tx_desc failed\n");
        return retval;
    }

    // Allocate and set up nb_tx_rings TX queue
    for (uint16_t q = 0; q < nb_tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd, socket_id, NULL);
        if (retval < 0) {
            fprintf(stderr, "rte_eth_tx_queue_setup failed for queue %hu\n", q);
            return retval;
        }
    }
    printf("Set up %hu TX rings (%hu descriptors per ring)\n", nb_tx_rings, nb_txd);

    // Allocate and set up nb_rx_rings RX queue
    for (uint16_t q = 0; q < nb_rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd, socket_id, NULL, mbuf_pool);
        if (retval < 0) {
            fprintf(stderr, "rte_eth_rx_queue_setup failed for queue %hu\n", q);
            return retval;
        }
    }
    printf("Set up %hu RX rings (%hu descriptors per ring)\n", nb_rx_rings, nb_rxd);

    // Start the Ethernet port.
    retval = rte_eth_dev_start(port);
    if (retval < 0) {
        fprintf(stderr, "rte_eth_dev_start failed\n");
        return retval;
    }

    // Display the port MAC address
    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0) {
        fprintf(stderr, "rte_eth_macaddr_get failed\n");
        return retval;
    }
    print_mac(port, addr);

    my_ether_addr = addr;

#if 0
    // Enable RX in promiscuous mode for the Ethernet device.
    retval = rte_eth_promiscuous_enable(port);
    // End of setting RX port in promiscuous mode.
    if (retval != 0) {
        fprintf(stderr, "rte_eth_promiscuous_enable failed\n");
        return retval;
    }
#endif

    return 0;
}

// dummy timer
static inline void delay_nanoseconds(uint64_t nanoseconds) {
    uint64_t start_time, end_time;
    uint64_t elapsed_nanoseconds;
    // uint64_t hz = rte_get_timer_hz();
    uint64_t cycles = (hz * nanoseconds) / 1E9;
    // printf("hz: %lu, nanoseconds: %lu\n", hz, nanoseconds);

    start_time = rte_get_timer_cycles();
    do {
        end_time = rte_get_timer_cycles();
        elapsed_nanoseconds = end_time - start_time;
    } while (elapsed_nanoseconds < cycles);
}

static inline void delay_cycles(uint64_t cycles) {
    uint64_t start_time, end_time;
    uint64_t elapsed_cycles;

    start_time = rte_get_timer_cycles();
    do {
        end_time = rte_get_timer_cycles();
        elapsed_cycles = end_time - start_time;
    } while (elapsed_cycles < cycles);
}

// Parse arguments
static int parse_port(char *arg) {
    long n;
    char **endptr;
    n = (uint16_t)strtol(arg, endptr, 10);
    if (n < 0 || n >= rte_eth_dev_count_avail()) {
        fprintf(stderr, "Invalid port\n");
        return -1;
    }
    port_id = (uint16_t)n;
    return 0;
}

static int parse_interval(char *arg) {
    unsigned int n;
    char **endptr;
    n = (uint16_t)strtoul(arg, endptr, 10);
    if (n == 0) {
        fprintf(stderr, "Invalid interval, should be positve\n");
        return -1;
    }
    interval = n;
    return 0;
}

static int parse_latency(char *arg) {
    uint32_t n;
    char **endptr;
    n = (uint32_t)strtoul(arg, endptr, 10);
    if (n <= 0) {
        fprintf(stderr, "Invalid latency size, should be positve\n");
        return -1;
    }
    latency_size = n;
    return 0;
}

static int parse_pkt_rate(char *arg) {
    uint32_t n;
    char **endptr;
    n = (uint32_t)strtoul(arg, endptr, 10);
    if (n == 0) {
        fprintf(stderr, "Invalid packet rate, should be positve\n");
        return -1;
    }
    pkts_per_sec = n;
    return 0;
}

static int parse_test_pps(char *arg)
{
    uint32_t n;
    char **endptr;
    n = (uint32_t)strtoul(arg, endptr, 10);
    test_pps = n;
    return 0;
}

static int parse_call_main(char *arg)
{
    uint32_t n;
    char **endptr;
    n = (uint32_t)strtoul(arg, endptr, 10);
    call_main = n;
    return 0;
}

static int parse_delay(char *arg)
{
    uint32_t n;
    char **endptr;
    n = (unsigned long)strtoul(arg, endptr, 10);
    if (n < 0) {
        fprintf(stderr, "Invalid delay, cannot be negative\n");
        return -1;    }
    dummy_delay = n;
    return 0;
}

static int parse_app_id(char *arg)
{
    uint32_t n;
    char **endptr;
    n = (uint32_t)strtoul(arg, endptr, 10);
    if (n < 0) {
        fprintf(stderr, "Invalid application id\n");
        return -1;
    }
    app_id = n;
    return 0;
}

static int parse_app_arg1(char *arg)
{
    char **endptr;
    app_arg1 = (uint32_t)strtoul(arg, endptr, 10);
    return 0;
}

static int parse_app_arg2(char *arg)
{
    char **endptr;
    app_arg2 = (uint32_t)strtoul(arg, endptr, 10);
    return 0;
}

static void print_usage(const char *prgname)
{
    printf("%s [EAL options] -- --options\n"
           "    -p, --port                  port to receive packets (default %hu)\n"
           "    -i, --interval              seconds between periodic reports, only appliable when call_main is disabled (default %u)\n"
           "    -l, --latency               test latency, it will store an array of latency stats (default array size is %u)\n"
           "    -r, --packet-rate           maximum number of packets to receive per second (no rate limiting by default)\n"
           "    -t, --test-pps              whether record pps, enable=1, disable=0 (default disable)\n"
           "    -m, --call-main             whether call main thread, enable=1, disable=0 (default enable)\n"
           "    -d, --delay                 add dummy delay after touching the payload (default %lu nanoseconds)\n"
           "    -a, --app-id                application id, enable=1, disable=0 (default 0, add dummy delay)\n"
           "    -b, --app-arg1              first application argument\n"
           "    -c, --app-arg2              second application argument\n"
           "    -h, --help                  print usage of the program\n",
           prgname, port_id, interval, latency_size, dummy_delay);
}

static struct option long_options[] = {
    {"port",        required_argument,  0,  'p'},
    {"interval",    required_argument,  0,  'i'},
    {"latency",     required_argument,  0,  'l'},
    {"rate",        required_argument,  0,  'r'},
    {"test-pps",    required_argument,  0,  't'},
    {"call-main",   required_argument,  0,  'm'},
    {"delay",       required_argument,  0,  'd'},
    {"app-id",      required_argument,  0,  'a'},
    {"app-arg1",    required_argument,  0,  'b'},
    {"app-arg2",    required_argument,  0,  'c'},
    {"help",        no_argument,        0,  'h'},
    {0,             0,                  0,  0 }
};

static int parse_args(int argc, char **argv)
{
    char *prgname = argv[0];
    const char short_options[] = "p:i:l:r:t:m:d:a:b:c:h";
    int c;
    int ret;

    while ((c = getopt_long(argc, argv, short_options, long_options, NULL)) != EOF) {
        switch (c) {
            case 'p':
                ret = parse_port(optarg);
                if (ret < 0) {
                    printf("Failed to parse port\n");
                    return -1;
                }
                break;

            case 'i':
                ret = parse_interval(optarg);
                if (ret < 0) {
                    printf("Failed to parse interval\n");
                    return -1;
                }
                break;

            case 'l':
                ret = parse_latency(optarg);
                if (ret < 0) {
                    return -1;
                }
                break;

            case 'r':
                ret = parse_pkt_rate(optarg);
                if (ret < 0) {
                    printf("Failed to parse pkt rate\n");
                    return -1;
                }
                break;

            case 't':
                ret = parse_test_pps(optarg);
                if (ret < 0) {
                    printf("Failed to parse test pps option\n");
                    return -1;
                }
                break;

            case 'm':
                ret = parse_call_main(optarg);
                if (ret < 0) {
                    printf("Failed to parse call main option\n");
                    return -1;
                }
                break;

            case 'd':
                ret = parse_delay(optarg);
                if (ret < 0) {
                    printf("Failed to parse dummy delay\n");
                    return -1;
                }
                break;

            case 'a':
                ret = parse_app_id(optarg);
                if (ret < 0) {
                    printf("Failed to parse pkt rate\n");
                    return -1;
                }
                break;
            
            case 'b':
                ret = parse_app_arg1(optarg);
                if (ret < 0) {
                    printf("Failed to parse pkt rate\n");
                    return -1;
                }
                break;

            case 'c':
                ret = parse_app_arg2(optarg);
                if (ret < 0) {
                    printf("Failed to parse pkt rate\n");
                    return -1;
                }
                break;
            
            case 'h':
            default:
                print_usage(prgname);
                return -1;
        }
    }

    if (optind >= 0) {
        argv[optind-1] = prgname;
    }
    optind = 1;

	return 0;
}

#endif /* _MAIN_H_ */