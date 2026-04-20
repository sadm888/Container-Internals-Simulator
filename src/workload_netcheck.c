#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <net/if.h>

static void print_netns_id(void) {
    char buf[256];
    ssize_t n = readlink("/proc/self/ns/net", buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("netns: readlink failed: %s\n", strerror(errno));
        return;
    }
    buf[n] = '\0';
    printf("netns: %s\n", buf);
}

static void print_lo_flags(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    if (fd < 0) {
        printf("lo: socket failed: %s\n", strerror(errno));
        return;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "lo");

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        int saved = errno;
        close(fd);
        printf("lo: SIOCGIFFLAGS failed: %s\n", strerror(saved));
        return;
    }

    printf("lo: flags=0x%x UP=%s RUNNING=%s\n",
           (unsigned int)(unsigned short)ifr.ifr_flags,
           (ifr.ifr_flags & IFF_UP) ? "yes" : "no",
           (ifr.ifr_flags & IFF_RUNNING) ? "yes" : "no");

    close(fd);
}

int main(int argc, char **argv) {
    print_netns_id();
    print_lo_flags();
    if (argc >= 2) {
        unsigned int seconds = (unsigned int)strtoul(argv[1], NULL, 10);
        if (seconds > 0) {
            sleep(seconds);
        }
    }
    return 0;
}
