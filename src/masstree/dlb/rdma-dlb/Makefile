.PHONY: all clean

CC = gcc
LDLIBS = -libverbs -lrt -lm -L$(REPO_PATH)/src/dlb_8.9.0/libdlb/ -pthread
CFLAGS = -Wall -g -O3 -march=native
APPS = rdma_dlb_snic
OBJS = client.o server.o common.o snic.o

all: ${APPS}

rdma_dlb_snic: snic.o common.o
	$(CC) $(CFLAGS) -o rdma_dlb_snic snic.o common.o $(LDLIBS) 

snic.o: snic.c common.h
	$(CC) $(CFLAGS) -c -o snic.o snic.c

common.o: common.c common.h
	$(CC) $(CFLAGS) -c -o common.o common.c

clean:
	rm -f *.o $(APPS)
