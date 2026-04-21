#ifndef SECURITY_H
#define SECURITY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Bitmask of Linux capability numbers (bit N = CAP_N).
 *
 * Docker's default keep set (14 capabilities):
 *   CAP_CHOWN(0)  CAP_DAC_OVERRIDE(1)  CAP_FOWNER(3)   CAP_FSETID(4)
 *   CAP_KILL(5)   CAP_SETGID(6)        CAP_SETUID(7)   CAP_SETPCAP(8)
 *   CAP_NET_BIND_SERVICE(10)  CAP_NET_RAW(13)  CAP_SYS_CHROOT(18)
 *   CAP_MKNOD(27) CAP_AUDIT_WRITE(29)  CAP_SETFCAP(31)
 *
 * Every capability NOT in this set is dropped.
 */
#define SECURITY_CAP_DEFAULT_KEEP \
    (  (UINT64_C(1) <<  0) | (UINT64_C(1) <<  1) | (UINT64_C(1) <<  3) \
     | (UINT64_C(1) <<  4) | (UINT64_C(1) <<  5) | (UINT64_C(1) <<  6) \
     | (UINT64_C(1) <<  7) | (UINT64_C(1) <<  8) | (UINT64_C(1) << 10) \
     | (UINT64_C(1) << 13) | (UINT64_C(1) << 18) | (UINT64_C(1) << 27) \
     | (UINT64_C(1) << 29) | (UINT64_C(1) << 31) )

typedef struct {
    int      privileged;       /* 1 = bypass all restrictions (--privileged) */
    int      readonly_rootfs;  /* 1 = remount rootfs read-only (--read-only) */
    uint64_t cap_keep;         /* bitmask of capabilities to retain          */
    int      seccomp_enabled;  /* 1 = apply BPF deny-list filter             */
} SecurityConfig;

/* Constructors. */
SecurityConfig security_config_default(void);   /* Docker-like hardened profile */
SecurityConfig security_config_none(void);      /* --privileged: no restrictions */

/* Cap name → number; accepts "CAP_NET_ADMIN" or "NET_ADMIN" (case-insensitive). */
int  security_cap_number(const char *name);
void security_cap_add(SecurityConfig *cfg, int cap_nr);
void security_cap_drop(SecurityConfig *cfg, int cap_nr);

/*
 * Apply inside the container child process — call after setuid(0) and
 * prctl(PR_SET_NO_NEW_PRIVS) but before execvp.  Both are best-effort:
 * they return 0 on success, -1 if the kernel rejects the operation.
 */
int security_apply_caps(const SecurityConfig *cfg);
int security_apply_seccomp(const SecurityConfig *cfg);

/* Remount "/" read-only — call after filesystem_isolate_rootfs(). */
int security_apply_readonly(void);

/* Formatting helpers. */
const char *security_profile_label(const SecurityConfig *cfg);
void        security_format_inspect(const SecurityConfig *cfg, char *buf, size_t size);
void        security_print_detail(const SecurityConfig *cfg);

#endif
