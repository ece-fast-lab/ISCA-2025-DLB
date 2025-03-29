APP = dpdk-rx
CC = gcc
PKGCONF ?= pkg-config
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS = $(shell $(PKGCONF) --libs libdpdk) -libverbs
# LDFLAGS = $(shell $(PKGCONF) --libs libdpdk) -libverbs -lisal_crypto -lcrypto
# LDFLAGS = $(shell $(PKGCONF) --libs libdpdk) -libverbs -lcrypto

all: $(APP)

$(APP): main.o
	$(CC) $(CFLAGS) main.o -o $(APP) $(LDFLAGS)

main.o: main.c
	$(CC) -c $(CFLAGS) main.c -o main.o

clean:
	rm -f *.o $(APP)
