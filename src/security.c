#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>

#include "security.h"

/* SECCOMP_RET_KILL_PROCESS requires Linux 4.14+; fall back to KILL. */
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif

/* ── Constructors ────────────────────────────────────────────────────── */

SecurityConfig security_config_default(void) {
    SecurityConfig cfg;
    cfg.privileged      = 0;
    cfg.readonly_rootfs = 0;
    cfg.cap_keep        = SECURITY_CAP_DEFAULT_KEEP;
    cfg.seccomp_enabled = 1;
    return cfg;
}

SecurityConfig security_config_none(void) {
    SecurityConfig cfg;
    cfg.privileged      = 1;
    cfg.readonly_rootfs = 0;
    cfg.cap_keep        = ~UINT64_C(0); /* all caps */
    cfg.seccomp_enabled = 0;
    return cfg;
}

/* ── Capability name → number ────────────────────────────────────────── */

static const struct { const char *name; int nr; } g_cap_names[] = {
    {"CAP_CHOWN",            0},  {"CHOWN",            0},
    {"CAP_DAC_OVERRIDE",     1},  {"DAC_OVERRIDE",     1},
    {"CAP_DAC_READ_SEARCH",  2},  {"DAC_READ_SEARCH",  2},
    {"CAP_FOWNER",           3},  {"FOWNER",           3},
    {"CAP_FSETID",           4},  {"FSETID",           4},
    {"CAP_KILL",             5},  {"KILL",             5},
    {"CAP_SETGID",           6},  {"SETGID",           6},
    {"CAP_SETUID",           7},  {"SETUID",           7},
    {"CAP_SETPCAP",          8},  {"SETPCAP",          8},
    {"CAP_LINUX_IMMUTABLE",  9},  {"LINUX_IMMUTABLE",  9},
    {"CAP_NET_BIND_SERVICE", 10}, {"NET_BIND_SERVICE", 10},
    {"CAP_NET_BROADCAST",    11}, {"NET_BROADCAST",    11},
    {"CAP_NET_ADMIN",        12}, {"NET_ADMIN",        12},
    {"CAP_NET_RAW",          13}, {"NET_RAW",          13},
    {"CAP_IPC_LOCK",         14}, {"IPC_LOCK",         14},
    {"CAP_IPC_OWNER",        15}, {"IPC_OWNER",        15},
    {"CAP_SYS_MODULE",       16}, {"SYS_MODULE",       16},
    {"CAP_SYS_RAWIO",        17}, {"SYS_RAWIO",        17},
    {"CAP_SYS_CHROOT",       18}, {"SYS_CHROOT",       18},
    {"CAP_SYS_PTRACE",       19}, {"SYS_PTRACE",       19},
    {"CAP_SYS_PACCT",        20}, {"SYS_PACCT",        20},
    {"CAP_SYS_ADMIN",        21}, {"SYS_ADMIN",        21},
    {"CAP_SYS_BOOT",         22}, {"SYS_BOOT",         22},
    {"CAP_SYS_NICE",         23}, {"SYS_NICE",         23},
    {"CAP_SYS_RESOURCE",     24}, {"SYS_RESOURCE",     24},
    {"CAP_SYS_TIME",         25}, {"SYS_TIME",         25},
    {"CAP_SYS_TTY_CONFIG",   26}, {"SYS_TTY_CONFIG",   26},
    {"CAP_MKNOD",            27}, {"MKNOD",            27},
    {"CAP_LEASE",            28}, {"LEASE",            28},
    {"CAP_AUDIT_WRITE",      29}, {"AUDIT_WRITE",      29},
    {"CAP_AUDIT_CONTROL",    30}, {"AUDIT_CONTROL",    30},
    {"CAP_SETFCAP",          31}, {"SETFCAP",          31},
    {"CAP_MAC_OVERRIDE",     32}, {"MAC_OVERRIDE",     32},
    {"CAP_MAC_ADMIN",        33}, {"MAC_ADMIN",        33},
    {"CAP_SYSLOG",           34}, {"SYSLOG",           34},
    {"CAP_WAKE_ALARM",       35}, {"WAKE_ALARM",       35},
    {"CAP_BLOCK_SUSPEND",    36}, {"BLOCK_SUSPEND",    36},
    {"CAP_AUDIT_READ",       37}, {"AUDIT_READ",       37},
};

#define G_CAP_NAMES_LEN ((int)(sizeof(g_cap_names)/sizeof(g_cap_names[0])))

int security_cap_number(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 0; i < G_CAP_NAMES_LEN; i++) {
        if (strcasecmp(name, g_cap_names[i].name) == 0)
            return g_cap_names[i].nr;
    }
    return -1;
}

void security_cap_add(SecurityConfig *cfg, int cap_nr) {
    if (!cfg || cap_nr < 0 || cap_nr > 63) return;
    cfg->cap_keep |= (UINT64_C(1) << cap_nr);
}

void security_cap_drop(SecurityConfig *cfg, int cap_nr) {
    if (!cfg || cap_nr < 0 || cap_nr > 63) return;
    cfg->cap_keep &= ~(UINT64_C(1) << cap_nr);
}

/* ── Capability dropping ─────────────────────────────────────────────── */

int security_apply_caps(const SecurityConfig *cfg) {
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct   data[2];
    int cap, rc;

    if (!cfg || cfg->privileged) return 0;

    /* Step 1: drop from bounding set — prevents any exec from regaining caps. */
    for (cap = 0; cap <= 37; cap++) {
        if (!(cfg->cap_keep & (UINT64_C(1) << cap))) {
            prctl(PR_CAPBSET_DROP, (unsigned long)cap, 0UL, 0UL, 0UL);
        }
    }

    /* Step 2: read current process caps and mask down to keep set. */
    memset(&hdr,  0, sizeof(hdr));
    memset(data, 0, sizeof(data));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;

    rc = (int)syscall(SYS_capget, &hdr, data);
    if (rc != 0) return -1;

    data[0].effective   &= (uint32_t)( cfg->cap_keep        & 0xFFFFFFFFULL);
    data[0].permitted   &= (uint32_t)( cfg->cap_keep        & 0xFFFFFFFFULL);
    data[0].inheritable  = 0;
    data[1].effective   &= (uint32_t)((cfg->cap_keep >> 32) & 0xFFFFFFFFULL);
    data[1].permitted   &= (uint32_t)((cfg->cap_keep >> 32) & 0xFFFFFFFFULL);
    data[1].inheritable  = 0;

    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid     = 0;
    return (int)syscall(SYS_capset, &hdr, data);
}

/* ── Seccomp BPF deny-list ───────────────────────────────────────────── */

/*
 * Each BLK() expands to two BPF instructions:
 *   JEQ <nr>: if match → fall-through to DENY; if no match → skip DENY.
 *   RET ERRNO(EPERM)
 *
 * The syscall number is already in the BPF accumulator (loaded once before
 * the chain). The ALLOW at the end is reached only if no syscall matched.
 */
#define BLK(nr) \
    BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (unsigned int)(nr), 0, 1), \
    BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|(EPERM & SECCOMP_RET_DATA))

int security_apply_seccomp(const SecurityConfig *cfg) {
    struct sock_filter filter[] = {
        /*
         * Architecture guard: kill the process if it's not the expected arch.
         * This prevents a 32-bit process from bypassing 64-bit syscall checks
         * by issuing 32-bit syscall numbers which overlap with different calls.
         */
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, arch)),
#if defined(AUDIT_ARCH_X86_64)
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64,  1, 0),
#elif defined(AUDIT_ARCH_AARCH64)
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#else
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 0, 1, 0), /* unknown arch → allow */
#endif
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* Load syscall number into accumulator (persists across JEQ checks). */
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, nr)),

        /*
         * Blocked syscalls — mirrors Docker's default seccomp profile.
         * Each BLK() is 2 BPF instructions.  The accumulator retains the
         * syscall number across all JEQ checks (loaded once above).
         */
        BLK(__NR_ptrace),            /* process tracing / debugging          */
        BLK(__NR_mount),             /* mount filesystems                    */
        BLK(__NR_umount2),           /* unmount filesystems                  */
        BLK(__NR_kexec_load),        /* load a new kernel for later exec     */
        BLK(__NR_reboot),            /* reboot or signal init                */
        BLK(__NR_swapon),            /* start swapping                       */
        BLK(__NR_swapoff),           /* stop swapping                        */
        BLK(__NR_acct),              /* enable/disable process accounting    */
        BLK(__NR_settimeofday),      /* set system time                      */
        BLK(__NR_adjtimex),          /* tune kernel clock                    */
        BLK(__NR_clock_settime),     /* set a POSIX clock                    */
        BLK(__NR_quotactl),          /* manipulate disk quotas               */
        BLK(__NR_add_key),           /* add key to kernel key ring           */
        BLK(__NR_request_key),       /* request a key from kernel            */
        BLK(__NR_keyctl),            /* manipulate kernel key ring           */
        BLK(__NR_unshare),           /* create new namespaces (escape path)  */
        BLK(__NR_setns),             /* join another namespace               */
        BLK(__NR_perf_event_open),   /* performance monitoring (side-channel)*/
        BLK(__NR_init_module),       /* load kernel module                   */
        BLK(__NR_delete_module),     /* remove kernel module                 */
        BLK(__NR_finit_module),      /* load kernel module from fd           */
        BLK(__NR_open_by_handle_at), /* bypass mount-point restrictions      */
        BLK(__NR_lookup_dcookie),    /* kernel debugging interface           */
        BLK(__NR_pivot_root),        /* change root filesystem               */
#if defined(__NR_iopl)
        BLK(__NR_iopl),              /* change I/O privilege level (x86)     */
#endif
#if defined(__NR_ioperm)
        BLK(__NR_ioperm),            /* set I/O port permissions (x86)       */
#endif
#if defined(__NR_kexec_file_load)
        BLK(__NR_kexec_file_load),   /* load new kernel from fd              */
#endif

        /*
         * Argument filter: block clone(CLONE_NEWUSER, ...).
         * The accumulator still holds nr from the initial LD above.
         * Pattern (5 instructions):
         *   JEQ __NR_clone  → if no match, skip 3 instructions to ALLOW
         *   LD args[0]      → load clone flags (lower 32 bits)
         *   JSET CLONE_NEWUSER → if flag set, fall to DENY; else skip to ALLOW
         *   RET DENY
         *   RET ALLOW
         * This prevents containers from creating user namespaces (a common
         * container-escape prerequisite) while still allowing normal clone().
         */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, __NR_clone, 0, 3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K, CLONE_NEWUSER, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|(EPERM & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog;

    if (!cfg || cfg->privileged || !cfg->seccomp_enabled) return 0;

    prog.len    = (unsigned short)(sizeof(filter) / sizeof(filter[0]));
    prog.filter = filter;

    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
}

#undef BLK

/* ── Read-only rootfs ────────────────────────────────────────────────── */

int security_apply_readonly(void) {
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0) return 0;
    /* Some kernels need MS_BIND alongside MS_RDONLY|MS_REMOUNT. */
    return mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY | MS_BIND, NULL);
}

/* ── Formatting ──────────────────────────────────────────────────────── */

const char *security_profile_label(const SecurityConfig *cfg) {
    if (!cfg)            return "unknown";
    if (cfg->privileged) return "none";
    return "default";
}

void security_format_inspect(const SecurityConfig *cfg, char *buf, size_t size) {
    char dropped[512] = "";
    int  cap, first = 1;

    if (!cfg || !buf || !size) return;

    if (!cfg->privileged) {
        int i;
        for (cap = 0; cap <= 37; cap++) {
            if (cfg->cap_keep & (UINT64_C(1) << cap)) continue;
            /* find canonical "CAP_" name */
            for (i = 0; i < G_CAP_NAMES_LEN; i++) {
                if (g_cap_names[i].nr != cap) continue;
                if (strncmp(g_cap_names[i].name, "CAP_", 4) != 0) continue;
                if (!first)
                    strncat(dropped, " ", sizeof(dropped) - strlen(dropped) - 1);
                strncat(dropped, g_cap_names[i].name,
                        sizeof(dropped) - strlen(dropped) - 1);
                first = 0;
                break;
            }
        }
    }

    snprintf(buf, size,
             "{\n"
             "    \"Mode\"          : \"%s\",\n"
             "    \"Privileged\"    : %s,\n"
             "    \"ReadOnlyRootfs\": %s,\n"
             "    \"Seccomp\"       : \"%s\",\n"
             "    \"CapDrop\"       : \"%s\"\n"
             "  }",
             security_profile_label(cfg),
             cfg->privileged       ? "true"        : "false",
             cfg->readonly_rootfs  ? "true"        : "false",
             cfg->seccomp_enabled  ? "default"     : "unconfined",
             cfg->privileged       ? "(none)"      : dropped);
}

void security_print_detail(const SecurityConfig *cfg) {
    int cap, i, first;

    if (!cfg) return;

    printf("  Mode        : %s\n", security_profile_label(cfg));
    printf("  Privileged  : %s\n", cfg->privileged      ? "yes" : "no");
    printf("  ReadOnlyFS  : %s\n", cfg->readonly_rootfs ? "yes" : "no");
    printf("  Seccomp     : %s\n", cfg->seccomp_enabled
               ? "enabled (25+ syscalls + CLONE_NEWUSER argument filter)" : "disabled");

    if (cfg->privileged) {
        printf("  Capabilities: all (no restrictions)\n");
        return;
    }

    printf("  Capabilities kept   :");
    first = 1;
    for (cap = 0; cap <= 37; cap++) {
        if (!(cfg->cap_keep & (UINT64_C(1) << cap))) continue;
        for (i = 0; i < G_CAP_NAMES_LEN; i++) {
            if (g_cap_names[i].nr != cap) continue;
            if (strncmp(g_cap_names[i].name, "CAP_", 4) != 0) continue;
            printf("%s %s", first ? "" : ",", g_cap_names[i].name + 4);
            first = 0;
            break;
        }
    }
    printf("\n");

    printf("  Capabilities dropped:");
    first = 1;
    for (cap = 0; cap <= 37; cap++) {
        if (cfg->cap_keep & (UINT64_C(1) << cap)) continue;
        for (i = 0; i < G_CAP_NAMES_LEN; i++) {
            if (g_cap_names[i].nr != cap) continue;
            if (strncmp(g_cap_names[i].name, "CAP_", 4) != 0) continue;
            printf("%s %s", first ? "" : ",", g_cap_names[i].name + 4);
            first = 0;
            break;
        }
    }
    printf("\n");
}
