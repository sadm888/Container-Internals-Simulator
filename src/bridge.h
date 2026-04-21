#ifndef BRIDGE_H
#define BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define BRIDGE_NAME   "csbr0"
#define BRIDGE_IP     "172.17.0.1"
#define BRIDGE_PREFIX 16
#define CONTAINER_GW  BRIDGE_IP

typedef struct {
    uint16_t host_port;
    uint16_t container_port;
    char     proto[4]; /* "tcp" or "udp" */
} PortMapping;

#define MAX_PORT_MAPS 8

/* Bridge lifecycle */
int  bridge_init(void);
int  bridge_teardown(void);
int  bridge_is_up(void);

/* IP pool */
int  bridge_alloc_ip(char *buf, size_t buf_size);
void bridge_mark_ip_used(const char *ip);

/* veth lifecycle */
int  bridge_setup_veth(const char *container_id, pid_t container_pid,
                       const char *container_ip,
                       char *host_veth_out, size_t host_veth_size,
                       char *peer_veth_out, size_t peer_veth_size);
int  bridge_teardown_veth(const char *host_veth);

/* Port forwarding */
int  bridge_add_port_forward(const PortMapping *pm, const char *container_ip);
int  bridge_del_port_forward(const PortMapping *pm, const char *container_ip);

/* Serialisation */
void bridge_serialize_port_maps(const PortMapping *maps, int count,
                                char *buf, size_t buf_size);
int  bridge_parse_port_maps(const char *str,
                            PortMapping *maps, int max_maps);

#endif
