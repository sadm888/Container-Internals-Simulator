#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void print_iface(int fd, const char *name) {
    struct ifreq ifr;
    struct sockaddr_in *sin;
    char ip_buf[INET_ADDRSTRLEN];

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        printf("%-6s: not found\n", name);
        return;
    }

    int up      = (ifr.ifr_flags & IFF_UP)      ? 1 : 0;
    int running = (ifr.ifr_flags & IFF_RUNNING) ? 1 : 0;

    ip_buf[0] = '\0';
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        sin = (struct sockaddr_in *)&ifr.ifr_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
    }

    printf("%-6s: UP=%-3s RUNNING=%-3s IP=%-15s\n",
           name,
           up      ? "yes" : "no",
           running ? "yes" : "no",
           ip_buf[0] ? ip_buf : "(none)");
}

int main(int argc, char **argv) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    print_netns_id();

    if (fd < 0) {
        printf("socket failed: %s\n", strerror(errno));
        return 1;
    }

    print_iface(fd, "lo");
    print_iface(fd, "eth0");
    close(fd);

    if (argc >= 2) {
        unsigned int seconds = (unsigned int)strtoul(argv[1], NULL, 10);
        if (seconds > 0) {
            printf("sleeping %us...\n", seconds);
            fflush(stdout);
            sleep(seconds);
        }
    }
    return 0;
}
