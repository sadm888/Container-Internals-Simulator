#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdarg.h>
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
static char g_last_error[256];

static void set_last_error(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
}

void bridge_clear_last_error(void) {
    g_last_error[0] = '\0';
}

const char *bridge_last_error(void) {
    return g_last_error[0] != '\0' ? g_last_error : "networking unavailable";
}

static void format_command(char *buf, size_t buf_size, char *const argv[]) {
    size_t off = 0;

    if (buf == NULL || buf_size == 0) {
        return;
    }

    buf[0] = '\0';
    if (argv == NULL) {
        return;
    }

    for (int i = 0; argv[i] != NULL && off + 1 < buf_size; i++) {
        int written = snprintf(buf + off, buf_size - off, "%s%s",
                               i == 0 ? "" : " ",
                               argv[i]);
        if (written < 0 || (size_t)written >= buf_size - off) {
            break;
        }
        off += (size_t)written;
    }
}

static int command_in_path(const char *cmd) {
    const char *path_env;
    const char *start;

    if (cmd == NULL || cmd[0] == '\0') {
        return 0;
    }
    if (strchr(cmd, '/') != NULL) {
        return access(cmd, X_OK) == 0;
    }

    path_env = getenv("PATH");
    if (path_env == NULL || path_env[0] == '\0') {
        return 0;
    }

    start = path_env;
    while (*start != '\0') {
        const char *end = strchr(start, ':');
        char candidate[256];
        size_t len = end ? (size_t)(end - start) : strlen(start);

        if (len == 0) {
            snprintf(candidate, sizeof(candidate), "./%s", cmd);
        } else {
            snprintf(candidate, sizeof(candidate), "%.*s/%s",
                     (int)len, start, cmd);
        }

        if (access(candidate, X_OK) == 0) {
            return 1;
        }

        if (end == NULL) {
            break;
        }
        start = end + 1;
    }

    return 0;
}

static int read_ip_forward_state(void) {
    FILE *f;
    char buf[16];

    f = fopen("/proc/sys/net/ipv4/ip_forward", "r");
    if (f == NULL) {
        return -1;
    }
    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return buf[0] == '1' ? 1 : 0;
}

void bridge_collect_doctor(BridgeDoctor *doctor) {
    if (doctor == NULL) {
        return;
    }

    memset(doctor, 0, sizeof(*doctor));
    doctor->is_root      = geteuid() == 0;
    doctor->ip_cmd       = command_in_path("ip");
    doctor->iptables_cmd = command_in_path("iptables");
    doctor->bridge_up    = bridge_is_up();
    doctor->ip_forward   = read_ip_forward_state();
}

int bridge_preflight(int require_bridge_up, int require_iptables) {
    BridgeDoctor doctor;

    bridge_collect_doctor(&doctor);
    bridge_clear_last_error();

    if (!doctor.ip_cmd) {
        errno = ENOENT;
        set_last_error("missing 'ip' command; install iproute2/iproute");
        return -1;
    }
    if (!doctor.is_root) {
        errno = EPERM;
        set_last_error("bridge networking requires root or CAP_NET_ADMIN");
        return -1;
    }
    if (require_iptables && !doctor.iptables_cmd) {
        errno = ENOENT;
        set_last_error("missing 'iptables' command; published ports require iptables");
        return -1;
    }
    if (require_bridge_up && !doctor.bridge_up) {
        errno = ENETDOWN;
        set_last_error("bridge %s is down; run 'net init' as root first", BRIDGE_NAME);
        return -1;
    }
    if (require_iptables && doctor.ip_forward == 0) {
        errno = ENETDOWN;
        set_last_error("host IPv4 forwarding is disabled (/proc/sys/net/ipv4/ip_forward = 0)");
        return -1;
    }

    return 0;
}

void bridge_print_doctor(void) {
    BridgeDoctor doctor;

    bridge_collect_doctor(&doctor);

    printf("\n");
    printf("  Network doctor\n");
    printf("  --------------\n");
    printf("  privilege : %s\n", doctor.is_root ? "root/CAP_NET_ADMIN OK" : "needs root/CAP_NET_ADMIN");
    printf("  ip        : %s\n", doctor.ip_cmd ? "found" : "missing");
    printf("  iptables  : %s\n", doctor.iptables_cmd ? "found" : "missing");
    printf("  bridge    : %s\n", doctor.bridge_up ? "UP" : "DOWN");
    if (doctor.ip_forward < 0) {
        printf("  ip_forward: unreadable\n");
    } else {
        printf("  ip_forward: %s\n", doctor.ip_forward ? "enabled" : "disabled");
    }
    if (doctor.ip_cmd && doctor.is_root && doctor.bridge_up) {
        printf("  mode      : bridge networking can be attempted\n");
    } else {
        printf("  mode      : loopback-only fallback is likely\n");
    }
    printf("\n");
}

static int run_silent(const char *cmd, char *const argv[], const char *step) {
    pid_t pid;
    int   status;
    int   devnull;
    char  command_buf[192];

    format_command(command_buf, sizeof(command_buf), argv);

    pid = fork();
    if (pid < 0) {
        set_last_error("%s failed before exec: %s", step, strerror(errno));
        return -1;
    }
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
    if (waitpid(pid, &status, 0) < 0) {
        set_last_error("%s failed while waiting: %s", step, strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        errno = ENOENT;
        set_last_error("%s failed: command not found while running `%s`", step, command_buf);
        return -1;
    }
    if (WIFEXITED(status)) {
        errno = EIO;
        set_last_error("%s failed: `%s` exited with status %d",
                       step, command_buf, WEXITSTATUS(status));
        return -1;
    }

    errno = EIO;
    set_last_error("%s failed: `%s` terminated by signal %d",
                   step, command_buf, WTERMSIG(status));
    return -1;
}

int bridge_init(void) {
    char subnet[32];
    char *argv[16];

    bridge_clear_last_error();
    if (bridge_is_up()) {
        return 0;
    }
    if (bridge_preflight(0, 0) != 0) {
        return -1;
    }

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "add";
    argv[3] = BRIDGE_NAME; argv[4] = "type"; argv[5] = "bridge"; argv[6] = NULL;
    if (run_silent("ip", argv, "bridge create") != 0) return -1;

    snprintf(subnet, sizeof(subnet), "%s/%d", BRIDGE_IP, BRIDGE_PREFIX);
    argv[0] = "ip"; argv[1] = "addr"; argv[2] = "add";
    argv[3] = subnet; argv[4] = "dev"; argv[5] = BRIDGE_NAME; argv[6] = NULL;
    if (run_silent("ip", argv, "bridge address assignment") != 0) { bridge_teardown(); return -1; }

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "set";
    argv[3] = BRIDGE_NAME; argv[4] = "up"; argv[5] = NULL;
    if (run_silent("ip", argv, "bridge bring-up") != 0) { bridge_teardown(); return -1; }

    {
        FILE *f = fopen("/proc/sys/net/ipv4/ip_forward", "w");
        if (f) { fputs("1\n", f); fclose(f); }
    }

    argv[0] = "iptables"; argv[1] = "-t"; argv[2] = "nat";
    argv[3] = "-A"; argv[4] = "POSTROUTING";
    argv[5] = "-s"; argv[6] = "172.17.0.0/16";
    argv[7] = "!"; argv[8] = "-o"; argv[9] = BRIDGE_NAME;
    argv[10] = "-j"; argv[11] = "MASQUERADE"; argv[12] = NULL;
    (void)run_silent("iptables", argv, "bridge masquerade rule"); /* best-effort */

    return 0;
}

int bridge_teardown(void) {
    char *argv[16];

    bridge_clear_last_error();
    if (!bridge_is_up()) {
        return 0;
    }

    argv[0] = "iptables"; argv[1] = "-t"; argv[2] = "nat";
    argv[3] = "-D"; argv[4] = "POSTROUTING";
    argv[5] = "-s"; argv[6] = "172.17.0.0/16";
    argv[7] = "!"; argv[8] = "-o"; argv[9] = BRIDGE_NAME;
    argv[10] = "-j"; argv[11] = "MASQUERADE"; argv[12] = NULL;
    (void)run_silent("iptables", argv, "bridge masquerade cleanup");

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "delete";
    argv[3] = BRIDGE_NAME; argv[4] = NULL;
    return run_silent("ip", argv, "bridge delete");
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

void bridge_mark_ip_used(const char *ip) {
    int last;
    if (ip == NULL || ip[0] == '\0') return;
    if (sscanf(ip, "172.17.0.%d", &last) == 1 && last >= g_next_ip_host)
        g_next_ip_host = last + 1;
}

int bridge_alloc_ip(char *buf, size_t buf_size) {
    bridge_clear_last_error();
    if (g_next_ip_host > 254) {
        errno = ENOSPC;
        set_last_error("bridge IP pool exhausted in 172.17.0.0/16");
        return -1;
    }
    snprintf(buf, buf_size, "172.17.0.%d", g_next_ip_host++);
    return 0;
}

int bridge_setup_veth(const char *container_id,
                      pid_t       container_pid,
                      const char *container_ip __attribute__((unused)),
                      char       *host_veth_out, size_t host_veth_size,
                      char       *peer_veth_out, size_t peer_veth_size) {
    char host[16], peer[16], pid_s[16];
    char *argv[16];
    const char *suf;

    bridge_clear_last_error();
    suf = strrchr(container_id, '-');
    suf = suf ? suf + 1 : container_id;

    snprintf(host, sizeof(host), "veth%s", suf);
    snprintf(peer, sizeof(peer), "csp%s", suf);
    snprintf(pid_s, sizeof(pid_s), "%d", (int)container_pid);

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "add";
    argv[3] = host; argv[4] = "type"; argv[5] = "veth";
    argv[6] = "peer"; argv[7] = "name"; argv[8] = peer; argv[9] = NULL;
    if (run_silent("ip", argv, "veth pair creation") != 0) return -1;

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "set";
    argv[3] = host; argv[4] = "master"; argv[5] = BRIDGE_NAME; argv[6] = NULL;
    if (run_silent("ip", argv, "bridge attach") != 0) goto fail;

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "set";
    argv[3] = host; argv[4] = "up"; argv[5] = NULL;
    if (run_silent("ip", argv, "host veth bring-up") != 0) goto fail;

    argv[0] = "ip"; argv[1] = "link"; argv[2] = "set";
    argv[3] = peer; argv[4] = "netns"; argv[5] = pid_s; argv[6] = NULL;
    if (run_silent("ip", argv, "move veth peer into container netns") != 0) goto fail;

    if (host_veth_out) snprintf(host_veth_out, host_veth_size, "%s", host);
    if (peer_veth_out) snprintf(peer_veth_out, peer_veth_size, "%s", peer);
    return 0;

fail:
    {
        char *d[] = {"ip", "link", "delete", host, NULL};
        (void)run_silent("ip", d, "veth cleanup");
    }
    return -1;
}

int bridge_teardown_veth(const char *host_veth) {
    char *argv[] = {"ip", "link", "delete", (char *)host_veth, NULL};
    bridge_clear_last_error();
    return run_silent("ip", argv, "veth delete");
}

static int iptables_dnat(const char *op,
                         const PortMapping *pm,
                         const char *container_ip) {
    char hport[8], dest[32];
    char *argv[20];

    snprintf(hport, sizeof(hport), "%d", (int)pm->host_port);
    snprintf(dest, sizeof(dest), "%s:%d", container_ip, (int)pm->container_port);

    argv[0] = "iptables"; argv[1] = "-t"; argv[2] = "nat";
    argv[3] = (char *)op; argv[4] = "PREROUTING";
    argv[5] = "-p"; argv[6] = (char *)pm->proto;
    argv[7] = "--dport"; argv[8] = hport;
    argv[9] = "-j"; argv[10] = "DNAT";
    argv[11] = "--to-destination"; argv[12] = dest; argv[13] = NULL;
    return run_silent("iptables", argv,
                      strcmp(op, "-A") == 0 ? "port forward install" : "port forward removal");
}

int bridge_add_port_forward(const PortMapping *pm, const char *container_ip) {
    bridge_clear_last_error();
    return iptables_dnat("-A", pm, container_ip);
}

int bridge_del_port_forward(const PortMapping *pm, const char *container_ip) {
    bridge_clear_last_error();
    return iptables_dnat("-D", pm, container_ip);
}

void bridge_serialize_port_maps(const PortMapping *maps, int count,
                                char *buf, size_t buf_size) {
    int i;
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
    char buf[256];
    char *tok;
    int count = 0;

    if (str == NULL || str[0] == '\0') return 0;
    snprintf(buf, sizeof(buf), "%s", str);

    tok = strtok(buf, ",");
    while (tok != NULL && count < max_maps) {
        PortMapping *pm = &maps[count];
        int hp, cp;
        char proto[4] = "tcp";

        if (sscanf(tok, "%d:%d/%3s", &hp, &cp, proto) >= 2 ||
            sscanf(tok, "%d:%d", &hp, &cp) == 2) {
            if (hp < 1 || hp > 65535 || cp < 1 || cp > 65535) {
                tok = strtok(NULL, ",");
                continue;
            }
            pm->host_port = (uint16_t)hp;
            pm->container_port = (uint16_t)cp;
            snprintf(pm->proto, sizeof(pm->proto), "%s", proto);
            count++;
        }
        tok = strtok(NULL, ",");
    }
    return count;
}
