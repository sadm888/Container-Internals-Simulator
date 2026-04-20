#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    unsigned int mb = 512;
    unsigned char *buffer = NULL;
    size_t bytes = 0;

    if (argc >= 2) {
        mb = (unsigned int)strtoul(argv[1], NULL, 10);
        if (mb == 0) {
            mb = 512;
        }
    }

    bytes = (size_t)mb * 1024U * 1024U;
    buffer = malloc(bytes);
    if (buffer == NULL) {
        printf("malloc(%uMB) failed: %s\n", mb, strerror(errno));
        return 1;
    }

    memset(buffer, 0xA5, bytes);
    printf("allocated and touched %uMB; pid=%d\n", mb, (int)getpid());

    /* Keep it alive long enough to observe via list/stats later. */
    sleep(60);
    free(buffer);
    return 0;
}

