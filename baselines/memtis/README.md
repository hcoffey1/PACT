# Memtis - Linux v5.15.19 kernel

This folder builds the Memtis kernel used as a baseline in PACT evaluation. Memtis is a hardware-performance-counter-driven tiered memory management system that profiles per-page access frequency using PEBS and migrates pages between tiers accordingly.

---

## Folder contents

| File | Purpose |
|------|---------|
| `setup_memtis.sh` | One-shot build script - clone, patch, configure, compile |
| `memtis.patch` | Memtis patch applied to clean Linux v5.15.19 |
| `memtis-vmstat.patch`, `memtis-pte-allocation.patch`, `memtis-c6420.patch`, `memtis-watermark-underflow.patch` | Build patches applied after `memtis.patch` (see [Build patches](#build-patches)) |
| `config` | Base `.config` used as the starting configuration |

---

## Requirements

- `git`, `make`, `gcc` (gcc 9+ recommended)
- Kernel build dependencies:

```bash
# Ubuntu / Debian
sudo apt install git build-essential bc bison flex libelf-dev libssl-dev \
     libncurses-dev dwarves pahole

# RHEL / CentOS / Fedora
sudo dnf install git gcc make bc bison flex elfutils-libelf-devel openssl-devel \
     ncurses-devel dwarves
```

- At least **20 GB** free disk space
- At least **8 GB RAM** recommended for parallel build

---

## Usage

Memtis builds via `setup_memtis.sh` (not `compile.sh`). The script clones the
kernel tree into `./linux` itself; `v5.15.19` is a **stable** point release, so
it comes from `linux-stable` (not `torvalds/linux`). The build does not need
root; only the install below does:

```bash
cd memtis
./setup_memtis.sh
```

The script will:
1. Clone `linux-stable` at tag `v5.15.19` into `linux/`
2. Apply `memtis.patch` (sets `EXTRAVERSION = -htmm`)
3. Apply `memtis-vmstat.patch` and `memtis-pte-allocation.patch`
4. Apply the shared host-tool fix with `../common/apply-build-fixes.sh`
   (see [`../README.md`](../README.md))
5. Copy `config` as the base `.config`
6. Run `make olddefconfig` to resolve any new symbols
7. Force `CONFIG_INTEL_UNCORE_FREQ_CONTROL=y` (built-in `*`)
8. Build with `make -j$(nproc)`

If the repo is already present the clone step is skipped automatically. The
patch steps fail on a tree that is already patched, so before you re-run the
script, reset the tree:

```bash
git -C linux checkout -f v5.15.19 && git -C linux clean -fdx
```

---

## Installation (optional)

```bash
cd linux
sudo make modules_install
sudo make install
sudo update-grub   # make sure GRUB boots 5.15.19-htmm
sudo reboot
```

Verify after reboot:
```bash
uname -r
# expected: 5.15.19-htmm
```

## Build patches

`memtis-sampler-trace.patch` is an optional diagnostic patch, not applied by
`setup_memtis.sh`: apply it after the others to log each CPU's PEBS event state
(`htmm_dbg` lines in `/sys/kernel/debug/tracing/trace`) every 2 s.

`memtis-vmstat.patch` makes the build work without memory-balloon support.
`memtis-pte-allocation.patch` checks the result of a page-table allocation
before Memtis uses it. `memtis-c6420.patch` sizes the sampler's per-CPU
event table by `nr_cpu_ids` instead of the online-CPU count (c6420's CPU ids
are not contiguous, so the old size overflowed) and sets the split model's
DRAM/CXL latencies to the 100/258 ns measured on c6420. It also fixes two
LRU-accounting bugs: the PMD split no longer flips a still-compound THP's
PG_active per subpage (that left the THP on the wrong LRU list and underflowed
`lru_size`), and the deferred-split scan uses each page's own lruvec. Finally, `ksamplingd` re-arms every PEBS event once a second
(`pebs_rearm()`, via `perf_event_period()` with the unchanged period). This is
a departure from stock Memtis: on c6420, a read event's auto-reload counter
could overflow without writing a PEBS record, after which it kept counting
from 0 but never sampled again, stalling recovery after a hot-set change. `memtis-watermark-underflow.patch` stops the demotion size
from underflowing when the fast tier is ~2.44-3.26 GiB (e.g. 16 G at 20% fast),
which soft-locked kmigraterd0. To test the second patch, run from the repository
root after applying the patches:

```sh
python3 tests/test-memtis-pte-allocation.py baselines/memtis/linux/arch/x86/mm/pgtable.c
```

## Running workloads

[`memtis-env.sh`](memtis-env.sh) sets up Memtis on the booted `5.15.19-htmm`
kernel. Run [`../../setup/env/prepare_environment.sh`](../../setup/env/) once
per boot first.

```bash
sudo ./memtis-env.sh setup                 # cxl mode, htmm_mode 2, cgroup "memtis" capped at 8G on node 0, PEBS sampling on
sudo ./memtis-env.sh run -- numactl -C 2-9 -- <program> <args>
sudo ./memtis-env.sh status
sudo ./memtis-env.sh teardown              # cgroup must be empty
```

`MEMTIS_FAST` sets the fast-tier cap (`memory.max_at_node0`, default `8G`),
`MEMTIS_CG` the cgroup name, `MEMTIS_MODE` the `htmm_mode`. `htmm_cxl_mode`
must be `enabled` on a two-node host: without it, Memtis expects NVM nodes 2
and 3. Sampling (`ksamplingd`) is system-wide; its statistics go to `dmesg`
at teardown.
