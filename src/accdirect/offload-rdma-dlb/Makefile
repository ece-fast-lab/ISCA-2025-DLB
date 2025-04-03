.PHONY: all clean

CC = gcc
LDLIBS = -libverbs -lrt -lm -L$(REPO_PATH)/src/dlb_8.9.0/libdlb/ -lpthread
CFLAGS = -O3 -Wall -g -march=native -D_GNU_SOURCE
APPS = rdma_dlb_client rdma_dlb_server rdma_dlb_snic
OBJS = client.o server.o common.o snic.o

all: ${APPS}

rdma_dlb_server: server.o common.o
	$(CC) $(CFLAGS) -o rdma_dlb_server server.o common.o $(LDLIBS) -ldlb

rdma_dlb_client: client.o common.o
	$(CC) $(CFLAGS) -o rdma_dlb_client client.o common.o $(LDLIBS)

rdma_dlb_snic: snic.o common.o
	$(CC) $(CFLAGS) -o rdma_dlb_snic snic.o common.o $(LDLIBS)

server.o: server.c common.c
	$(CC) $(CFLAGS) -c -o server.o server.c

client.o: client.c common.h
	$(CC) $(CFLAGS) -c -o client.o client.c

snic.o: snic.c common.h
	$(CC) $(CFLAGS) -c -o snic.o snic.c

common.o: common.c common.h
	$(CC) $(CFLAGS) -c -o common.o common.c

clean:
	rm -f *.o $(APPS)
