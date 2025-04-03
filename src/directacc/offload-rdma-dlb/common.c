#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <inttypes.h>
#include <dirent.h>

#include "common.h"

// ==== RDMA variables
int num_qps = 3;
int msg_size = 256;
int mr_size = 4096;
int ib_port = 1;
char *server_ip = NULL;
char *client_ip = NULL;
char *snic_ip = NULL;
int host_type = -100;
//=========================
uint64_t num_events = 4*128;
int meta_size = 1024;
uint64_t metadata_mr_size;

int num_dlb_pp = 1;
int num_meta_conns = 1;
int num_client_conns = 1;
int num_dlb_workers = 1;


int test_finished = 0;

// In a list of IB devices (dev_list), given a IB device's name
// (ib_dev_name), the function returns its ID.
static inline int ib_dev_id_by_name(char *ib_dev_name,
                                    struct ibv_device **dev_list,
                                    int num_devices)
{
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), ib_dev_name) == 0) {
            return i;
        }
    }

    return -1;
}

static int cmpfunc_rdma(const void * a, const void * b) {
   return (*(int*)a - *(int*)b);
}

// Check if the gid referenced by gid_index is a ipv4-gid
static bool is_ipv4_gid(struct dev_context *ctx, int gid_index) {
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

// Get the index of all the GIDs whose types are RoCE v2
// Refer to https://docs.mellanox.com/pages/viewpage.action?pageId=12013422#RDMAoverConvergedEthernet(RoCE)-RoCEv2 for more details
static void get_rocev2_gid_index(struct dev_context *ctx)
{
    const size_t max_gid_count = sizeof(ctx->gid_index_list) / sizeof(ctx->gid_index_list[0]);
    int gid_index_list[max_gid_count];
    int gid_count = 0;

    ctx->gid_count = 0;

    char dir_name[128] = {0};
    snprintf(dir_name, sizeof(dir_name),
             "/sys/class/infiniband/%s/ports/%d/gid_attrs/types",
             ctx->ib_dev_name, ctx->dev_port);
    DIR *dir = opendir(dir_name);
    if (!dir) {
        fprintf(stderr, "Fail to open folder %s\n", dir_name);
        return;
    }

    struct dirent *dp = NULL;
    char file_name[384] = {0};
    FILE * fp = NULL;
    ssize_t read;
    char * line = NULL;
    size_t len = 0;
    int gid_index;

    while ((dp = readdir(dir)) && gid_count < max_gid_count) {
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

    closedir(dir);

    qsort(gid_index_list, gid_count, sizeof(int), cmpfunc_rdma);
    ctx->gid_count = gid_count;
    for (int i = 0; i < gid_count; i++) {
        ctx->gid_index_list[i] = gid_index_list[i];
        printf("Get RoCE V2 GID index is %d\n", gid_index_list[i]);
    }
    //Debug
    printf("Get %lu RoCE V2 GIDs\n", ctx->gid_count);
}

// Initialize device context
int init_device_ctx(struct dev_context *dev_ctx) {

    if (dev_ctx == NULL) {
        goto err;
    }

    struct ibv_device **dev_list;
    int num_devices;
    int ib_dev_id = -1;

    // get IB devices list
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB device list.\n");
        goto err;
    }

    if (num_devices <= 0) {
        fprintf(stderr, "No available IB device found.\n");
        goto clean_dev_list;
    }

    // get IB deivce by name
    ib_dev_id = ib_dev_id_by_name(dev_ctx->ib_dev_name, dev_list, num_devices);
    if (ib_dev_id < 0) {
        fprintf(stderr, "Failed to get IB device.\n");
        goto clean_dev_list;
    }

    // open IB device context
    dev_ctx->ctx = ibv_open_device(dev_list[ib_dev_id]);
    if (!dev_ctx->ctx) {
        fprintf(stderr, "Cannot open IB device context for %s\n", ibv_get_device_name(dev_list[ib_dev_id]));
        goto clean_dev_list;
    } else {
        fprintf(stdout, "Open IB device context for %s\n", ibv_get_device_name(dev_list[ib_dev_id]));
    }


    // get the index of GIDs whose types are RoCE v2
    get_rocev2_gid_index(dev_ctx);
    if (dev_ctx->gid_count == 0) {
        fprintf(stderr, "Cannot find any RoCE v2 GID\n");
        goto clean_device;
    }

    // get RoCE v2 GIDs
    for (size_t i = 0; i < dev_ctx->gid_count; i++) {
        if (ibv_query_gid(dev_ctx->ctx, dev_ctx->dev_port, dev_ctx->gid_index_list[i], &(dev_ctx->gid_list[i])) != 0) {
            fprintf(stderr, "Cannot read GID of index %d\n", dev_ctx->gid_index_list[i]);
            goto clean_device;
        }
    }

    // create a completion channel
    if (dev_ctx->use_event) {
        dev_ctx->channel = ibv_create_comp_channel(dev_ctx->ctx);
        if (!(dev_ctx->channel)) {
            fprintf(stderr, "Cannot create completion channel\n");
            goto clean_device;
        }
    } else {
        dev_ctx->channel = NULL;
    }

    // query device attributes
    if (ibv_query_device(dev_ctx->ctx, &(dev_ctx->dev_attr)) != 0) {
        fprintf(stderr, "Fail to query device attributes\n");
        goto clean_pd;
    }

    // query device attributes ex
    struct ibv_device_attr_ex device_attr_ex;
    struct ibv_query_device_ex_input input;
    memset(&input, 0, sizeof(input));
    memset(&device_attr_ex, 0, sizeof(device_attr_ex));
    if (ibv_query_device_ex(dev_ctx->ctx, &input, &device_attr_ex) != 0) {
        fprintf(stderr, "Fail to query device attributes\n");
        goto clean_pd;
    }
    // Print the PCI atomic capabilities
    printf("PCI atomic capabilities: %d\n",dev_ctx->dev_attr.atomic_cap);
    // print_hca_cap(dev_list[ib_dev_id], ib_port);

    // query IB port attributes
    if (ibv_query_port(dev_ctx->ctx, dev_ctx->dev_port, &(dev_ctx->port_attr)));

    // allocate protection domain
    dev_ctx->pd = ibv_alloc_pd(dev_ctx->ctx);
    if (!dev_ctx->pd) {
        fprintf(stderr, "Failed to allocate protection domain.\n");
        goto clean_device;
    }

    // // create a completion queue
    // dev_ctx->cq = ibv_create_cq(dev_ctx->ctx, dev_ctx->dev_attr.max_cqe, NULL, dev_ctx->channel, 0);
    // if (!dev_ctx->cq) {
    //     fprintf(stderr, "Failed to create completion queue.\n");
    //     goto clean_device;
    // }

    // if (dev_ctx->use_event) {
    //     if (ibv_req_notify_cq(dev_ctx->cq, 0)) {
    //         fprintf(stderr, "Cannot request CQ notification\n");
    //         goto clean_cq;
    //     }
    // }

    ibv_free_device_list(dev_list);

    return 0;

// clean_cq:
//     ibv_destroy_cq(dev_ctx->cq);

clean_pd:
	ibv_dealloc_pd(dev_ctx->pd);

clean_comp_channel:
    if (dev_ctx->channel) {
        ibv_destroy_comp_channel(dev_ctx->channel);
    }

clean_device:
	ibv_close_device(dev_ctx->ctx);

clean_dev_list:
    ibv_free_device_list(dev_list);

err:
    return -1;
}


int init_connection_ctx(struct conn_context *conn_ctx) {
    if (!conn_ctx) {
        return -1;
    }

#if IS_CLIENT || IS_SERVER
    if (conn_ctx->rdma_data_qp != -1 && conn_ctx->data_buf == NULL) {
        conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), PAGE_SIZE*bufs_num*2);
        if (!(conn_ctx->data_buf)) {
            fprintf(stderr, "Fail to allocate data buffer memory\n");
            goto err;
        }
        // conn_ctx->recv_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), PAGE_SIZE*bufs_num);
        // if (!(conn_ctx->recv_buf)) {
        //     fprintf(stderr, "Fail to allocate data buffer memory\n");
        //     goto err;
        // }
        for (unsigned int j = 0; j<bufs_num; j++) {
            conn_ctx->buf_recv[j] = (char*)(conn_ctx->data_buf + PAGE_SIZE*j);
            if (!conn_ctx->buf_recv[j]) {
                fprintf(stderr, "Couldn't allocate work buf.\n");
                goto err;
            }
            #if IS_CLIENT
            memset(conn_ctx->buf_recv[j], 0x7b, 4096);
            #elif IS_SERVER
            memset(conn_ctx->buf_recv[j], 0x00, 4096);
            #endif

            conn_ctx->buf_send[j] = (char*)(conn_ctx->data_buf + PAGE_SIZE*bufs_num + PAGE_SIZE*j);
            if (!conn_ctx->buf_send[j]) {
                fprintf(stderr, "Couldn't allocate work buf.\n");
                goto err;
            }
            #if IS_CLIENT
            memset(conn_ctx->buf_send[j], 0x7b, 4096);
            #elif IS_SERVER
            memset(conn_ctx->buf_send[j], 0x00, 4096);
            #endif

            int mean = 2000;
            unsigned int mean_lower = mean % 0x100;
            unsigned int mean_upper = mean / 0x100;
            conn_ctx->buf_send[j][0] = mean_lower;
            conn_ctx->buf_send[j][1] = mean_upper;
        }

        conn_ctx->data_mr = ibv_reg_mr(conn_ctx->dev_ctx->pd, conn_ctx->data_buf, PAGE_SIZE*bufs_num*2, conn_ctx->mr_access_flags);
                            // IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
                            //  | IBV_ACCESS_MW_BIND | IBV_ACCESS_ZERO_BASED | IBV_ACCESS_ON_DEMAND | IBV_ACCESS_FLUSH_GLOBAL);
        if (!conn_ctx->data_mr) {
            perror("Failed to register memory region.");
            // fprintf(stderr, "Failed to register memory region.\n");
            goto clean_data_buf;
        }
    } else if (IS_CLIENT && conn_ctx->dlb_data_qp != -1 && conn_ctx->data_buf == NULL) {
        conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), conn_ctx->data_buf_size);
        if (!(conn_ctx->data_buf)) {
            fprintf(stderr, "Fail to allocate data buffer memory\n");
            goto err;
        }
        conn_ctx->data_mr = ibv_reg_mr(conn_ctx->dev_ctx->pd, conn_ctx->data_buf, conn_ctx->data_buf_size, conn_ctx->mr_access_flags);
                            // IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
                            //  | IBV_ACCESS_MW_BIND | IBV_ACCESS_ZERO_BASED | IBV_ACCESS_ON_DEMAND | IBV_ACCESS_FLUSH_GLOBAL);
        if (!conn_ctx->data_mr) {
            perror("Failed to register memory region.");
            // fprintf(stderr, "Failed to register memory region.\n");
            goto clean_data_buf;
        }
    } else {
        // register memory region
        printf("[%s()] RDMA tries to register address: %p\n", __func__, conn_ctx->data_buf);
        conn_ctx->data_mr = ibv_reg_mr(conn_ctx->dev_ctx->pd, conn_ctx->data_buf, conn_ctx->data_buf_size, conn_ctx->mr_access_flags);
                            // IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
                            //  | IBV_ACCESS_MW_BIND | IBV_ACCESS_ZERO_BASED | IBV_ACCESS_ON_DEMAND | IBV_ACCESS_FLUSH_GLOBAL);
        if (!conn_ctx->data_mr) {
            perror("Failed to register memory region.");
            // fprintf(stderr, "Failed to register memory region.\n");
            goto clean_data_buf;
        }
    }
#elif IS_SNIC
    // allocate memory if it is a regular QP and the buffer is not allocated
    if ((conn_ctx->rdma_data_qp != -1 || conn_ctx->dlb_data_qp != -1) && conn_ctx->data_buf == NULL) {
        conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), conn_ctx->data_buf_size);
        if (!(conn_ctx->data_buf)) {
            fprintf(stderr, "Fail to allocate data buffer memory\n");
            goto err;
        }
    }
    // conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), conn_ctx->data_buf_size);
    if (!(conn_ctx->data_buf)) {
        fprintf(stderr, "Fail to allocate data buffer memory\n");
        goto err;
    }

    // register memory region
    printf("[%s()] RDMA tries to register address: %p\n", __func__, conn_ctx->data_buf);
    conn_ctx->data_mr = ibv_reg_mr(conn_ctx->dev_ctx->pd, conn_ctx->data_buf, conn_ctx->data_buf_size, conn_ctx->mr_access_flags);
                        // IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
                        //  | IBV_ACCESS_MW_BIND | IBV_ACCESS_ZERO_BASED | IBV_ACCESS_ON_DEMAND | IBV_ACCESS_FLUSH_GLOBAL);
    if (!conn_ctx->data_mr) {
        perror("Failed to register memory region.");
        // fprintf(stderr, "Failed to register memory region.\n");
        goto clean_data_buf;
    }
#else
    // allocate memory if it is a regular QP and the buffer is not allocated
    if (conn_ctx->rdma_data_qp != -1 && conn_ctx->data_buf == NULL) {
        conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), conn_ctx->data_buf_size);
        if (!(conn_ctx->data_buf)) {
            fprintf(stderr, "Fail to allocate data buffer memory\n");
            goto err;
        }
    }
    // conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), conn_ctx->data_buf_size);
    if (!(conn_ctx->data_buf)) {
        fprintf(stderr, "Fail to allocate data buffer memory\n");
        goto err;
    }

    // register memory region
    printf("[%s()] RDMA tries to register address: %p\n", __func__, conn_ctx->data_buf);
    conn_ctx->data_mr = ibv_reg_mr(conn_ctx->dev_ctx->pd, conn_ctx->data_buf, conn_ctx->data_buf_size, conn_ctx->mr_access_flags);
                        // IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
                        //  | IBV_ACCESS_MW_BIND | IBV_ACCESS_ZERO_BASED | IBV_ACCESS_ON_DEMAND | IBV_ACCESS_FLUSH_GLOBAL);
    if (!conn_ctx->data_mr) {
        perror("Failed to register memory region.");
        // fprintf(stderr, "Failed to register memory region.\n");
        goto clean_data_buf;
    }
#endif

    // create a completion queue
    conn_ctx->cq = ibv_create_cq(conn_ctx->dev_ctx->ctx, conn_ctx->dev_ctx->dev_attr.max_cqe / 4, NULL, conn_ctx->dev_ctx->channel, 0);
    if (!conn_ctx->cq) {
        fprintf(stderr, "Failed to create completion queue.\n");
    }

    if (conn_ctx->dev_ctx->use_event) {
        if (ibv_req_notify_cq(conn_ctx->cq, 0)) {
            fprintf(stderr, "Cannot request CQ notification\n");
            goto clean_cq;
        }
    }

    // create a queue pair
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = conn_ctx->cq,
        .recv_cq = conn_ctx->cq,
        .cap = {
            .max_send_wr = conn_ctx->dev_ctx->dev_attr.max_qp_wr / 4,
            .max_recv_wr = conn_ctx->dev_ctx->dev_attr.max_qp_wr / 4,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    printf("depth is %d\n", conn_ctx->dev_ctx->dev_attr.max_qp_wr / 4);
    conn_ctx->qp = ibv_create_qp(conn_ctx->dev_ctx->pd, &qp_init_attr);
    if (!conn_ctx->qp) {
        fprintf(stderr, "Failed to create queue pair\n");
        goto clean_data_mr;
    }

    struct ibv_qp_attr qp_attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = conn_ctx->dev_ctx->dev_port,
        .qp_access_flags = conn_ctx->qp_access_flags,
    };

    // modify qp
    if (ibv_modify_qp(conn_ctx->qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify QP to INIT.\n");
        goto clean_qp;
    }

    // get LID
    conn_ctx->local_dest.lid = conn_ctx->dev_ctx->port_attr.lid;
    // get QON
    conn_ctx->local_dest.qpn = conn_ctx->qp->qp_num;
    // get PSN
    srand48(getpid() * time(NULL) * conn_ctx->local_dest.qpn);
    conn_ctx->local_dest.psn = lrand48() & 0xffffff;

    // get GID
    conn_ctx->gid_index = conn_ctx->dev_ctx->gid_index_list[0];
    conn_ctx->local_dest.gid = conn_ctx->dev_ctx->gid_list[0];
    
    return 0;

clean_cq:
    ibv_destroy_cq(conn_ctx->cq);

clean_qp:
    ibv_destroy_qp(conn_ctx->qp);

clean_data_mr:
    ibv_dereg_mr(conn_ctx->data_mr);

clean_data_buf:
    if (conn_ctx->rdma_data_qp != -1 || host_type != SERVER)
        free(conn_ctx->data_buf);

err:
    return -1;
}

void destroy_connection_ctx(struct conn_context *conn_ctx) {
    if (conn_ctx == NULL) return;

    if (conn_ctx->cq) ibv_destroy_cq(conn_ctx->cq);

    // destroy queue pair
    if (conn_ctx->qp) ibv_destroy_qp(conn_ctx->qp);

    // delete memory region
    if (conn_ctx->data_mr) ibv_dereg_mr(conn_ctx->data_mr);

    // free memory
#if !USE_DLB
    if (conn_ctx->data_buf) free(conn_ctx->data_buf);
#else
// printf("host_type is %d\n", host_type);
    if (conn_ctx->data_buf && (conn_ctx->rdma_data_qp != -1 || host_type != SERVER)) 
        free(conn_ctx->data_buf);
#endif

    return;
}

void destroy_device_ctx(struct dev_context *dev_ctx) {
    if (dev_ctx == NULL) return;

    // destroy completion queue
    if (dev_ctx->cq) ibv_destroy_cq(dev_ctx->cq);

    // de-allocate protection region
    if (dev_ctx->pd) ibv_dealloc_pd(dev_ctx->pd);

    // destroy completion channel
    if (dev_ctx->channel) ibv_destroy_comp_channel(dev_ctx->channel);

    // destroy device context
    if(dev_ctx->ctx) ibv_close_device(dev_ctx->ctx);

    return;
}

int connect_qps(struct conn_context *conn_ctx) {
    struct ibv_qp_attr qp_attr = {
            .qp_state		= IBV_QPS_RTR,
            .path_mtu		= IBV_MTU_1024,
            .dest_qp_num	= conn_ctx->remote_dest.qpn,
            .rq_psn			= conn_ctx->remote_dest.psn,
            .max_dest_rd_atomic	= 1,
            .min_rnr_timer		= 12,
            .ah_attr		= {
                    .is_global	= 0,
                    .dlid		= conn_ctx->remote_dest.lid,
                    .sl		= 0,
                    .src_path_bits	= 0,
                    .port_num	= conn_ctx->dev_ctx->dev_port
            }
    };

    if (conn_ctx->remote_dest.gid.global.interface_id) {
        qp_attr.ah_attr.is_global = 1;
        // Set attributes of the Global Routing Headers (GRH)
        // When using RoCE, GRH must be configured!
		qp_attr.ah_attr.grh.hop_limit = 1;
		qp_attr.ah_attr.grh.dgid = conn_ctx->remote_dest.gid;
		qp_attr.ah_attr.grh.sgid_index = conn_ctx->gid_index;
    }


    // Modify the attributes of a Queue Pair to RTR
    if (ibv_modify_qp(conn_ctx->qp, &qp_attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                           IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER | IBV_QP_AV)) {
        perror("Failed to modify QP to RTR.");
        return -1;
    }

    qp_attr.qp_state	    = IBV_QPS_RTS;
    qp_attr.timeout	        = 14;
    qp_attr.retry_cnt	    = 7;
    qp_attr.rnr_retry	    = 7;
    qp_attr.sq_psn	        = conn_ctx->local_dest.psn;
    qp_attr.max_rd_atomic   = 1;
    if (ibv_modify_qp(conn_ctx->qp, &qp_attr, IBV_QP_STATE |
                                            IBV_QP_TIMEOUT |
                                            IBV_QP_RETRY_CNT |
                                            IBV_QP_RNR_RETRY |
                                            IBV_QP_SQ_PSN |
                                            IBV_QP_MAX_QP_RD_ATOMIC))
    {
        perror("Failed to modify QP to RTS.");
        return -1;
    }

    return 0;
}

int post_recv(struct conn_context *conn_ctx, uint64_t offset, int size) {
    struct ibv_sge list = {
        .addr	= (uint64_t)(conn_ctx->data_buf) + offset,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_recv_wr *bad_wr, wr = {
        .wr_id	    = RECV_WRID,
        .sg_list    = &list,
        .num_sge    = 1,
        .next       = NULL
    };

    return ibv_post_recv(conn_ctx->qp, &wr, &bad_wr) == 0;
}


int post_send(struct conn_context *conn_ctx, uint64_t offset, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = SEND_WRID,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_SEND,
        .send_flags             = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset,
        .wr.rdma.rkey           = conn_ctx->remote_mr.rkey
	};

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int post_send_read(struct conn_context *conn_ctx, uint64_t offset, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = READ_WRID,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_READ,
        .send_flags             = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset,
        .wr.rdma.rkey           = conn_ctx->remote_mr.rkey,
        .next                   = NULL
    };

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int post_send_write(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset_local,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = WRITE_WRID,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_WRITE,
        .send_flags             = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote,
        .wr.rdma.rkey           = conn_ctx->remote_mr.rkey,
        .next                   = NULL
    };

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int prepare_send_write_batch(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size, int signal) 
{
    int idx = conn_ctx->batch_count;
    if (idx >= BATCH_SIZE) {
        // Should not happen if flush is called appropriately.
        fprintf(stderr, "Batch is full! Flush before adding more.\n");
        return -1;
    }

    /* Prepare the SGE */
    conn_ctx->batch_sges[idx].addr   = (uint64_t)conn_ctx->data_buf + offset_local;
    conn_ctx->batch_sges[idx].length = size;
    conn_ctx->batch_sges[idx].lkey   = conn_ctx->data_mr->lkey;

    /* Fill in the work request */
    conn_ctx->batch_wrs[idx].wr_id      = WRITE_WRID;
    conn_ctx->batch_wrs[idx].sg_list    = &conn_ctx->batch_sges[idx];
    conn_ctx->batch_wrs[idx].num_sge    = 1;
    conn_ctx->batch_wrs[idx].opcode     = IBV_WR_RDMA_WRITE;
    conn_ctx->batch_wrs[idx].send_flags = signal ? IBV_SEND_SIGNALED : 0;
    conn_ctx->batch_wrs[idx].wr.rdma.remote_addr = conn_ctx->remote_mr.addr + offset_remote;
    conn_ctx->batch_wrs[idx].wr.rdma.rkey        = conn_ctx->remote_mr.rkey;
    conn_ctx->batch_wrs[idx].next = NULL;

    /* Link previous WR to this one, if any */
    if (idx > 0) {
        conn_ctx->batch_wrs[idx - 1].next = &conn_ctx->batch_wrs[idx];
    }

    conn_ctx->batch_count++;

    return 0;
}

int post_send_write_batch(struct conn_context *conn_ctx)
{
    if (conn_ctx->batch_count == 0)
        return 0;  // Nothing to do

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(conn_ctx->qp, &conn_ctx->batch_wrs[0], &bad_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_send failed: %d\n", ret);
        return ret;
    }

    /* Reset batch count so new requests start at index 0 */
    conn_ctx->batch_count = 0;
    return 0;
}


int post_send_write_no_signal(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset_local,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = WRITE_WRID,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_WRITE,
        .send_flags             = 0,
        .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote,
        .wr.rdma.rkey           = conn_ctx->remote_mr.rkey,
        .next                   = NULL
    };

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int post_send_atomic_cmp_and_swap(struct conn_context *conn_ctx, uint64_t offset, int size, uint64_t compare_add, uint64_t swap)
{
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset + 8,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = ATOMIC_WRID,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_ATOMIC_CMP_AND_SWP,
        .send_flags             = IBV_SEND_SIGNALED,
        .wr.atomic.rkey         = conn_ctx->remote_mr.rkey,
        .wr.atomic.remote_addr  = conn_ctx->remote_mr.addr + offset,
        .wr.atomic.compare_add  = compare_add,
        .wr.atomic.swap         = swap,
        .next                   = NULL
    };

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int post_send_atomic_fetch_and_add(struct conn_context *conn_ctx, uint64_t offset, int size, uint64_t compare_add)
{
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset + 8,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = ATOMIC_WRID,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_ATOMIC_FETCH_AND_ADD,
        .send_flags             = IBV_SEND_SIGNALED,
        .wr.atomic.rkey         = conn_ctx->remote_mr.rkey,
        .wr.atomic.remote_addr  = conn_ctx->remote_mr.addr + offset,
        .wr.atomic.compare_add  = compare_add,
        .wr.atomic.swap         = 0,
        .next                   = NULL
    };

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int wait_completions(struct conn_context *conn_ctx, int wr_id, int cnt)
{
    int finished = 0, count = cnt;
    int wc_batch = count;
    struct ibv_wc wc[wc_batch];

    while (finished < count && test_finished == 0) {
        // printf("waiting...\n");
        int n = 0;
        while (n <= 0 && test_finished == 0) {
            // printf("poll cq...\n");
            n = ibv_poll_cq(conn_ctx->cq, wc_batch, wc);

            if (n < 0) {
                fprintf(stderr, "Poll CQ failed %d\n", n);
                return -1;
            }
        }

        // printf("poll %d\n", wr_id);

        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS)
            {
                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status), wc[i].status, (int)wc[i].wr_id);
                return 1;
            }
            if (wc[i].wr_id == wr_id)
                finished++;
        }

        wc_batch = cnt - finished;
    }

    return 0;
}

uint32_t query_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size) {
    post_send_read(conn_ctx, 0, sizeof(uint64_t));
    wait_completions(conn_ctx, READ_WRID, 1);
    return *(uint32_t *)(conn_ctx->data_buf);
}

uint32_t acquire_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size, uint64_t global_credits, uint64_t new_global_credits) {
    // *(uint64_t *)(conn_ctx->data_buf + 8) = 0;
    post_send_atomic_cmp_and_swap(conn_ctx, 0, sizeof(uint64_t), global_credits, new_global_credits);
    // post_send_atomic_fetch_and_add(conn_ctx, sizeof(uint32_t), -128);
    wait_completions(conn_ctx, ATOMIC_WRID, 1);
    uint32_t swap = *(uint32_t *)(conn_ctx->data_buf + 8);
    // uint32_t remote_global_credits = query_credits(dev_ctx, conn_ctx, sizeof(uint32_t));

    // printf("global credits is %lu, local_credits is %lu\n", global_credits, new_global_credits);
    // if (swap == (uint32_t)global_credits && remote_global_credits == (uint32_t)new_global_credits) {
    if (swap == (uint32_t)global_credits) {
        // printf("acquire_credits: global credits is %u %u, swap value is %u\n", (uint32_t)global_credits, (uint32_t) new_global_credits, swap);
        return (uint32_t)(global_credits - new_global_credits);
    } else {
        // printf("UNSUCCESS: acquire_credits: global credits is %u %u, swap value is %u\n", (uint32_t)global_credits, (uint32_t) new_global_credits, swap);
        return 0;
    }
}

// Modified from https://github.com/Arlu/RDMA-Hello-World
int get_server_dest(struct conn_context *conn_ctx, char *server_ip, char *port, int num_qps) {
    int sockfd = -1;
    struct addrinfo *result, *rp;

    struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};

    if (getaddrinfo(server_ip, port, &hints, &result) < 0) {
        perror("Found address failed!\n");
        return -1;
    }

    // Start connection:
    for (rp = result; rp; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd == -1)
            continue;

        if (!connect(sockfd, rp->ai_addr, rp->ai_addrlen))
            break; // Success.

        close(sockfd);
    }

    if (!rp) {
        perror("Connection with the server failed.\n");
        return -1;
    }

    freeaddrinfo(result);

    // Get server details:
    {
        char *buf;
        unsigned long buf_size = sizeof(struct conn_dest) + sizeof(struct conn_mem);
        unsigned long offset;
        buf = (char *)malloc(buf_size);

        for (int i = 0; i < num_qps; i++) {
            memset(buf, 0, buf_size);
            if (conn_ctx[i].rdma_data_qp != -1) {
                // NOT DLB QP
                conn_ctx[i].local_mr.addr = (uint64_t)(conn_ctx[i].data_buf);
                conn_ctx[i].local_mr.rkey = conn_ctx[i].data_mr->rkey;

                // Send my details:
                memset(buf, 0, buf_size);
                memcpy(buf, (char*)&(conn_ctx[i].local_dest), sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy(buf+offset, (char*)&(conn_ctx[i].local_mr), sizeof(struct conn_mem));
                write(sockfd, buf, buf_size);

                // Get server details:
                memset(buf, 0, buf_size);
                offset = 0;
                while (offset < buf_size) {
                    offset += read(sockfd, buf + offset, buf_size - offset);
                }
                memcpy((char*)&(conn_ctx[i].remote_dest), buf, sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy((char*)&(conn_ctx[i].remote_mr), buf + offset, sizeof(struct conn_mem));
            }
        }

        // Connection finished notification
        memset(buf, 0, buf_size);
        offset = 0;
        while (offset < sizeof(READY_MSG)) {
            offset += read(sockfd, buf + offset, sizeof(READY_MSG) - offset);
        }
        printf("Connection setup %s on client\n", buf);

        free(buf);
    }

    // Finish connection:
    close(sockfd);

    return 0;
}

int get_server_dlb_dest(struct conn_context *conn_ctx, char *server_ip, char *port, int num_qps) {
    int sockfd = -1;
    struct addrinfo *result, *rp;

    struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};

    if (getaddrinfo(server_ip, port, &hints, &result) < 0) {
        perror("Found address failed!\n");
        return -1;
    }

    // Start connection:
    for (rp = result; rp; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sockfd == -1)
            continue;

        if (!connect(sockfd, rp->ai_addr, rp->ai_addrlen))
            break; // Success.

        close(sockfd);
    }

    if (!rp) {
        perror("Connection with the server failed.\n");
        return -1;
    }

    freeaddrinfo(result);

    // Get server details:
    {
        char *buf;
        unsigned long buf_size = sizeof(struct conn_dest) + sizeof(struct conn_mem);
        unsigned long offset;
        buf = (char *)malloc(buf_size);

        for (int i = 0; i < num_qps; i++) {
            memset(buf, 0, buf_size);
            if (conn_ctx[i].rdma_data_qp == -1) {
                // IS DLB QP
                conn_ctx[i].local_mr.addr = (uint64_t)(conn_ctx[i].data_buf);
                conn_ctx[i].local_mr.rkey = conn_ctx[i].data_mr->rkey;

                // Send my details:
                memset(buf, 0, buf_size);
                memcpy(buf, (char*)&(conn_ctx[i].local_dest), sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy(buf+offset, (char*)&(conn_ctx[i].local_mr), sizeof(struct conn_mem));
                write(sockfd, buf, buf_size);

                // Get server details:
                memset(buf, 0, buf_size);
                offset = 0;
                while (offset < buf_size) {
                    offset += read(sockfd, buf + offset, buf_size - offset);
                }
                memcpy((char*)&(conn_ctx[i].remote_dest), buf, sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy((char*)&(conn_ctx[i].remote_mr), buf + offset, sizeof(struct conn_mem));
            }
        }

        // Connection finished notification
        memset(buf, 0, buf_size);
        offset = 0;
        while (offset < sizeof(READY_MSG)) {
            offset += read(sockfd, buf + offset, sizeof(READY_MSG) - offset);
        }
        printf("Connection setup %s on client\n", buf);

        free(buf);
    }

    // Finish connection:
    close(sockfd);

    return 0;
}

int get_client_dest(struct conn_context *conn_ctx, int num_qps, uint16_t port) {
    int sockfd, connfd;
    struct sockaddr_in server_address, client_address;

    // Start connection:
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed!\n");
        return -1;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&server_address, sizeof(server_address))) {
        perror("Socket bind failed!\n");
        return -1;
    }

    if (listen(sockfd, 5)) {
        perror("Listen failed...\n");
        return -1;
    }

    socklen_t len = sizeof(client_address);

    connfd = accept(sockfd, (struct sockaddr*)&client_address, &len);
    if (connfd < 0) {
        perror("Server accept failed!\n");
        return -1;
    }

    // Get client details:
    {
        char *buf;
        unsigned long buf_size = sizeof(struct conn_dest) + sizeof(struct conn_mem);
        unsigned long offset = 0;
        buf = (char *)malloc(buf_size);

        for (int i = 0; i < num_qps; i++) {
            memset(buf, 0, buf_size);
            if (conn_ctx[i].rdma_data_qp != -1) {
                // NOT DLB QP
                conn_ctx[i].local_mr.addr = (uint64_t)(conn_ctx[i].data_buf);
                conn_ctx[i].local_mr.rkey = conn_ctx[i].data_mr->rkey;

                // Get client details.
                // buf_size = sizeof(struct conn_dest);
                memset(buf, 0, sizeof(struct conn_dest) + sizeof(struct conn_mem));
                offset = 0;
                while (offset < buf_size) {
                    offset += read(connfd, buf + offset, buf_size - offset);
                }
                memcpy((char*)&(conn_ctx[i].remote_dest), buf, sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy((char*)&(conn_ctx[i].remote_mr), buf + offset, sizeof(struct conn_mem));

                // Send my details:
                buf_size = sizeof(struct conn_dest) + sizeof(struct conn_mem);
                printf("dest %ld, mr %ld, sum %ld\n", sizeof(struct conn_dest), sizeof(struct conn_mem), buf_size);
                memset(buf, 0, buf_size);
                memcpy(buf, (char*)&(conn_ctx[i].local_dest), sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy(buf+offset, (char*)&(conn_ctx[i].local_mr), sizeof(struct conn_mem));
                write(connfd, buf, buf_size);
            }
        }
        
        // Connection finished notification
        memset(buf, 0, buf_size);
        memcpy(buf, READY_MSG, sizeof(READY_MSG));
        printf("Connection setup %s on server\n", buf);
        write(connfd, buf, sizeof(READY_MSG));

        free(buf);
    }

    // Finish connection:
    close(sockfd);

    return 0;
}

int get_client_dlb_dest(struct conn_context *conn_ctx, int num_qps, uint16_t port) {
    int sockfd, connfd;
    struct sockaddr_in server_address, client_address;

    // Start connection:
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("Socket creation failed!\n");
        return -1;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&server_address, sizeof(server_address))) {
        perror("Socket bind failed!\n");
        return -1;
    }

    if (listen(sockfd, 5)) {
        perror("Listen failed...\n");
        return -1;
    }

    socklen_t len = sizeof(client_address);

    connfd = accept(sockfd, (struct sockaddr*)&client_address, &len);
    if (connfd < 0) {
        perror("Server accept failed!\n");
        return -1;
    }

    // Get client details:
    {
        char *buf;
        unsigned long buf_size = sizeof(struct conn_dest) + sizeof(struct conn_mem);
        unsigned long offset = 0;
        buf = (char *)malloc(buf_size);

        for (int i = 0; i < num_qps; i++) {
            memset(buf, 0, buf_size);
            if (conn_ctx[i].rdma_data_qp == -1) {
                // IS DLB QP
                conn_ctx[i].local_mr.addr = (uint64_t)(conn_ctx[i].data_buf);
                conn_ctx[i].local_mr.rkey = conn_ctx[i].data_mr->rkey;

                // Get client details.
                // buf_size = sizeof(struct conn_dest);
                memset(buf, 0, sizeof(struct conn_dest) + sizeof(struct conn_mem));
                offset = 0;
                while (offset < buf_size) {
                    offset += read(connfd, buf + offset, buf_size - offset);
                }
                memcpy((char*)&(conn_ctx[i].remote_dest), buf, sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy((char*)&(conn_ctx[i].remote_mr), buf + offset, sizeof(struct conn_mem));

                // Send my details:
                buf_size = sizeof(struct conn_dest) + sizeof(struct conn_mem);
                printf("dest %ld, mr %ld, sum %ld\n", sizeof(struct conn_dest), sizeof(struct conn_mem), buf_size);
                memset(buf, 0, buf_size);
                memcpy(buf, (char*)&(conn_ctx[i].local_dest), sizeof(struct conn_dest));
                offset = sizeof(struct conn_dest);
                memcpy(buf+offset, (char*)&(conn_ctx[i].local_mr), sizeof(struct conn_mem));
                write(connfd, buf, buf_size);
            }
        }
        
        // Connection finished notification
        memset(buf, 0, buf_size);
        memcpy(buf, READY_MSG, sizeof(READY_MSG));
        printf("Connection setup %s on server\n", buf);
        write(connfd, buf, sizeof(READY_MSG));

        free(buf);
    }

    // Finish connection:
    close(sockfd);

    return 0;
}

void print_conn_ctx(struct conn_context *conn_ctx)
{
    if (!conn_ctx) {
        fprintf(stderr, "No valid connection context!\n");
        return;
    }

    char gid[33] = {0};
    inet_ntop(AF_INET6, &(conn_ctx->local_dest.gid), gid, sizeof(gid));

    printf("Connection ID:            %u\n", conn_ctx->id);
    printf("Data buffer size:         %lu\n", conn_ctx->data_buf_size);
    printf("Local identifier:         %hu\n", conn_ctx->local_dest.lid);
    printf("Queue pair number:        %u\n", conn_ctx->local_dest.qpn);
    printf("Packet sequence number:   %u\n", conn_ctx->local_dest.psn);
    printf("Global Identifier:        %s\n", gid);
}

void print_dest(struct conn_dest *dest)
{
    if (!dest) {
        return;
    }

    char gid[33] = {0};
    inet_ntop(AF_INET6, &(dest->gid), gid, sizeof(gid));
    printf("LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       dest->lid, dest->qpn, dest->psn, gid);
}

char* generate_message(size_t n) {
    // Allocate memory for the string
    char *str = (char *)malloc(n); // +1 for the null terminator
    if (str == NULL) {
        perror("Failed to allocate memory");
        exit(EXIT_FAILURE);
    }

    // Fill the string with characters (e.g., printable ASCII characters)
    for (size_t i = 0; i < n-1; i++) {
        str[i] = 'A' + (i % 26); // Random character from 'A' to 'Z'
    }

    // Null-terminate the string
    str[n-1] = '\0';

    return str;
}

static void print_usage(const char *prgname) {
    printf("%s \n"
           "    -n, --num_events        number of events to test, default=%ld\n"
           "    -s, --msg_size          size of message to be sent, default=%d\n"
           "    -m, --mr_size           memory region size registered\n"
           "    -q, --num_qps           number of qps created, default=%d\n"
           "    -J, --server_ip         server ip address by this format X.X.X.X\n"
           "    -j, --client_ip         client ip address by this format X.X.X.X\n"
           "    -d, --dlb_qps           number of dlb/client qps\n"
           "    -f, --num-workers=N     Number of 'worker' threads that forward events (default: 0)\n"
           "    -w, --meta-size         metadata size\n"
           "    -h, --help              print usage of the program\n",
           prgname, num_events, msg_size, num_qps);
}

static struct option prog_long_options[] = {
    {"num_events",  required_argument,  0,  'n'},
    {"msg_size",    required_argument,  0,  's'},
    {"mr_size",     required_argument,  0,  'm'},
    {"num_qps",     required_argument,  0,  'q'},
    {"server_ip",   required_argument,  0,  'J'},
    {"client_ip",   required_argument,  0,  'j'},
    {"dlb_qps",     required_argument,  0,  'd'},
    {"num-workers", required_argument,  0,  'f'},
    {"meta-size",   required_argument,  0,  'w'},
    {"help",        no_argument,        0,  'h'},
    {0,             0,                  0,  0 }
};

static int parse_num_events(char *arg) {
    uint32_t n;
    char *endptr;
    n = (uint32_t)strtoul(arg, &endptr, 10);
    if (n == 0) {
        fprintf(stderr, "num_events should be a positive integer argument\n");
        return -1;
    }
    num_events = n;
    return 0;
}

static int parse_msg_size(char *arg) {
    uint32_t n;
    char *endptr;
    n = (uint32_t)strtoul(arg, &endptr, 10);
    if (n == 0) {
        fprintf(stderr, "msg_size should be a positive integer argument\n");
        return -1;
    }
    msg_size = n;
    return 0;
}

static int parse_mr_size(char *arg) {
    uint32_t n;
    char *endptr;
    n = (uint32_t)strtoul(arg, &endptr, 10);
    if (n == 0) {
        fprintf(stderr, "mr_size should be a positive integer argument\n");
        return -1;
    }
    mr_size = n;
    return 0;
}

static int parse_num_qps(char *arg) {
    uint32_t n;
    char *endptr;
    n = (uint32_t)strtoul(arg, &endptr, 10);
    if (n == 0) {
        fprintf(stderr, "num_qps should be a positive integer argument\n");
        return -1;
    }
    num_qps = n;
    return 0;
}

static int parse_server_ip(char *arg) {
    if (inet_pton(AF_INET, arg, &server_ip) <= 0) {
        fprintf(stderr, "Invalid server_ip %s\n", arg);
        return -1;
    }

    return 0;
}

static int parse_client_ip(char *arg) {
    if (inet_pton(AF_INET, arg, &client_ip) <= 0) {
        fprintf(stderr, "Invalid client_ip %s\n", arg);
        return -1;
    }

    return 0;
}

static int parse_dlb_qps(char *arg) {
    uint32_t n;
    char *endptr;
    n = (uint32_t)strtoul(arg, &endptr, 10);
    if (n == 0) {
        fprintf(stderr, "num_qps should be a positive integer argument\n");
        return -1;
    }
    num_dlb_pp = n;
    num_meta_conns = n;
    num_client_conns = n;
    return 0;
}

static int parse_metadata(char *arg) {
    uint32_t n;
    char *endptr;
    n = (uint32_t)strtoul(arg, &endptr, 10);
    if (n == 0) {
        fprintf(stderr, "num_qps should be a positive integer argument\n");
        return -1;
    }
    meta_size = n;
    return 0;
}


int parse_args(int argc, char **argv) {
    char *prgname = argv[0];
    const char short_options[] = "n:s:m:q:J:j:d:f:w:h";
    int c;
    int ret;
    while ((c = getopt_long(argc, argv, short_options, prog_long_options, NULL)) != EOF) {
        switch (c) {
            case 'n':
                ret = parse_num_events(optarg);
                if (ret < 0) {
                    printf("Failed to parse num_events\n");
                    return -1;
                }
                break;
            case 's':
                ret = parse_msg_size(optarg);
                if (ret < 0) {
                    printf("Failed to parse msg_size\n");
                    return -1;
                }
                break;
            case 'm':
                ret = parse_mr_size(optarg);
                if (ret < 0) {
                    printf("Failed to parse mr_size\n");
                    return -1;
                }
                break;
            case 'q':
                ret = parse_num_qps(optarg);
                if (ret < 0) {
                    printf("Failed to parse num_qps\n");
                    return -1;
                }
                break;
            case 'J':
                ret = parse_server_ip(optarg);
                if (ret < 0) {
                    printf("Failed to parse server_ip\n");
                    return -1;
                }
                break;
            case 'j':
                ret = parse_client_ip(optarg);
                if (ret < 0) {
                    printf("Failed to parse client_ip\n");
                    return -1;
                }
                break;
            case 'd':
                ret = parse_dlb_qps(optarg);
                if (ret < 0) {
                    printf("Failed to parse client_ip\n");
                    return -1;
                }
                break;
            case 'f':
                num_dlb_workers = atoi(optarg);
                break;
            case 'w':
                ret = parse_metadata(optarg);
                break;
            case 'h':
            default:
                print_usage(prgname);
                return -1;
        }
    }

    metadata_mr_size = meta_size * sizeof(struct rdma_metadata);

    if (optind >= 0) {
        argv[optind-1] = prgname;
    }

    // reset getopt lib
    optind = 1;

	return 0;
}
