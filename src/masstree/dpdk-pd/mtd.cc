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
// -*- mode: c++ -*-
// mtd: key/value server
//

#include <ctype.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <rte_mbuf_core.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#if HAVE_SYS_EPOLL_H
#include <sys/epoll.h>
#endif
#if __linux__
#include <asm-generic/mman.h>
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#ifdef __linux__
#include <malloc.h>
#endif
#include "checkpoint.hh"
#include "clp.h"
#include "file.hh"
#include "json.hh"
#include "kvproto.hh"
#include "kvrandom.hh"
#include "kvstats.hh"
#include "kvtest.hh"
#include "log.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "masstree_tcursor.hh"
#include "msgpack.hh"
#include "nodeversion.hh"
#include "query_masstree.hh"
#include <algorithm>
#include <deque>

////////////////////////added by Hamed for RDMA/////////////////////
#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <sched.h>
#include <string>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <vector>

#include "ServerRDMAConnection.cc"
#include <infiniband/verbs.h>
#include <linux/types.h> //for __be32 type
////////////////////////DPDK stuff/////////////////////
#include <rte_distributor.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#ifndef debug
#define debug 0
#endif

#define NUM_MBUFS ((64 * 1024) - 1)
#define MBUF_CACHE_SIZE 128
#define RING_SIZE 1024
#define RING_CACHE_SIZE 256
#define RECV_RING_NAME "recv_dist_ring"
#define RETURN_RING_NAME "mbuf_return_ring"
#define BURST_SIZE 32

struct rte_distributor *distributor = NULL;
struct rte_mempool *mbuf_pool = NULL;
struct rte_ring *recv_dist_ring = NULL;
struct rte_ring *mbuf_return_ring = NULL;

struct mbuf_metadata {
  int qp_num;
  int msg_id;
  struct ibv_mr *mr;
};

// static int wr_id [96];
// 0-47 is for send, 48-95 is for recv

// extern struct pingpong_dest    *rem_dest;

struct thread_data {
  ServerRDMAConnection *conn;
  threadinfo *ti;
  int id;
  unsigned lcore;
};

using namespace std;

/*
inline double tv_to_double(struct timeval *tv) {
  return tv->tv_sec + (double) tv->tv_usec / 1000000;
}

inline double get_time() {
  //#if USE_CLOCK_GETTIME
  //  struct timespec ts;
  //  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  //  //  clock_gettime(CLOCK_REALTIME, &ts);
  //  return ts.tv_sec + (double) ts.tv_nsec / 1000000000;
  //#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv_to_double(&tv);
  //#endif
}
*/
/////////////////////////added by Hamed for getting time ////////

using lcdf::StringAccum;

#include <chrono> //added for service time measurement
#define MEAS_SERV_TIME 0

#if MEAS_SERV_TIME
int isGet = 0;
int isPut = 0;
int isScan = 0;
int isRemove = 0;
int isReplace = 0;

int getCount = 0;
int putCount = 0;
int scanCount = 0;
int removeCount = 0;
int replaceCount = 0;
#endif

enum { CKState_Quit, CKState_Uninit, CKState_Ready, CKState_Go };

volatile bool timeout[2] = {false, false};
double duration[2] = {10, 0};

Masstree::default_table *tree;

// all default to the number of cores
static int udpthreads = 0;
static int tcpthreads = 0;
static int nckthreads = 0;
static int testthreads = 0;
static int nlogger = 0;
static std::vector<int> cores;

static bool logging = true;
static bool pinthreads = false;
static bool recovery_only = false;
volatile uint64_t globalepoch =
    1; // global epoch, updated by main thread regularly
volatile uint64_t active_epoch = 1;
static int port = 2117;
static uint64_t test_limit = ~uint64_t(0);
static int doprint = 0;
int kvtest_first_seed = 31949;

static volatile sig_atomic_t go_quit = 0;
static int quit_pipe[2];

static std::vector<const char *> logdirs;
static std::vector<const char *> ckpdirs;

static logset *logs;
volatile bool recovering =
    false; // so don't add log entries, and free old value immediately

static double checkpoint_interval = 1000000;
static kvepoch_t ckp_gen = 0; // recover from checkpoint
static ckstate *cks = NULL;   // checkpoint status of all checkpointing threads
static pthread_cond_t rec_cond;
pthread_mutex_t rec_mu;
static int rec_nactive;
static int rec_state = REC_NONE;

kvtimestamp_t initial_timestamp;

static pthread_cond_t checkpoint_cond;
static pthread_mutex_t checkpoint_mu;

static void prepare_thread(threadinfo *ti);
static int *tcp_thread_pipes;
static void *tcp_threadfunc(void *ti);
static int udp_threadfunc(void *ti);
static int distributor_thread(void *ti);
static int receiver_thread(void *ti);

static void log_init();
static void recover(threadinfo *);
static kvepoch_t read_checkpoint(threadinfo *, const char *path);

static void *conc_checkpointer(void *ti);
static void recovercheckpoint(threadinfo *ti);

static void *canceling(void *);
static void catchint(int);
static void epochinc(int);

int register_mbuf_mr(struct rte_mbuf *mbuf, struct ibv_pd *pd) {
  struct mbuf_metadata *meta = (struct mbuf_metadata *)rte_mbuf_to_priv(mbuf);
  void *addr = rte_pktmbuf_mtod(mbuf, void *);
  size_t length = RTE_MBUF_DEFAULT_DATAROOM;

  meta->mr = ibv_reg_mr(pd, addr, length,
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                            IBV_ACCESS_REMOTE_READ);

  if (!meta->mr) {
    return 1;
  }

  return 0;
}

void deregister_mbuf_mr(struct rte_mbuf *mbuf) {
  struct mbuf_metadata *meta = (struct mbuf_metadata *)rte_mbuf_to_priv(mbuf);
  if (meta->mr) {
    ibv_dereg_mr(meta->mr);
  }
}

/* running local tests */
void test_timeout(int) {
  size_t n;
  for (n = 0; n < arraysize(timeout) && timeout[n]; ++n)
    /* do nothing */;
  if (n < arraysize(timeout)) {
    timeout[n] = true;
    if (n + 1 < arraysize(timeout) && duration[n + 1])
      xalarm(duration[n + 1]);
  }
}

struct kvtest_client {
  kvtest_client() : checks_(0), kvo_() {}
  kvtest_client(const char *testname)
      : testname_(testname), checks_(0), kvo_() {}

  int id() const { return ti_->index(); }
  int nthreads() const { return testthreads; }
  void set_thread(threadinfo *ti) { ti_ = ti; }
  void register_timeouts(int n) {
    always_assert(n <= (int)arraysize(::timeout));
    for (int i = 1; i < n; ++i)
      if (duration[i] == 0)
        duration[i] = 0; // duration[i - 1];
  }
  bool timeout(int which) const { return ::timeout[which]; }
  uint64_t limit() const { return test_limit; }
  Json param(const String &) const { return Json(); }
  double now() const { return ::now(); }

  void get(long ikey, Str *value);
  void get(const Str &key);
  void get(long ikey) {
    quick_istr key(ikey);
    get(key.string());
  }
  void get_check(const Str &key, const Str &expected);
  void get_check(const char *key, const char *expected) {
    get_check(Str(key, strlen(key)), Str(expected, strlen(expected)));
  }
  void get_check(long ikey, long iexpected) {
    quick_istr key(ikey), expected(iexpected);
    get_check(key.string(), expected.string());
  }
  void get_check_key8(long ikey, long iexpected) {
    quick_istr key(ikey, 8), expected(iexpected);
    get_check(key.string(), expected.string());
  }
  void get_check_key10(long ikey, long iexpected) {
    quick_istr key(ikey, 10), expected(iexpected);
    get_check(key.string(), expected.string());
  }
  void get_col_check(const Str &key, int col, const Str &expected);
  void get_col_check_key10(long ikey, int col, long iexpected) {
    quick_istr key(ikey, 10), expected(iexpected);
    get_col_check(key.string(), col, expected.string());
  }
  bool get_sync(long ikey);

  void put(const Str &key, const Str &value);
  void put(const char *key, const char *val) {
    put(Str(key, strlen(key)), Str(val, strlen(val)));
  }
  void put(long ikey, long ivalue) {
    quick_istr key(ikey), value(ivalue);
    put(key.string(), value.string());
  }
  void put_key8(long ikey, long ivalue) {
    quick_istr key(ikey, 8), value(ivalue);
    put(key.string(), value.string());
  }
  void put_key10(long ikey, long ivalue) {
    quick_istr key(ikey, 10), value(ivalue);
    put(key.string(), value.string());
  }

  int put_async() { return -1; }
  int get_async(int32_t *a, unsigned n) { return -1; }

  void put_col(const Str &key, int col, const Str &value);
  void put_col_key10(long ikey, int col, long ivalue) {
    quick_istr key(ikey, 10), value(ivalue);
    put_col(key.string(), col, value.string());
  }

  bool remove_sync(long ikey);

  void puts_done() {}
  void wait_all() {}
  void rcu_quiesce() {}
  String make_message(StringAccum &sa) const;
  void notice(const char *fmt, ...);
  void fail(const char *fmt, ...);
  const Json &report(const Json &x) { return report_.merge(x); }
  void finish() {
    fprintf(stderr, "%d: %s\n", ti_->index(), report_.unparse().c_str());
  }
  threadinfo *ti_;
  query<row_type> q_[10];
  const char *testname_;
  kvrandom_lcg_nr rand;
  int checks_;
  Json report_;
  struct kvout *kvo_;
  static volatile int failing;
};

volatile int kvtest_client::failing;

void kvtest_client::get(long ikey, Str *value) {
  quick_istr key(ikey);
  if (!q_[0].run_get1(tree->table(), key.string(), 0, *value, *ti_))
    *value = Str();
}

void kvtest_client::get(const Str &key) {
  Str val;
  (void)q_[0].run_get1(tree->table(), key, 0, val, *ti_);
}

void kvtest_client::get_check(const Str &key, const Str &expected) {
  Str val;
  if (!q_[0].run_get1(tree->table(), key, 0, val, *ti_)) {
    fail("get(%.*s) failed (expected %.*s)\n", key.len, key.s, expected.len,
         expected.s);
    return;
  }
  if (val.len != expected.len || memcmp(val.s, expected.s, val.len) != 0)
    fail("get(%.*s) returned unexpected value %.*s (expected %.*s)\n", key.len,
         key.s, std::min(val.len, 40), val.s, std::min(expected.len, 40),
         expected.s);
  else
    ++checks_;
}

void kvtest_client::get_col_check(const Str &key, int col,
                                  const Str &expected) {
  Str val;
  if (!q_[0].run_get1(tree->table(), key, col, val, *ti_)) {
    fail("get.%d(%.*s) failed (expected %.*s)\n", col, key.len, key.s,
         expected.len, expected.s);
    return;
  }
  if (val.len != expected.len || memcmp(val.s, expected.s, val.len) != 0)
    fail("get.%d(%.*s) returned unexpected value %.*s (expected %.*s)\n", col,
         key.len, key.s, std::min(val.len, 40), val.s,
         std::min(expected.len, 40), expected.s);
  else
    ++checks_;
}

bool kvtest_client::get_sync(long ikey) {
  quick_istr key(ikey);
  Str val;
  return q_[0].run_get1(tree->table(), key.string(), 0, val, *ti_);
}

void kvtest_client::put(const Str &key, const Str &value) {
  while (failing)
    /* do nothing */;
  q_[0].run_replace(tree->table(), key, value, *ti_);
  if (ti_->logger()) // NB may block
    ti_->logger()->record(logcmd_replace, q_[0].query_times(), key, value);
}

void kvtest_client::put_col(const Str &key, int col, const Str &value) {
  while (failing)
    /* do nothing */;
#if !MASSTREE_ROW_TYPE_STR
  if (!kvo_)
    kvo_ = new_kvout(-1, 2048);
  Json req[2] = {Json(col), Json(String::make_stable(value))};
  (void)q_[0].run_put(tree->table(), key, &req[0], &req[2], *ti_);
  if (ti_->logger()) // NB may block
    ti_->logger()->record(logcmd_put, q_[0].query_times(), key, &req[0],
                          &req[2]);
#else
  (void)key, (void)col, (void)value;
  assert(0);
#endif
}

bool kvtest_client::remove_sync(long ikey) {
  quick_istr key(ikey);
  bool removed = q_[0].run_remove(tree->table(), key.string(), *ti_);
  if (removed && ti_->logger()) // NB may block
    ti_->logger()->record(logcmd_remove, q_[0].query_times(), key.string(),
                          Str());
  return removed;
}

String kvtest_client::make_message(StringAccum &sa) const {
  const char *begin = sa.begin();
  while (begin != sa.end() && isspace((unsigned char)*begin))
    ++begin;
  String s = String(begin, sa.end());
  if (!s.empty() && s.back() != '\n')
    s += '\n';
  return s;
}

void kvtest_client::notice(const char *fmt, ...) {
  va_list val;
  va_start(val, fmt);
  String m = make_message(StringAccum().vsnprintf(500, fmt, val));
  va_end(val);
  if (m)
    fprintf(stderr, "%d: %s", ti_->index(), m.c_str());
}

void kvtest_client::fail(const char *fmt, ...) {
  static nodeversion32 failing_lock(false);
  static nodeversion32 fail_message_lock(false);
  static String fail_message;
  failing = 1;

  va_list val;
  va_start(val, fmt);
  String m = make_message(StringAccum().vsnprintf(500, fmt, val));
  va_end(val);
  if (!m)
    m = "unknown failure";

  fail_message_lock.lock();
  if (fail_message != m) {
    fail_message = m;
    fprintf(stderr, "%d: %s", ti_->index(), m.c_str());
  }
  fail_message_lock.unlock();

  if (doprint) {
    failing_lock.lock();
    fprintf(stdout, "%d: %s", ti_->index(), m.c_str());
    tree->print(stdout);
    fflush(stdout);
  }

  always_assert(0);
}

static void *testgo(void *x) {
  kvtest_client *kc = reinterpret_cast<kvtest_client *>(x);
  kc->ti_->pthread() = pthread_self();
  prepare_thread(kc->ti_);

  if (strcmp(kc->testname_, "rw1") == 0)
    kvtest_rw1(*kc);
  else if (strcmp(kc->testname_, "rw2") == 0)
    kvtest_rw2(*kc);
  else if (strcmp(kc->testname_, "rw3") == 0)
    kvtest_rw3(*kc);
  else if (strcmp(kc->testname_, "rw4") == 0)
    kvtest_rw4(*kc);
  else if (strcmp(kc->testname_, "rwsmall24") == 0)
    kvtest_rwsmall24(*kc);
  else if (strcmp(kc->testname_, "rwsep24") == 0)
    kvtest_rwsep24(*kc);
  else if (strcmp(kc->testname_, "palma") == 0)
    kvtest_palma(*kc);
  else if (strcmp(kc->testname_, "palmb") == 0)
    kvtest_palmb(*kc);
  else if (strcmp(kc->testname_, "rw16") == 0)
    kvtest_rw16(*kc);
  else if (strcmp(kc->testname_, "rw5") == 0 ||
           strcmp(kc->testname_, "rw1fixed") == 0)
    kvtest_rw1fixed(*kc);
  else if (strcmp(kc->testname_, "ycsbk") == 0)
    kvtest_ycsbk(*kc);
  else if (strcmp(kc->testname_, "wd1") == 0)
    kvtest_wd1(10000000, 1, *kc);
  else if (strcmp(kc->testname_, "wd1check") == 0)
    kvtest_wd1_check(10000000, 1, *kc);
  else if (strcmp(kc->testname_, "w1") == 0)
    kvtest_w1_seed(*kc, kvtest_first_seed + kc->id());
  else if (strcmp(kc->testname_, "r1") == 0)
    kvtest_r1_seed(*kc, kvtest_first_seed + kc->id());
  else if (strcmp(kc->testname_, "wcol1") == 0)
    kvtest_wcol1at(*kc, kc->id() % 24, kvtest_first_seed + kc->id() % 48,
                   5000000);
  else if (strcmp(kc->testname_, "rcol1") == 0)
    kvtest_rcol1at(*kc, kc->id() % 24, kvtest_first_seed + kc->id() % 48,
                   5000000);
  else
    kc->fail("unknown test '%s'", kc->testname_);
  return 0;
}

static const char *const kvstats_name[] = {
    "ops",   "ops_per_sec",  "puts",         "gets",
    "scans", "puts_per_sec", "gets_per_sec", "scans_per_sec"};

void runtest(const char *testname, int nthreads) {
  std::vector<kvtest_client> clients(nthreads, kvtest_client(testname));
  ::testthreads = nthreads;
  for (int i = 0; i < nthreads; ++i)
    clients[i].set_thread(threadinfo::make(threadinfo::TI_PROCESS, i));
  bzero((void *)timeout, sizeof(timeout));
  signal(SIGALRM, test_timeout);
  if (duration[0])
    xalarm(duration[0]);
  for (int i = 0; i < nthreads; ++i) {
    int r = pthread_create(&clients[i].ti_->pthread(), 0, testgo, &clients[i]);
    always_assert(r == 0);
  }
  for (int i = 0; i < nthreads; ++i)
    pthread_join(clients[i].ti_->pthread(), 0);

  kvstats kvs[arraysize(kvstats_name)];
  for (int i = 0; i < nthreads; ++i)
    for (int j = 0; j < (int)arraysize(kvstats_name); ++j)
      if (double x = clients[i].report_.get_d(kvstats_name[j]))
        kvs[j].add(x);
  for (int j = 0; j < (int)arraysize(kvstats_name); ++j)
    kvs[j].print_report(kvstats_name[j]);
}

struct conn {
  int fd;
  enum { inbufsz = 20 * 1024, inbufrefill = 16 * 1024 };

  conn(int s)
      : fd(s), inbuf_(new char[inbufsz]), inbufpos_(0), inbuflen_(0),
        kvout(new_kvout(s, 20 * 1024)), inbuftotal_(0) {}
  ~conn() {
    close(fd);
    free_kvout(kvout);
    delete[] inbuf_;
    for (char *x : oldinbuf_)
      delete[] x;
  }

  Json &receive() {
    while (!parser_.done() && check(2))
      inbufpos_ += parser_.consume(inbuf_ + inbufpos_, inbuflen_ - inbufpos_,
                                   String::make_stable(inbuf_, inbufsz));
    if (parser_.success() && parser_.result().is_a())
      parser_.reset();
    else
      parser_.result() = Json();
    return parser_.result();
  }

  int check(int tryhard) {
    if (inbufpos_ == inbuflen_ && tryhard)
      hard_check(tryhard);
    return inbuflen_ - inbufpos_;
  }

  uint64_t xposition() const { return inbuftotal_ + inbufpos_; }
  Str recent_string(uint64_t xposition) const {
    if (xposition - inbuftotal_ <= unsigned(inbufpos_))
      return Str(inbuf_ + (xposition - inbuftotal_), inbuf_ + inbufpos_);
    else
      return Str();
  }

private:
  char *inbuf_;
  int inbufpos_;
  int inbuflen_;
  std::vector<char *> oldinbuf_;
  msgpack::streaming_parser parser_;

public:
  struct kvout *kvout;

private:
  uint64_t inbuftotal_;

  void hard_check(int tryhard);
};

void conn::hard_check(int tryhard) {
  masstree_precondition(inbufpos_ == inbuflen_);
  if (parser_.empty()) {
    inbuftotal_ += inbufpos_;
    inbufpos_ = inbuflen_ = 0;
    for (auto x : oldinbuf_)
      delete[] x;
    oldinbuf_.clear();
  } else if (inbufpos_ == inbufsz) {
    oldinbuf_.push_back(inbuf_);
    inbuf_ = new char[inbufsz];
    inbuftotal_ += inbufpos_;
    inbufpos_ = inbuflen_ = 0;
  }
  if (tryhard == 1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {0, 0};
    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
      return;
  } else
    kvflush(kvout);

  ssize_t r = read(fd, inbuf_ + inbufpos_, inbufsz - inbufpos_);
  if (r != -1)
    inbuflen_ += r;
}

struct conninfo {
  int s;
  Json handshake;
};

/* main loop */

enum { clp_val_suffixdouble = Clp_ValFirstUser };
enum {
  opt_nolog = 1,
  opt_pin,
  opt_logdir,
  opt_port,
  opt_ckpdir,
  opt_duration,
  opt_test,
  opt_test_name,
  opt_threads,
  opt_cores,
  opt_print,
  opt_norun,
  opt_checkpoint,
  opt_limit,
  opt_epoch_interval
};
static const Clp_Option options[] = {
    {"no-log", 0, opt_nolog, 0, 0},
    {0, 'n', opt_nolog, 0, 0},
    {"no-run", 0, opt_norun, 0, 0},
    {"pin", 'p', opt_pin, 0, Clp_Negate},
    {"logdir", 0, opt_logdir, Clp_ValString, 0},
    {"ld", 0, opt_logdir, Clp_ValString, 0},
    {"checkpoint", 'c', opt_checkpoint, Clp_ValDouble,
     Clp_Optional | Clp_Negate},
    {"ckp", 0, opt_checkpoint, Clp_ValDouble, Clp_Optional | Clp_Negate},
    {"ckpdir", 0, opt_ckpdir, Clp_ValString, 0},
    {"ckdir", 0, opt_ckpdir, Clp_ValString, 0},
    {"cd", 0, opt_ckpdir, Clp_ValString, 0},
    {"port", 0, opt_port, Clp_ValInt, 0},
    {"duration", 'd', opt_duration, Clp_ValDouble, 0},
    {"limit", 'l', opt_limit, clp_val_suffixdouble, 0},
    {"test", 0, opt_test, Clp_ValString, 0},
    {"test-rw1", 0, opt_test_name, 0, 0},
    {"test-rw2", 0, opt_test_name, 0, 0},
    {"test-rw3", 0, opt_test_name, 0, 0},
    {"test-rw4", 0, opt_test_name, 0, 0},
    {"test-rw5", 0, opt_test_name, 0, 0},
    {"test-rw16", 0, opt_test_name, 0, 0},
    {"test-palm", 0, opt_test_name, 0, 0},
    {"test-ycsbk", 0, opt_test_name, 0, 0},
    {"test-rw1fixed", 0, opt_test_name, 0, 0},
    {"threads", 'j', opt_threads, Clp_ValInt, 0},
    {"cores", 0, opt_cores, Clp_ValString, 0},
    {"print", 0, opt_print, 0, Clp_Negate},
    {"epoch-interval", 0, opt_epoch_interval, Clp_ValDouble, 0}};

uint64_t rdtscp() {
  uint32_t low, high;
  __asm__ volatile("rdtscp" : "=a"(low), "=d"(high)::"ecx");
  uint64_t timestamp = (((uint64_t)high) << 32) | low;
  return timestamp;
}

int main(int argc, char *argv[]) {

  using std::swap;
  int s, ret, yes = 1, i = 1, firstcore = -1, corestride = 1;
  const char *dotest = 0;
  nlogger = tcpthreads = udpthreads = nckthreads =
      sysconf(_SC_NPROCESSORS_ONLN);

  // Handle DPDK eal args first.
  ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    fprintf(stderr, "Error: Cannot init EAL\n");
    exit(1);
  }
  // Update argc/argv to skip DPDK arguments
  argc -= ret;
  argv += ret;

  Clp_Parser *clp = Clp_NewParser(argc, argv, (int)arraysize(options), options);
  Clp_AddType(clp, clp_val_suffixdouble, Clp_DisallowOptions,
              clp_parse_suffixdouble, 0);
  int opt;
  double epoch_interval_ms = 1000;

  while ((opt = Clp_Next(clp)) >= 0) {
    switch (opt) {
    case opt_nolog:
      logging = false;
      break;
    case opt_pin:
      pinthreads = !clp->negated;
      break;
    case opt_threads: {
      nlogger = tcpthreads = udpthreads = nckthreads = clp->val.i;
      break;
    }
    case opt_logdir: {
      const char *s = strtok((char *)clp->vstr, ",");
      for (; s; s = strtok(NULL, ","))
        logdirs.push_back(s);
      break;
    }
    case opt_ckpdir: {
      const char *s = strtok((char *)clp->vstr, ",");
      for (; s; s = strtok(NULL, ","))
        ckpdirs.push_back(s);
      break;
    }
    case opt_checkpoint:
      if (clp->negated || (clp->have_val && clp->val.d <= 0))
        checkpoint_interval = -1;
      else if (clp->have_val)
        checkpoint_interval = clp->val.d;
      else
        checkpoint_interval = 30;
      break;
    case opt_port:
      port = clp->val.i;
      break;
    case opt_duration:
      duration[0] = clp->val.d;
      break;
    case opt_limit:
      test_limit = (uint64_t)clp->val.d;
      break;
    case opt_test:
      dotest = clp->vstr;
      break;
    case opt_test_name:
      dotest = clp->option->long_name + 5;
      break;
    case opt_print:
      doprint = !clp->negated;
      break;
    case opt_cores:
      if (firstcore >= 0 || cores.size() > 0) {
        Clp_OptionError(clp, "%<%O%> already given");
        exit(EXIT_FAILURE);
      } else {
        const char *plus = strchr(clp->vstr, '+');
        Json ij = Json::parse(clp->vstr),
             aj = Json::parse(String("[") + String(clp->vstr) + String("]")),
             pj1 = Json::parse(plus ? String(clp->vstr, plus) : "x"),
             pj2 = Json::parse(plus ? String(plus + 1) : "x");
        for (int i = 0; aj && i < aj.size(); ++i)
          if (!aj[i].is_int() || aj[i].to_i() < 0)
            aj = Json();
        if (ij && ij.is_int() && ij.to_i() >= 0)
          firstcore = ij.to_i(), corestride = 1;
        else if (pj1 && pj2 && pj1.is_int() && pj1.to_i() >= 0 && pj2.is_int())
          firstcore = pj1.to_i(), corestride = pj2.to_i();
        else if (aj) {
          for (int i = 0; i < aj.size(); ++i)
            cores.push_back(aj[i].to_i());
        } else {
          Clp_OptionError(clp, "bad %<%O%>, expected %<CORE1%>, "
                               "%<CORE1+STRIDE%>, or %<CORE1,CORE2,...%>");
          exit(EXIT_FAILURE);
        }
      }
      break;
    case opt_norun:
      recovery_only = true;
      break;
    case opt_epoch_interval:
      epoch_interval_ms = clp->val.d;
      break;
    default:
      fprintf(
          stderr,
          "Usage: mtd [-np] [--ld dir1[,dir2,...]] [--cd dir1[,dir2,...]]\n");
      exit(EXIT_FAILURE);
    }
  }
  Clp_DeleteParser(clp);
  if (logdirs.empty())
    logdirs.push_back(".");
  if (ckpdirs.empty())
    ckpdirs.push_back(".");
  if (firstcore < 0)
    firstcore = cores.size() ? cores.back() + 1 : 0;
  for (; (int)cores.size() < udpthreads; firstcore += corestride)
    cores.push_back(firstcore);

  // for -pg profiling
  signal(SIGINT, catchint);

  // log epoch starts at 1
  global_log_epoch = 1;
  global_wake_epoch = 0;
  log_epoch_interval.tv_sec = 0;
  log_epoch_interval.tv_usec = 200000;

  // set a timer for incrementing the global epoch
  if (!dotest) {
    if (!epoch_interval_ms) {
      printf("WARNING: epoch interval is 0, it means no GC is executed\n");
    } else {
      signal(SIGALRM, epochinc);
      struct itimerval etimer;
      etimer.it_interval.tv_sec = epoch_interval_ms / 1000;
      etimer.it_interval.tv_usec = fmod(epoch_interval_ms, 1000) * 1000;
      etimer.it_value.tv_sec = epoch_interval_ms / 1000;
      etimer.it_value.tv_usec = fmod(epoch_interval_ms, 1000) * 1000;
      ret = setitimer(ITIMER_REAL, &etimer, NULL);
      always_assert(ret == 0);
    }
  }

  // for parallel recovery
  ret = pthread_cond_init(&rec_cond, 0);
  always_assert(ret == 0);
  ret = pthread_mutex_init(&rec_mu, 0);
  always_assert(ret == 0);

  // for waking up the checkpoint thread
  ret = pthread_cond_init(&checkpoint_cond, 0);
  always_assert(ret == 0);
  ret = pthread_mutex_init(&checkpoint_mu, 0);
  always_assert(ret == 0);

  threadinfo *main_ti = threadinfo::make(threadinfo::TI_MAIN, -1);
  main_ti->pthread() = pthread_self();

  initial_timestamp = timestamp();
  tree = new Masstree::default_table;
  tree->initialize(*main_ti);
  printf("%s, %s, pin-threads %s, ", tree->name(), row_type::name(),
         pinthreads ? "enabled" : "disabled");
  if (logging) {
    printf("logging enabled\n");
    log_init();
    recover(main_ti);
  } else {
    printf("logging disabled\n");
  }

  /////////////////Packet distributor ////////////
  // Create mbuf pool
  mbuf_pool = rte_pktmbuf_pool_create(
      "MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, sizeof(struct mbuf_metadata),
      RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (mbuf_pool == NULL) {
    fprintf(stderr, "Cannot create mbuf pool\n");
    exit(1);
  }
  printf("num_workers: %d\n", udpthreads);
  distributor = rte_distributor_create("PKT_DIST", rte_socket_id(), udpthreads,
                                       RTE_DIST_ALG_BURST);

  // Create ring for communication between receiver and distributor
  recv_dist_ring = rte_ring_create(RECV_RING_NAME, RING_SIZE, rte_socket_id(),
                                   RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (recv_dist_ring == NULL) {
    fprintf(stderr, "Cannot create ring\n");
    exit(1);
  }
  mbuf_return_ring =
      rte_ring_create(RETURN_RING_NAME, RING_SIZE, rte_socket_id(),
                      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (mbuf_return_ring == NULL) {
    fprintf(stderr, "Cannot create return ring\n");
    exit(1);
  }

  // recv and distributor thread
  struct thread_data recv_data, dist_data;
  recv_data.id = 0; // Use ID 0 for receiver
  recv_data.conn = nullptr;
  recv_data.lcore = rte_get_next_lcore(-1, 1, 0);
  unsigned last_used_lcore = recv_data.lcore;

  if (last_used_lcore == RTE_MAX_LCORE) {
    fprintf(stderr, "Failed to get lcore for receiver\n");
    exit(1);
  }

  // Setup distributor thread
  dist_data.id = 1; // Use ID 1 for distributor
  dist_data.conn = nullptr;
  dist_data.lcore = rte_get_next_lcore(last_used_lcore, 1, 0);
  last_used_lcore = dist_data.lcore;

  if (last_used_lcore == RTE_MAX_LCORE) {
    fprintf(stderr, "Failed to get lcore for distributor\n");
    exit(1);
  }

  // Launch receiver thread
  if (rte_eal_remote_launch(receiver_thread, &recv_data, recv_data.lcore) < 0) {
    fprintf(stderr, "Failed to launch receiver thread\n");
    exit(1);
  }

  // Launch distributor thread
  if (rte_eal_remote_launch(distributor_thread, &dist_data, dist_data.lcore) <
      0) {
    fprintf(stderr, "Failed to launch distributor thread\n");
    exit(1);
  }
  // pthread_t distributor_tid;
  // struct thread_data dist_data;
  // dist_data.id = 0; // Use ID 0 for distributor
  // dist_data.conn = nullptr;
  //
  // ret = pthread_create(&distributor_tid, NULL, distributor_thread,
  // &dist_data); if (ret != 0) {
  //   fprintf(stderr, "Failed to create distributor thread\n");
  //   exit(1);
  // }
  // Sleep for connectin stuff
  sleep(10);

  /////////////////RDMA control path initialization////////////

  // UDP threads, each with its own port.
  if (udpthreads == 0)
    printf("0 udp threads\n");
  else if (udpthreads == 1)
    printf("1 udp thread (port %d)\n", port);
  else
    printf("%d udp threads (ports %d-%d)\n", udpthreads, port,
           port + udpthreads - 1);

  // for(int r = 0; r < 96 ; r++) wr_id[r] = r;

  vector<ServerRDMAConnection *> connections;
  for (int i = 0; i < udpthreads + 1; i++) {
    ServerRDMAConnection *conn; // = new ServerRDMAConnection(i);
    connections.push_back(conn);
  }

  // struct my_thread threads[udpthreads];
  struct thread_data tdata[connections.size()];
  pthread_t pt[udpthreads];
  for (i = 0; i < udpthreads; i++) {
    threadinfo *ti = threadinfo::make(threadinfo::TI_PROCESS, i);
    tdata[i].ti = ti;
    tdata[i].conn = connections[i];
    tdata[i].id = i;
    tdata[i].lcore = rte_get_next_lcore(last_used_lcore, 1, 0);
    last_used_lcore = tdata[i].lcore;
    if (last_used_lcore == RTE_MAX_LCORE) {
      fprintf(stderr, "No available core for worker %d\n", i);
      break;
    }

    ret = rte_eal_remote_launch(udp_threadfunc, &tdata[i], tdata[i].lcore);
    if (ret < 0) {
      fprintf(stderr, "Failed to launch worker thread %d\n", i);
      continue;
    }
    // ret = pthread_create(&pt[i], NULL, udp_threadfunc, &tdata[i]);
    // printf("exited udp_threadfunc \n");
    // always_assert(ret == 0);
    // if (i == 0) {
    //   printf("inside sleep \n");
    // sleep(10);
  }

  rte_eal_mp_wait_lcore();
  // for (i = 0; i < udpthreads; i++) {
  //   ret = pthread_join(pt[i], NULL);
  //   assert(!ret);
  // }
  // // Join distributor thread
  // ret = pthread_join(distributor_tid, NULL);
  // assert(!ret);

  if (dotest) {
    if (strcmp(dotest, "palm") == 0) {
      runtest("palma", 1);
      runtest("palmb", tcpthreads);
    } else
      runtest(dotest, tcpthreads);
    tree->stats(stderr);
    if (doprint)
      tree->print(stdout);
    exit(0);
  }

  /*
  // TCP socket and threads

  s = socket(AF_INET, SOCK_STREAM, 0);
  always_assert(s >= 0);
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  sin.sin_port = htons(port);
  ret = bind(s, (struct sockaddr *) &sin, sizeof(sin));
  if (ret < 0) {
      perror("bind");
      exit(EXIT_FAILURE);
  }

  ret = listen(s, 100);
  if (ret < 0) {
      perror("listen");
      exit(EXIT_FAILURE);
  }

  threadinfo **tcpti = new threadinfo *[tcpthreads];
  tcp_thread_pipes = new int[tcpthreads * 2];
  printf("%d tcp threads (port %d)\n", tcpthreads, port);
  for(i = 0; i < tcpthreads; i++){
    threadinfo *ti = threadinfo::make(threadinfo::TI_PROCESS, i);
    ret = pipe(&tcp_thread_pipes[i * 2]);
    always_assert(ret == 0);
    ret = pthread_create(&ti->pthread(), 0, tcp_threadfunc, ti);
    always_assert(ret == 0);
    tcpti[i] = ti;
  }
  */
  // Create a canceling thread.
  ret = pipe(quit_pipe);
  always_assert(ret == 0);
  pthread_t canceling_tid;
  ret = pthread_create(&canceling_tid, NULL, canceling, NULL);
  always_assert(ret == 0);

  static int next = 0;
  while (1) {
    int s1;
    struct sockaddr_in sin1;
    socklen_t sinlen = sizeof(sin1);

    bzero(&sin1, sizeof(sin1));
    s1 = accept(s, (struct sockaddr *)&sin1, &sinlen);
    always_assert(s1 >= 0);
    setsockopt(s1, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    // Complete handshake.
    char buf[BUFSIZ];

    ssize_t nr = -1; // read(s1, buf, BUFSIZ);
    if (nr == -1) {
      perror("read");

    kill_connection:
      close(s1);
      // continue;
    }

    msgpack::streaming_parser sp;
    if (nr == 0 || sp.consume(buf, nr) != (size_t)nr || !sp.result().is_a() ||
        sp.result().size() < 2 || !sp.result()[1].is_i() ||
        sp.result()[1].as_i() != Cmd_Handshake) {
      fprintf(stderr, "failed handshake\n");
      goto kill_connection;
    }

    int target_core = -1;
    if (sp.result().size() >= 3 && sp.result()[2].is_o() &&
        sp.result()[2]["core"].is_i())
      target_core = sp.result()[2]["core"].as_i();
    if (target_core < 0 || target_core >= tcpthreads) {
      target_core = next % tcpthreads;
      ++next;
    }

    conninfo *ci = new conninfo;
    ci->s = s1;
    swap(ci->handshake, sp.result());

    ssize_t w = write(tcp_thread_pipes[2 * target_core + 1], &ci, sizeof(ci));
    always_assert((size_t)w == sizeof(ci));
  }
}

void catchint(int) {
  go_quit = 1;
  char cmd = 0;
  // Does not matter if the write fails (when the pipe is full)
  int r = write(quit_pipe[1], &cmd, sizeof(cmd));
  (void)r;
}

inline const char *threadtype(int type) {
  switch (type) {
  case threadinfo::TI_MAIN:
    return "main";
  case threadinfo::TI_PROCESS:
    return "process";
  case threadinfo::TI_LOG:
    return "log";
  case threadinfo::TI_CHECKPOINT:
    return "checkpoint";
  default:
    always_assert(0 && "Unknown threadtype");
    break;
  };
}

void *canceling(void *) {
  /*
  char cmd;
  int r = read(quit_pipe[0], &cmd, sizeof(cmd));
  (void) r;
  assert(r == sizeof(cmd) && cmd == 0);
  // Cancel wake up checkpointing threads
  pthread_mutex_lock(&checkpoint_mu);
  pthread_cond_signal(&checkpoint_cond);
  pthread_mutex_unlock(&checkpoint_mu);

  fprintf(stderr, "\n");
  // cancel outstanding threads. Checkpointing threads will exit safely
  // when the checkpointing thread 0 sees go_quit, and don't need cancel
  for (threadinfo *ti = threadinfo::allthreads; ti; ti = ti->next())
      if (ti->purpose() != threadinfo::TI_MAIN
          && ti->purpose() != threadinfo::TI_CHECKPOINT) {
          int r = pthread_cancel(ti->pthread());
          always_assert(r == 0);
      }
  */

  // join canceled threads
  for (threadinfo *ti = threadinfo::allthreads; ti; ti = ti->next())
    if (ti->purpose() != threadinfo::TI_MAIN) {
      fprintf(stderr, "joining thread %s:%d\n", threadtype(ti->purpose()),
              ti->index());
      int r = pthread_join(ti->pthread(), 0);
      always_assert(r == 0);
    }
  tree->stats(stderr);
  exit(0);
}

void epochinc(int) {
  globalepoch += 2;
  active_epoch = threadinfo::min_active_epoch();
}

// Return 1 if success, -1 if I/O error or protocol unmatch
int handshake(Json &request, threadinfo &ti) {
  always_assert(request.is_a() && request.size() >= 2 && request[1].is_i() &&
                request[1].as_i() == Cmd_Handshake &&
                (request.size() == 2 || request[2].is_o()));
  if (request.size() >= 2 && request[2]["maxkeylen"].is_i() &&
      request[2]["maxkeylen"].as_i() > MASSTREE_MAXKEYLEN) {
    request[2] = false;
    request[3] = "bad maxkeylen";
    request.resize(4);
  } else {
    request[2] = true;
    request[3] = ti.index();
    request[4] = row_type::name();
    request.resize(5);
  }
  request[1] = Cmd_Handshake + 1;
  return request[2].as_b() ? 1 : -1;
}

// execute command, return result.
int onego(query<row_type> &q, Json &request, Str request_str, threadinfo &ti) {

  int command = request[1].as_i();
  // printf("command = %d \n", command);
  if (command == Cmd_Checkpoint) {
#if debug
    printf("Cmd_Checkpoint \n");
#endif
    // force checkpoint
    pthread_mutex_lock(&checkpoint_mu);
    pthread_cond_broadcast(&checkpoint_cond);
    pthread_mutex_unlock(&checkpoint_mu);
    request.resize(2);
  } else if (command == Cmd_Get) {
#if debug
    printf("Cmd_Get \n");
#endif
#if MEAS_SERV_TIME
    // printf("get \n");
    isGet = 1;
    getCount++;
#endif
    q.run_get(tree->table(), request, ti);
  } else if (command == Cmd_Put && request.size() > 3 &&
             (request.size() % 2) == 1) { // insert or update
#if debug
    printf("Cmd_Put \n");
#endif
#if MEAS_SERV_TIME
    // printf("put \n");
    isPut = 1;
    putCount++;
#endif
    Str key(request[2].as_s());
    const Json *req = request.array_data() + 3;
    const Json *end_req = request.end_array_data();
    request[2] = q.run_put(tree->table(), request[2].as_s(), req, end_req, ti);
    if (ti.logger() && request_str) {
      // use the client's parsed version of the request
      msgpack::parser mp(request_str.data());
      mp.skip_array_size().skip_primitives(3);
      ti.logger()->record(logcmd_put, q.query_times(), key,
                          Str(mp.position(), request_str.end()));
    } else if (ti.logger())
      ti.logger()->record(logcmd_put, q.query_times(), key, req, end_req);
    request.resize(3);
  } else if (command == Cmd_Replace) { // insert or update
#if debug
    printf("Cmd_Replace \n");
#endif
#if MEAS_SERV_TIME
    // printf("replace \n");
    isReplace = 1;
    replaceCount++;
#endif
    Str key(request[2].as_s()), value(request[3].as_s());
    request[2] = q.run_replace(tree->table(), key, value, ti);
    if (ti.logger()) // NB may block
      ti.logger()->record(logcmd_replace, q.query_times(), key, value);
    request.resize(3);
  } else if (command == Cmd_Remove) { // remove
#if debug
    printf("Cmd_Remove \n");
#endif
#if MEAS_SERV_TIME
    isRemove = 1;
    removeCount++;
    // printf("remove \n");
#endif
    Str key(request[2].as_s());
    bool removed = q.run_remove(tree->table(), key, ti);
    if (removed && ti.logger()) // NB may block
      ti.logger()->record(logcmd_remove, q.query_times(), key, Str());
    request[2] = removed;
    request.resize(3);
  } else if (command == Cmd_Scan) {
#if debug
    printf("Cmd_Scan \n");
#endif
#if MEAS_SERV_TIME
    // printf("scan \n");
    isScan = 1;
    scanCount++;
    // printf("request[3].as_i() = %d \n",request[3].as_i());
#endif
    q.run_scan(tree->table(), request, ti);
  } else {
#if debug
    printf("didn't recognize command = %d \n", command);
#endif
    request[1] = -1;
    request.resize(2);
    return -1;
  }
  request[1] = command + 1;
  return 1;
}

#if HAVE_SYS_EPOLL_H
struct tcpfds {
  int epollfd;

  tcpfds(int pipefd) {
    epollfd = epoll_create(10);
    if (epollfd < 0) {
      perror("epoll_create");
      exit(EXIT_FAILURE);
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)1;
    int r = epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd, &ev);
    always_assert(r == 0);
  }

  enum { max_events = 100 };
  typedef struct epoll_event eventset[max_events];
  int wait(eventset &es) { return epoll_wait(epollfd, es, max_events, -1); }

  conn *event_conn(eventset &es, int i) const { return (conn *)es[i].data.ptr; }

  void add(int fd, conn *c) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    int r = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    always_assert(r == 0);
  }

  void remove(int fd) {
    int r = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    always_assert(r == 0);
  }
};
#else
class tcpfds {
  int nfds_;
  fd_set rfds_;
  std::vector<conn *> conns_;

public:
  tcpfds(int pipefd) : nfds_(pipefd + 1) {
    always_assert(pipefd < FD_SETSIZE);
    FD_ZERO(&rfds_);
    FD_SET(pipefd, &rfds_);
    conns_.resize(nfds_, 0);
    conns_[pipefd] = (conn *)1;
  }

  typedef fd_set eventset;
  int wait(eventset &es) {
    es = rfds_;
    int r = select(nfds_, &es, 0, 0, 0);
    return r > 0 ? nfds_ : r;
  }

  conn *event_conn(eventset &es, int i) const {
    return FD_ISSET(i, &es) ? conns_[i] : 0;
  }

  void add(int fd, conn *c) {
    always_assert(fd < FD_SETSIZE);
    FD_SET(fd, &rfds_);
    if (fd >= nfds_) {
      nfds_ = fd + 1;
      conns_.resize(nfds_, 0);
    }
    conns_[fd] = c;
  }

  void remove(int fd) {
    always_assert(fd < FD_SETSIZE);
    FD_CLR(fd, &rfds_);
    if (fd == nfds_ - 1) {
      while (nfds_ > 0 && !FD_ISSET(nfds_ - 1, &rfds_))
        --nfds_;
    }
  }
};
#endif

void prepare_thread(threadinfo *ti) {
#if __linux__
  if (pinthreads) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    // CPU_SET(cores[ti->index()], &cs);
    CPU_SET(ti->index(), &cs);
    // printf("ti->index() = %d, cores[ti->index()] = %d \n",ti->index(),
    // cores[ti->index()]);
    always_assert(sched_setaffinity(0, sizeof(cs), &cs) == 0);
  }
#else
  always_assert(!pinthreads && "pinthreads not supported\n");
#endif
  if (logging)
    ti->set_logger(&logs->log(ti->index() % nlogger));
}

void *tcp_threadfunc(void *x) {
  threadinfo *ti = reinterpret_cast<threadinfo *>(x);
  ti->pthread() = pthread_self();
  prepare_thread(ti);

  int myfd = tcp_thread_pipes[2 * ti->index()];
  tcpfds sloop(myfd);
  tcpfds::eventset events;
  std::deque<conn *> ready;
  query<row_type> q;

  while (1) {
    int nev = sloop.wait(events);
    for (int i = 0; i < nev; i++)
      if (conn *c = sloop.event_conn(events, i))
        ready.push_back(c);

    while (!ready.empty()) {
      conn *c = ready.front();
      ready.pop_front();

      if (c == (conn *)1) {
        // new connections
#define MAX_NEWCONN 100
        conninfo *ci[MAX_NEWCONN];
        ssize_t len = read(myfd, ci, sizeof(ci));
        always_assert(len > 0 && len % sizeof(int) == 0);
        for (int j = 0; j * sizeof(*ci) < (size_t)len; ++j) {
          struct conn *c = new conn(ci[j]->s);
          sloop.add(c->fd, c);
          int ret = handshake(ci[j]->handshake, *ti);
          msgpack::unparse(*c->kvout, ci[j]->handshake);
          kvflush(c->kvout);
          if (ret < 0) {
            sloop.remove(c->fd);
            delete c;
          }
          delete ci[j];
        }
      } else if (c) {
        // Should not block as suggested by epoll
        uint64_t xposition = c->xposition();
        Json &request = c->receive();
        int ret;
        if (unlikely(!request))
          goto closed;
        ti->rcu_start();
        ret = onego(q, request, c->recent_string(xposition), *ti);
        ti->rcu_stop();
        msgpack::unparse(*c->kvout, request);
        request.clear();
        if (likely(ret >= 0)) {
          if (c->check(0))
            ready.push_back(c);
          else
            kvflush(c->kvout);
          continue;
        }
        printf("socket read error\n");
      closed:
        kvflush(c->kvout);
        sloop.remove(c->fd);
        delete c;
      }
    }
  }
  return 0;
}

inline double tv_to_double(struct timeval *tv) {
  return tv->tv_sec + (double)tv->tv_usec / 1000000;
}

inline double get_time() {
  // #if USE_CLOCK_GETTIME
  //   struct timespec ts;
  //   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  //   //  clock_gettime(CLOCK_REALTIME, &ts);
  //   return ts.tv_sec + (double) ts.tv_nsec / 1000000000;
  // #else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv_to_double(&tv);
  // #endif
}

// Recv thread
static int receiver_thread(void *arg) {
  struct thread_data *tdata = (struct thread_data *)arg;
  ServerRDMAConnection *conn = new ServerRDMAConnection(tdata->id);

  const int num_bufs = conn->recv_bufs_num;
  struct ibv_wc wc[num_bufs];
  struct rte_mbuf *mbufs[num_bufs];
  struct ibv_recv_wr wrs[num_bufs];
  struct ibv_sge sges[num_bufs];
  int ret;

  printf("Receiver thread started\n");

  // Bulk allocate mbufs
  if (rte_pktmbuf_alloc_bulk(mbuf_pool, mbufs, num_bufs) != 0) {
    fprintf(stderr, "Failed to allocate %d mbufs\n", num_bufs);
    return -1;
  }

  // Register mbufs with RDMA
  for (int i = 0; i < num_bufs; i++) {
    ret = register_mbuf_mr(mbufs[i], conn->ctx->pd);
    if (ret) {
      fprintf(stderr, "Failed to register mbuf %d\n", i);
      return -1;
    }
  }

  // Set RSS tags
  for (int i = 0; i < num_bufs; i++) {
    mbufs[i]->hash.rss = i;
  }

  // Update receive buffer pointers
  for (int i = 0; i < num_bufs; i++) {
    conn->buf_recv[i] = rte_pktmbuf_mtod(mbufs[i], char *);
  }

  // Clear arrays
  memset(sges, 0, sizeof(sges));
  memset(wrs, 0, sizeof(wrs));

  // Post initial receives
  for (int i = 0; i < num_bufs; i++) {
    struct mbuf_metadata *meta =
        (struct mbuf_metadata *)rte_mbuf_to_priv(mbufs[i]);
    sges[i].addr = (uintptr_t)rte_pktmbuf_mtod(mbufs[i], void *);
    sges[i].length = RTE_MBUF_DEFAULT_DATAROOM;
    sges[i].lkey = meta->mr->lkey;

    wrs[i].wr_id = i;
    wrs[i].sg_list = &sges[i];
    wrs[i].num_sge = 1;
    wrs[i].next = (i < num_bufs - 1) ? &wrs[i + 1] : NULL;

    meta->msg_id = i;
  }

  struct ibv_recv_wr *bad_wr = NULL;
  ret = ibv_post_recv(conn->ctx->qp, &wrs[0], &bad_wr);
  if (ret) {
    fprintf(stderr, "Failed to post initial receives\n");
    return -1;
  }
  conn->routs += num_bufs;

  while (!go_quit) {
    // Check for returned mbufs and post new receives
    struct rte_mbuf *returned_mbufs[BURST_SIZE];
    unsigned int nb_returned = rte_ring_dequeue_burst(
        mbuf_return_ring, (void **)returned_mbufs, BURST_SIZE, NULL);
    if (nb_returned > 0) {
      // Prepare batch of receive work requests for returned mbufs
      for (unsigned int i = 0; i < nb_returned; i++) {
        struct mbuf_metadata *meta =
            (struct mbuf_metadata *)rte_mbuf_to_priv(returned_mbufs[i]);

        sges[i].addr = (uintptr_t)rte_pktmbuf_mtod(returned_mbufs[i], void *);
        sges[i].length = RTE_MBUF_DEFAULT_DATAROOM;
        sges[i].lkey = meta->mr->lkey;

        wrs[i].wr_id = meta->msg_id;
        wrs[i].sg_list = &sges[i];
        wrs[i].num_sge = 1;
        wrs[i].next = (i < nb_returned - 1) ? &wrs[i + 1] : NULL;
      }

      // Post new receives for returned mbufs
      struct ibv_recv_wr *bad_wr = NULL;
      if (ibv_post_recv(conn->ctx->qp, &wrs[0], &bad_wr)) {
        fprintf(stderr, "Failed to post new receives for returned mbufs\n");
      } else {
        conn->routs += nb_returned;
      }
    }

    // Poll for new completions
    int ne = ibv_poll_cq(conn->ctx->cq, num_bufs, wc);
    if (ne < 0) {
      fprintf(stderr, "poll CQ failed %d\n", ne);
      break;
    }

    // Process completions
    struct rte_mbuf *recv_mbufs[num_bufs]; // Buffer to collect messages
    int msg_count = 0; // Track number of messages in current batch
    //
    for (int i = 0; i < ne; ++i) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc[i].status), wc[i].status,
                (int)wc[i].wr_id);
        continue;
      }

      if (wc[i].opcode == IBV_WC_RECV) {
        struct mbuf_metadata *meta =
            (struct mbuf_metadata *)rte_mbuf_to_priv(mbufs[wc[i].wr_id]);
        meta->qp_num = wc[i].src_qp;
        recv_mbufs[msg_count] = mbufs[wc[i].wr_id];
        msg_count++;
        conn->routs--;
      }
    }

    // Enqueue batch of messages if we have any
    if (msg_count > 0) {
      unsigned int nb_enqueued = rte_ring_enqueue_burst(
          recv_dist_ring, (void **)recv_mbufs, msg_count, NULL);
      if (nb_enqueued < msg_count) {
        fprintf(stderr,
                "Failed to enqueue all messages: only enqueued %u of %d\n",
                nb_enqueued, msg_count);
      }
    }
  }

  // Cleanup
  for (int i = 0; i < num_bufs; i++) {
    deregister_mbuf_mr(mbufs[i]);
    rte_pktmbuf_free(mbufs[i]);
  }

  return 0;
}

// Load balancing thread
static int distributor_thread(void *arg) {
  struct thread_data *tdata = (struct thread_data *)arg;
  printf("Distributor thread started\n");

  struct rte_mbuf *mbufs[BURST_SIZE];
  unsigned int nb_rx;

  while (!go_quit) {
    // Dequeue messages from recv_dist_ring
    nb_rx = rte_ring_dequeue_burst(recv_dist_ring, (void **)mbufs, BURST_SIZE,
                                   NULL);
    if (nb_rx > 0) {

      // Distribute packets to workers
      int ret = rte_distributor_process(distributor, mbufs, nb_rx);
      if (ret < 0) {
        fprintf(stderr, "Failed to distribute packets\n");
      }

      // Get returned mbufs from workers
      struct rte_mbuf *returned_mbufs[BURST_SIZE];
      uint16_t nb_returns = rte_distributor_returned_pkts(
          distributor, returned_mbufs, BURST_SIZE);

      // Return processed mbufs back to receiver thread
      if (nb_returns > 0) {
        ret = rte_ring_enqueue_burst(mbuf_return_ring, (void **)returned_mbufs,
                                     nb_returns, NULL);
        if (ret < nb_returns) {
          fprintf(stderr, "Failed to return all mbufs to receiver\n");
        }
      }
    }
  }

  return 0;
}

// serve a client udp socket, in a dedicated thread
static int udp_threadfunc(void *x) {

  struct thread_data *tdata = (struct thread_data *)x;

  if (tdata->id != 0)
    sleep(2);

  ServerRDMAConnection *conn = tdata->conn;
  conn = new ServerRDMAConnection(tdata->id);

  threadinfo *ti = reinterpret_cast<threadinfo *>(tdata->ti);
  ti->pthread() = pthread_self();
  // prepare_thread(ti);

  // int cpu = tdata->id;
  // cpu_set_t cpuset;
  // CPU_ZERO(&cpuset);
  // CPU_SET(cpu, &cpuset);
  // sched_setaffinity(0, sizeof(cpuset), &cpuset);

  struct kvout *kvout = new_bufkvout();
  msgpack::streaming_parser parser;
  StringAccum sa;
  query<row_type> q;

  const int num_bufs = conn->recv_bufs_num;
  struct ibv_wc wc[num_bufs * 2];
  struct rte_mbuf *bufs[32];
  uint16_t nb_rx = 0;

  while (!go_quit) {
    // Poll for send completions
    int ne = ibv_poll_cq(conn->ctx->cq, num_bufs * 2, wc);
    if (ne < 0) {
      fprintf(stderr, "poll CQ failed %d\n", ne);
      break;
    }

    // Handle send completions
    for (int i = 0; i < ne; ++i) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc[i].status), wc[i].status,
                (int)wc[i].wr_id);
      }
      if (wc[i].opcode == IBV_WC_SEND) {
        conn->souts--;
      }
    }

    // Receive batch of work from distributor
    // printf("lcore: %d\n", tdata->lcore);
    nb_rx = rte_distributor_get_pkt(distributor, tdata->id, bufs, bufs, nb_rx);
    // printf("Received %d packets\n", nb_rx);

    for (uint16_t i = 0; i < nb_rx; i++) {
      struct mbuf_metadata *meta =
          (struct mbuf_metadata *)rte_mbuf_to_priv(bufs[i]);
      char *msg_data = rte_pktmbuf_mtod_offset(bufs[i], char *, 40);

      kvout_reset(kvout);
      parser.reset();

      unsigned consumed =
          parser.consume(msg_data + 2, 256, String(msg_data + 2));

      // printf("parser success = %d, msg_info: qp_num = %d, msg_id = %d\n",
      //        parser.success(), info->qp_num, info->msg_id);
      if (parser.success() && parser.result().is_a()) {
        ti->rcu_start();

        if (onego(q, parser.result(), Str(msg_data + 2, consumed), *ti) >= 0) {
          sa.clear();
          msgpack::unparser<StringAccum> cu(sa);
          cu << parser.result();

          // Copy first 3 bytes as in original code
          for (int q = 0; q <= 2; q++) {
            conn->buf_send[meta->msg_id][q] = (uint)msg_data[q];
          }

          memcpy(conn->buf_send[meta->msg_id] + 2, sa.data(), sa.length());
          // printf("Sending response %d\n", info->msg_id);
          int success = conn->pp_post_send(conn->ctx, meta->qp_num,
                                           sa.length() + 2, meta->msg_id);

          if (success == 0) {
            ++conn->souts;
          } else {
            fprintf(stderr, "Couldn't post send response\n");
          }
        }
        ti->rcu_stop();
      }

      // Free the mbuf
      // rte_pktmbuf_free(bufs[i]);
    }
  }

  // Cleanup
  if (conn->pp_close_ctx(conn->ctx)) {
    printf("close ctx returned 1\n");
  }
  ibv_free_device_list(conn->dev_list);

  return 0;
}

static String log_filename(const char *logdir, int logindex) {
  struct stat sb;
  int r = stat(logdir, &sb);
  if (r < 0 && errno == ENOENT) {
    r = mkdir(logdir, 0777);
    if (r < 0) {
      fprintf(stderr, "%s: %s\n", logdir, strerror(errno));
      always_assert(0);
    }
  }

  StringAccum sa;
  sa.snprintf(strlen(logdir) + 24, "%s/kvd-log-%d", logdir, logindex);
  return sa.take_string();
}

void log_init() {
  int ret, i;

  logs = logset::make(nlogger);
  for (i = 0; i < nlogger; i++)
    logs->log(i).initialize(log_filename(logdirs[i % logdirs.size()], i));

  cks = (ckstate *)malloc(sizeof(ckstate) * nckthreads);
  for (i = 0; i < nckthreads; i++) {
    threadinfo *ti = threadinfo::make(threadinfo::TI_CHECKPOINT, i);
    cks[i].state = CKState_Uninit;
    cks[i].ti = ti;
    ret = pthread_create(&ti->pthread(), 0, conc_checkpointer, ti);
    always_assert(ret == 0);
  }
}

// read a checkpoint, insert key/value pairs into tree.
// must be followed by a read of the log!
// since checkpoint is not consistent
// with any one point in time.
// returns the timestamp of the first log record that needs
// to come from the log.
kvepoch_t read_checkpoint(threadinfo *ti, const char *path) {
  double t0 = now();

  int fd = open(path, 0);
  if (fd < 0) {
    printf("no %s\n", path);
    return 0;
  }
  struct stat sb;
  int ret = fstat(fd, &sb);
  always_assert(ret == 0);
  char *p =
      (char *)mmap(0, sb.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
  always_assert(p != MAP_FAILED);
  close(fd);

  msgpack::parser par(String::make_stable(p, sb.st_size));
  Json j;
  par >> j;
  std::cerr << j << "\n";
  always_assert(j["generation"].is_i() && j["size"].is_i());
  uint64_t gen = j["generation"].as_i();
  uint64_t n = j["size"].as_i();
  printf("reading checkpoint with %" PRIu64 " nodes\n", n);

  // read data
  for (uint64_t i = 0; i != n; ++i)
    ckstate::insert(tree->table(), par, *ti);

  munmap(p, sb.st_size);
  double t1 = now();
  printf("%.1f MB, %.2f sec, %.1f MB/sec\n", sb.st_size / 1000000.0, t1 - t0,
         (sb.st_size / 1000000.0) / (t1 - t0));
  return gen;
}

void waituntilphase(int phase) {
  always_assert(pthread_mutex_lock(&rec_mu) == 0);
  while (rec_state != phase)
    always_assert(pthread_cond_wait(&rec_cond, &rec_mu) == 0);
  always_assert(pthread_mutex_unlock(&rec_mu) == 0);
}

void inactive(void) {
  always_assert(pthread_mutex_lock(&rec_mu) == 0);
  rec_nactive--;
  always_assert(pthread_cond_broadcast(&rec_cond) == 0);
  always_assert(pthread_mutex_unlock(&rec_mu) == 0);
}

void recovercheckpoint(threadinfo *ti) {
  waituntilphase(REC_CKP);
  char path[256];
  sprintf(path, "%s/kvd-ckp-%" PRId64 "-%d",
          ckpdirs[ti->index() % ckpdirs.size()], ckp_gen.value(), ti->index());
  kvepoch_t gen = read_checkpoint(ti, path);
  always_assert(ckp_gen == gen);
  inactive();
}

void recphase(int nactive, int state) {
  rec_nactive = nactive;
  rec_state = state;
  always_assert(pthread_cond_broadcast(&rec_cond) == 0);
  while (rec_nactive)
    always_assert(pthread_cond_wait(&rec_cond, &rec_mu) == 0);
}

// read the checkpoint file.
// read each log file.
// insert will ignore attempts to update with timestamps
// less than what was in the entry from the checkpoint file.
// so we don't have to do an explicit merge by time of the log files.
void recover(threadinfo *) {
  recovering = true;
  // XXX: discard temporary checkpoint and ckp-gen files generated before crash

  // get the generation of the checkpoint from ckp-gen, if any
  char path[256];
  sprintf(path, "%s/kvd-ckp-gen", ckpdirs[0]);
  ckp_gen = 0;
  rec_ckp_min_epoch = rec_ckp_max_epoch = 0;
  int fd = open(path, O_RDONLY);
  if (fd >= 0) {
    Json ckpj = Json::parse(read_file_contents(fd));
    close(fd);
    if (ckpj && ckpj["kvdb_checkpoint"] && ckpj["generation"].is_number()) {
      ckp_gen = ckpj["generation"].to_u64();
      rec_ckp_min_epoch = ckpj["min_epoch"].to_u64();
      rec_ckp_max_epoch = ckpj["max_epoch"].to_u64();
      printf("recover from checkpoint %" PRIu64 " [%" PRIu64 ", %" PRIu64 "]\n",
             ckp_gen.value(), rec_ckp_min_epoch.value(),
             rec_ckp_max_epoch.value());
    }
  } else {
    printf("no %s\n", path);
  }
  always_assert(pthread_mutex_lock(&rec_mu) == 0);

  // recover from checkpoint, and set timestamp of the checkpoint
  recphase(nckthreads, REC_CKP);

  // find minimum maximum timestamp of entries in each log
  rec_log_infos = new logreplay::info_type[nlogger];
  recphase(nlogger, REC_LOG_TS);

  // replay log entries, remove inconsistent entries, and append
  // an empty log entry with minimum timestamp

  // calculate log range

  // Maximum epoch seen in the union of the logs and the checkpoint. (We
  // don't commit a checkpoint until all logs are flushed past the
  // checkpoint's max_epoch.)
  kvepoch_t max_epoch = rec_ckp_max_epoch;
  if (max_epoch)
    max_epoch = max_epoch.next_nonzero();
  for (logreplay::info_type *it = rec_log_infos; it != rec_log_infos + nlogger;
       ++it)
    if (it->last_epoch && (!max_epoch || max_epoch < it->last_epoch))
      max_epoch = it->last_epoch;

  // Maximum first_epoch seen in the logs. Full log information is not
  // available for epochs before max_first_epoch.
  kvepoch_t max_first_epoch = 0;
  for (logreplay::info_type *it = rec_log_infos; it != rec_log_infos + nlogger;
       ++it)
    if (it->first_epoch &&
        (!max_first_epoch || max_first_epoch < it->first_epoch))
      max_first_epoch = it->first_epoch;

  // Maximum epoch of all logged wake commands.
  kvepoch_t max_wake_epoch = 0;
  for (logreplay::info_type *it = rec_log_infos; it != rec_log_infos + nlogger;
       ++it)
    if (it->wake_epoch && (!max_wake_epoch || max_wake_epoch < it->wake_epoch))
      max_wake_epoch = it->wake_epoch;

  // Minimum last_epoch seen in QUIESCENT logs.
  kvepoch_t min_quiescent_last_epoch = 0;
  for (logreplay::info_type *it = rec_log_infos; it != rec_log_infos + nlogger;
       ++it)
    if (it->quiescent && (!min_quiescent_last_epoch ||
                          min_quiescent_last_epoch > it->last_epoch))
      min_quiescent_last_epoch = it->last_epoch;

  // If max_wake_epoch && min_quiescent_last_epoch <= max_wake_epoch, then a
  // wake command was missed by at least one quiescent log. We can't replay
  // anything at or beyond the minimum missed wake epoch. So record, for
  // each log, the minimum wake command that at least one quiescent thread
  // missed.
  if (max_wake_epoch && min_quiescent_last_epoch <= max_wake_epoch)
    rec_replay_min_quiescent_last_epoch = min_quiescent_last_epoch;
  else
    rec_replay_min_quiescent_last_epoch = 0;
  recphase(nlogger, REC_LOG_ANALYZE_WAKE);

  // Calculate upper bound of epochs to replay.
  // This is the minimum of min_post_quiescent_wake_epoch (if any) and the
  // last_epoch of all non-quiescent logs.
  rec_replay_max_epoch = max_epoch;
  for (logreplay::info_type *it = rec_log_infos; it != rec_log_infos + nlogger;
       ++it) {
    if (!it->quiescent && it->last_epoch &&
        it->last_epoch < rec_replay_max_epoch)
      rec_replay_max_epoch = it->last_epoch;
    if (it->min_post_quiescent_wake_epoch &&
        it->min_post_quiescent_wake_epoch < rec_replay_max_epoch)
      rec_replay_max_epoch = it->min_post_quiescent_wake_epoch;
  }

  // Calculate lower bound of epochs to replay.
  rec_replay_min_epoch = rec_ckp_min_epoch;
  // XXX what about max_first_epoch?

  // Checks.
  if (rec_ckp_min_epoch) {
    always_assert(rec_ckp_min_epoch > max_first_epoch);
    always_assert(rec_ckp_min_epoch < rec_replay_max_epoch);
    always_assert(rec_ckp_max_epoch < rec_replay_max_epoch);
    fprintf(stderr,
            "replay [%" PRIu64 ",%" PRIu64 ") from [%" PRIu64 ",%" PRIu64
            ") into ckp [%" PRIu64 ",%" PRIu64 "]\n",
            rec_replay_min_epoch.value(), rec_replay_max_epoch.value(),
            max_first_epoch.value(), max_epoch.value(),
            rec_ckp_min_epoch.value(), rec_ckp_max_epoch.value());
  }

  // Actually replay.
  delete[] rec_log_infos;
  rec_log_infos = 0;
  recphase(nlogger, REC_LOG_REPLAY);

  // done recovering
  recphase(0, REC_DONE);
#if !NDEBUG
  // check that all delta markers have been recycled (leaving only remove
  // markers and real values)
  uint64_t deltas_created = 0, deltas_removed = 0;
  for (threadinfo *ti = threadinfo::allthreads; ti; ti = ti->next()) {
    deltas_created += ti->counter(tc_replay_create_delta);
    deltas_removed += ti->counter(tc_replay_remove_delta);
  }
  if (deltas_created)
    fprintf(stderr, "deltas created: %" PRIu64 ", removed: %" PRIu64 "\n",
            deltas_created, deltas_removed);
  always_assert(deltas_created == deltas_removed);
#endif

  global_log_epoch = rec_replay_max_epoch.next_nonzero();

  always_assert(pthread_mutex_unlock(&rec_mu) == 0);
  recovering = false;
  if (recovery_only)
    exit(0);
}

void writecheckpoint(const char *path, ckstate *c, double t0) {
  double t1 = now();
  printf("memory phase: %" PRIu64 " nodes, %.2f sec\n", c->count, t1 - t0);

  int fd = creat(path, 0666);
  always_assert(fd >= 0);

  // checkpoint file format, all msgpack:
  //   {"generation": generation, "size": size, ...}
  //   then `size` triples of key (string), timestmap (int), value (whatever)
  Json j = Json()
               .set("generation", ckp_gen.value())
               .set("size", c->count)
               .set("firstkey", c->startkey);
  StringAccum sa;
  msgpack::unparse(sa, j);
  checked_write(fd, sa.data(), sa.length());
  checked_write(fd, c->vals->buf, c->vals->n);

  int ret = fsync(fd);
  always_assert(ret == 0);
  ret = close(fd);
  always_assert(ret == 0);

  double t2 = now();
  c->bytes = c->vals->n;
  printf("file phase (%s): %" PRIu64 " bytes, %.2f sec, %.1f MB/sec\n", path,
         c->bytes, t2 - t1, (c->bytes / 1000000.0) / (t2 - t1));
}

void conc_filecheckpoint(threadinfo *ti) {
  ckstate *c = &cks[ti->index()];
  c->vals = new_bufkvout();
  double t0 = now();
  tree->table().scan(c->startkey, true, *c, *ti);
  char path[256];
  sprintf(path, "%s/kvd-ckp-%" PRId64 "-%d",
          ckpdirs[ti->index() % ckpdirs.size()], ckp_gen.value(), ti->index());
  writecheckpoint(path, c, t0);
  c->count = 0;
  free(c->vals);
}

static Json prepare_checkpoint(kvepoch_t min_epoch, int nckthreads,
                               const Str *pv) {
  Json j;
  j.set("kvdb_checkpoint", true)
      .set("min_epoch", min_epoch.value())
      .set("max_epoch", global_log_epoch.value())
      .set("generation", ckp_gen.value())
      .set("nckthreads", nckthreads);

  Json pvj;
  for (int i = 1; i < nckthreads; ++i)
    pvj.push_back(Json::make_string(pv[i].s, pv[i].len));
  j.set("pivots", pvj);

  return j;
}

static void commit_checkpoint(Json ckpj) {
  // atomically commit a set of checkpoint files by incrementing
  // the checkpoint generation on disk
  char path[256];
  sprintf(path, "%s/kvd-ckp-gen", ckpdirs[0]);
  int r = atomic_write_file_contents(path, ckpj.unparse());
  always_assert(r == 0);
  fprintf(stderr, "kvd-ckp-%" PRIu64 " [%s,%s]: committed\n", ckp_gen.value(),
          ckpj["min_epoch"].to_s().c_str(), ckpj["max_epoch"].to_s().c_str());

  // delete old checkpoint files
  for (int i = 0; i < nckthreads; i++) {
    char path[256];
    sprintf(path, "%s/kvd-ckp-%" PRId64 "-%d", ckpdirs[i % ckpdirs.size()],
            ckp_gen.value() - 1, i);
    unlink(path);
  }
}

static kvepoch_t max_flushed_epoch() {
  kvepoch_t mfe = 0, ge = global_log_epoch;
  for (int i = 0; i < nlogger; ++i) {
    loginfo &log = logs->log(i);
    kvepoch_t fe = log.quiescent() ? ge : log.flushed_epoch();
    if (!mfe || fe < mfe)
      mfe = fe;
  }
  return mfe;
}

// concurrent periodic checkpoint
void *conc_checkpointer(void *x) {
  threadinfo *ti = reinterpret_cast<threadinfo *>(x);
  ti->pthread() = pthread_self();
  recovercheckpoint(ti);
  ckstate *c = &cks[ti->index()];
  c->count = 0;
  pthread_cond_init(&c->state_cond, NULL);
  c->state = CKState_Ready;
  while (recovering)
    sleep(1);
  if (checkpoint_interval <= 0)
    return 0;
  if (ti->index() == 0) {
    for (int i = 1; i < nckthreads; i++)
      while (cks[i].state != CKState_Ready)
        ;
    Str *pv = new Str[nckthreads + 1];
    Json uncommitted_ckp;

    while (1) {
      struct timespec ts;
      set_timespec(ts, now() + (uncommitted_ckp ? 0.25 : checkpoint_interval));

      pthread_mutex_lock(&checkpoint_mu);
      if (!go_quit)
        pthread_cond_timedwait(&checkpoint_cond, &checkpoint_mu, &ts);
      if (go_quit) {
        for (int i = 0; i < nckthreads; i++) {
          cks[i].state = CKState_Quit;
          pthread_cond_signal(&cks[i].state_cond);
        }
        pthread_mutex_unlock(&checkpoint_mu);
        break;
      }
      pthread_mutex_unlock(&checkpoint_mu);

      if (uncommitted_ckp) {
        kvepoch_t mfe = max_flushed_epoch();
        if (!mfe || mfe > uncommitted_ckp["max_epoch"].to_u64()) {
          commit_checkpoint(uncommitted_ckp);
          uncommitted_ckp = Json();
        }
        continue;
      }

      double t0 = now();
      ti->rcu_start();
      for (int i = 0; i < nckthreads + 1; i++)
        pv[i].assign(NULL, 0);
      tree->findpivots(pv, nckthreads + 1);
      ti->rcu_stop();

      kvepoch_t min_epoch = global_log_epoch;
      pthread_mutex_lock(&checkpoint_mu);
      ckp_gen = ckp_gen.next_nonzero();
      for (int i = 0; i < nckthreads; i++) {
        cks[i].startkey = pv[i];
        cks[i].endkey = (i == nckthreads - 1 ? Str() : pv[i + 1]);
        cks[i].state = CKState_Go;
        pthread_cond_signal(&cks[i].state_cond);
      }
      pthread_mutex_unlock(&checkpoint_mu);

      ti->rcu_start();
      conc_filecheckpoint(ti);
      ti->rcu_stop();

      cks[0].state = CKState_Ready;
      uint64_t bytes = cks[0].bytes;
      pthread_mutex_lock(&checkpoint_mu);
      for (int i = 1; i < nckthreads; i++) {
        while (cks[i].state != CKState_Ready)
          pthread_cond_wait(&cks[i].state_cond, &checkpoint_mu);
        bytes += cks[i].bytes;
      }
      pthread_mutex_unlock(&checkpoint_mu);

      uncommitted_ckp = prepare_checkpoint(min_epoch, nckthreads, pv);

      for (int i = 0; i < nckthreads + 1; i++)
        if (pv[i].s)
          free((void *)pv[i].s);
      double t = now() - t0;
      fprintf(stderr,
              "kvd-ckp-%" PRIu64 " [%s,%s]: prepared (%.2f sec, %" PRIu64
              " MB, %" PRIu64 " MB/sec)\n",
              ckp_gen.value(), uncommitted_ckp["min_epoch"].to_s().c_str(),
              uncommitted_ckp["max_epoch"].to_s().c_str(), t, bytes / (1 << 20),
              (uint64_t)(bytes / t) >> 20);
    }
  } else {
    while (1) {
      pthread_mutex_lock(&checkpoint_mu);
      while (c->state != CKState_Go && c->state != CKState_Quit)
        pthread_cond_wait(&c->state_cond, &checkpoint_mu);
      if (c->state == CKState_Quit) {
        pthread_mutex_unlock(&checkpoint_mu);
        break;
      }
      pthread_mutex_unlock(&checkpoint_mu);

      ti->rcu_start();
      conc_filecheckpoint(ti);
      ti->rcu_stop();

      pthread_mutex_lock(&checkpoint_mu);
      c->state = CKState_Ready;
      pthread_cond_signal(&c->state_cond);
      pthread_mutex_unlock(&checkpoint_mu);
    }
  }
  return 0;
}
