#!/bin/bash
# Memtis environment on the 5.15.19-htmm kernel: one cgroup with a fast-tier
# cap, Memtis enabled on it, and PEBS sampling running.
#
#   sudo ./memtis-env.sh setup              # globals, cgroup, start sampling
#   sudo ./memtis-env.sh run -- <cmd...>    # run <cmd> inside the cgroup
#   sudo ./memtis-env.sh status
#   sudo ./memtis-env.sh teardown           # stop sampling, remove the cgroup
#
# Run ../../setup/env/prepare_environment.sh (or tiering_solutions' onboot.sh,
# which keeps one node-1 CPU online) once per boot first. setup checks that
# node 1 has at most one online CPU.
#
# Environment: MEMTIS_CG (cgroup name, default memtis), MEMTIS_FAST (fast-tier
# cap, memory.max_at_node0, default 8G), MEMTIS_MODE (htmm_mode, default 2 =
# HUGEPAGE_OPT, the only mode the patch marks as used), MEMTIS_CPUS (CPUs for
# ksamplingd/kmigraterd*, default 2,4; empty = unpinned).
set -euo pipefail

CG=/sys/fs/cgroup/${MEMTIS_CG:-memtis}
FAST=${MEMTIS_FAST:-8G}
MODE=${MEMTIS_MODE:-2}
# CPUs for Memtis's kernel threads, kept off the workload's CPUs (as ARMS's
# threads are on 2 and 4); empty leaves them on any node-0 CPU.
KCPUS=${MEMTIS_CPUS-2,4}
KTHREADS="ksamplingd kmigraterd0 kmigraterd1"
HTMM=/sys/kernel/mm/htmm
SYS_HTMM_START=449 # arch/x86/entry/syscalls/syscall_64.tbl of memtis.patch
SYS_HTMM_END=450

die() { echo "memtis-env: $*" >&2; exit 1; }
put() { printf '%s\n' "$1" > "$2" || die "cannot write '$1' to $2"; }
sampling() { pgrep -x ksamplingd > /dev/null; }
# Perl's syscall needs numeric args (strings go in as pointers) and takes the
# number as a scalar, so unpack it rather than passing an array.
syscall() { perl -e 'my ($n, @a) = map { $_ + 0 } @ARGV; exit(syscall($n, @a) == -1 ? 1 : 0)' "$@"; }

check_host() {
    [[ $EUID -eq 0 ]] || die "run as root"
    [[ -d $HTMM ]] || die "no $HTMM: not the Memtis kernel ($(uname -r))"
    # At most one node-1 CPU may stay online: tiering_solutions' onboot.sh keeps
    # one so the socket-1 uncore IMC (bandwidth) counters stay readable.
    local n1
    n1=$(numactl --hardware | awk '/^node 1 cpus:/ {print NF - 3}')
    [[ ${n1:-0} -le 1 ]] ||
        die "node 1 has $n1 online CPUs (max 1): run setup/env/prepare_environment.sh or tiering_solutions/scripts/onboot.sh first"
}

setup() {
    check_host
    # cxl mode hard-wires node 0 fast / node 1 slow. Without it, Memtis
    # expects NVM nodes 2 and 3 and finds no demotion target on this host.
    put enabled $HTMM/htmm_cxl_mode
    put "$MODE" $HTMM/htmm_mode

    [[ -d $CG ]] || mkdir "$CG"
    put "$FAST" "$CG/memory.max_at_node0"
    # Only processes that exec inside the cgroup after this are managed.
    put enabled "$CG/memory.htmm_enabled"

    if ! sampling; then
        syscall $SYS_HTMM_START 0 0 || die "htmm_start failed"
        # htmm_start returns 0 even when PEBS setup fails, so check the thread.
        sleep 1
        sampling || die "ksamplingd did not start; see dmesg | grep -i htmm"
    fi
    pin_threads
    status
}

# The threads set their CPUs once, at start (set_cpus_allowed_ptr, not
# kthread_bind), so this sticks until they are restarted by the next setup.
pin_threads() {
    [[ -n $KCPUS ]] || return 0
    local t pid
    for t in $KTHREADS; do
        pid=$(pgrep -x "$t") || die "$t is not running"
        taskset -pc "$KCPUS" "$pid" > /dev/null || die "cannot pin $t to CPUs $KCPUS"
    done
}

run() {
    [[ ${1-} == -- ]] && shift
    [[ $# -gt 0 ]] || die "usage: run -- <cmd...>"
    [[ -f $CG/memory.htmm_enabled && $(<"$CG/memory.htmm_enabled") == *enabled* ]] ||
        die "$CG is not set up: run setup first"
    sampling || echo "memtis-env: warning: ksamplingd is not running" >&2
    # Join before exec: Memtis tags the mm at exec, the cap counts from then.
    echo $$ > "$CG/cgroup.procs"
    exec "$@"
}

status() {
    echo "kernel:        $(uname -r)"
    echo "cxl_mode:      $(<$HTMM/htmm_cxl_mode)"
    echo "htmm_mode:     $(<$HTMM/htmm_mode)"
    echo "THP:           $(</sys/kernel/mm/transparent_hugepage/enabled)"
    echo "ksamplingd:    $(sampling && echo running || echo stopped)"
    echo "kmigraterd:    $(pgrep -l kmigraterd | awk '{print $2}' | xargs)"
    local t pid
    for t in $KTHREADS; do
        pid=$(pgrep -x "$t") && echo "  $t cpus: $(taskset -pc "$pid" | awk -F': ' '{print $2}')"
    done
    if [[ -d $CG ]]; then
        echo "cgroup:        $CG"
        echo "  htmm:        $(<"$CG/memory.htmm_enabled")"
        echo "  max_at_node0: $(<"$CG/memory.max_at_node0")"
        echo "  procs:       $(xargs < "$CG/cgroup.procs")"
        awk '/^anon |^file /' "$CG/memory.numa_stat"
    else
        echo "cgroup:        $CG (absent)"
    fi
}

teardown() {
    [[ $EUID -eq 0 ]] || die "run as root"
    if [[ -d $CG ]]; then
        [[ -z $(<"$CG/cgroup.procs") ]] || die "$CG still has processes: $(xargs < "$CG/cgroup.procs")"
    fi
    # htmm_end with no sampling running touches unset PEBS state: guard it.
    if sampling; then syscall $SYS_HTMM_END 0 || die "htmm_end failed"; fi
    if [[ -d $CG ]]; then
        put disabled "$CG/memory.htmm_enabled"
        rmdir "$CG"
    fi
    echo "memtis-env: torn down (sample stats: dmesg | grep nr_sampled)"
}

case ${1-} in
setup | status | teardown) "$1" ;;
run) shift; run "$@" ;;
*) sed -n '2,17p' "$0"; exit 1 ;;
esac
