#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    unsigned int count = 128;
    unsigned int sleep_seconds = 60;
    unsigned int started = 0;

    if (argc >= 2) {
        count = (unsigned int)strtoul(argv[1], NULL, 10);
        if (count == 0) {
            count = 128;
        }
    }
    if (argc >= 3) {
        sleep_seconds = (unsigned int)strtoul(argv[2], NULL, 10);
        if (sleep_seconds == 0) {
            sleep_seconds = 60;
        }
    }

    for (unsigned int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("fork failed after %u children: %s\n", started, strerror(errno));
            break;
        }
        if (pid == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            sleep(sleep_seconds);
            _exit(0);
        }

        started++;
    }

    printf("started %u/%u children; parent pid=%d\n", started, count, (int)getpid());
    sleep(sleep_seconds);

    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* reap */
    }

    return 0;
}

