#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned long long now_ns(void) {
    struct timespec ts;
    /* Use CPU-time clock so tests are independent of host load/throttling. */
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

int main(int argc, char **argv) {
    unsigned int seconds = 60;
    volatile unsigned long long acc = 0;
    unsigned long long deadline = 0;

    if (argc >= 2) {
        seconds = (unsigned int)strtoul(argv[1], NULL, 10);
        if (seconds == 0) {
            seconds = 60;
        }
    }

    deadline = now_ns() + (unsigned long long)seconds * 1000000000ULL;

    while (now_ns() < deadline) {
        /* Busy work to consume CPU. */
        acc = acc * 1664525ULL + 1013904223ULL;
        acc ^= (acc >> 13);
        acc ^= (acc << 7);
        acc ^= (acc >> 17);
    }

    printf("cpu burn done (%u cpu-s), acc=%llu\n", seconds, (unsigned long long)acc);
    return 0;
}
