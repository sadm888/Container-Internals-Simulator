#ifndef NETWORK_H
#define NETWORK_H

#define NETWORK_PROFILE "loopback up (netns)"

/* Passed from parent → child via continue_pipe to configure eth0 inside the container. */
typedef struct {
    char peer_veth[16]; /* "" or "cspXXXX" — interface name inside the container */
    char ip[16];        /* "" or "172.17.0.N" */
    char gateway[16];   /* "" or "172.17.0.1" */
} NetConfig;

int         network_setup_loopback(void);
int         network_setup_eth0(const NetConfig *cfg); /* called inside container */
const char *network_profile(void);

#endif
