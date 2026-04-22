#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bridge.h"

static int g_next_ip_host = 2; /* 172.17.0.2, .3, ... */

/* ── helpers ─────────────────────────────────────────────────────────── */

static int run_silent(const char *cmd, char *const argv[]) {
    pid_t pid;
    int   status;
    int   devnull;

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(cmd, argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ── bridge lifecycle ─────────────────────────────────────────────────── */

int bridge_init(void) {
    char subnet[32];
    char *argv[16];

    argv[0]="ip"; argv[1]="link"; argv[2]="add";
    argv[3]=BRIDGE_NAME; argv[4]="type"; argv[5]="bridge"; argv[6]=NULL;
    if (run_silent("ip", argv) != 0) return -1;

    snprintf(subnet, sizeof(subnet), "%s/%d", BRIDGE_IP, BRIDGE_PREFIX);
    argv[0]="ip"; argv[1]="addr"; argv[2]="add";
    argv[3]=subnet; argv[4]="dev"; argv[5]=BRIDGE_NAME; argv[6]=NULL;
    if (run_silent("ip", argv) != 0) { bridge_teardown(); return -1; }

    argv[0]="ip"; argv[1]="link"; argv[2]="set";
    argv[3]=BRIDGE_NAME; argv[4]="up"; argv[5]=NULL;
    if (run_silent("ip", argv) != 0) { bridge_teardown(); return -1; }

    { FILE *f = fopen("/proc/sys/net/ipv4/ip_forward", "w");
      if (f) { fputs("1\n", f); fclose(f); } }

    /* MASQUERADE so containers can reach the internet */
    argv[0]="iptables"; argv[1]="-t"; argv[2]="nat";
    argv[3]="-A"; argv[4]="POSTROUTING";
    argv[5]="-s"; argv[6]="172.17.0.0/16";
    argv[7]="!"; argv[8]="-o"; argv[9]=BRIDGE_NAME;
    argv[10]="-j"; argv[11]="MASQUERADE"; argv[12]=NULL;
    run_silent("iptables", argv); /* best-effort */

    return 0;
}

int bridge_teardown(void) {
    char *argv[16];

    argv[0]="iptables"; argv[1]="-t"; argv[2]="nat";
    argv[3]="-D"; argv[4]="POSTROUTING";
    argv[5]="-s"; argv[6]="172.17.0.0/16";
    argv[7]="!"; argv[8]="-o"; argv[9]=BRIDGE_NAME;
    argv[10]="-j"; argv[11]="MASQUERADE"; argv[12]=NULL;
    run_silent("iptables", argv);

    argv[0]="ip"; argv[1]="link"; argv[2]="delete";
    argv[3]=BRIDGE_NAME; argv[4]=NULL;
    return run_silent("ip", argv);
}

int bridge_is_up(void) {
    int fd;
    struct ifreq ifr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", BRIDGE_NAME);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) { close(fd); return 0; }
    close(fd);
    return (ifr.ifr_flags & IFF_UP) ? 1 : 0;
}

/* ── IP allocation ────────────────────────────────────────────────────── */

void bridge_mark_ip_used(const char *ip) {
    int last;
    if (ip == NULL || ip[0] == '\0') return;
    if (sscanf(ip, "172.17.0.%d", &last) == 1 && last >= g_next_ip_host)
        g_next_ip_host = last + 1;
}

int bridge_alloc_ip(char *buf, size_t buf_size) {
    if (g_next_ip_host > 254) { errno = ENOSPC; return -1; }
    snprintf(buf, buf_size, "172.17.0.%d", g_next_ip_host++);
    return 0;
}

/* ── veth lifecycle ───────────────────────────────────────────────────── */

int bridge_setup_veth(const char *container_id,
                      pid_t       container_pid,
                      const char *container_ip __attribute__((unused)),
                      char       *host_veth_out, size_t host_veth_size,
                      char       *peer_veth_out, size_t peer_veth_size) {
    char host[16], peer[16], pid_s[16];
    char *argv[16];
    const char *suf;

    suf = strrchr(container_id, '-');
    suf = suf ? suf + 1 : container_id;

    snprintf(host, sizeof(host), "veth%s", suf);
    snprintf(peer, sizeof(peer), "csp%s",  suf);
    snprintf(pid_s, sizeof(pid_s), "%d", (int)container_pid);

    /* create veth pair */
    argv[0]="ip"; argv[1]="link"; argv[2]="add";
    argv[3]=host; argv[4]="type"; argv[5]="veth";
    argv[6]="peer"; argv[7]="name"; argv[8]=peer; argv[9]=NULL;
    if (run_silent("ip", argv) != 0) return -1;

    /* attach host side to bridge */
    argv[0]="ip"; argv[1]="link"; argv[2]="set";
    argv[3]=host; argv[4]="master"; argv[5]=BRIDGE_NAME; argv[6]=NULL;
    if (run_silent("ip", argv) != 0) goto fail;

    /* bring host side up */
    argv[0]="ip"; argv[1]="link"; argv[2]="set";
    argv[3]=host; argv[4]="up"; argv[5]=NULL;
    if (run_silent("ip", argv) != 0) goto fail;

    /* move peer into container's network namespace by PID */
    argv[0]="ip"; argv[1]="link"; argv[2]="set";
    argv[3]=peer; argv[4]="netns"; argv[5]=pid_s; argv[6]=NULL;
    if (run_silent("ip", argv) != 0) goto fail;

    if (host_veth_out) snprintf(host_veth_out, host_veth_size, "%s", host);
    if (peer_veth_out) snprintf(peer_veth_out, peer_veth_size, "%s", peer);
    return 0;

fail:
    { char *d[] = {"ip","link","delete",host,NULL}; run_silent("ip", d); }
    return -1;
}

int bridge_teardown_veth(const char *host_veth) {
    char *argv[] = {"ip","link","delete",(char *)host_veth,NULL};
    return run_silent("ip", argv);
}

/* ── port forwarding ──────────────────────────────────────────────────── */

static int iptables_dnat(const char *op,
                         const PortMapping *pm,
                         const char *container_ip) {
    char hport[8], dest[32];
    char *argv[20];

    snprintf(hport, sizeof(hport), "%d", (int)pm->host_port);
    snprintf(dest,  sizeof(dest),  "%s:%d", container_ip, (int)pm->container_port);

    argv[0]="iptables"; argv[1]="-t"; argv[2]="nat";
    argv[3]=(char *)op; argv[4]="PREROUTING";
    argv[5]="-p";       argv[6]=(char *)pm->proto;
    argv[7]="--dport";  argv[8]=hport;
    argv[9]="-j";       argv[10]="DNAT";
    argv[11]="--to-destination"; argv[12]=dest; argv[13]=NULL;
    return run_silent("iptables", argv);
}

int bridge_add_port_forward(const PortMapping *pm, const char *container_ip) {
    return iptables_dnat("-A", pm, container_ip);
}

int bridge_del_port_forward(const PortMapping *pm, const char *container_ip) {
    return iptables_dnat("-D", pm, container_ip);
}

/* ── serialisation ────────────────────────────────────────────────────── */

void bridge_serialize_port_maps(const PortMapping *maps, int count,
                                char *buf, size_t buf_size) {
    int    i;
    size_t off = 0;

    buf[0] = '\0';
    for (i = 0; i < count && off < buf_size; i++) {
        off += (size_t)snprintf(buf + off, buf_size - off, "%s%d:%d/%s",
                                i ? "," : "",
                                (int)maps[i].host_port,
                                (int)maps[i].container_port,
                                maps[i].proto);
    }
}

int bridge_parse_port_maps(const char *str,
                           PortMapping *maps, int max_maps) {
    char  buf[256];
    char *tok;
    int   count = 0;

    if (str == NULL || str[0] == '\0') return 0;
    snprintf(buf, sizeof(buf), "%s", str);

    tok = strtok(buf, ",");
    while (tok != NULL && count < max_maps) {
        PortMapping *pm = &maps[count];
        int  hp, cp;
        char proto[4] = "tcp";

        if (sscanf(tok, "%d:%d/%3s", &hp, &cp, proto) >= 2 ||
            sscanf(tok, "%d:%d",     &hp, &cp)         == 2) {
            pm->host_port      = (uint16_t)hp;
            pm->container_port = (uint16_t)cp;
            snprintf(pm->proto, sizeof(pm->proto), "%s", proto);
            count++;
        }
        tok = strtok(NULL, ",");
    }
    return count;
}
