#ifndef NETWORK_H
#define NETWORK_H

#define NETWORK_PROFILE "loopback up (netns)"

int network_setup_loopback(void);
const char *network_profile(void);

#endif

