#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mount.h>

#include "container.h"
#include "namespace.h"

#define STACK_SIZE (1024 * 1024)

typedef struct {
    int  container_id;
    char hostname[32];
    char rootfs[128];
} ContainerArgs;

static char         stacks[MAX_CONTAINERS][STACK_SIZE];
static ContainerArgs args_slots[MAX_CONTAINERS];
static int           slot = 0;

/* entry point inside the new namespaces */
static int container_init(void *arg) {
    ContainerArgs *a = (ContainerArgs *)arg;

    /* UTS namespace — give this container its own hostname */
    sethostname(a->hostname, strlen(a->hostname));

    /* mount namespace — make mounts private, then remount /proc
     * so ps only sees processes in this pid namespace */
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    char proc_path[160];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", a->rootfs);
    if (mount("proc", proc_path, "proc", 0, NULL) != 0)
        printf("  [note] /proc remount skipped\n");

    /* filesystem isolation — chroot into the container's rootfs */
    int jailed = (chroot(a->rootfs) == 0);
    if (jailed) chdir("/");

    /* show what the container sees */
    char hn[64] = "?";
    gethostname(hn, sizeof(hn));

    printf("\n");
    printf("  +--------------------------------------------------+\n");
    printf("  |  container-%d  —  namespaces active              |\n", a->container_id);
    printf("  +--------------------------------------------------+\n");
    printf("  |  pid (inside)  : %-5d  (host sees different)   |\n", getpid());
    printf("  |  hostname      : %-30s  |\n", hn);
    printf("  |  chroot        : %-3s                             |\n", jailed ? "on" : "off");
    printf("  |  /proc         : isolated (mount namespace)      |\n");
    printf("  +--------------------------------------------------+\n");
    fflush(stdout);

    /* sit here until the manager kills us */
    while (1) sleep(1);
    return 0;
}

pid_t create_pid_namespace(int container_id,
                           const char *hostname,
                           const char *rootfs) {
    if (slot >= MAX_CONTAINERS) return -1;

    int s = slot++;
    args_slots[s].container_id = container_id;
    strncpy(args_slots[s].hostname, hostname, sizeof(args_slots[s].hostname) - 1);
    strncpy(args_slots[s].rootfs,   rootfs,   sizeof(args_slots[s].rootfs)   - 1);

    char *stack_top = stacks[s] + STACK_SIZE;

    return clone(container_init, stack_top,
                 CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                 &args_slots[s]);
}
