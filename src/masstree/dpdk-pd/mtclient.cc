/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2014 President and Fellows of Harvard College
 * Copyright (c) 2012-2014 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <math.h>
#include <fcntl.h>
#include "kvstats.hh"
#include "kvio.hh"
#include "json.hh"
#include "kvtest.hh"
#include "mtclient.hh"
#include "kvrandom.hh"
#include "clp.h"

#include <infiniband/verbs.h>
#include <linux/types.h>  //for __be32 type
#include "ClientRDMAConnection.cc"
#include <sys/mman.h>

//#define active_thread_num 12+1  		//n loading threads,1 meas. thread 
#define debug 0
#define INTERVAL 10000000 		//RPS MEAS INTERVAL
#define SYNC_INTERVAL 100000 	//RPS MEAS INTERVAL

#define RR 0					//enables round robin request distribution per thread
#define RANDQP 0				//enables random request distribution per thread
#define MEAS_RAND_NUM_GEN_LAT 0	//enables measuring latency of random number generator 

bool printpoll = false; 

enum {
	FIXED = 0,
	NORMAL = 1,
	UNIFORM = 2,
	EXPONENTIAL = 3,
    BIMODAL = 4,
};

pthread_barrier_t barrier; 
pthread_barrierattr_t attr;
int ret; 

int remote_qp0;
int distribution_mode = -1;
double *rps;
unsigned int window_size = 1;
int active_thread_num = 1;
const char* output_dir_const;
char* output_dir;
const char *ib_devname_in_const;
char *ib_devname_in;
int gidx_in;
int remote_qp0_in;
const char* servername_const = "192.168.1.1";
char* servername = "192.168.1.1";
int terminate_load = 0;
int server_threads = 2;

//for bimodal service time distribution
int bimodal_ratio = -1;
int long_query_percent = -1;
int scan_query_numpairs = -1;

//put (1x), get (1x), scan2 (2x), scan16 (5x), scan40 (10x), scan64 (15x), scan95 (20x), scan115 (25x)
//service_times = [1,1,2,5,10,15,20]
//array in hardware = [0,1,1,2,5,10,15,20]
uint8_t scanReqIDs[5] = {3,4,5,6,7};
uint8_t scanNumPairs[5]= {3,18,46,75,105};

uint8_t discreteDist[7]= {0,0,0,0,0,0,0};

/*
inline double now() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}
*/


uint8_t genRandDestQP(uint8_t thread_num) {          

	#if MEAS_RAND_NUM_GEN_LAT
		struct timespec randStart, randEnd;
		clock_gettime(CLOCK_MONOTONIC, &randStart);
	#endif
    	
	static __uint64_t g_lehmer32_state = 0x60bee2bee120fc15;
	g_lehmer32_state *= 0xe4dd58b5;
	uint8_t ret = (g_lehmer32_state >> (32-2*thread_num)) % server_threads;

	#if MEAS_RAND_NUM_GEN_LAT
		clock_gettime(CLOCK_MONOTONIC, &randEnd);
		printf("rand-time = %f \n",(randEnd.tv_sec-randStart.tv_sec)/1e-9 +(randEnd.tv_nsec-randStart.tv_nsec));
	#endif

	return ret;
}



void* create_shared_memory(size_t size) {
  // Our memory buffer will be readable and writable:
  int protection = PROT_READ | PROT_WRITE;

  // The buffer will be shared (meaning other processes can access it), but
  // anonymous (meaning third-party processes cannot obtain an address for it),
  // so only this process and its children will be able to use it:
  int visibility = MAP_SHARED | MAP_ANONYMOUS;

  // The remaining parameters to `mmap()` are not important for this use case,
  // but the manpage for `mmap` explains their purpose.
  return mmap(NULL, size, protection, visibility, -1, 0);
}

//void* shmem = create_shared_memory(8);
//void* shmem2 = create_shared_memory(8);
//void* qp0 = create_shared_memory(8);

const char *serverip = "127.0.0.1";
static Json test_param;

typedef void (*get_async_cb)(struct child *c, struct async *a,
                             bool has_val, const Str &val);
typedef void (*put_async_cb)(struct child *c, struct async *a,
                             int status);
typedef void (*remove_async_cb)(struct child *c, struct async *a,
                                int status);

struct async {
    int cmd; // Cmd_ constant
    unsigned seq;
    union {
        get_async_cb get_fn;
        put_async_cb put_fn;
        remove_async_cb remove_fn;
    };
    char key[16]; // just first 16 bytes
    char wanted[16]; // just first 16 bytes
    int wantedlen;
    int acked;
};
#define MAXWINDOW 512
unsigned window = MAXWINDOW;

struct child {
    int s;
    int udp; // 1 -> udp, 0 -> tcp
    KVConn *conn;
    
    #if RDMA
        ClientRDMAConnection *rdmaconn;
    #endif

    struct async a[MAXWINDOW];

    unsigned seq0_;
    unsigned seq1_;
    unsigned long long nsent_;
    int childno;

    inline void check_flush();
};

void checkasync(struct child *c, int force);
//void checkasync_rdma(struct child *c_);

inline void child::check_flush() {
    if ((seq1_ & ((window - 1) >> 1)) == 0)
        conn->flush();
    while (seq1_ - seq0_ >= window)
        checkasync(this, 1);
}

void aget(struct child *, const Str &key, const Str &wanted, get_async_cb fn);
void aget(struct child *c, long ikey, long iwanted, get_async_cb fn);
void aget_col(struct child *c, const Str& key, int col, const Str& wanted,
              get_async_cb fn);
int get(struct child *c, const Str &key, char *val, int max);

void asyncgetcb(struct child *, struct async *a, bool, const Str &val);
void asyncgetcb_int(struct child *, struct async *a, bool, const Str &val);

void aput(struct child *c, const Str &key, const Str &val,
          put_async_cb fn = 0, const Str &wanted = Str());
void aput_col(struct child *c, const Str &key, int col, const Str &val,
              put_async_cb fn = 0, const Str &wanted = Str());
int put(struct child *c, const Str &key, const Str &val);
void asyncputcb(struct child *, struct async *a, int status);

void aremove(struct child *c, const Str &key, remove_async_cb fn);
bool remove(struct child *c, const Str &key);

void udp1(struct child *);
void w1b(struct child *);
void u1(struct child *);
void over1(struct child *);
void over2(struct child *);
void rec1(struct child *);
void rec2(struct child *);
void cpa(struct child *);
void cpb(struct child *);
void cpc(struct child *);
void cpd(struct child *);
void volt1a(struct child *);
void volt1b(struct child *);
void volt2a(struct child *);
void volt2b(struct child *);
//void scantest(struct child *);

static int children = 1;
static uint64_t nkeys = 0;
static int prefixLen = 0;
static int keylen = 0;
static uint64_t limit = ~uint64_t(0);
double duration = 10;
double duration2 = 0;
int udpflag = 1;
int quiet = 0;
int first_server_port = 2117;
// Should all child processes connects to the same UDP PORT on server
bool share_server_port = false;
volatile bool timeout[2] = {false, false};
int first_local_port = 0;
const char *input = NULL;
static int rsinit_part = 0;
int kvtest_first_seed = 0;
static int rscale_partsz = 0;
static int getratio = -1;
static int minkeyletter = '0';
static int maxkeyletter = '9';


struct kvtest_client {
    kvtest_client(struct child& c)
        : c_(&c) {
    }
    struct child* child() const {
        return c_;
    }

    int id() const {
        return c_->childno;
    }
    int nthreads() const {
        return ::children;
    }
    char minkeyletter() const {
        return ::minkeyletter;
    }
    char maxkeyletter() const {
        return ::maxkeyletter;
    }
    void register_timeouts(int n) {
        (void) n;
    }
    bool timeout(int which) const {
        return ::timeout[which];
    }
    uint64_t limit() const {
        return ::limit;
    }
    int getratio() const {
        assert(::getratio >= 0);
        return ::getratio;
    }
    uint64_t nkeys() const {
        return ::nkeys;
    }
    int keylen() const {
        return ::keylen;
    }
    int prefixLen() const {
        return ::prefixLen;
    }
    Json param(const String& name, Json default_value = Json()) {
        return test_param.count(name) ? test_param.at(name) : default_value;
    }
    double now() const {
        return ::now();
    }

    void get(long ikey, Str *value) {
        quick_istr key(ikey);
        aget(c_, key.string(),
             Str(reinterpret_cast<const char *>(&value), sizeof(value)),
             asyncgetcb);
    }
    void get(const Str &key, int *ivalue) {
        aget(c_, key,
             Str(reinterpret_cast<const char *>(&ivalue), sizeof(ivalue)),
             asyncgetcb_int);
    }
    bool get_sync(long ikey) {
        char got[512];
        quick_istr key(ikey);
        return ::get(c_, key.string(), got, sizeof(got)) >= 0;
    }
    void get_check(long ikey, long iexpected) { //this the async
        #if SYNC
            char key[512], val[512], got[512];
            sprintf(key, "%010ld", ikey);
            sprintf(val, "%ld", iexpected);
            memset(got, 0, sizeof(got));
            ::get(c_, Str(key), got, sizeof(got));
            //if (strcmp(val, got)) {
            //    fprintf(stderr, "key %s, expected %s, got %s\n", key, val, got);
            //    always_assert(0);
            //}
        #else
            #if !RDMA
                aget(c_, ikey, iexpected, 0);
            #endif
        #endif

    }
    void get_check(const char *key, const char *val) {
        aget(c_, Str(key), Str(val), 0);
    }
    void get_check(const Str &key, const Str &val) {
        aget(c_, key, val, 0);
    }
    void get_check_key8(long ikey, long iexpected) {
        quick_istr key(ikey, 8), expected(iexpected);
        aget(c_, key.string(), expected.string(), 0);
    }
    void get_check_key10(long ikey, long iexpected) {
        quick_istr key(ikey, 10), expected(iexpected);
        aget(c_, key.string(), expected.string(), 0);
    }
    void many_get_check(int, long [], long []) {
        assert(0);
    }
    void get_col_check(const Str &key, int col, const Str &value) {
        aget_col(c_, key, col, value, 0);
    }
    void get_col_check(long ikey, int col, long ivalue) {
        quick_istr key(ikey), value(ivalue);
        get_col_check(key.string(), col, value.string());
    }
    void get_col_check_key10(long ikey, int col, long ivalue) {
        quick_istr key(ikey, 10), value(ivalue);
        get_col_check(key.string(), col, value.string());
    }
    void get_check_sync(long ikey, long iexpected) {
        char key[512], val[512], got[512];
        sprintf(key, "%010ld", ikey);
        sprintf(val, "%ld", iexpected);
        memset(got, 0, sizeof(got));
        ::get(c_, Str(key), got, sizeof(got));
        if (strcmp(val, got)) {
            fprintf(stderr, "key %s, expected %s, got %s\n", key, val, got);
            always_assert(0);
        }
    }

    void put(const Str &key, const Str &value) {
        aput(c_, key, value);
    }
    void put(const Str &key, const Str &value, int *status) {
        aput(c_, key, value,
             asyncputcb,
             Str(reinterpret_cast<const char *>(&status), sizeof(status)));
    }
    void put(const char *key, const char *value) {
        aput(c_, Str(key), Str(value));
    }
    void put(const Str &key, long ivalue) {
        quick_istr value(ivalue);
        aput(c_, key, value.string());
    }
    void put(long ikey, long ivalue) {
        #if SYNC
            put_sync(ikey, ivalue);
        #else
            #if !RDMA  
                quick_istr key(ikey), value(ivalue);
                aput(c_, key.string(), value.string());
            #endif
        #endif
    }
    void put_key8(long ikey, long ivalue) {
        quick_istr key(ikey, 8), value(ivalue);
        aput(c_, key.string(), value.string());
    }
    void put_key10(long ikey, long ivalue) {
        quick_istr key(ikey, 10), value(ivalue);
        aput(c_, key.string(), value.string());
    }
    void put_col(const Str &key, int col, const Str &value) {
        aput_col(c_, key, col, value);
    }
    void put_col(long ikey, int col, long ivalue) {
        quick_istr key(ikey), value(ivalue);
        put_col(key.string(), col, value.string());
    }
    void put_col_key10(long ikey, int col, long ivalue) {
        quick_istr key(ikey, 10), value(ivalue);
        put_col(key.string(), col, value.string());
    }
    void put_sync(long ikey, long ivalue) {
        quick_istr key(ikey), value(ivalue);
        //printf("inside put sync");
        ::put(c_, key.string(), value.string());
    }

    #if !SYNC && RDMA
    int put_async() {
        unsigned int iters = ITERS;
        const unsigned int num_bufs = c_->rdmaconn->bufs_num;
        for (unsigned int r = 0; r < num_bufs; ++r) {
            if(!c_->rdmaconn->pp_post_recv(c_->rdmaconn->ctx, r+num_bufs,false)) 
                ++c_->rdmaconn->routs;
        }

        if (c_->rdmaconn->routs != num_bufs) 
            fprintf(stderr,"Begin: Couldn't post receive (%d)\n",c_->rdmaconn->routs);


        for (unsigned int q = 0; q < num_bufs; ++q) {
            int32_t x = (int32_t) rand();
            c_->rdmaconn->wr_id = q;
            //put(x, x + 1);
            quick_istr key(x, 10), value(x+1);
            ::put(c_, key.string(), value.string());
        }

        struct ibv_wc wc[num_bufs*2];
        int ne, i;
        
        while(c_->rdmaconn->rcnt < iters) {
        //for (n = 0; !client.timeout(0) && n <= client.limit(); ++n) {
            if(DEBUG) printf("inside while loop, rcnt = %d, scnt = %d \n", c_->rdmaconn->rcnt, c_->rdmaconn->scnt);

            //poll
            //if receive, send
            do {
                ne = ibv_poll_cq(c_->rdmaconn->ctx->cq, num_bufs*2, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }
            } while (ne < 1);
            
            if(DEBUG) printf("dowhile ne = %d \n",ne);

            for (i = 0; i < ne; ++i) 
            {
                if (wc[i].status != IBV_WC_SUCCESS) 
                {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }
                int a = (int) wc[i].wr_id;
                //if(debug) printf("in switch, a = %d tid = %d \n",a,tid);
                switch (a) 
                {
                    case 0 ... num_bufs-1:
                        if(DEBUG) printf("sent \n");
                        ++c_->rdmaconn->scnt;
                        --c_->rdmaconn->souts;

                        //printf("send complete, rcnt = %d, scnt = %d, routs = %d, souts = %d \n", c_->rdmaconn->rcnt,c_->rdmaconn->scnt,c_->rdmaconn->routs,c_->rdmaconn->souts);
                        c_->conn->out_->n = 0;
                        //if(debug) printf("souts = %d \n",souts); 	
                        break;
                    case num_bufs ... 2*num_bufs-1:
                        if(DEBUG) printf("received \n");
                        ++c_->rdmaconn->rcnt;
                        --c_->rdmaconn->routs;

                        //printf("recv complete, rcnt = %d, scnt = %d, routs = %d, souts = %d \n", c_->rdmaconn->rcnt,c_->rdmaconn->scnt,c_->rdmaconn->routs,c_->rdmaconn->souts);

                        //client.c_->rdmaconn->received = true;
                        //if(debug) printf("request received, a = %d, tid = %d \n",a,tid);
                        //if (--routs <= 1) 
                        //{
                        //if(debug) printf("posting receive request, tid = %d \n",tid);

                        if(!c_->rdmaconn->pp_post_recv(c_->rdmaconn->ctx, a, false)) 
                            ++c_->rdmaconn->routs;
                        if (c_->rdmaconn->routs != num_bufs) 
                            fprintf(stderr,"Couldn't post receive (%d)\n",c_->rdmaconn->routs);

                        if(c_->rdmaconn->scnt < iters) {
                            int32_t x = (int32_t) rand();
                            c_->rdmaconn->wr_id = a-num_bufs;
                            quick_istr key(x, 10), value(x+1);
                            ::put(c_, key.string(), value.string());
                        }
                        
                
                        break;
                }
            }
        }
        return c_->rdmaconn->rcnt;
    }


    int get_async(int32_t *arr, unsigned n) {

        c_->rdmaconn->rcnt = 0;
        c_->rdmaconn->scnt = 0;
        //c_->rdmaconn->souts = 0;
        //c_->rdmaconn->routs = 0;
        const unsigned int num_bufs = c_->rdmaconn->bufs_num;

        for (unsigned int r = 0; r < num_bufs; ++r) {
            if(!c_->rdmaconn->pp_post_recv(c_->rdmaconn->ctx, r+num_bufs, false)) 
                ++c_->rdmaconn->routs;
        }
        if (c_->rdmaconn->routs < num_bufs) 
            fprintf(stderr,"Couldn't post receive (%d)\n",c_->rdmaconn->routs);

        unsigned long g = 0;
        unsigned long ikey;
        for (g = 0; g < num_bufs; ++g) {
            ikey = arr[g];
            c_->rdmaconn->wr_id = g;
            char key[512], val[512], got[512];
            sprintf(key, "%010ld", ikey);
            sprintf(val, "%ld", ikey+1);
            memset(got, 0, sizeof(got));
            ::get(c_, Str(key), got, sizeof(got));
        }
        //printf("inside get async after second loop\n");

        struct ibv_wc wc[num_bufs*2];
        int ne, i;
        
        while(c_->rdmaconn->rcnt < n) {
        //for (n = 0; !client.timeout(0) && n <= client.limit(); ++n) {
            if(DEBUG) printf("inside while loop, n = %d \n", c_->rdmaconn->rcnt);

            //poll
            //if receive, send
            do {
                //printf("inside dowhile tid = %d \n",tid);
                ne = ibv_poll_cq(c_->rdmaconn->ctx->cq, num_bufs*2, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }
            } while (ne < 1);

            for (i = 0; i < ne; ++i) 
            {
                if (wc[i].status != IBV_WC_SUCCESS) 
                {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }
                int a = (int) wc[i].wr_id;
                //if(debug) printf("in switch, a = %d tid = %d \n",a,tid);
                switch (a) 
                {
                    case 0 ... num_bufs-1:
                        ++c_->rdmaconn->scnt;
                        --c_->rdmaconn->souts;

                        //printf("send complete, rcnt = %d, scnt = %d, routs = %d, souts = %d \n", c_->rdmaconn->rcnt,c_->rdmaconn->scnt,c_->rdmaconn->routs,c_->rdmaconn->souts);

                        c_->conn->out_->n = 0;
                        //if(debug) printf("souts = %d \n",souts); 	
                        break;
                    case num_bufs ... 2*num_bufs-1:
                        ++c_->rdmaconn->rcnt;
                        --c_->rdmaconn->routs;

                        //printf("recv complete, rcnt = %d, scnt = %d, routs = %d, souts = %d \n", c_->rdmaconn->rcnt,c_->rdmaconn->scnt,c_->rdmaconn->routs,c_->rdmaconn->souts);

                        //client.c_->rdmaconn->received = true;
                        //if (--routs <= 1) 
                        //{
                        //if(debug) printf("posting receive request, tid = %d \n",tid);


                        if(!c_->rdmaconn->pp_post_recv(c_->rdmaconn->ctx, a, false)) 
                            ++c_->rdmaconn->routs;
                        if (c_->rdmaconn->routs < num_bufs) 
                            fprintf(stderr,"Couldn't post receive (%d)\n",c_->rdmaconn->routs);


                        if(c_->rdmaconn->scnt < n) {
                            c_->rdmaconn->wr_id = a-num_bufs;
                            ikey = arr[g];
                            g++;
                            char key[512], val[512], got[512];
                            sprintf(key, "%010ld", ikey);
                            sprintf(val, "%ld", ikey+1);
                            memset(got, 0, sizeof(got));
                            ::get(c_, Str(key), got, sizeof(got));
                        }
                
                        break;
                }
            }
        }
        return c_->rdmaconn->rcnt;
    }
    #endif

    void remove(const Str &key) {
        aremove(c_, key, 0);
    }
    void remove(long ikey) {
        quick_istr key(ikey);
        remove(key.string());
    }
    bool remove_sync(long ikey) {
        quick_istr key(ikey);
        return ::remove(c_, key.string());
    }

    int ruscale_partsz() const {
        return ::rscale_partsz;
    }
    int ruscale_init_part_no() const {
        return ::rsinit_part;
    }
    long nseqkeys() const {
        return 16 * ::rscale_partsz;
    }
    void wait_all() {
        //#if RDMA 
        //    checkasync_rdma(c_);
        //#else
            checkasync(c_, 2);
        //#endif
    }
    void puts_done() {
    }
    void rcu_quiesce() {
    }
    void notice(String s) {
        if (!quiet) {
            if (!s.empty() && s.back() == '\n')
                s = s.substr(0, -1);
            if (s.empty() || isspace((unsigned char) s[0]))
                fprintf(stderr, "%d%.*s\n", c_->childno, s.length(), s.data());
            else
                fprintf(stderr, "%d %.*s\n", c_->childno, s.length(), s.data());
        }
    }
    void notice(const char *fmt, ...) {
        if (!quiet) {
            va_list val;
            va_start(val, fmt);
            String x;
            if (!*fmt || isspace((unsigned char) *fmt))
                x = String(c_->childno) + fmt;
            else
                x = String(c_->childno) + String(" ") + fmt;
            vfprintf(stderr, x.c_str(), val);
            va_end(val);
        }
    }
    const Json& report(const Json& x) {
        return report_.merge(x);
    }
    void finish() {
        if (!quiet) {
            lcdf::StringAccum sa;
            double dv;
            if (report_.count("puts"))
                sa << " total " << report_.get("puts");
            if (report_.get("puts_per_sec", dv))
                sa.snprintf(100, " %.0f put/s", dv);
            if (report_.get("gets_per_sec", dv))
                sa.snprintf(100, " %.0f get/s", dv);
            if (!sa.empty())
                notice(sa.take_string());
        }
        printf("%s\n", report_.unparse().c_str());
    }
    kvrandom_random rand;
    struct child *c_;
    Json report_;
};


#define TESTRUNNER_CLIENT_TYPE kvtest_client&
#include "testrunner.hh"

struct thread_data{
	ClientRDMAConnection *conn;
	int id;
    //testrunner* test;
};
testrunner* test = 0;

MAKE_TESTRUNNER(rw1, kvtest_rw1(client));
MAKE_TESTRUNNER(rw2, kvtest_rw2(client));
MAKE_TESTRUNNER(rw3, kvtest_rw3(client));
MAKE_TESTRUNNER(rw4, kvtest_rw4(client));
MAKE_TESTRUNNER(rw1fixed, kvtest_rw1fixed(client));
MAKE_TESTRUNNER(rw16, kvtest_rw16(client));
MAKE_TESTRUNNER(sync_rw1, kvtest_sync_rw1(client));
MAKE_TESTRUNNER(r1, kvtest_r1_seed(client, kvtest_first_seed + client.id()));
MAKE_TESTRUNNER(w1, kvtest_w1_seed(client, kvtest_first_seed + client.id()));
MAKE_TESTRUNNER(u1, u1(client.child()));
MAKE_TESTRUNNER(wd1, kvtest_wd1(10000000, 1, client));
MAKE_TESTRUNNER(wd1m1, kvtest_wd1(100000000, 1, client));
MAKE_TESTRUNNER(wd1m2, kvtest_wd1(1000000000, 4, client));
MAKE_TESTRUNNER(wd1check, kvtest_wd1_check(10000000, 1, client));
MAKE_TESTRUNNER(wd1m1check, kvtest_wd1_check(100000000, 1, client));
MAKE_TESTRUNNER(wd1m2check, kvtest_wd1_check(1000000000, 4, client));
MAKE_TESTRUNNER(wd2, kvtest_wd2(client));
MAKE_TESTRUNNER(wd2check, kvtest_wd2_check(client));
MAKE_TESTRUNNER(tri1, kvtest_tri1(10000000, 1, client));
MAKE_TESTRUNNER(tri1check, kvtest_tri1_check(10000000, 1, client));
MAKE_TESTRUNNER(same, kvtest_same(client));
MAKE_TESTRUNNER(wcol1, kvtest_wcol1at(client, client.id() % 24, kvtest_first_seed + client.id() % 48, 5000000));
MAKE_TESTRUNNER(rcol1, kvtest_rcol1at(client, client.id() % 24, kvtest_first_seed + client.id() % 48, 5000000));
MAKE_TESTRUNNER(wcol1o1, kvtest_wcol1at(client, (client.id() + 1) % 24, kvtest_first_seed + client.id() % 48, 5000000));
MAKE_TESTRUNNER(rcol1o1, kvtest_rcol1at(client, (client.id() + 1) % 24, kvtest_first_seed + client.id() % 48, 5000000));
MAKE_TESTRUNNER(wcol1o2, kvtest_wcol1at(client, (client.id() + 2) % 24, kvtest_first_seed + client.id() % 48, 5000000));
MAKE_TESTRUNNER(rcol1o2, kvtest_rcol1at(client, (client.id() + 2) % 24, kvtest_first_seed + client.id() % 48, 5000000));
MAKE_TESTRUNNER(over1, over1(client.child()));
MAKE_TESTRUNNER(over2, over2(client.child()));
MAKE_TESTRUNNER(rec1, rec1(client.child()));
MAKE_TESTRUNNER(rec2, rec2(client.child()));
MAKE_TESTRUNNER(cpa, cpa(client.child()));
MAKE_TESTRUNNER(cpb, cpb(client.child()));
MAKE_TESTRUNNER(cpc, cpc(client.child()));
MAKE_TESTRUNNER(cpd, cpd(client.child()));
MAKE_TESTRUNNER(scantest, scantest(client));
MAKE_TESTRUNNER(wscale, kvtest_wscale(client));
MAKE_TESTRUNNER(ruscale_init, kvtest_ruscale_init(client));
MAKE_TESTRUNNER(rscale, kvtest_rscale(client));
MAKE_TESTRUNNER(uscale, kvtest_uscale(client));
MAKE_TESTRUNNER(long_init, kvtest_long_init(client));
MAKE_TESTRUNNER(long_go, kvtest_long_go(client));
MAKE_TESTRUNNER(udp1, kvtest_udp1(client));

void run_child(testrunner*, int childno);
//void* run_child(void *x);

void
usage()
{
  fprintf(stderr, "Usage: mtclient [-s serverip] [-w window] [--udp] "\
          "[-j nchildren] [-d duration] [--ssp] [--flp first_local_port] "\
          "[--fsp first_server_port] [-i json_input]\nTests:\n");
  testrunner::print_names(stderr, 5);
  exit(1);
}

void
settimeout(int)
{
  if (!timeout[0]) {
    timeout[0] = true;
    if (duration2)
        alarm((int) ceil(duration2));
  } else
    timeout[1] = true;
}

enum { clp_val_suffixdouble = Clp_ValFirstUser };
enum { opt_threads = 1, opt_threads_deprecated, opt_duration, opt_duration2,
       opt_window, opt_server, opt_first_server_port, opt_quiet, opt_udp,
       opt_first_local_port, opt_share_server_port, opt_input,
       opt_rsinit_part, opt_first_seed, opt_rscale_partsz, opt_keylen,
       opt_limit, opt_prefix_len, opt_nkeys, opt_get_ratio, opt_minkeyletter,
       opt_maxkeyletter, opt_nofork, opt_output_dir, opt_gidx, opt_ib_devname_in, opt_remote_qp0_in, opt_bimodal_ratio, opt_long_query_percent, opt_server_threads};
static const Clp_Option options[] = {
    { "threads", 'j', opt_threads, Clp_ValInt, 0 },
    { 0, 'n', opt_threads_deprecated, Clp_ValInt, 0 },
    { "duration", 0, opt_duration, Clp_ValDouble, 0 },
    { "duration2", 0, opt_duration2, Clp_ValDouble, 0 },
    { "d2", 0, opt_duration2, Clp_ValDouble, 0 },
    { "window", 'w', opt_window, Clp_ValUnsigned, 0 },
    { "server-ip", 's', opt_server, Clp_ValString, 0 },
    { "first-server-port", 0, opt_first_server_port, Clp_ValInt, 0 },
    { "fsp", 0, opt_first_server_port, Clp_ValInt, 0 },
    { "quiet", 0, opt_quiet, 0, Clp_Negate },
    { "udp", 'u', opt_udp, 0, Clp_Negate },
    { "first-local-port", 0, opt_first_local_port, Clp_ValInt, 0 },
    { "flp", 0, opt_first_local_port, Clp_ValInt, 0 },
    { "share-server-port", 0, opt_share_server_port, 0, Clp_Negate },
    { "ssp", 0, opt_share_server_port, 0, Clp_Negate },
    { "input", 'i', opt_input, Clp_ValString, 0 },
    { "rsinit_part", 0, opt_rsinit_part, Clp_ValInt, 0 },
    { "first_seed", 0, opt_first_seed, Clp_ValInt, 0 },
    { "rscale_partsz", 0, opt_rscale_partsz, Clp_ValInt, 0 },
    { "keylen", 0, opt_keylen, Clp_ValInt, 0 },
    { "limit", 0, opt_limit, clp_val_suffixdouble, 0 },
    { "prefixLen", 0, opt_prefix_len, Clp_ValInt, 0 },
    { "nkeys", 0, opt_nkeys, Clp_ValInt, 0 },
    { "getratio", 0, opt_get_ratio, Clp_ValInt, 0 },
    { "minkeyletter", 0, opt_minkeyletter, Clp_ValString, 0 },
    { "maxkeyletter", 0, opt_maxkeyletter, Clp_ValString, 0 },
    { "no-fork", 0, opt_nofork, 0, 0 },
    { "output_dir", 'd', opt_output_dir, Clp_ValString, 0 },
    { "gidx_in", 'g', opt_gidx, Clp_ValInt, 0 },
    { "ib_devname_in", 'v', opt_ib_devname_in, Clp_ValString, 0 },
    { "remote_qp0_in", 'q', opt_remote_qp0_in, Clp_ValInt, 0 },
    { "bimodal_ratio", 'r', opt_bimodal_ratio, Clp_ValInt, 0 },
    { "long_query_percent", 'p', opt_long_query_percent, Clp_ValInt, 0 },
    { "server_threads", 'k', opt_server_threads, Clp_ValInt, 0 }
};

uint8_t sendCmd(struct child &c, ClientRDMAConnection *conn, uint8_t thread_num)
{
    /*
    std::random_device rd; // obtain a random number from hardware
    std::mt19937 gen(rd()); // seed the generator
    std::uniform_int_distribution<> d(0, conn->iters*(active_thread_num-1)); // define the range
    int32_t k = d(gen);
    //printf("key = %u \n",k);

    std::random_device rd1;
    std::mt19937 gen1(rd1());
    std::discrete_distribution<> d1({0 , 100-long_query_percent, long_query_percent});
    int cmdType = d1(gen1);
    */

   /*
    uint32_t k = conn->random_keys[conn->key_number];
    uint8_t cmdType = conn->op_type[conn->key_number];
    conn->key_number++;
    */



	//static __uint64_t g_lehmer32_state = 0x60bee2bee120fc15;
	//g_lehmer32_state *= 0xe4dd58b5;
	//uint32_t k = (g_lehmer32_state >> (32-2*thread_num)) % (conn->iters*(active_thread_num-1));


    uint32_t k = conn->random_keys[conn->key_number];

    //printf("key = %d \n",k);

    uint8_t cmdType = conn->op_type[conn->key_number];
    conn->key_number++;
    if(conn->key_number == conn->iters) conn->key_number = 0;


    //printf("cmdType = %lu \n",cmdType);

    //int cmdType = 1;
    //std::random_device rd1;
    //std::mt19937 gen1(rd1());
    //std::discrete_distribution<> d1({0 , 100-long_query_percent, long_query_percent});
    //uint8_t cmdType = d1(gen1);

    //int32_t k = conn->iters*thread_num;
    /*
    int cmdType = 1;

    uint32_t k = (conn->key_offset+conn->key_number);
    conn->key_number++;
    if(k == conn->iters*(active_thread_num-1)) {
        conn->key_number = 0;
        conn->key_offset = 0;
    }
    else conn->key_number++;
    */

   uint8_t reqID = 0;
   switch(cmdType) {
    case 0:
    {
        //PUT
        //printf("PUT \n");
        quick_istr key(k), value(k+1);
        c.conn->sendputwhole(key.string(), value.string(), 0);
        reqID = 1;
        break;
    }
    case 1:
    {
        //GET
        //printf("GET \n");
        quick_istr key(k);
        c.conn->sendgetwhole(key.string(), 0);
        reqID = 2;
        break;
    }
    case 2 ... 6:
    {
        //SCAN
        //printf("T%d, SCAN %d \n",thread_num, scanNumPairs[cmdType-2]);
        //printf("scan_key = %d, \n",scan_key);
        quick_istr key(k);
        c.conn->sendscanwhole(key.string(), scanNumPairs[cmdType-2], 1);
        reqID = scanReqIDs[cmdType-2];
        break;
    }
    default: 
        printf("invalid command type %d \n",cmdType);
   }
   return reqID;
}

uint8_t sendGet(struct child &c, ClientRDMAConnection *conn, uint8_t thread_num)
{

    /*
    std::random_device rd; // obtain a random number from hardware
    std::mt19937 gen(rd()); // seed the generator
    std::uniform_int_distribution<> d(0, conn->iters*(active_thread_num-1)); // define the range
    int32_t k = d(gen);
    //printf("key = %u \n",k);
    // GET
    //printf("GET \n");
    */

    //int32_t k = conn->iters*thread_num;

    /*
    uint32_t k = conn->random_keys[conn->key_number];
    conn->key_number++;
    */

   	static __uint64_t g_lehmer32_state = 0x60bee2bee120fc15;
	g_lehmer32_state *= 0xe4dd58b5;
	uint32_t k = (g_lehmer32_state >> (32-2*thread_num)) % (conn->iters*(active_thread_num-1));
    //printf("key =  %lu \n",k);

    /*
    int32_t k = (conn->key_offset+conn->key_number);
    conn->key_number++;
    if(k == conn->iters*(active_thread_num-1)) {
        conn->key_number = 0;
        conn->key_offset = 0;
    }
    else conn->key_number++;
    */

    quick_istr key(k);
    c.conn->sendgetwhole(key.string(), 0);
    return 2;
}

void* client_threadfunc(void* x) {
	struct thread_data *tdata = (struct thread_data*) x;
	int thread_num = tdata->id;


	ClientRDMAConnection *conn = tdata->conn;
    struct child c;
    bzero(&c, sizeof(c));
    c.childno = thread_num;
    c.conn = new KVConn(c.s, !udpflag);
    kvtest_client client(c);

    client.rand.seed(thread_num);
    //printf("before put async \n");

	if(thread_num == 0) {
		conn = new ClientRDMAConnection(thread_num,0,ib_devname_in,gidx_in,remote_qp0_in,servername);
		remote_qp0 = rem_dest->qpn;
	}
	else if(thread_num == active_thread_num - 1) {
		conn = new ClientRDMAConnection(thread_num,1,ib_devname_in,gidx_in,remote_qp0_in,servername);
		conn->measured_latency = (double *)malloc(sizeof(double)*(conn->sync_iters));
	}
	else conn = new ClientRDMAConnection(thread_num,0,ib_devname_in,gidx_in,remote_qp0_in,servername);
	int offset = thread_num%server_threads;
	//printf("thread_num = %d, offset = %d \n", thread_num, offset);

	cpu_set_t cpuset;
    CPU_ZERO(&cpuset);       //clears the cpuset
    CPU_SET(thread_num, &cpuset);  //set CPU 2 on cpuset
	sched_setaffinity(0, sizeof(cpuset), &cpuset);


    /////////////////////////////////////////////////////////////////////
    //generate random keys

    //std::random_device rd; // obtain a random number from hardware
    //std::mt19937 gen(rd()); // seed the generator
    //std::uniform_int_distribution<> d(0, conn->iters*(active_thread_num-1)); // define the range

    std::random_device rd1;
    std::mt19937 gen1(rd1());

    if(bimodal_ratio == 50) {
        discreteDist[1] = 50;
        discreteDist[0] = 50;
    }
    else if(bimodal_ratio == 33) {
        discreteDist[1] = 34;
        discreteDist[6] = 33;
        discreteDist[4] = 33;
    }    
    else if(bimodal_ratio == 20) {
        discreteDist[1] = 100-long_query_percent;
        discreteDist[6] = long_query_percent;
    }
    else if(bimodal_ratio == 15) {
        discreteDist[1] = 100-long_query_percent;
        discreteDist[5] = long_query_percent;
    }   
    else if(bimodal_ratio == 10) {
        discreteDist[1] = 100-long_query_percent;
        discreteDist[4] = long_query_percent;
    }
    else if(bimodal_ratio == 5) {
        discreteDist[1] = 100-long_query_percent;
        discreteDist[3] = long_query_percent;
    }
    else if(bimodal_ratio == 2) {
        discreteDist[1] = 100-long_query_percent;
        discreteDist[2] = long_query_percent;
    }
    else {
        printf("INVALID BIMODAL DISPARITY RATIO! Valid options include: 2,5,10,15,20 \n");
    }
    
    std::discrete_distribution<> d1({discreteDist[0],discreteDist[1],discreteDist[2],discreteDist[3],discreteDist[4],discreteDist[5],discreteDist[6]});        

    conn->random_keys = (uint32_t*)malloc(conn->iters*sizeof(uint32_t));
    conn->op_type = (uint8_t*)malloc(conn->iters*sizeof(uint8_t));

    for (int ii = 0; ii < conn->iters; ii++) {
        //conn->random_keys[ii] = d(gen);
        conn->op_type[ii] = d1(gen1);
    }

    static __uint64_t g_lehmer32_state = 0x60bee2bee120fc15;

    for (int jj = 0; jj < conn->iters; jj++) {
        g_lehmer32_state *= 0xe4dd58b5;
        uint32_t k = (g_lehmer32_state >> (32-2*thread_num)) % (conn->iters*(active_thread_num-1));
        //conn->random_keys[ii] = d(gen);
        conn->random_keys[jj] = k;
    }


    /////////////////////////////////////////////////////////////////////

	sleep(1);
    ret = pthread_barrier_wait(&barrier);

    // printf("starting PUTs \n");

	conn->dest_qpn = remote_qp0;
    //printf("T%d key offset = %d \n", thread_num, conn->key_offset);
	struct timeval start, end;

	if(thread_num == active_thread_num - 1) {
        //conn->key_offset = (conn->iters*thread_num)/2;
        struct timespec requestStart, requestEnd;
        const int num_bufs = conn->sync_bufs_num;
        //printf("Meas: T%d, num_bufs = %d \n", thread_num, num_bufs);

        ret = pthread_barrier_wait(&barrier);

        for (unsigned int r = 0; r < num_bufs; ++r) {
            if(!conn->pp_post_recv(conn->ctx, r+num_bufs, true)) 
                ++conn->routs;
        }
        if (conn->routs != num_bufs) fprintf(stderr,"Couldn't post initial receive (%d)\n",conn->routs);
            
        #if debug
            printf("T%d - remote_qp0 = %d \n",thread_num,remote_qp0);
        #endif

        //sending packet to clear FPGA state
        for (unsigned int i = 0; i < num_bufs; i++) {
  
            sendGet(c, conn, thread_num);
            
            conn->buf_send[i][0] = 255;
            conn->buf_send[i][1] = 255;

            memcpy(conn->buf_send[i]+2, c.conn->out_->buf, c.conn->out_->n);
            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+2, i);
            if (success == EINVAL) printf("Invalid value provided in wr \n");
            else if (success == ENOMEM)	printf("Send Queue %d is full or not enough resources to complete this operation \n",thread_num);
            else if (success == EFAULT) printf("Invalid value provided in qp \n");
            else if (success != 0) {
                printf("success = %d, \n",success);
                fprintf(stderr, "Couldn't post send 2 \n");
                //return 1;
            }
            else {
                ++conn->souts;
                c.conn->out_->n = 0;

                #if debug 
                    printf("T%d - send posted... souts = %d, \n",thread_num,conn->souts);
                #endif
            }

            #if debug
                printf("T%d - posted send, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                for(int p = 0; p < 50 ; p++)
                {	
                    printf("%x",conn->buf_send[i][p]);
                }
                printf("\n");
            #endif

            #if RR
                offset = (offset+1)%SERVER_THREADS;
                conn->dest_qpn = remote_qp0+offset;
            #endif	

            #if RANDQP
                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
            #endif
        }

        while (conn->rcnt < 1 || conn->scnt < 1) {
            struct ibv_wc wc[num_bufs*2];
            int ne, i;
            if (terminate_load == 1) break;
            do {
                ne = ibv_poll_cq(conn->ctx->cq, 2*num_bufs, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }

                #if debug
                    if(printpoll) {
                        printf("thread_num %d polling \n",thread_num);
                        printpoll = false;
                    }
                #endif

            } while (!conn->use_event && ne < 1);

            for (i = 0; i < ne; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }

                int a = (int) wc[i].wr_id;
                switch (a) {
                    case 0 ... num_bufs-1: // SEND_WRID
                        clock_gettime(CLOCK_MONOTONIC, &requestStart);
                        ++conn->scnt;
                        --conn->souts;
                        #if debug
                            printf("T%d - send complete, a = %d, rcnt = %d, scnt = %d, routs = %d, souts = %d  \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                        #endif
                        break;

                    case num_bufs ... 2*num_bufs-1:
                        clock_gettime(CLOCK_MONOTONIC, &requestEnd);
                        conn->measured_latency[conn->rcnt] = (requestEnd.tv_sec-requestStart.tv_sec)/1e-6 +(requestEnd.tv_nsec-requestStart.tv_nsec)/1e3;
                        ++conn->rcnt;
                        --conn->routs;
                        #if debug
                            printf("T%d - recv complete, a = %d, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                            for(int p = 0; p < 50 ; p++)
                            {	
                                printf("%x",conn->buf_recv[a-num_bufs][40+p]);
                            }
                            printf("\n");
                        #endif
                    

                        if(!conn->pp_post_recv(conn->ctx, a, true)) ++conn->routs;
                        if (conn->routs != num_bufs) fprintf(stderr,"Loading thread %d couldn't post receive (%d)\n", thread_num, conn->routs);

                        if(conn->rcnt < conn->sync_iters) {

                            uint8_t reqID = sendGet(c, conn, thread_num);

                            conn->buf_send[a-num_bufs][0] = 0;
                            conn->buf_send[a-num_bufs][1] = reqID;

                            memcpy(conn->buf_send[a-num_bufs]+2, c.conn->out_->buf, c.conn->out_->n);

                            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+2, a-num_bufs);
                            if (success == EINVAL) printf("Invalid value provided in wr \n");
                            else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                            else if (success == EFAULT) printf("Invalid value provided in qp \n");
                            else if (success != 0) {
                                printf("success = %d, \n",success);
                                fprintf(stderr, "Couldn't post send 2 \n");
                            }
                            else {
                                ++conn->souts;
                                c.conn->out_->n = 0;
                                #if debug
                                    printf("T%d - send posted... souts = %d, \n",thread_num,conn->souts);
                                #endif
                            }

                            #if debug
                                printf("T%d - posted send, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                                for(int p = 0; p < 50 ; p++)
                                {	
                                    printf("%x",conn->buf_send[a-num_bufs][p]);
                                }
                                printf("\n");
                            #endif

                            #if RR
                                offset = (offset+1)%SERVER_THREADS;
                                conn->dest_qpn = remote_qp0+offset;
                            #endif	

                            #if RANDQP
                                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
                            #endif
                        }
                    
                        break;

                    default:
                        fprintf(stderr, "Completion for unknown wr_id %d\n",
                            (int) wc[i].wr_id);
                }
            
                #if debug 
                    printf("Thread %d rcnt = %d , scnt = %d \n",thread_num, conn->rcnt,conn->scnt);
                #endif
            }
        }

        printf("cleared FPGA state \n");
        ret = pthread_barrier_wait(&barrier);


        conn->key_number = 0;
        sleep(5);
        printf("meas thread passed barrier \n");
        
        /*
        for (unsigned int i = 0; i < num_bufs; i++) {
  
            sendGet(c, conn, thread_num);
            
            conn->buf_send[i][0] = 0;
            conn->buf_send[i][1] = 0;
            conn->buf_send[i][2] = 0;
            conn->buf_send[i][3] = 0;

            memcpy(conn->buf_send[i]+4, c.conn->out_->buf, c.conn->out_->n);
            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+4, i);
            if (success == EINVAL) printf("Invalid value provided in wr \n");
            else if (success == ENOMEM)	printf("Send Queue %d is full or not enough resources to complete this operation \n",thread_num);
            else if (success == EFAULT) printf("Invalid value provided in qp \n");
            else if (success != 0) {
                printf("success = %d, \n",success);
                fprintf(stderr, "Couldn't post send 2 \n");
                //return 1;
            }
            else {
                ++conn->souts;
                c.conn->out_->n = 0;

                #if debug 
                    printf("send posted... souts = %d, \n",conn->souts);
                #endif
            }

            #if RR
                offset = (offset+1)%SERVER_THREADS;
                conn->dest_qpn = remote_qp0+offset;
            #endif	

            #if RANDQP
                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
            #endif
        }
*/        

        if (gettimeofday(&start, NULL)) {	
            perror("gettimeofday");
        }

        double prev_clock = now();
        #if debug 
            bool printpoll = false; 
        #endif

        while (conn->rcnt < conn->sync_iters || conn->scnt < conn->sync_iters) {
            struct ibv_wc wc[num_bufs*2];
            int ne, i;
            if (terminate_load == 1) break;
            do {
                if (terminate_load == 1) break;
                ne = ibv_poll_cq(conn->ctx->cq, 2*num_bufs, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }

                #if debug
                    if(printpoll) {
                        printf("thread_num %d polling \n",thread_num);
                        printpoll = false;
                    }
                #endif

            } while (!conn->use_event && ne < 1);

            for (i = 0; i < ne; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }

                int a = (int) wc[i].wr_id;
                switch (a) {
                    case 0 ... num_bufs-1: // SEND_WRID
                        clock_gettime(CLOCK_MONOTONIC, &requestStart);
                        ++conn->scnt;
                        --conn->souts;
                        //printf("T%d - send complete, a = %d, rcnt = %d, scnt = %d, routs = %d, souts = %d  \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);

                        #if debug
                            printf("T%d - send complete, a = %d, rcnt = %d, scnt = %d, routs = %d, souts = %d  \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                        #endif
                        break;

                    case num_bufs ... 2*num_bufs-1:
                        clock_gettime(CLOCK_MONOTONIC, &requestEnd);
                        conn->measured_latency[conn->rcnt] = (requestEnd.tv_sec-requestStart.tv_sec)/1e-6 +(requestEnd.tv_nsec-requestStart.tv_nsec)/1e3;
                        ++conn->rcnt;
                        --conn->routs;

                        #if debug
                            printf("T%d - recv complete, a = %d, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                            for(int p = 0; p < 50 ; p++)
                            {	
                                printf("%x",conn->buf_recv[a-num_bufs][40+p]);
                            }
                            printf("\n");
                        #endif
                        #if debug
                            if(conn->rcnt % SYNC_INTERVAL == 0) {
                                double curr_clock = now();
                                printf("Meas: T%d - %f rps, rcnt = %d, scnt = %d \n",thread_num,SYNC_INTERVAL/(curr_clock-prev_clock),conn->rcnt,conn->scnt);
                                prev_clock = curr_clock;
                            }
                        #endif
                        //if(conn->rcnt > conn->iters - INTERVAL/1000000 && conn->rcnt % 1 == 0) printf("T%d - rcnt = %d, scnt = %d \n",thread_num,conn->rcnt,conn->scnt);


                        if(!conn->pp_post_recv(conn->ctx, a, true)) ++conn->routs;
                        if (conn->routs != num_bufs) fprintf(stderr,"Loading thread %d couldn't post receive (%d)\n", thread_num, conn->routs);

                        if(conn->rcnt < conn->sync_iters) {

                            uint8_t reqID = sendGet(c, conn, thread_num);

                            conn->buf_send[a-num_bufs][0] = 0;
                            conn->buf_send[a-num_bufs][1] = reqID;

                            memcpy(conn->buf_send[a-num_bufs]+2, c.conn->out_->buf, c.conn->out_->n);

                            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+2, a-num_bufs);
                            if (success == EINVAL) printf("Invalid value provided in wr \n");
                            else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                            else if (success == EFAULT) printf("Invalid value provided in qp \n");
                            else if (success != 0) {
                                printf("success = %d, \n",success);
                                fprintf(stderr, "Couldn't post send 2 \n");
                            }
                            else {
                                ++conn->souts;
                                c.conn->out_->n = 0;
                                #if debug
                                    printf("T%d - send posted... souts = %d, \n",thread_num,conn->souts);
                                #endif
                            }

                            #if debug
                                printf("T%d - posted send, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                                for(int p = 0; p < 50 ; p++)
                                {	
                                    printf("%x",conn->buf_send[a-num_bufs][p]);
                                }
                                printf("\n");
                            #endif

                            #if RR
                                offset = (offset+1)%SERVER_THREADS;
                                conn->dest_qpn = remote_qp0+offset;
                            #endif	

                            #if RANDQP
                                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
                            #endif
                        }
                    
                        break;

                    default:
                        fprintf(stderr, "Completion for unknown wr_id %d\n",
                            (int) wc[i].wr_id);
                }
            
                #if debug 
                    printf("Thread %d rcnt = %d , scnt = %d \n",thread_num, conn->rcnt,conn->scnt);
                #endif
            }
        }
        if (gettimeofday(&end, NULL)) {
			perror("gettimeofday");
		}
    }
    else {
        conn->key_offset = conn->iters*thread_num;
        const int num_bufs = conn->bufs_num;
        //printf("T%d, num_bufs = %d \n", thread_num, num_bufs);

        for (unsigned int r = 0; r < window_size; ++r) {
            if(!conn->pp_post_recv(conn->ctx, r+num_bufs, false)) 
                ++conn->routs;
        }
        if (conn->routs != window_size) fprintf(stderr,"Couldn't post initial receive (%d)\n",conn->routs);
            
        #if debug
            printf("T%d - remote_qp0 = %d \n",thread_num,remote_qp0);
        #endif

        for (unsigned int i = 0; i < window_size; i++) {

            int32_t x = conn->key_offset+conn->key_number;
            conn->key_number++;
            quick_istr key(x), value(x+1);
            c.conn->sendputwhole(key.string(), value.string(), 0);  
              
            conn->buf_send[i][0] = 0;
            conn->buf_send[i][1] = 0;

            memcpy(conn->buf_send[i]+2, c.conn->out_->buf, c.conn->out_->n);
            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+2, i);
            if (success == EINVAL) printf("Invalid value provided in wr \n");
            else if (success == ENOMEM)	printf("Send Queue %d is full or not enough resources to complete this operation \n",thread_num);
            else if (success == EFAULT) printf("Invalid value provided in qp \n");
            else if (success != 0) {
                printf("success = %d, \n",success);
                fprintf(stderr, "Couldn't post send 2 \n");
                //return 1;
            }
            else {
                ++conn->souts;
                c.conn->out_->n = 0;

                #if debug 
                    printf("T%d - send posted... souts = %d, \n",thread_num,conn->souts);
                #endif
            }

            #if debug
                printf("T%d - posted send, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                for(int p = 0; p < 50 ; p++)
                {	
                    printf("%x",conn->buf_send[i][p]);
                }
                printf("\n");
            #endif

            #if RR
                offset = (offset+1)%SERVER_THREADS;
                conn->dest_qpn = remote_qp0+offset;
            #endif	

            #if RANDQP
                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
            #endif
        }
        
        double prev_clock = now();
        #if debug 
            printpoll = true; 
        #endif

        while (conn->rcnt < conn->iters || conn->scnt < conn->iters) {
            if (terminate_load == 1) break;

            struct ibv_wc wc[num_bufs*2];
            int ne, i;
            do {
                if (terminate_load == 1) break;
                ne = ibv_poll_cq(conn->ctx->cq, 2*num_bufs, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }

                #if debug
                    if(printpoll) {
                        printf("thread_num %d polling \n",thread_num);
                        printpoll = false;
                    }
                #endif

            } while (!conn->use_event && ne < 1);

            for (i = 0; i < ne; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }

                int a = (int) wc[i].wr_id;
                switch (a) {
                    case 0 ... num_bufs-1: { // SEND_WRID
                        ++conn->scnt;
                        --conn->souts;
                        #if debug
                            printf("T%d - send complete, a = %d, rcnt = %d, scnt = %d, routs = %d, souts = %d  \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                        #endif
                        break;
                    }
                    case num_bufs ... 2*num_bufs-1: {
                        ++conn->rcnt;
                        --conn->routs;

                        #if debug
                            printf("T%d - recv complete, a = %d, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                            for(int p = 0; p < 50 ; p++)
                            {	
                                printf("%x",conn->buf_recv[a-num_bufs][40+p]);
                            }
                            printf("\n");
                        #endif
                        #if debug
                            if(conn->rcnt % INTERVAL == 0) {
                                double curr_clock = now();
                                printf("T%d - %f rps, rcnt = %d, scnt = %d \n",thread_num,INTERVAL/(curr_clock-prev_clock),conn->rcnt,conn->scnt);
                                prev_clock = curr_clock;
                            }
                        #endif
                        //if(conn->rcnt > conn->iters - INTERVAL/1000000 && conn->rcnt % 1 == 0) printf("T%d - rcnt = %d, scnt = %d \n",thread_num,conn->rcnt,conn->scnt);


                        if(!conn->pp_post_recv(conn->ctx, a, false)) ++conn->routs;
                        if (conn->routs != window_size) fprintf(stderr,"Loading thread %d couldn't post receive (%d)\n", thread_num, conn->routs);

                        //if(conn->startedLoad == false) {

                            int32_t x = conn->key_offset+conn->key_number; //(int32_t) rand();
                            conn->key_number++;
                            quick_istr key(x), value(x+1);

                            c.conn->sendputwhole(key.string(), value.string(), 0);   

                            conn->buf_send[a-num_bufs][0] = 0;
                            conn->buf_send[a-num_bufs][1] = 0;

                            memcpy(conn->buf_send[a-num_bufs]+2, c.conn->out_->buf, c.conn->out_->n);

                            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+2, a-num_bufs);
                            if (success == EINVAL) printf("Invalid value provided in wr \n");
                            else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                            else if (success == EFAULT) printf("Invalid value provided in qp \n");
                            else if (success != 0) {
                                printf("success = %d, \n",success);
                                fprintf(stderr, "Couldn't post send 2 \n");
                            }
                            else {
                                ++conn->souts;
                                c.conn->out_->n = 0;
                                #if debug
                                    printf("T%d - send posted... souts = %d, \n",thread_num,conn->souts);
                                #endif
                            }


                            #if debug
                                printf("T%d - posted send, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                                for(int p = 0; p < 50 ; p++)
                                {	
                                    printf("%x",conn->buf_send[a-num_bufs][p]);
                                }
                                printf("\n");
                            #endif

                            #if RR
                                offset = (offset+1)%SERVER_THREADS;
                                conn->dest_qpn = remote_qp0+offset;
                            #endif	

                            #if RANDQP
                                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
                            #endif
                        //}
                        /*
                        else {
                            
                            std::random_device rd; // obtain a random number from hardware
                            std::mt19937 gen(rd()); // seed the generator
                            std::uniform_int_distribution<> distr(0, conn->iters*(active_thread_num-1)); // define the range
                            // GET
                            //quick_istr key(distr(gen));
                            //c.conn->sendgetwhole(key.string(), 0);
                            
                            //PUT
                            //int32_t x = distr(gen);
                            //quick_istr key(x), value(x+1);
                            //c.conn->sendputwhole(key.string(), value.string(), 0);    
                            
                            //SCAN
                            quick_istr key(distr(gen));
                            c.conn->sendscanwhole(key.string(), 4, 0);

                            memcpy(conn->buf_send[a-num_bufs], c.conn->out_->buf, c.conn->out_->n);

                            int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n, a-num_bufs);
                            if (success == EINVAL) printf("Invalid value provided in wr \n");
                            else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                            else if (success == EFAULT) printf("Invalid value provided in qp \n");
                            else if (success != 0) {
                                printf("success = %d, \n",success);
                                fprintf(stderr, "Couldn't post send 2 \n");
                            }
                            else {
                                ++conn->souts;
                                c.conn->out_->n = 0;
                                #if debug
                                    printf("send posted... souts = %d, \n",conn->souts);
                                #endif
                            }

                            #if RR
                                offset = (offset+1)%SERVER_THREADS;
                                conn->dest_qpn = remote_qp0+offset;
                            #endif	

                            #if RANDQP
                                conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
                            #endif
                        }*/
                        
                        break;
                    }
                    default: {
                        fprintf(stderr, "Completion for unknown wr_id %d\n",
                            (int) wc[i].wr_id);
                    }
                }
            
                #if debug 
                    printf("Thread %d rcnt = %d , scnt = %d \n",thread_num, conn->rcnt,conn->scnt);
                #endif
            }
        }
        printf("T%d TREE POPUlATION w/ PUTS DONE! Starting GETs and SCANs \n",thread_num);
        conn->rcnt = 0;
        conn->scnt = 0; 
        conn->key_number = 0;
        ret = pthread_barrier_wait(&barrier);
        sleep(2);
        ret = pthread_barrier_wait(&barrier);
        sleep(1);
        if (gettimeofday(&start, NULL)) {
            perror("gettimeofday");
        }	

        prev_clock = now();

        //while (conn->rcnt < conn->iters || conn->scnt < conn->iters) {
        while (1) {
            if (terminate_load == 1) break;

            struct ibv_wc wc[num_bufs*2];
            int ne, i;
            do {
                if (terminate_load == 1) break;
                ne = ibv_poll_cq(conn->ctx->cq, 2*num_bufs, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }

                #if debug
                    if(printpoll) {
                        printf("thread_num %d polling \n",thread_num);
                        printpoll = false;
                    }
                #endif

            } while (!conn->use_event && ne < 1);

            for (i = 0; i < ne; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }

                int a = (int) wc[i].wr_id;
                switch (a) {
                    case 0 ... num_bufs-1: {// SEND_WRID
                        ++conn->scnt;
                        --conn->souts;
                        #if debug
                            printf("T%d - send complete, a = %d, rcnt = %d, scnt = %d, routs = %d, souts = %d  \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                        #endif
                        break;
                    }
                    case num_bufs ... 2*num_bufs-1: {
                        ++conn->rcnt;
                        --conn->routs;
                        #if debug
                            printf("T%d - recv complete, a = %d, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,a,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                            for(int p = 0; p < 50 ; p++)
                            {	
                                printf("%x",conn->buf_recv[a-num_bufs][40+p]);
                            }
                            printf("\n");
                        #endif
                        #if debug
                            if(conn->rcnt % INTERVAL == 0) {
                                double curr_clock = now();
                                printf("T%d - %f rps, rcnt = %d, scnt = %d \n",thread_num,INTERVAL/(curr_clock-prev_clock),conn->rcnt,conn->scnt);
                                prev_clock = curr_clock;
                            }
                        #endif
                        //if(conn->rcnt > conn->iters - INTERVAL/1000000 && conn->rcnt % 1 == 0) printf("T%d - rcnt = %d, scnt = %d \n",thread_num,conn->rcnt,conn->scnt);


                        if(!conn->pp_post_recv(conn->ctx, a, false)) ++conn->routs;
                        if (conn->routs != window_size) fprintf(stderr,"Loading thread %d couldn't post receive (%d)\n", thread_num, conn->routs);

                        uint8_t reqID = sendCmd(c, conn, thread_num);

                        conn->buf_send[a-num_bufs][0] = 0;
                        conn->buf_send[a-num_bufs][1] = reqID;

                        memcpy(conn->buf_send[a-num_bufs]+2, c.conn->out_->buf, c.conn->out_->n);

                        int success = conn->pp_post_send(conn->ctx, conn->dest_qpn, c.conn->out_->n+2, a-num_bufs);
                        if (success == EINVAL) printf("Invalid value provided in wr \n");
                        else if (success == ENOMEM)	printf("Send Queue is full or not enough resources to complete this operation \n");
                        else if (success == EFAULT) printf("Invalid value provided in qp \n");
                        else if (success != 0) {
                            printf("success = %d, \n",success);
                            fprintf(stderr, "Couldn't post send 2 \n");
                        }
                        else {
                            ++conn->souts;
                            c.conn->out_->n = 0;
                            #if debug
                                printf("T%d - send posted... souts = %d, \n",thread_num,conn->souts);
                            #endif
                        }

                        #if debug
                            printf("T%d - posted send, rcnt = %d , scnt = %d, routs = %d, souts = %d \n",thread_num,conn->rcnt,conn->scnt,conn->routs,conn->souts);
                            for(int p = 0; p < 50 ; p++)
                            {	
                                printf("%x",conn->buf_send[a-num_bufs][p]);
                            }
                            printf("\n");
                        #endif

                        #if RR
                            offset = (offset+1)%SERVER_THREADS;
                            conn->dest_qpn = remote_qp0+offset;
                        #endif	

                        #if RANDQP
                            conn->dest_qpn = remote_qp0+genRandDestQP(thread_num);
                        #endif
                        
                        break;
                    }
                    default: {
                        fprintf(stderr, "Completion for unknown wr_id %d\n",
                            (int) wc[i].wr_id);
                    }
                }
            
                #if debug 
                    printf("Thread %d rcnt = %d , scnt = %d \n",thread_num, conn->rcnt,conn->scnt);
                #endif
            }
        }
        if (gettimeofday(&end, NULL)) {
			perror("gettimeofday");
		}
    }
    
	float usec = (end.tv_sec - start.tv_sec) * 1000000 +(end.tv_usec - start.tv_usec);
	//long long bytes = (long long) conn->size * conn->iters * 2;

	if(thread_num == active_thread_num - 1){
		rps[thread_num] = conn->sync_iters/(usec/1000000.);
		printf("Meas. Thread: %d iters in %.5f seconds, rps = %f \n", conn->rcnt, usec/1000000., conn->rcnt/(usec/1000000.));
	}
	else if (thread_num < active_thread_num - 1) {
		rps[thread_num] = conn->rcnt/(usec/1000000.);
		printf("Thread %d: %d iters in %.5f seconds, rps = %f \n", thread_num, conn->rcnt, usec/1000000., rps[thread_num]);
	}
    

	// Dump out measurement results from measurement thread
	if(thread_num == active_thread_num - 1) {
		terminate_load = 1;
		printf("Measurement done, terminating loading threads\n");
		sleep(2);
		double totalRPS = 0;
		//aggregate RPS
		for(int i = 0; i < active_thread_num-1; i++){
			totalRPS += rps[i];
		}
        printf("total RPS = %d \n", (int)totalRPS);char* result;
		printf("avgRPS = %f \n",totalRPS/(active_thread_num-1));

        
		char* output_name;
        //printf("T%d after output name \n",thread_num);
    	asprintf(&output_name, "%s/%d_%d_%d.result", output_dir, window_size, active_thread_num, (int)totalRPS);
        //printf("T%d after asprintf \n",thread_num);
        FILE *f = fopen(output_name, "wb");
        //printf("T%d after fopen \n",thread_num);

		for (int i=0; i<conn->sync_iters; i++) {
            //printf("T%d writing i = %d \n",thread_num,i);

			float latency = (float)conn->measured_latency[i];
            //printf("T%d after reading i = %d \n",thread_num,i);
			fprintf(f, "%.5f \n", latency);
		}
        //printf("T%d after file \n",thread_num);
		fclose(f);
        //printf("T%d after file close \n",thread_num);

        asprintf(&output_name, "%s/%d_%d_%d_%.0f.time", output_dir, window_size, active_thread_num, (int)totalRPS, usec/1000000);
		FILE *ftime = fopen(output_name, "wb");
		float run_time = (float)(usec/1000000);
		fprintf(ftime, "%.5f \n", run_time);
		fclose(ftime);
	    free(conn->measured_latency);
	}
	//printf("T%d before close ctx \n",thread_num);
	if (conn->pp_close_ctx(conn->ctx))
		printf("Thread %d couldn't close ctx \n",thread_num);

    printf("T%d after close ctx \n",thread_num);
	ibv_free_device_list(conn->dev_list);
	
	//free(rem_dest);

    free(conn->random_keys);
    free(conn->op_type);

	return 0;
}


int
main(int argc, char *argv[])
{
  int i; //pid, status;
  //testrunner* test = 0;
  //int pipes[512];
  int dofork = 1;
    printf("hello \n");
  Clp_Parser *clp = Clp_NewParser(argc, argv, (int) arraysize(options), options);
  Clp_AddType(clp, clp_val_suffixdouble, Clp_DisallowOptions, clp_parse_suffixdouble, 0);
  int opt;

  
  while ((opt = Clp_Next(clp)) != Clp_Done) {
      switch (opt) {
      case opt_threads:
          //printf("thread \n");
          children = clp->val.i;
          active_thread_num = children;
          break;
      case opt_gidx:
          //printf("gid \n");
          gidx_in = clp->val.i;
          break;
      case opt_remote_qp0_in:
          //printf("rem qp \n");
          remote_qp0_in = clp->val.i;
          break;
      case opt_bimodal_ratio:
          bimodal_ratio = clp->val.i;
          printf("parsed bimodal ratio = %d \n",bimodal_ratio);
          break;
      case opt_long_query_percent:
          //printf("long query perc \n");
          long_query_percent = clp->val.i;
          break;
      case opt_server_threads:
          server_threads = clp->val.i;
          break;
      case opt_threads_deprecated:
          Clp_OptionError(clp, "%<%O%> is deprecated, use %<-j%>");
          children = clp->val.i;
          break;
      case opt_duration:
          duration = clp->val.d;
          break;
      case opt_duration2:
          duration2 = clp->val.d;
          break;
      case opt_window:
          //printf("window \n");
          window = clp->val.u;
          window_size = window;
          always_assert(window <= MAXWINDOW);
          //always_assert((window & (window - 1)) == 0); // power of 2
          break;
      case opt_server:
          //printf("server \n");
          serverip = clp->vstr;
          servername_const = serverip;
          servername = (char*)serverip;
          //strcpy(servername,servername_const);
          break;
      case opt_output_dir:
          //printf("out dir \n");
          output_dir_const = clp->vstr;
          output_dir = (char*)output_dir_const;
          //strcpy(output_dir,output_dir_const);
          break;
      case opt_ib_devname_in:
          //printf("ib dev name \n");
          ib_devname_in_const = clp->vstr;
          ib_devname_in = (char*)ib_devname_in_const;
          //strcpy(ib_devname_in,ib_devname_in_const);
          break;
      case opt_first_server_port:
          first_server_port = clp->val.i;
          break;
      case opt_quiet:
          quiet = !clp->negated;
          break;
      case opt_udp:
          udpflag = !clp->negated;
          break;
      case opt_first_local_port:
          first_local_port = clp->val.i;
          break;
      case opt_share_server_port:
          share_server_port = !clp->negated;
          break;
      case opt_input:
          input = clp->vstr;
          break;
      case opt_rsinit_part:
          rsinit_part = clp->val.i;
          break;
      case opt_first_seed:
          kvtest_first_seed = clp->val.i;
          break;
      case opt_rscale_partsz:
          rscale_partsz = clp->val.i;
          break;
      case opt_keylen:
          keylen = clp->val.i;
          break;
      case opt_limit:
          limit = (uint64_t) clp->val.d;
          break;
      case opt_prefix_len:
          prefixLen = clp->val.i;
          break;
      case opt_nkeys:
          nkeys = clp->val.i;
          break;
      case opt_get_ratio:
          getratio = clp->val.i;
          break;
      case opt_minkeyletter:
          assert(strlen(clp->vstr) == 1);
          minkeyletter = clp->vstr[0];
          break;
      case opt_maxkeyletter:
          assert(strlen(clp->vstr) == 1);
          maxkeyletter = clp->vstr[0];
          break;
      case opt_nofork:
          dofork = !clp->negated;
          break;
      case Clp_NotOption: {
          //printf("notOption \n");
          // check for parameter setting
          if (const char* eqchr = strchr(clp->vstr, '=')) {
              Json& param = test_param[String(clp->vstr, eqchr)];
              const char* end_vstr = clp->vstr + strlen(clp->vstr);
              if (param.assign_parse(eqchr + 1, end_vstr)) {
                  // OK, param was valid JSON
              } else if (eqchr[1] != 0) {
                  param = String(eqchr + 1, end_vstr);
              } else {
                  param = Json();
              }
          } else {
              test = testrunner::find(clp->vstr);
              if (!test) {
                  usage();
              }
          }
          break;
      }
      case Clp_BadOption:
          usage();
          break;
      }
  }
    //printf("%w %d, children %d\n", window, children);
	printf("window: %d; threads is %d; out dir is %s; devname is %s, gidx is %d, mode is %d, server ip = %s, ratio = %d, percent = %d \n", window_size, active_thread_num, output_dir, ib_devname_in, gidx_in, distribution_mode, servername, bimodal_ratio,  long_query_percent);

	assert(active_thread_num >= 1);

    /*
    if(bimodal_ratio == 25) scan_query_numpairs = 115;
    else if(bimodal_ratio == 20) scan_query_numpairs = 95;
    else if(bimodal_ratio == 15) scan_query_numpairs = 64;
    else if(bimodal_ratio == 10) scan_query_numpairs = 40;
    else if(bimodal_ratio == 5) scan_query_numpairs = 16;
    else if(bimodal_ratio == 2) scan_query_numpairs = 2;
    else {
        scan_query_numpairs = -1;
        printf("INVALID BIMODAL DISPARITY RATIO! Valid options include: 2,5,10,15,20,25 \n");
    }
    */

    rps = (double*)malloc(sizeof(double));
	vector<ClientRDMAConnection *> connections;
	for (int i = 0; i < active_thread_num; i++) {
		ClientRDMAConnection *conn;
		connections.push_back(conn);
	}

	ret = pthread_barrier_init(&barrier, &attr, active_thread_num);
	assert(ret == 0);

  	struct thread_data tdata [connections.size()];
	pthread_t pt[active_thread_num];
	for(int i = 0; i < active_thread_num; i++){
		tdata[i].conn = connections[i];  
		tdata[i].id = i;
        //tdata[i].test = test;

		int ret = pthread_create(&pt[i], NULL, client_threadfunc, &tdata[i]);
		assert(ret == 0);
		if(i == 0) sleep(3);
	}

	for(int i = 0; i < active_thread_num; i++){
		int ret = pthread_join(pt[i], NULL);
		assert(!ret);
	}
    printf("after joining all threads \n");
    //free(rps);

    //if (children < 1 || (children != 1 && !dofork)) usage();
    //if (!test) test = testrunner::first();


  //fflush(stdout);
/*
  if (dofork) {
      //printf("forking \n");
        bool globalsense = false;
        unsigned int numThreads = children;
        *((int*)shmem)=numThreads;
        *((bool*)shmem2)=globalsense;

        //memcpy(shmem, (void*)numThreads, sizeof(numThreads));
        
      for(i = 0; i < children; i++){
          int ptmp[2];
          int r = pipe(ptmp);
          always_assert(r == 0);
          pid = fork();
          if(pid < 0){
              perror("fork");
              exit(1);
          }
          if(pid == 0){
              //printf("running child, i = %d \n",i);
              //int myid = getpid(); printf("forked to process id: %d \n", myid);
              //fflush(stdout);

              close(ptmp[0]);
              //dup2(ptmp[1], 1);
              close(ptmp[1]);
              signal(SIGALRM, settimeout);
              alarm((int) ceil(duration));
              //printf("before run_child function call \n");     
              fflush(stdout);
              run_child(test, i);
              //printf("exited run_child \n");
              exit(0);
          }
          pipes[i] = ptmp[0];
          close(ptmp[1]);
      }
      for(i = 0; i < children; i++){
          if(wait(&status) <= 0){
              perror("wait");
              exit(1);
          }
          if (WIFSIGNALED(status))
              fprintf(stderr, "child %d died by signal %d\n", i, WTERMSIG(status));
      }
  } else {
      int ptmp[2];
      int r = pipe(ptmp);
      always_assert(r == 0);
      pipes[0] = ptmp[0];
      int stdout_fd = dup(STDOUT_FILENO);
      always_assert(stdout_fd > 0);
      r = dup2(ptmp[1], STDOUT_FILENO);
      always_assert(r >= 0);
      close(ptmp[1]);
      signal(SIGALRM, settimeout);
      alarm((int) ceil(duration));
      run_child(test, 0);
      fflush(stdout);
      r = dup2(stdout_fd, STDOUT_FILENO);
      always_assert(r >= 0);
      close(stdout_fd);
  }
  */

    /*
  long long total = 0;
  kvstats puts, gets, scans, puts_per_sec, gets_per_sec, scans_per_sec;
  for(i = 0; i < children; i++){
    char buf[2048];
    //if (pid == 0) printf("reading from pipes \n");
    //int cc = read(pipes[i], buf, sizeof(buf)-1);
    assert(cc > 0);
    //buf[cc] = 0;
    printf("%s", buf);
    Json bufj = Json::parse(buf, buf + cc);
    long long iv;
    double dv;
    if (bufj.to_i(iv)) {
        printf("inside if \n");
        total += iv;
    }
    else if (bufj.is_object()) {
        printf("inside else if \n");

        if (bufj.get("ops", iv)
            || bufj.get("total", iv)
            || bufj.get("count", iv))
            total += iv;
        if (bufj.get("puts", iv)) {
            puts.add(iv);
            //printf("PUTS = %d \n",iv);
        }
        if (bufj.get("gets", iv)) {
            gets.add(iv);
            //printf("GETS = %d \n",iv);
        }
        if (bufj.get("scans", iv)) {
            scans.add(iv);
            //printf("SCANS = %d \n",iv);
        }
        if (bufj.get("puts_per_sec", dv)) {
            puts_per_sec.add(dv);
            //printf("PUTS/SEC = %d \n",iv);
        }
        if (bufj.get("gets_per_sec", dv)) {
            gets_per_sec.add(dv);
            //printf("GETS/SEC = %d \n",iv);
        }
        if (bufj.get("scans_per_sec", dv)) {
            scans_per_sec.add(dv);
            //printf("SCANS/SEC = %d \n",iv);
        }
    }
  }

  printf("total %lld\n", total);
  puts.print_report("puts");
  gets.print_report("gets");
  scans.print_report("scans");
  puts_per_sec.print_report("puts/s");
  gets_per_sec.print_report("gets/s");
  scans_per_sec.print_report("scans/s");
  */
  exit(0);
}

//void* run_child(void *x)
void run_child(testrunner* test, int childno)
{
   
    struct child c;
    bzero(&c, sizeof(c));
    c.childno = childno;
    struct sockaddr_in sin;
    int ret, yes = 1;
  if(udpflag){
    c.udp = 1;

    #if RDMA
        //sleep(childno);
        //c.rdmaconn = conn;//new ClientRDMAConnection(childno);
        //if(childno == 0) *((int*)qp0)= c.rdmaconn->rem_dest->qpn;
    }
    #else


        c.s = socket(AF_INET, SOCK_DGRAM, 0);
    

        } else {
            c.s = socket(AF_INET, SOCK_STREAM, 0);
        }
        if (first_local_port) {
            bzero(&sin, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(first_local_port + (childno % 48));
            ret = ::bind(c.s, (struct sockaddr *) &sin, sizeof(sin));
            if (ret < 0) {
            perror("bind");
            exit(1);
            }
        }

        assert(c.s >= 0);
        setsockopt(c.s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        bzero(&sin, sizeof(sin));
        sin.sin_family = AF_INET;
        if (udpflag && !share_server_port)
            sin.sin_port = htons(first_server_port + (childno % 48));
        else
            sin.sin_port = htons(first_server_port);
        sin.sin_addr.s_addr = inet_addr(serverip);
        ret = connect(c.s, (struct sockaddr *) &sin, sizeof(sin));
        if(ret < 0){
            perror("connect");
            exit(1);
        }
    #endif


  c.conn = new KVConn(c.s, !udpflag);
  kvtest_client client(c);


  //printf("qp0 = %d\n",*((int*)qp0));
  //if(childno == 0) printf("before client starts to run \n");

/*
    int ret = __sync_fetch_and_sub ((int *)shmem, 1);
    //printf("b4 barrier, ret = %d \n",ret);
    if ( ret == 1) {
        //printf("inside if \n");
        //count = 4;
        *(bool *)shmem2 = true;
    } else {
        //printf("inside else \n");
        while (*(bool *)shmem2 != false);
    }
    //printf("after barrier \n");
*/

  test->run(client);

    #if !RDMA
        checkasync(&c, 2);
    #endif

  delete c.conn;
  close(c.s);
}

void KVConn::hard_check(int tryhard) {
    masstree_precondition(inbufpos_ == inbuflen_);
    if (parser_.empty()) {
        inbufpos_ = inbuflen_ = 0;
        for (auto x : oldinbuf_)
            delete[] x;
        oldinbuf_.clear();
    } else if (inbufpos_ == inbufsz) {
        oldinbuf_.push_back(inbuf_);
        inbuf_ = new char[inbufsz];
        inbufpos_ = inbuflen_ = 0;
    }
    if (tryhard == 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(infd_, &rfds);
        struct timeval tv = {0, 0};
        if (select(infd_ + 1, &rfds, NULL, NULL, &tv) <= 0)
            return;
    } else
        kvflush(out_);

    //if(pid == 0) printf("reading from infd from hard_check \n");
    ssize_t r = read(infd_, inbuf_ + inbufpos_, inbufsz - inbufpos_);
    if (r != -1)
        inbuflen_ += r;
}


//rdma gets happen here
int
get(struct child *c, const Str &key, char *val, int max)
{
    
}

// builtin aget callback: no check
void
nocheck(struct child *, struct async *, bool, const Str &)
{
}

// builtin aget callback: store string
void
asyncgetcb(struct child *, struct async *a, bool, const Str &val)
{
    Str *sptr;
    assert(a->wantedlen == sizeof(Str *));
    memcpy(&sptr, a->wanted, sizeof(Str *));
    sptr->len = std::min(sptr->len, val.len);
    memcpy(const_cast<char *>(sptr->s), val.s, sptr->len);
}

// builtin aget callback: store string
void
asyncgetcb_int(struct child *, struct async *a, bool, const Str &val)
{
    int *vptr;
    assert(a->wantedlen == sizeof(int *));
    memcpy(&vptr, a->wanted, sizeof(int *));
    long x = 0;
    if (val.len <= 0)
        x = -1;
    else
        for (int i = 0; i < val.len; ++i)
            if (val.s[i] >= '0' && val.s[i] <= '9')
                x = (x * 10) + (val.s[i] - '0');
            else {
                x = -1;
                break;
            }
    *vptr = x;
}

// default aget callback: check val against wanted
void
defaultget(struct child *, struct async *a, bool have_val, const Str &val)
{
    // check that we got the expected value
    int wanted_avail = std::min(a->wantedlen, int(sizeof(a->wanted)));
    if (!have_val
        || a->wantedlen != val.len
        || memcmp(val.s, a->wanted, wanted_avail) != 0)
        fprintf(stderr, "oops wanted %.*s(%d) got %.*s(%d)\n",
                wanted_avail, a->wanted, a->wantedlen, val.len, val.s, val.len);
    else {
        always_assert(a->wantedlen == val.len);
        always_assert(memcmp(val.s, a->wanted, wanted_avail) == 0);
    }
}

// builtin aput/aremove callback: store status
void
asyncputcb(struct child *, struct async *a, int status)
{
    int *sptr;
    assert(a->wantedlen == sizeof(int *));
    memcpy(&sptr, a->wanted, sizeof(int *));
    *sptr = status;
}

// process any waiting replies to aget() and aput().
// force=0 means non-blocking check if anything waiting on socket.
// force=1 means wait for at least one reply.
// force=2 means wait for all pending (nw-nr) replies.
void
checkasync(struct child *c, int force)
{
    while (c->seq0_ != c->seq1_) {
        if (force)
            c->conn->flush();
        if (c->conn->check(force ? 2 : 1) > 0) {
            const Json& result = c->conn->receive();
            always_assert(result);

            // is rseq in the nr..nw window?
            // replies might arrive out of order if UDP
            unsigned rseq = result[0].as_i();
            always_assert(rseq - c->seq0_ < c->seq1_ - c->seq0_);
            struct async *a = &c->a[rseq & (window - 1)];
            always_assert(a->seq == rseq);

            // advance the nr..nw window
            always_assert(a->acked == 0);
            a->acked = 1;
            while (c->seq0_ != c->seq1_ && c->a[c->seq0_ & (window - 1)].acked)
                ++c->seq0_;

            // might have been the last free slot,
            // don't want to re-use it underfoot.
            struct async tmpa = *a;

            if(tmpa.cmd == Cmd_Get){
                // this is a reply to a get
                String s = result.size() > 2 ? result[2].as_s() : String();
                if (tmpa.get_fn)
                    (tmpa.get_fn)(c, &tmpa, result.size() > 2, s);
            } else if (tmpa.cmd == Cmd_Put || tmpa.cmd == Cmd_Replace) {
                // this is a reply to a put
                if (tmpa.put_fn)
                    (tmpa.put_fn)(c, &tmpa, result[2].as_i());
            } else if(tmpa.cmd == Cmd_Scan){
                // this is a reply to a scan
                always_assert((result.size() - 2) / 2 <= tmpa.wantedlen);
            } else if (tmpa.cmd == Cmd_Remove) {
                // this is a reply to a remove
                if (tmpa.remove_fn)
                    (tmpa.remove_fn)(c, &tmpa, result[2].as_i());
            } else {
                always_assert(0);
            }

            if (force < 2)
                force = 0;
        } else if (!force)
            break;
    }
}

// async get, checkasync() will eventually check reply
// against wanted.
void
aget(struct child *c, const Str &key, const Str &wanted, get_async_cb fn)
{
    c->check_flush();

    c->conn->sendgetwhole(key, c->seq1_);
    if (c->udp)
        c->conn->flush();

    struct async *a = &c->a[c->seq1_ & (window - 1)];
    a->cmd = Cmd_Get;
    a->seq = c->seq1_;
    a->get_fn = (fn ? fn : defaultget);
    assert(key.len < int(sizeof(a->key)));
    memcpy(a->key, key.s, key.len);
    a->key[key.len] = 0;
    a->wantedlen = wanted.len;
    int wantedavail = std::min(wanted.len, int(sizeof(a->wanted)));
    memcpy(a->wanted, wanted.s, wantedavail);
    a->acked = 0;

    ++c->seq1_;
    ++c->nsent_;
}

void aget_col(struct child *c, const Str& key, int col, const Str& wanted,
              get_async_cb fn)
{
    c->check_flush();

    c->conn->sendgetcol(key, col, c->seq1_);
    if (c->udp)
        c->conn->flush();

    struct async *a = &c->a[c->seq1_ & (window - 1)];
    a->cmd = Cmd_Get;
    a->seq = c->seq1_;
    a->get_fn = (fn ? fn : defaultget);
    assert(key.len < int(sizeof(a->key)));
    memcpy(a->key, key.s, key.len);
    a->key[key.len] = 0;
    a->wantedlen = wanted.len;
    int wantedavail = std::min(wanted.len, int(sizeof(a->wanted)));
    memcpy(a->wanted, wanted.s, wantedavail);
    a->acked = 0;

    ++c->seq1_;
    ++c->nsent_;
}

void
aget(struct child *c, long ikey, long iwanted, get_async_cb fn)
{
    quick_istr key(ikey), wanted(iwanted);
    aget(c, key.string(), wanted.string(), fn);
}

int
scan(struct child *c)
{
    /*
    always_assert(c->seq0_ == c->seq1_);
    //fflush(stdout);
    unsigned int sseq = c->seq1_;
    #if RDMA
        if(DEBUG) printf("routs = %d, souts = %d \n",c->rdmaconn->routs, c->rdmaconn->souts);
        if(DEBUG) printf("out_->n = %d \n",c->conn->out_->n);
        //switch to using buf_send[a]
        //if(c->rdmaconn->souts < c->rdmaconn->bufs_num) {
            c->rdmaconn->wr_id = (++c->rdmaconn->wr_id)%c->rdmaconn->bufs_num;
            memcpy(c->rdmaconn->buf_send[c->rdmaconn->wr_id], c->conn->out_->buf, c->conn->out_->n);

            #if LOAD_SAME_SERVER_QP
                int success = c->rdmaconn->pp_post_send(c->rdmaconn->ctx, *((int*)qp0), c->conn->out_->n, c->rdmaconn->wr_id);
            #else
                int success = c->rdmaconn->pp_post_send(c->rdmaconn->ctx, c->rdmaconn->rem_dest->qpn, c->conn->out_->n, c->rdmaconn->wr_id);
            #endif

            if (success) {
                fprintf(stderr, "Couldn't post send (scan): scnt = %d, rcnt = %d, souts = %d, routs = %d \n",c->rdmaconn->scnt, c->rdmaconn->rcnt, c->rdmaconn->souts, c->rdmaconn->routs);
            }
            else c->rdmaconn->souts++;
        //}

        struct ibv_wc wc[c->rdmaconn->bufs_num*2];
        int ne, i;
        //remove condition when doing async
        while(!c->rdmaconn->received) 
        {
            do {
                //printf("inside dowhile tid = %d \n",tid);
                ne = ibv_poll_cq(c->rdmaconn->ctx->cq, c->rdmaconn->bufs_num*2, wc);
                if (ne < 0) {
                    fprintf(stderr, "poll CQ failed %d\n", ne);
                }
            } while (!c->rdmaconn->use_event && ne < 1);

            for (i = 0; i < ne; ++i) 
            {
                if (wc[i].status != IBV_WC_SUCCESS) 
                {
                    fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                        ibv_wc_status_str(wc[i].status),
                        wc[i].status, (int) wc[i].wr_id);
                }
                int a = (int) wc[i].wr_id;
                //if(debug) printf("in switch, a = %d tid = %d \n",a,tid);
                switch (a) 
                {
                    case 0 ... c->rdmaconn->bufs_num-1:
                        if(DEBUG) printf("sent \n");
                        //if(debug)printf("request sent, tid = %d \n", tid);
                        ++c->rdmaconn->scnt;
                        c->conn->out_->n = 0;
                        //if(debug) printf("souts = %d \n",souts); 	
                        break;
                    case c->rdmaconn->bufs_num ... 2*c->rdmaconn->bufs_num-1:
                        if(DEBUG) printf("received \n");
                        c->rdmaconn->received = true;
                        --c->rdmaconn->routs;
                        //if(debug) printf("request received, a = %d, tid = %d \n",a,tid);
                        //if (--routs <= 1) 
                        //{
                        //if(debug) printf("posting receive request, tid = %d \n",tid);
                        if(!c->rdmaconn->pp_post_recv(c->rdmaconn->ctx, a, false)) 
                            ++c->rdmaconn->routs;
                        if (c->rdmaconn->routs < c->rdmaconn->bufs_num) 
                            fprintf(stderr,"Couldn't post receive (%d)\n",c->rdmaconn->routs);

                        ++c->rdmaconn->rcnt;
                        --c->rdmaconn->souts;
                
                        break;
                }
            }
        }  
        c->rdmaconn->received = false;
        if(DEBUG) printf("child exited while loop \n");
    #endif
    */
    ++c->seq0_;
    ++c->seq1_;
    ++c->nsent_;
    if(DEBUG) printf("in put, c->seq0_ = %d, c->seq1_ = %d \n", c->seq0_,c->seq1_);

    return 0;
}

int
put(struct child *c, const Str &key, const Str &val)
{
    
}

void
aput(struct child *c, const Str &key, const Str &val,
     put_async_cb fn, const Str &wanted)
{
    c->check_flush();

    c->conn->sendputwhole(key, val, c->seq1_);
    if (c->udp)
        c->conn->flush();

    struct async *a = &c->a[c->seq1_ & (window - 1)];
    a->cmd = Cmd_Put;
    a->seq = c->seq1_;
    assert(key.len < int(sizeof(a->key)) - 1);
    memcpy(a->key, key.s, key.len);
    a->key[key.len] = 0;
    a->put_fn = fn;
    if (fn) {
        assert(wanted.len <= int(sizeof(a->wanted)));
        a->wantedlen = wanted.len;
        memcpy(a->wanted, wanted.s, wanted.len);
    } else {
        a->wantedlen = -1;
        a->wanted[0] = 0;
    }
    a->acked = 0;

    ++c->seq1_;
    ++c->nsent_;
}

void aput_col(struct child *c, const Str &key, int col, const Str &val,
              put_async_cb fn, const Str &wanted)
{
    c->check_flush();

    c->conn->sendputcol(key, col, val, c->seq1_);
    if (c->udp)
        c->conn->flush();

    struct async *a = &c->a[c->seq1_ & (window - 1)];
    a->cmd = Cmd_Put;
    a->seq = c->seq1_;
    assert(key.len < int(sizeof(a->key)) - 1);
    memcpy(a->key, key.s, key.len);
    a->key[key.len] = 0;
    a->put_fn = fn;
    if (fn) {
        assert(wanted.len <= int(sizeof(a->wanted)));
        a->wantedlen = wanted.len;
        memcpy(a->wanted, wanted.s, wanted.len);
    } else {
        a->wantedlen = -1;
        a->wanted[0] = 0;
    }
    a->acked = 0;

    ++c->seq1_;
    ++c->nsent_;
}

bool remove(struct child *c, const Str &key)
{
    always_assert(c->seq0_ == c->seq1_);

    unsigned int sseq = c->seq1_;
    c->conn->sendremove(key, sseq);
    c->conn->flush();

    const Json& result = c->conn->receive();
    always_assert(result && result[0] == sseq);

    ++c->seq0_;
    ++c->seq1_;
    ++c->nsent_;
    return result[2].to_b();
}

void
aremove(struct child *c, const Str &key, remove_async_cb fn)
{
    c->check_flush();

    c->conn->sendremove(key, c->seq1_);
    if (c->udp)
        c->conn->flush();

    struct async *a = &c->a[c->seq1_ & (window - 1)];
    a->cmd = Cmd_Remove;
    a->seq = c->seq1_;
    assert(key.len < int(sizeof(a->key)) - 1);
    memcpy(a->key, key.s, key.len);
    a->key[key.len] = 0;
    a->acked = 0;
    a->remove_fn = fn;

    ++c->seq1_;
    ++c->nsent_;
}

int
xcompar(const void *xa, const void *xb)
{
  long *a = (long *) xa;
  long *b = (long *) xb;
  if(*a == *b)
    return 0;
  if(*a < *b)
    return -1;
  return 1;
}


// update random keys from a set of 10 million.
// maybe best to run it twice, first time to
// populate the database.
void
u1(struct child *c)
{
  int i, n;
  double t0 = now();

  srandom(kvtest_first_seed + c->childno);

  for(i = 0; i < 10000000; i++){
    char key[512], val[512];
    long x = random() % 10000000;
    sprintf(key, "%ld", x);
    sprintf(val, "%ld", x + 1);
    aput(c, Str(key), Str(val));
  }
  n = i;

  checkasync(c, 2);

  double t1 = now();
  Json result = Json().set("total", (long) (n / (t1 - t0)))
    .set("puts", n)
    .set("puts_per_sec", n / (t1 - t0));
  printf("%s\n", result.unparse().c_str());
}

#define CPN 10000000

void
cpa(struct child *c)
{
  int i, n;
  double t0 = now();

  srandom(kvtest_first_seed + c->childno);

  for(i = 0; i < CPN; i++){
    char key[512], val[512];
    long x = random();
    sprintf(key, "%ld", x);
    sprintf(val, "%ld", x + 1);
    aput(c, Str(key), Str(val));
  }
  n = i;

  checkasync(c, 2);

  double t1 = now();
  Json result = Json().set("total", (long) (n / (t1 - t0)))
    .set("puts", n)
    .set("puts_per_sec", n / (t1 - t0));
  printf("%s\n", result.unparse().c_str());

}

void
cpc(struct child *c)
{
  int i, n;
  double t0 = now();

  srandom(kvtest_first_seed + c->childno);

  for(i = 0; !timeout[0]; i++){
    char key[512], val[512];
    if (i % CPN == 0)
      srandom(kvtest_first_seed + c->childno);
    long x = random();
    sprintf(key, "%ld", x);
    sprintf(val, "%ld", x + 1);
    aget(c, Str(key), Str(val), NULL);
  }
  n = i;

  checkasync(c, 2);

  double t1 = now();
  Json result = Json().set("total", (long) (n / (t1 - t0)))
    .set("gets", n)
    .set("gets_per_sec", n / (t1 - t0));
  printf("%s\n", result.unparse().c_str());
}

void
cpd(struct child *c)
{
  int i, n;
  double t0 = now();

  srandom(kvtest_first_seed + c->childno);

  for(i = 0; !timeout[0]; i++){
    char key[512], val[512];
    if (i % CPN == 0)
      srandom(kvtest_first_seed + c->childno);
    long x = random();
    sprintf(key, "%ld", x);
    sprintf(val, "%ld", x + 1);
    aput(c, Str(key), Str(val));
  }
  n = i;

  checkasync(c, 2);

  double t1 = now();
  Json result = Json().set("total", (long) (n / (t1 - t0)))
    .set("puts", n)
    .set("puts_per_sec", n / (t1 - t0));
  printf("%s\n", result.unparse().c_str());
}

// multiple threads simultaneously update the same key.
// keep track of the winning value.
// use over2 to make sure it's the same after a crash/restart.
void
over1(struct child *c)
{
  int ret, iter;

  srandom(kvtest_first_seed + c->childno);

  iter = 0;
  while(!timeout[0]){
    char key1[64], key2[64], val1[64], val2[64];
    time_t xt = time(0);
    while(xt == time(0))
      ;
    sprintf(key1, "%d", iter);
    sprintf(val1, "%ld", random());
    put(c, Str(key1), Str(val1));
    napms(500);
    ret = get(c, Str(key1), val2, sizeof(val2));
    always_assert(ret > 0);
    sprintf(key2, "%d-%d", iter, c->childno);
    put(c, Str(key2), Str(val2));
    if(c->childno == 0)
      printf("%d: %s\n", iter, val2);
    iter++;
  }
  checkasync(c, 2);
  printf("0\n");
}

// check each round of over1()
void
over2(struct child *c)
{
  int iter;

  for(iter = 0; ; iter++){
    char key1[64], key2[64], val1[64], val2[64];
    int ret;
    sprintf(key1, "%d", iter);
    ret = get(c, Str(key1), val1, sizeof(val1));
    if(ret == -1)
      break;
    sprintf(key2, "%d-%d", iter, c->childno);
    ret = get(c, Str(key2), val2, sizeof(val2));
    if(ret == -1)
      break;
    if(c->childno == 0)
      printf("%d: %s\n", iter, val2);
    always_assert(strcmp(val1, val2) == 0);
  }

  checkasync(c, 2);
  fprintf(stderr, "child %d checked %d\n", c->childno, iter);
  printf("0\n");
}

// do a bunch of inserts to distinct keys.
// rec2() checks that a prefix of those inserts are present.
// meant to be interrupted by a crash/restart.
void
rec1(struct child *c)
{
  int i;
  double t0 = now(), t1;

  srandom(kvtest_first_seed + c->childno);

  for(i = 0; !timeout[0]; i++){
    char key[512], val[512];
    long x = random();
    sprintf(key, "%ld-%d-%d", x, i, c->childno);
    sprintf(val, "%ld", x);
    aput(c, Str(key), Str(val));
  }
  checkasync(c, 2);
  t1 = now();

  fprintf(stderr, "child %d: done %d %.0f put/s\n",
          c->childno,
          i,
          i / (t1 - t0));
  printf("%.0f\n", i / (t1 - t0));
}

void
rec2(struct child *c)
{
  int i;

  srandom(kvtest_first_seed + c->childno);

  for(i = 0; ; i++){
    char key[512], val[512], wanted[512];
    long x = random();
    sprintf(key, "%ld-%d-%d", x, i, c->childno);
    sprintf(wanted, "%ld", x);
    int ret = get(c, Str(key), val, sizeof(val));
    if(ret == -1)
      break;
    val[ret] = 0;
    if(strcmp(val, wanted) != 0){
      fprintf(stderr, "oops key %s got %s wanted %s\n", key, val, wanted);
      exit(1);
    }
  }

  int i0 = i; // first missing record
  for(i = i0+1; i < i0 + 10000; i++){
    char key[512], val[512];
    long x = random();
    sprintf(key, "%ld-%d-%d", x, i, c->childno);
    val[0] = 0;
    int ret = get(c, Str(key), val, sizeof(val));
    if(ret != -1){
      printf("child %d: oops first missing %d but %d present\n",
             c->childno, i0, i);
      exit(1);
    }
  }
  checkasync(c, 2);

  fprintf(stderr, "correct prefix of %d records\n", i0);
  printf("0\n");
}

// ask server to checkpoint
void
cpb(struct child *c)
{
    if (c->childno == 0)
        c->conn->checkpoint(c->childno);
    checkasync(c, 2);
}

using std::vector;
using std::string;
/*
template <typename C>
void scantest(C& client)
{
  int i;

  for(i = 1; i < 1000000; i++){
    //char key[32], val[32];
    //int kl = sprintf(key, "k%04d", i);
    //sprintf(val, "v%04d", i);
    //aput(c, Str(key, kl), Str(val));
    int32_t x = (int32_t) i;
    //put_sync(ikey, ivalue);
    client.put(x, x);

  }

  int wanted = 10;//(random() % 10);

  for(i = 1; i < 1000000-wanted; i++){
    char key[32];
    sprintf(key, "%d", i);
    //int wanted = 9;//(random() % 10);
    if (wanted == 0) continue;
    c->conn->sendscanwhole(key, wanted, 1);
    c->conn->flush();

    {
        const Json& result = c->conn->receive();
        always_assert(result && result[0] == 1);
        int n = (result.size() - 2) / 2;
        if(i <= 1000000 - wanted){
            always_assert(n == wanted);
        } else if(i <= 1000000){
            always_assert(n == 1000000 - i);
        } else {
            always_assert(n == 0);
        }

        /*
        int k0 = (i < 1 ? 1 : i);
        int j, ki, off = 2;
        for(j = k0, ki = 0; j < k0 + wanted && j < 1000000; j++, ki++, off += 2){
            char xkey[32], xval[32];
            sprintf(xkey, "k%04d", j);
            sprintf(xval, "v%04d", j);
            if (!result[off].as_s().equals(xkey)) {
                fprintf(stderr, "Assertion failed @%d: strcmp(%s, %s) == 0\n", ki, result[off].as_s().c_str(), xkey);
                always_assert(0);
            }
            always_assert(result[off + 1].as_s().equals(xval));
        }
        */
    //}

    /*
    {
        sprintf(key, "k%04d-a", i);
        c->conn->sendscanwhole(key, 1, 1);
        c->conn->flush();

        const Json& result = c->conn->receive();
        always_assert(result && result[0] == 1);
        int n = (result.size() - 2) / 2;
        if(i >= 1 && i < 199){
            always_assert(n == 1);
            sprintf(key, "k%04d", i+1);
            always_assert(result[2].as_s().equals(key));
        }
    }
    */
  //}

/*
  c->conn->sendscanwhole("k015", 10, 1);
  c->conn->flush();

  const Json& result = c->conn->receive();
  always_assert(result && result[0] == 1);
  int n = (result.size() - 2) / 2;
  always_assert(n == 10);
  always_assert(result[2].as_s().equals("k0150"));
  always_assert(result[3].as_s().equals("v0150"));
*/
//  fprintf(stderr, "scantest OK\n");
//  printf("0\n");
//}
