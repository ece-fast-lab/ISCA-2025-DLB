#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/mman.h>

#include "rdma_ldb_dlb.c"

int get_mem_info() {
    FILE *file;
    char filename[256];
    char line[256];
    pid_t pid = getpid();

    snprintf(filename, sizeof(filename), "/proc/%d/smaps", pid);
    file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        return -1;
    }

    while (fgets(line, sizeof(line), file)) {
        printf("%s", line);
    }

    fclose(file);
    return 0;
}


int main (int argc, char *argv[]) {

    int server_sockfd = -1;
    int client_sockfd = -1;
    int snic_sockfd = -1;
    int ret;

    ret = parse_args(argc, argv);
    printf("%d application arguments used\n", ret);
    if (ret < 0) {
        printf("Invalid application arguments\n");
        return -1;
    }
    host_type = SERVER;

    // initialize DLB
    ret = ldb_traffic(0, NULL);
    return ret;
}