# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017-2018 Intel Corporation

LIBDIR ?= $(REPO_PATH)/src/dlb_8.9.0/libdlb/

LDFLAGS := -L$(LIBDIR)
LDLIBS := -ldlb -lrt
CFLAGS := -I$(LIBDIR)/ -g -fPIC $(EXTRA_CFLAGS) -pthread -march=native -D_GNU_SOURCE -lm -O3

ifdef CROSS_COMPILE
  SYSROOT ?= $(SDKTARGETSYSROOT)
  CC = $(CROSS_COMPILE)gcc --sysroot=$(SYSROOT)
endif

SRCS :=         \
  ldb_traffic.c \
  dir_traffic.c \
  test_atomic_queue.c \
  dlb_microarch.c \
  ldb_traffic_ldb_port.c \
  ldb_traffic_rate_limit.c \
  ldb_traffic_ldb_port_rate_limit.c \
  ldb_traffic_qos.c

OBJS := $(SRCS:.c=.o)
BINS := $(SRCS:.c=)
DEPS := $(SRCS:.c=.d)

LIB := $(BASE_DIR)/lib/libdlb.so

default: $(BINS)

%: %.c $(LIB)
	@echo Compiling $@
	@$(CC) $< $(LDFLAGS) $(LDLIBS) $(CFLAGS) -o $@
	@$(CC) $< $(LDFLAGS) $(LDLIBS) $(CFLAGS) -E -M $< -MT $@ -MF $*.d

clean:
	@rm -rf $(OBJS) $(DEPS) $(BINS)

-include $(DEPS)
