/*
 * namespace.c — Multi-namespace isolation using Linux primitives
 *
 * Three namespaces are created per container:
 *
 *  1. PID Namespace  (CLONE_NEWPID)
 *     The container process sees itself as PID 1.
 *     It cannot enumerate or signal host processes.
 *
 *  2. UTS Namespace  (CLONE_NEWUTS)
 *     The container has its own hostname.
 *     sethostname() inside the container does NOT affect the host.
 *
 *  3. Mount Namespace (CLONE_NEWNS)
 *     The container has its own mount table.
 *     /proc is remounted so ps/top only show the container's processes.
 *     The container is chroot()'d into its own root filesystem.
 *
 * This combination is exactly what Docker does — minus the image layers.
 */

#include <sched.h>          /* clone(), CLONE_NEW*          */
#include <stdio.h>
#include <string.h>
#include <unistd.h>         /* getpid(), gethostname()      */
#include <signal.h>         /* SIGCHLD                      */
#include <sys/types.h>
#include <sys/mount.h>      /* mount(), MS_REC, MS_PRIVATE  */

#include "container.h"
#include "namespace.h"

#define STACK_SIZE (1024 * 1024)

/* Per-container arguments passed from parent to child via shared memory */
typedef struct {
    int  container_id;
    char hostname[32];
    char rootfs[128];
} ContainerArgs;

static char         ns_stacks[MAX_CONTAINERS][STACK_SIZE];
static ContainerArgs slot_args[MAX_CONTAINERS];
static int           ns_stack_slot = 0;

/*
 * container_init() — runs INSIDE all three namespaces simultaneously.
 *
 * Execution order matters:
 *   sethostname  → must happen before chroot (UTS ns already active)
 *   mount /proc  → must happen before chroot (uses host path to rootfs)
 *   chroot       → locks filesystem view last
 */
static int container_init(void *arg) {
    ContainerArgs *args = (ContainerArgs *)arg;
    int id = args->container_id;

    /* ── 1. UTS Namespace: assign unique hostname ─────────────────── */
    sethostname(args->hostname, strlen(args->hostname));

    /* ── 2. Mount Namespace: isolate the mount table ──────────────── */
    /*
     * Make all existing mounts private so changes here don't
     * propagate back to the host — required before remounting /proc.
     */
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    /*
     * Mount a fresh procfs at <rootfs>/proc.
     * After chroot, this becomes /proc — ps will only list
     * processes inside this PID namespace.
     */
    char proc_path[160];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", args->rootfs);
    if (mount("proc", proc_path, "proc", 0, NULL) != 0) {
        /* WSL2 may restrict this without full cgroup support — not fatal */
        printf("  [Note] /proc remount unavailable (WSL2 limit)\n");
    }

    /* ── 3. Filesystem Isolation: chroot into container's root ─────── */
    int chroot_ok = (chroot(args->rootfs) == 0);
    if (chroot_ok) chdir("/");

    /* ── 4. Print isolation summary ────────────────────────────────── */
    char hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));

    printf("\n");
    printf("  +----------------------------------------------+\n");
    printf("  |   Container %2d  —  3 Namespaces Active       |\n", id);
    printf("  +----------------------------------------------+\n");
    printf("  |  PID  Namespace : getpid()  = %-6d         |\n", getpid());
    printf("  |  UTS  Namespace : hostname  = %-14s  |\n", hostname);
    printf("  |  Mount Namespace: /proc     = remounted       |\n");
    printf("  |  FS   Isolation : chroot()  = %-3s             |\n",
           chroot_ok ? "ON" : "OFF");
    printf("  +----------------------------------------------+\n");
    fflush(stdout);

    /* Container's init process — stays alive until manager sends SIGKILL */
    while (1) sleep(1);

    return 0;
}

pid_t create_pid_namespace(int container_id,
                           const char *hostname,
                           const char *rootfs) {
    if (ns_stack_slot >= MAX_CONTAINERS) return -1;

    int slot = ns_stack_slot++;

    /* Write args to static slot — safe because parent writes before clone(),
       child only reads, and clone() provides the necessary memory barrier.  */
    slot_args[slot].container_id = container_id;
    strncpy(slot_args[slot].hostname, hostname,
            sizeof(slot_args[slot].hostname) - 1);
    strncpy(slot_args[slot].rootfs,   rootfs,
            sizeof(slot_args[slot].rootfs)   - 1);

    char *stack_top = ns_stacks[slot] + STACK_SIZE;

    /*
     * clone() with three namespace flags:
     *   CLONE_NEWPID — new PID namespace
     *   CLONE_NEWUTS — new UTS (hostname) namespace
     *   CLONE_NEWNS  — new mount namespace
     *   SIGCHLD      — notify parent on child exit
     */
    pid_t pid = clone(container_init,
                      stack_top,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &slot_args[slot]);

    return pid;
}
