#include <arpa/inet.h>
#include <errno.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "network.h"

/* ── loopback (existing) ─────────────────────────────────────────────── */

int network_setup_loopback(void) {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "lo");

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP | IFF_RUNNING);
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    close(fd);
    return 0;
}

/* ── eth0 inside container ───────────────────────────────────────────── */

static int iface_rename(int fd, const char *oldname, const char *newname) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name,    IFNAMSIZ, "%s", oldname);
    snprintf(ifr.ifr_newname, IFNAMSIZ, "%s", newname);
    return ioctl(fd, SIOCSIFNAME, &ifr);
}

static int iface_set_ip(int fd, const char *iface,
                        const char *ip, const char *netmask) {
    struct ifreq ifr;
    struct sockaddr_in *sin;

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;

    inet_pton(AF_INET, ip, &sin->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) return -1;

    inet_pton(AF_INET, netmask, &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) return -1;

    return 0;
}

static int iface_up(int fd, const char *iface) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) return -1;
    ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP | IFF_RUNNING);
    return ioctl(fd, SIOCSIFFLAGS, &ifr);
}

/* Add default route via RTM_NEWROUTE netlink message. */
static int add_default_route(const char *gateway) {
    struct {
        struct nlmsghdr nh;
        struct rtmsg    rt;
        char            buf[128];
    } req;
    struct rtattr   *rta;
    struct sockaddr_nl sa;
    struct in_addr   gw;
    char             rbuf[256];
    ssize_t          n;
    int              fd;

    if (inet_pton(AF_INET, gateway, &gw) != 1) return -1;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;

    memset(&req, 0, sizeof(req));
    req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.nh.nlmsg_type  = RTM_NEWROUTE;
    req.rt.rtm_family   = AF_INET;
    req.rt.rtm_dst_len  = 0;          /* default route */
    req.rt.rtm_table    = RT_TABLE_MAIN;
    req.rt.rtm_protocol = RTPROT_STATIC;
    req.rt.rtm_scope    = RT_SCOPE_UNIVERSE;
    req.rt.rtm_type     = RTN_UNICAST;

    rta = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = RTA_GATEWAY;
    rta->rta_len  = RTA_LENGTH(sizeof(gw));
    memcpy(RTA_DATA(rta), &gw, sizeof(gw));
    req.nh.nlmsg_len = (uint32_t)(NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_LENGTH(sizeof(gw)));

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (sendto(fd, &req, req.nh.nlmsg_len, 0,
               (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd); return -1;
    }
    n = recv(fd, rbuf, sizeof(rbuf), 0);
    close(fd);
    if (n < 0) return -1;

    {
        struct nlmsghdr *nh2 = (struct nlmsghdr *)rbuf;
        if (nh2->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nh2);
            if (err->error != 0) { errno = -err->error; return -1; }
        }
    }
    return 0;
}

/*
 * Called inside the container (after pivot_root) to configure eth0.
 * Renames peer_veth → "eth0", assigns ip/mask, brings it up, adds default route.
 * No-op when cfg->ip is empty (bridge not active).
 */
int network_setup_eth0(const NetConfig *cfg) {
    int fd;

    if (cfg == NULL || cfg->ip[0] == '\0' || cfg->peer_veth[0] == '\0')
        return 0;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    if (iface_rename(fd, cfg->peer_veth, "eth0") != 0) {
        close(fd); return -1;
    }
    if (iface_set_ip(fd, "eth0", cfg->ip, "255.255.0.0") != 0) {
        close(fd); return -1;
    }
    if (iface_up(fd, "eth0") != 0) {
        close(fd); return -1;
    }
    close(fd);

    if (cfg->gateway[0] != '\0')
        add_default_route(cfg->gateway); /* best-effort */

    return 0;
}

const char *network_profile(void) {
    return NETWORK_PROFILE;
}
