#ifndef _RDMAConnectionRC_
#define _RDMAConnectionRC_
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <inttypes.h>
// #include <dirent.h>

#include "RDMAConnectionRC.h"

#if !IS_CLIENT && !IS_SNIC
#include "../../dlb_8.9.0/libdlb/dlb.h"
#include "../../dlb_8.9.0/libdlb/dlb_priv.h"
#include "../../dlb_8.9.0/libdlb/dlb2_ioctl.h"
#include "../../dlb_8.9.0/libdlb/dlb_common.h"
#endif

// ==== RDMA variables
int num_qps = 3;
int msg_size = DEFAULT_MSG_SIZE;
int mr_size = 4096;
int ib_port = 1;
char *server_ip = NULL;
char *client_ip = NULL;
char *snic_ip = NULL;
int host_type = -100;
// int num_dlb_pp = 1;
// int num_client_conns = 1;
//=========================
int meta_size = 1024;
uint64_t metadata_mr_size = meta_size * sizeof(struct rdma_metadata);

// In a list of IB devices (dev_list), given a IB device's name
// (ib_dev_name), the function returns its ID.
int RDMAConnectionRC::ib_dev_id_by_name(char *ib_dev_name,
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

// Initialize device context
int RDMAConnectionRC::init_device_ctx(struct dev_context *dev_ctx) {

    struct ibv_device **dev_list;
    int num_devices;
    int ib_dev_id = -1;

    struct ibv_device_attr_ex device_attr_ex;
    struct ibv_query_device_ex_input input;
    
    if (dev_ctx == NULL) {
        goto err;
    }

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
    memset(&input, 0, sizeof(input));
    memset(&device_attr_ex, 0, sizeof(device_attr_ex));
    if (ibv_query_device_ex(dev_ctx->ctx, &input, &device_attr_ex) != 0) {
        fprintf(stderr, "Fail to query device attributes\n");
        goto clean_pd;
    }
    // Print the PCI atomic capabilities
    printf("PCI atomic capabilities: %d\n", (dev_ctx->dev_attr.atomic_cap));

    // query IB port attributes
    if (ibv_query_port(dev_ctx->ctx, dev_ctx->dev_port, &(dev_ctx->port_attr))) {
        fprintf(stderr, "Fail to query port attributes\n");
        goto clean_pd;
    }

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


int RDMAConnectionRC::init_connection_ctx(struct conn_context *conn_ctx) {

    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_qp_attr qp_attr;


    if (!conn_ctx) {
        return -1;
    }

#if IS_CLIENT || IS_SERVER
    if (conn_ctx->rdma_data_qp != -1 && conn_ctx->data_buf == NULL) {
        conn_ctx->data_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), BUF_SIZE*bufs_num);
        if (!(conn_ctx->data_buf)) {
            fprintf(stderr, "Fail to allocate data buffer memory\n");
            goto err;
        }
        // conn_ctx->recv_buf = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), BUF_SIZE*bufs_num);
        // if (!(conn_ctx->recv_buf)) {
        //     fprintf(stderr, "Fail to allocate data buffer memory\n");
        //     goto err;
        // }
        for (unsigned int j = 0; j<bufs_num; j++) {

#if IS_SERVER
            conn_ctx->buf_recv[j] = (char*)(conn_ctx->data_buf + BUF_SIZE*j);
            if (!conn_ctx->buf_recv[j]) {
                fprintf(stderr, "Couldn't allocate work buf.\n");
                goto err;
            }
            #if IS_CLIENT
            memset(conn_ctx->buf_recv[j], 0x7b, BUF_SIZE);
            #elif IS_SERVER
            memset(conn_ctx->buf_recv[j], 0x00, BUF_SIZE);
            #endif
#endif

#if IS_CLIENT
            conn_ctx->buf_send[j] = (char*)(conn_ctx->data_buf + BUF_SIZE*j);
            if (!conn_ctx->buf_send[j]) {
                fprintf(stderr, "Couldn't allocate work buf.\n");
                goto err;
            }
            #if IS_CLIENT
            memset(conn_ctx->buf_send[j], 0x7b, BUF_SIZE);
            #elif IS_SERVER
            memset(conn_ctx->buf_send[j], 0x00, BUF_SIZE);
            #endif
#endif
            // conn_ctx->buf_send[j] = (char*)(conn_ctx->data_buf + BUF_SIZE*bufs_num + BUF_SIZE*j);
            // if (!conn_ctx->buf_send[j]) {
            //     fprintf(stderr, "Couldn't allocate work buf.\n");
            //     goto err;
            // }
            // #if IS_CLIENT
            // memset(conn_ctx->buf_send[j], 0x7b, BUF_SIZE);
            // #elif IS_SERVER
            // memset(conn_ctx->buf_send[j], 0x00, BUF_SIZE);
            // #endif

            // int mean = 2000;
            // uint mean_lower = mean % 0x100;
            // uint mean_upper = mean / 0x100;
            // conn_ctx->buf_send[j][0] = mean_lower;
            // conn_ctx->buf_send[j][1] = mean_upper;
        }

        conn_ctx->data_mr = ibv_reg_mr(conn_ctx->dev_ctx->pd, conn_ctx->data_buf, BUF_SIZE*bufs_num, conn_ctx->mr_access_flags);
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
    qp_init_attr = {
        .send_cq = conn_ctx->cq,
        .recv_cq = conn_ctx->cq,
        .cap = {
            .max_send_wr = conn_ctx->dev_ctx->dev_attr.max_qp_wr / 4,
            .max_recv_wr = conn_ctx->dev_ctx->dev_attr.max_qp_wr / 4,
            .max_send_sge = 1,
            .max_recv_sge = 1,
            .max_inline_data = 32
        },
        .qp_type = IBV_QPT_RC,
    };
    // printf("depth is %d\n", conn_ctx->dev_ctx->dev_attr.max_qp_wr / 4);
    conn_ctx->qp = ibv_create_qp(conn_ctx->dev_ctx->pd, &qp_init_attr);
    if (!conn_ctx->qp) {
        fprintf(stderr, "Failed to create queue pair\n");
        goto clean_data_mr;
    }

    qp_attr = {
        .qp_state = IBV_QPS_INIT,
        .qp_access_flags = conn_ctx->qp_access_flags,
        .pkey_index = 0,
        .port_num = conn_ctx->dev_ctx->dev_port,
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

void RDMAConnectionRC::destroy_connection_ctx(struct conn_context *conn_ctx) {
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
    if (conn_ctx->data_buf != NULL && (conn_ctx->rdma_data_qp != -1 || !IS_SERVER)) {
        // printf("Trying to free buffer %p\n", conn_ctx->data_buf);
        free(conn_ctx->data_buf);
    }
#endif

    return;
}

void RDMAConnectionRC::destroy_device_ctx(struct dev_context *dev_ctx) {
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

int RDMAConnectionRC::connect_qps(struct conn_context *conn_ctx) {
    struct ibv_qp_attr qp_attr = {
            .qp_state		= IBV_QPS_RTR,
            .path_mtu		= IBV_MTU_2048,
            .rq_psn			= conn_ctx->remote_dest.psn,
            .dest_qp_num	= conn_ctx->remote_dest.qpn,
            .ah_attr		= {
                    .dlid		= conn_ctx->remote_dest.lid,
                    .sl		= 0,
                    .src_path_bits	= 0,
                    .is_global	= 0,
                    .port_num	= conn_ctx->dev_ctx->dev_port
            },
            .max_dest_rd_atomic	= 1,
            .min_rnr_timer		= 12
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

int RDMAConnectionRC::post_recv(struct conn_context *conn_ctx, uint64_t offset, int size) {
    struct ibv_sge list = {
        .addr	= (uint64_t)(conn_ctx->data_buf) + offset,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_recv_wr *bad_wr, wr = {
        .wr_id	    = RECV_WRID,
        .next       = NULL,
        .sg_list    = &list,
        .num_sge    = 1
    };

    return ibv_post_recv(conn_ctx->qp, &wr, &bad_wr) == 0;
}

int RDMAConnectionRC::pp_post_recv(struct conn_context *ctx, int wr_id, bool sync)
{
	struct ibv_sge list;
	memset(&list, 0, sizeof(list));

	if(!sync) {
		list.addr = (uintptr_t)(ctx->buf_recv[wr_id - bufs_num]);
		list.lkey = ctx->data_mr->lkey;
	}
	else {
		list.addr = (uintptr_t)(ctx->buf_recv[wr_id - sync_bufs_num]);
		list.lkey = ctx->data_mr->lkey;
	}

	list.length = 40 + 4096;
	struct ibv_recv_wr wr;
	memset(&wr, 0, sizeof(wr));

	wr.wr_id = wr_id;
	wr.sg_list = &list;
	wr.num_sge = 1;

    // printf("wr_id is %d, addr: %p, lkey: %d, length: %d\n", wr_id, list.addr, list.lkey, list.length);

	struct ibv_recv_wr *bad_wr;
	memset(&bad_wr, 0, sizeof(bad_wr));
	return ibv_post_recv(ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::pp_post_send(struct conn_context *ctx, uint32_t qpn, unsigned int length, int wr_id)
{
	struct ibv_sge list;

	list.addr   = (uintptr_t)ctx->buf_send[wr_id];
	list.length = length;
	list.lkey	= ctx->data_mr->lkey;

	struct ibv_send_wr wr; 
	memset(&wr, 0, sizeof(wr));

	wr.wr_id	  = wr_id;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
	struct ibv_send_wr *bad_wr;
	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::post_send(struct conn_context *conn_ctx, uint64_t offset, int size) {
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
        .send_flags             = IBV_SEND_SIGNALED | IBV_SEND_INLINE
        // .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset,
        // .wr.rdma.rkey           = conn_ctx->remote_mr.rkey
	};

    // wr.wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset;
    // wr.wr.rdma.rkey           = conn_ctx->remote_mr.rkey;

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::post_send_read(struct conn_context *conn_ctx, uint64_t offset, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = READ_WRID,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_READ,
        .send_flags             = IBV_SEND_SIGNALED
        // .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset,
        // .wr.rdma.rkey           = conn_ctx->remote_mr.rkey,
    };
    wr.wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset;
    wr.wr.rdma.rkey           = conn_ctx->remote_mr.rkey;

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}


int RDMAConnectionRC::post_send_write(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset_local,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = WRITE_WRID,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_WRITE,
        .send_flags             = IBV_SEND_SIGNALED
        // .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote,
        // .wr.rdma.rkey           = conn_ctx->remote_mr.rkey,
    };

    wr.wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote;
    wr.wr.rdma.rkey           = conn_ctx->remote_mr.rkey;
    // printf("content is %s\n", (conn_ctx->data_buf) + offset_local);

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::post_send_write_no_signal(struct conn_context *conn_ctx, uint64_t offset_local, uint64_t offset_remote, int size) {
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset_local,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = WRITE_WRID,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_WRITE,
        .send_flags             = 0
        // .wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote,
        // .wr.rdma.rkey           = conn_ctx->remote_mr.rkey,
    };

    wr.wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote;
    wr.wr.rdma.rkey           = conn_ctx->remote_mr.rkey;
    // printf("content is %s\n", (conn_ctx->data_buf) + offset_local);
    // printf("remote addr is %p\n", wr.wr.rdma.remote_addr);

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::pp_post_send_write(struct conn_context *conn_ctx, uint64_t offset_remote, int size, int wr_id) {
    struct ibv_sge list = {
	    .addr   = (uint64_t)(conn_ctx->buf_send[wr_id]),
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    // printf("wr_id is %d, addr: %p, lkey: %d, length: %d\n", wr_id, list.addr, list.lkey, list.length);

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = wr_id,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_WRITE,
        .send_flags             = IBV_SEND_SIGNALED | IBV_SEND_INLINE
    };

    wr.wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote;
    wr.wr.rdma.rkey           = conn_ctx->remote_mr.rkey;

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::pp_post_send_write_no_signal(struct conn_context *conn_ctx, uint64_t offset_remote, int size, int wr_id) {
    struct ibv_sge list = {
	    .addr   = (uint64_t)conn_ctx->buf_send[wr_id],
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = wr_id,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_RDMA_WRITE,
        .send_flags             = IBV_SEND_INLINE
    };

    wr.wr.rdma.remote_addr    = conn_ctx->remote_mr.addr + offset_remote;
    wr.wr.rdma.rkey           = conn_ctx->remote_mr.rkey;

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::post_send_atomic_cmp_and_swap(struct conn_context *conn_ctx, uint64_t offset, int size, uint64_t compare_add, uint64_t swap)
{
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset + 8,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = ATOMIC_WRID,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_ATOMIC_CMP_AND_SWP,
        .send_flags             = IBV_SEND_SIGNALED
        // .wr.atomic.rkey         = conn_ctx->remote_mr.rkey,
        // .wr.atomic.remote_addr  = conn_ctx->remote_mr.addr + offset,
        // .wr.atomic.compare_add  = compare_add,
        // .wr.atomic.swap         = swap,
    };

    wr.wr.atomic.rkey         = conn_ctx->remote_mr.rkey;
    wr.wr.atomic.remote_addr  = conn_ctx->remote_mr.addr + offset;
    wr.wr.atomic.compare_add  = compare_add;
    wr.wr.atomic.swap         = swap;

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::post_send_atomic_fetch_and_add(struct conn_context *conn_ctx, uint64_t offset, int size, uint64_t compare_add)
{
    struct ibv_sge list = {
        .addr = (uint64_t)(conn_ctx->data_buf) + offset,
        .length = size,
        .lkey = conn_ctx->data_mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
        .wr_id	                = ATOMIC_WRID,
        .next                   = NULL,
        .sg_list                = &list,
        .num_sge                = 1,
        .opcode                 = IBV_WR_ATOMIC_FETCH_AND_ADD,
        .send_flags             = IBV_SEND_SIGNALED
        // .wr.atomic.rkey         = conn_ctx->remote_mr.rkey,
        // .wr.atomic.remote_addr  = conn_ctx->remote_mr.addr + offset,
        // .wr.atomic.compare_add  = compare_add,
        // .wr.atomic.swap         = 0,
    };

    wr.wr.atomic.rkey         = conn_ctx->remote_mr.rkey;
    wr.wr.atomic.remote_addr  = conn_ctx->remote_mr.addr + offset;
    wr.wr.atomic.compare_add  = compare_add;
    wr.wr.atomic.swap         = 0;

    return ibv_post_send(conn_ctx->qp, &wr, &bad_wr);
}

int RDMAConnectionRC::wait_completions(struct conn_context *conn_ctx, int wr_id, int cnt)
{
    int finished = 0, count = cnt;
    int wc_batch = count;
    struct ibv_wc wc[wc_batch];

    while (finished < count) {
        // printf("waiting...\n");
        int n = 0;
        while (n <= 0) {
            // printf("poll cq...\n");
            n = ibv_poll_cq(conn_ctx->cq, wc_batch, wc);

            if (n < 0) {
                fprintf(stderr, "Poll CQ failed %d\n", n);
                return -1;
            }
        }

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

uint32_t RDMAConnectionRC::query_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size) {
    post_send_read(conn_ctx, 0, sizeof(uint64_t));
    wait_completions(conn_ctx, READ_WRID, 1);
    return *(uint32_t *)(conn_ctx->data_buf);
}

uint32_t RDMAConnectionRC::acquire_credits(struct dev_context *dev_ctx, struct conn_context *conn_ctx, int size, uint64_t global_credits, uint64_t new_global_credits) {
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
int RDMAConnectionRC::get_server_dest(struct conn_context *conn_ctx, char *server_ip, char *port, int num_qps) {
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

int RDMAConnectionRC::get_server_dlb_dest(struct conn_context *conn_ctx, char *server_ip, char *port, int num_qps) {
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

int RDMAConnectionRC::get_client_dest(struct conn_context *conn_ctx, int num_qps, uint16_t port) {
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
                // conn_ctx[i].local_mr.addr = (uint64_t)(conn_ctx[i].recv_buf);
                // conn_ctx[i].local_mr.rkey = conn_ctx[i].recv_mr->rkey;
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

int RDMAConnectionRC::get_client_dlb_dest(struct conn_context *conn_ctx, int num_qps, uint16_t port) {
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
    printf("Start exchanging details...\n");
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

void RDMAConnectionRC::print_conn_ctx(struct conn_context *conn_ctx)
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

void RDMAConnectionRC::print_dest(struct conn_dest *dest)
{
    if (!dest) {
        return;
    }

    char gid[33] = {0};
    inet_ntop(AF_INET6, &(dest->gid), gid, sizeof(gid));
    printf("LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       dest->lid, dest->qpn, dest->psn, gid);
}

char* RDMAConnectionRC::generate_message(size_t n) {
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

// RDMAConnectionRC::RDMAConnectionRC(int id, int num_qps_in, uint64_t*** dlb_pp_addr, dlb_shared_domain_t** dlb_shared_domain_ptr, int num_dlb_pp_in, int num_client_conns_in)
#if IS_SERVER
	RDMAConnectionRC::RDMAConnectionRC(int id, int num_qps_in, uint64_t*** dlb_pp_addr, dlb_shared_domain_t** dlb_shared_domain_ptr, int num_dlb_pp_in, int num_client_conns_in)
#endif

#if IS_CLIENT
	RDMAConnectionRC::RDMAConnectionRC(int id, int num_qps_in, int num_dlb_pp_in, int num_client_conns_in)
#endif
{
    num_qps = num_qps_in;
    num_dlb_pp = num_dlb_pp_in;
    num_client_conns = num_client_conns_in;
    
    printf("num_qps = %d\n");
	dev_ctx = {
        .ib_dev_name = "mlx5_0",
        .dev_port = 1, // port id = 1
        .ctx = NULL,
        .channel = NULL,
        .pd = NULL,
        .cq = NULL,
        .use_event = false
    };
    
    if (init_device_ctx(&dev_ctx) != 0) {
        fprintf(stderr, "Failed to initialize device context.\n");
    }

    // void* map_base = (unsigned char*)memalign(sysconf(_SC_PAGESIZE), 4*PAGE_SIZE);
    // // void* map_base = malloc(4*sysconf(_SC_PAGESIZE));
    // if (map_base == (void *) -1) {
    //     fprintf(stderr, "Fail to allocate memory\n");
    // }

    // num_qps = 1 + num_dlb_pp + num_client_conns;
    conn_ctxs = (struct conn_context*)malloc(num_qps * sizeof(struct conn_context));
    if (conn_ctxs == NULL) {
        fprintf(stderr, "Failed to allocate memory for connection contexts.\n");
    }

    num_qps_init = 0;
#if IS_SERVER
    if (*(dlb_pp_addr[0]) != (void *) -1 && *dlb_shared_domain_ptr != (void *) -1) {
        for (int i = 0; i < num_qps; i++) {
            conn_ctxs[i].id = i;
            conn_ctxs[i].dev_ctx = &dev_ctx;
            conn_ctxs[i].validate_buf = false;
            conn_ctxs[i].inline_msg = false;
            conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
            conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            conn_ctxs[i].qp = NULL;
            conn_ctxs[i].cq = NULL;
            conn_ctxs[i].data_mr = NULL;
            conn_ctxs[i].dlb_data_qp = -1;
            conn_ctxs[i].dlb_credit_qp = -1;
            conn_ctxs[i].rdma_data_qp = -1;
            
            if (i == 0) {
                // for SNIC and server DLB credit communication
                conn_ctxs[i].data_buf = (unsigned char *)&((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits);
                printf("Server credit address: %p\n", conn_ctxs[i].data_buf);
                printf("Server credit address: %p\n", &((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[1].avail_credits));
                printf("credit pool: %u %u\n", ((*dlb_shared_domain_ptr)->sw_credits.ldb_pools[0].avail_credits), ((*dlb_shared_domain_ptr)->sw_credits.dir_pools[0].avail_credits));
                conn_ctxs[i].data_buf_size = 64;
                conn_ctxs[i].dlb_credit_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
            } else if (i <= num_dlb_pp) {
                // for SNIC to enqueue DLB
                conn_ctxs[i].data_buf = (unsigned char *)(*(dlb_pp_addr[i-1]));
                conn_ctxs[i].data_buf_size = PAGE_SIZE;
                conn_ctxs[i].dlb_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
            } else {
                // for client to RDMA write data to server
                conn_ctxs[i].data_buf = NULL;
                conn_ctxs[i].data_buf_size = bufs_num*BUF_SIZE*2;
                conn_ctxs[i].rdma_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
                conn_ctxs[i].qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
                // printf("Client payload address: %%hhn\n", conn_ctxs[i].data_buf);
            }

            if (init_connection_ctx(&conn_ctxs[i]) != 0) {
                fprintf(stderr, "Failed to initialize connection %d.\n", i);
            }
            num_qps_init++;
        }
    } else {
        printf("[%s()] Faield to allocate MMAP DLB MMIO Space addr: %p\n", __func__, dlb_pp_addr);
    }
#elif IS_CLIENT
    // if (map_base != (void *) -1) {
        for (int i = 0; i < num_qps; i++) {
            conn_ctxs[i].id = i;
            conn_ctxs[i].dev_ctx = &dev_ctx;
            conn_ctxs[i].validate_buf = false;
            conn_ctxs[i].inline_msg = false;
            conn_ctxs[i].qp = NULL;
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
                conn_ctxs[i].dlb_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE;
                // conn_ctxs[i].qp_access_flags = 0;
            } else {
                // use to send rdma data to server
                conn_ctxs[i].data_buf = NULL;
                conn_ctxs[i].data_buf_size = bufs_num*BUF_SIZE*2;
                conn_ctxs[i].rdma_data_qp = i;
                conn_ctxs[i].mr_access_flags = IBV_ACCESS_LOCAL_WRITE;
                // conn_ctxs[i].qp_access_flags = 0;
            }

            if (init_connection_ctx(&conn_ctxs[i]) != 0) {
                fprintf(stderr, "Failed to initialize connection %d.\n", i);
            }
            num_qps_init++;
        }
    // } else {
    //     printf("[%s()] Faield to allocate MMAP DLB MMIO Space addr: %p\n", __func__, map_base);
    // }
#endif
    printf("Initialize %d connection success\n", num_qps_init);

    // start connection
    if (host_type == CLIENT) {
        printf("Connecting server for data transter %s ...\n", server_ip);
        if ((server_sockfd = get_server_dest(&conn_ctxs[0], server_ip, "12340", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
        }
        printf("Connecting snic for metadata %s ...\n", snic_ip);
        if ((snic_sockfd = get_server_dlb_dest(&conn_ctxs[0], snic_ip, "12341", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
        }
    } else if (host_type == SNIC) {
        printf("Connecting server for metadata %s ...\n", server_ip);
        if ((server_sockfd = get_server_dlb_dest(&conn_ctxs[0], server_ip, "12342", num_qps_init) < 0)) {
            fprintf(stderr, "Failed to connect server\n");
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12341) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
        }
    } else if (host_type == SERVER) {
        printf("Waiting for snic's connections ...\n");
        if ((snic_sockfd = get_client_dlb_dest(&conn_ctxs[0], num_qps_init, 12342) < 0)) {
            fprintf(stderr, "Failed to connect snic\n");
        }
        printf("Waiting for client's connections ...\n");
        if ((client_sockfd = get_client_dest(&conn_ctxs[0], num_qps_init, 12340) < 0)) {
            fprintf(stderr, "Failed to connect client\n");
        }
    }
    printf("Connection success\n");

    // connect QPs
    for (int i = 0; i < num_qps_init; i++) {
        printf("==== Connect QP %d ====\n", i);
        print_conn_ctx(&conn_ctxs[i]);
        print_dest(&(conn_ctxs[i].local_dest));
        print_dest(&(conn_ctxs[i].remote_dest));
        if (connect_qps(&conn_ctxs[i]) != 0) {
            fprintf(stderr, "Failed to connect QPs\n");
        }
        printf("Connection QP success\n");
    }

}
#endif
