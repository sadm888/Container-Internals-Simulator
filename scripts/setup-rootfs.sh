#!/usr/bin/env bash
# setup-rootfs.sh — bootstrap minimal rootfs directories for testing
#
# Creates: rootfs/test-basic  test-isolation  test-mon
#          test-resources  test-sched  test-security
#
# Must be run on Linux (or WSL2 Ubuntu). Copies system binaries and their
# shared libraries using ldd, then places the compiled workloads.
# Does NOT require root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

ROOTFS_NAMES=(
    test-basic
    test-isolation
    test-mon
    test-resources
    test-sched
    test-security
)

# Binaries to copy into every rootfs
SYSTEM_BINS=(
    /usr/bin/dash
    /usr/bin/sh
    /usr/bin/hostname
    /usr/bin/ps
    /usr/bin/sleep
    /usr/bin/echo
    /usr/bin/true
    /usr/bin/false
    /usr/bin/cat
    /usr/bin/find
    /usr/bin/grep
)

# ── helpers ──────────────────────────────────────────────────────────────────

log()  { echo "  [setup] $*"; }
warn() { echo "  [warn]  $*"; }

copy_with_libs() {
    local src="$1"
    local destdir="$2"

    local bname
    bname="$(basename "$src")"

    # copy the binary itself
    cp -p "$src" "$destdir/bin/$bname"

    # copy every shared library it needs
    ldd "$src" 2>/dev/null | awk '{ print $3 }' | grep -E '^/' | while read -r lib; do
        local libname
        libname="$(basename "$lib")"
        local libdir
        libdir="$(dirname "$lib")"

        # mirror the host library directory structure inside the rootfs
        # e.g. /lib/x86_64-linux-gnu/libc.so.6 → rootfs/.../lib/x86_64-linux-gnu/libc.so.6
        local rel="${libdir#/}"   # strip leading /
        local dest_libdir="$destdir/$rel"
        mkdir -p "$dest_libdir"
        cp -p "$lib" "$dest_libdir/$libname" 2>/dev/null || true

        # also handle symlinks (e.g. libc.so.6 → libc-2.35.so)
        if [ -L "$lib" ]; then
            local target
            target="$(readlink -f "$lib")"
            cp -p "$target" "$dest_libdir/$(basename "$target")" 2>/dev/null || true
        fi
    done

    # copy the dynamic linker (ld-linux)
    local ld
    ld="$(ldd "$src" 2>/dev/null | grep 'ld-linux\|ld\.so' | awk '{ print $1 }' | head -1)"
    if [ -n "$ld" ] && [ -f "$ld" ]; then
        mkdir -p "$destdir/lib64"
        cp -p "$ld" "$destdir/lib64/$(basename "$ld")" 2>/dev/null || true
        # also symlink-style copy to /lib/ if needed
        local ld_libdir
        ld_libdir="$(dirname "$ld")"
        local rel_ld="${ld_libdir#/}"
        if [ -n "$rel_ld" ]; then
            mkdir -p "$destdir/$rel_ld"
            cp -p "$ld" "$destdir/$rel_ld/$(basename "$ld")" 2>/dev/null || true
        fi
    fi
}

# ── build workloads if needed ─────────────────────────────────────────────────

if [ ! -f bin/workload-cpu ]; then
    log "building workloads..."
    make workloads -s
fi

# ── bootstrap each rootfs ─────────────────────────────────────────────────────

for name in "${ROOTFS_NAMES[@]}"; do
    dir="rootfs/$name"

    if [ -f "$dir/.bootstrapped" ]; then
        log "$name: already bootstrapped, skipping"
        continue
    fi

    log "bootstrapping $name..."

    # create directory skeleton
    mkdir -p \
        "$dir/bin" \
        "$dir/dev" \
        "$dir/proc" \
        "$dir/sys" \
        "$dir/tmp" \
        "$dir/usr/bin" \
        "$dir/usr/lib" \
        "$dir/lib" \
        "$dir/lib64"

    # copy system binaries + their libs
    for bin in "${SYSTEM_BINS[@]}"; do
        if [ -f "$bin" ]; then
            copy_with_libs "$bin" "$dir"
        else
            warn "$bin not found on host, skipping"
        fi
    done

    # symlink sh → dash if sh is missing
    if [ ! -f "$dir/bin/sh" ] && [ -f "$dir/bin/dash" ]; then
        ln -sf dash "$dir/bin/sh"
    fi

    # place compiled workloads
    for wb in bin/workload-*; do
        [ -f "$wb" ] && cp -p "$wb" "$dir/bin/" && log "  copied $(basename $wb)"
    done

    touch "$dir/.bootstrapped"
    log "$name: done"
done

echo ""
echo "Rootfs setup complete. Run 'sudo ./container-sim' to start."
