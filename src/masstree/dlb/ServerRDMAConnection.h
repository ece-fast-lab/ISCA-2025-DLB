/*
 * Copyright (c) 2021 Georgia Institute of Technology.  All rights reserved.
 */

#pragma once

#ifndef _RDMACONNH_
#define _RDMACONNH_

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

using namespace std;
struct pingpong_dest    *rem_dest;


struct pingpong_context {
	struct ibv_context	*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	struct ibv_ah		*ah;
	int			 send_flags;
	int			 rx_depth;
	struct ibv_port_attr	portinfo;
};


struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};

class ServerRDMAConnection {
public:
	ServerRDMAConnection(int id);

	int pp_get_port_info(struct ibv_context *context, int port, struct ibv_port_attr *attr);
	void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
	void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

	struct pingpong_context* pp_init_ctx(struct ibv_device *ib_dev, int rx_depth, int port, int use_event, int id);
	int pp_close_ctx(struct pingpong_context *ctx);
	int pp_post_recv(struct pingpong_context *ctx, int wr_id);
	struct pingpong_dest* pp_server_exch_dest();
	int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn, int sl, int sgid_idx);
	int pp_post_send(struct pingpong_context *ctx, uint32_t qpn, unsigned int length, int wr_id);

//private:
	struct ibv_device       **dev_list;
	struct ibv_device		*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct timeval           start, end;
	uint64_t                 start_clock, end_clock;
	//char                    *ib_devname = NULL;
	const char                *servername = NULL;
	unsigned int             connect_port = 18515;
	int                      ib_port = 1;
	unsigned int             rx_depth = 200+1; //SCALE UP IF LOADING ONLY ONE SERVER CORE
	//unsigned int             size = 20;
	unsigned long long int   iters = 10000000;
	int                      use_event = 0;
	int                      routs = 0;
	int			 			 souts = 0;
	unsigned int             rcnt, scnt = 0;
	int                      num_cq_events = 0;
	int                      sl = 0;
	char			 		 gid[33];	

  	char ib_devname [7] = "mlx5_0";
	int gidx = 3;
	int page_size = sysconf(_SC_PAGESIZE);

	static const int recv_bufs_num = 1024;//768+1;
	char * buf_recv [recv_bufs_num];
	struct ibv_mr* mr_recv [recv_bufs_num];

	char * buf_send [recv_bufs_num];
	struct ibv_mr* mr_send [recv_bufs_num];

};

#endif
