// main.cpp
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include "pebs.h"

#define BIN_SHIFT 21                    // 2 MiB
#define NODE_SIZE (30ULL << 30)         // 30 GiB
#define K_TOP (NODE_SIZE >> BIN_SHIFT)   // 10240

static size_t map_bytes() {
    size_t bytes = (1+K_TOP) * sizeof(uint64_t);
    long pg = sysconf(_SC_PAGESIZE);
    return ((bytes + pg -1) / pg) * pg;
}

void handle_sigint(int sig) {
    (void)sig;
    printf("SIGINT received, stopping sampler...\n");
    sampler_deinit();
    printf("PEBS sampler 종료 완료\n");
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "No pid!!! -> ./main pid\n");
        return EXIT_FAILURE;
    }

    int fd = open("/dev/mmap_demo", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open"); return 1; }

    size_t len = map_bytes();
    //printf("user len=%zu\n", len);
    uint64_t *arr = static_cast<uint64_t*>(
        mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
    );
    if (arr == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    set_shared_arr(arr);

    pid_t target = (pid_t)atoi(argv[1]);
    set_target_pid(target);

    if (signal(SIGINT, handle_sigint) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    sampler_stop = 0;
    sampler_init();

    printf("Start PEBS sampling!! Ctrl+C to stop\n");
    for (;;) {
        pause();
    }

    munmap(arr, len);  
    close(fd);
    return 0;
}

