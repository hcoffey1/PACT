/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#include <unistd.h>
#include <stdio.h>
#include <cpuid.h>

#include "pmu-platform.h"
#include "pmu.h"
#include "error.h"

pmu_platform_t g_pmu_platform;

/* Skylake Xeon CHA-to-core mapping (CloudLab c220g5) */
static const int skx_cha_to_core_map[10] = {2, 3, 4, 5, 6, 7, 8, 9, 0, 1};
static const int skx_core_to_tid[10] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x08, 0x18, 0x28, 0x38, 0x48};

/*
 * Skylake Xeon Gold 6142 (CloudLab c6420): 16 CHAs per socket, SMT off, and
 * node-0 CPUs are the even Linux CPUs 0-30, so both tables are sized by Linux
 * CPU (31). TIDs measured 2026-11-19 (TOR_OCCUPANCY.IA_MISS DRD, one pinned
 * miss stream per CPU): APIC 0-14 -> APIC*8, APIC 16-30 -> (APIC-16)*8 + 8.
 * CHAs are picked by address hash, so each CHA sees ~1/16 of every core's
 * traffic and any one-to-one CHA->CPU pairing is valid: CHA i -> CPU 2i.
 * -1 = no such CHA / CPU not on node 0.
 */
static const int c6420_cha_to_core_map[31] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 18, 20,
                                              22, 24, 26, 28, 30, -1, -1, -1, -1, -1, -1,
                                              -1, -1, -1, -1, -1, -1, -1, -1, -1};
static const int c6420_core_to_tid[31] = {
    /* cpu0 */ 0x00, -1, /* cpu2 */ 0x70, -1, /* cpu4 */ 0x10, -1, /* cpu6 */ 0x60, -1,
    /* cpu8 */ 0x20, -1, /* cpu10 */ 0x50, -1, /* cpu12 */ 0x30, -1, /* cpu14 */ 0x40, -1,
    /* cpu16 */ 0x08, -1, /* cpu18 */ 0x78, -1, /* cpu20 */ 0x18, -1, /* cpu22 */ 0x68, -1,
    /* cpu24 */ 0x28, -1, /* cpu26 */ 0x58, -1, /* cpu28 */ 0x38, -1, /* cpu30 */ 0x48};

/*
 * Sapphire/Emerald Rapids CHA-to-core mapping. Both uarchs share the same
 * server CHA design and TOR event encodings, so one descriptor covers both.
 * The descriptor maps CHA offsets 0-31. Discovery skips the CHAs at offset
 * 32 and above (cha_offset >= nr_cha_mapping in filter_active_chas).
 */
static const int emr_cha_to_core_map[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                            11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                            22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
static const int emr_core_to_tid[32] = {
    0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78,
    0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8};

/* TID goes into config1 (ORed with config1_base). */
static void generic_fill_tor_config(const pmu_platform_t *plat, tor_pe_config_t *out, int event_idx,
                                    int tier, int core_id)
{
    out->config = plat->tor_base[event_idx][tier];
    out->config1 = plat->config1_base[tier];
    out->config2 = 0;

    if (core_id >= 0 && core_id < plat->nr_cha_mapping) {
        out->config |= plat->tid_en_bit;
        out->config1 |= plat->core_to_tid[core_id];
    }
}

static pmu_platform_id_t detect_platform(void)
{
    unsigned int eax, ebx, ecx, edx;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return PMU_PLATFORM_UNKNOWN;
    }

    unsigned int family = (eax >> 8) & 0xF;
    unsigned int model = (eax >> 4) & 0xF;

    if (family == 0xF) {
        family += (eax >> 20) & 0xFF;
    }
    if (family == 6 || family == 0xF) {
        model += ((eax >> 16) & 0xF) << 4;
    }

    /* Family 6: Intel Core microarchitectures */
    if (family == 6) {
        switch (model) {
        case 0x55: /* Skylake-X / Cascade Lake */
            return PMU_PLATFORM_SKX;
        case 0x8F: /* Sapphire Rapids */
        case 0xCF: /* Emerald Rapids */
            return PMU_PLATFORM_EMR;
        }
    }

    return PMU_PLATFORM_UNKNOWN;
}

/* Per-platform descriptors. Built once at file scope; pmu_platform_init copies
 * the matching descriptor into the global g_pmu_platform. */
static const pmu_platform_t platform_skx = {
    .id = PMU_PLATFORM_SKX,
    .name = "Skylake-X",
    .event_llc_miss_local = 0x01d3,
    .event_llc_miss_remote = 0x02d3,
    .event_llc_miss_any = 0x03d3,
    .event_stalls_l3_miss = 0x60006a3,
    .event_instructions = 0x00c0,
    .event_cycles = 0x003c,
    .event_llc_miss_all = 0x412e,
    .event_offcore_dd_out = 0x0160,
    .event_offcore_dd = 0x01b0,
    .nr_cha_mapping = 10,
    .cha_to_core_map = skx_cha_to_core_map,
    .core_to_tid = skx_core_to_tid,
    .tor_base =
        {
            {0x2136, 0x2136},               /* [TOR_OCC][local], [TOR_OCC][remote] */
            {0x0100211fULL, 0x0100211fULL}, /* [TOR_CYC][local], [TOR_CYC][remote] */
        },
    .config1_base =
        {
            0x4043200000000ULL, /* tier 0 (local): DRD_LOC opcode << 32. */
            0x4043100000000ULL, /* tier 1 (remote): DRD_REM opcode << 32. */
        },
    .tid_en_bit = (1ULL << 19),
    .fill_tor_config = generic_fill_tor_config,
    .mlp_min = 1.0,
    .mlp_max = 16.0,
    .k_constant_dram = 238,
    .k_constant_cxl = 771,
};

/*
 * Sapphire/Emerald Rapids. Core DRD events are unchanged from SKX
 * (MEM_LOAD_L3_MISS_RETIRED.LOCAL/REMOTE_DRAM). The CHA TOR events move to
 * the SPR/EMR encoding: the opcode match (DRD, local vs remote) lives in the
 * extended umask (config bits 32-55) inside tor_base, so config1_base is 0,
 * and the TID-enable bit is config:16. tor_base and the DRD opcode-match ext
 * bits (0xc816fe local, 0xc8177e remote) were validated on real SPR
 * and EMR silicon against Intel perfmon.
 *
 * NOTE: k_constant_dram / k_constant_cxl are still the SKX values pending
 * per-uarch latency calibration on SPR/EMR.
 */
static const pmu_platform_t platform_emr = {
    .id = PMU_PLATFORM_EMR,
    .name = "Sapphire/Emerald Rapids",
    .event_llc_miss_local = 0x01d3,
    .event_llc_miss_remote = 0x02d3,
    .event_llc_miss_any = 0x03d3,
    .event_stalls_l3_miss = 0x60006a3,
    .event_instructions = 0x00c0,
    .event_cycles = 0x003c,
    .event_llc_miss_all = 0x412e,
    .event_offcore_dd_out = 0x0120,
    .event_offcore_dd = 0x0121,
    .nr_cha_mapping = 32,
    .cha_to_core_map = emr_cha_to_core_map,
    .core_to_tid = emr_core_to_tid,
    .tor_base =
        {
            /* [TOR_OCC][local], [TOR_OCC][remote]: DRD opcode in ext umask. */
            {0x00c816fe00000136ULL, 0x00c8177e00000136ULL},
            /* [TOR_CYC][local], [TOR_CYC][remote]: cycles with >=1 outstanding. */
            {0x00c816fe0100011fULL, 0x00c8177e0100011fULL},
        },
    .config1_base = {0, 0}, /* opcode is in config ext umask, not config1 */
    .tid_en_bit = (1ULL << 16),
    .fill_tor_config = generic_fill_tor_config,
    .mlp_min = 1.0,
    .mlp_max = 20.0,
    .k_constant_dram = 238,
    .k_constant_cxl = 771,
};

int pmu_platform_init(void)
{
    pmu_platform_id_t id = detect_platform();
    const pmu_platform_t *src;
    switch (id) {
    case PMU_PLATFORM_EMR:
        src = &platform_emr;
        break;
    case PMU_PLATFORM_SKX:
        src = &platform_skx;
        break;
    case PMU_PLATFORM_UNKNOWN:
    default:
        log_warning("pmu_platform_init", "Unknown CPU (falling back to SKX defaults)");
        src = &platform_skx;
        break;
    }
    g_pmu_platform = *src;
    /* shortcut: any 16-CHA Skylake-SP gets the c6420 tables; another 16-core SKX host needs its own measured table. */
    if (id == PMU_PLATFORM_SKX && access("/sys/devices/uncore_cha_15", F_OK) == 0) {
        g_pmu_platform.name = "Skylake-X (Gold 6142, c6420)";
        g_pmu_platform.nr_cha_mapping = 31;
        g_pmu_platform.cha_to_core_map = c6420_cha_to_core_map;
        g_pmu_platform.core_to_tid = c6420_core_to_tid;
    }
    if (id == PMU_PLATFORM_UNKNOWN) {
        g_pmu_platform.id = PMU_PLATFORM_UNKNOWN;
        g_pmu_platform.name = "Unknown (SKX fallback)";
    }

    printf("PMU platform detected: %s\n", g_pmu_platform.name);

    extern event_config_t core_event_configs[CORE_EVENT_COUNT];
    core_event_configs[CORE_EVENT_LLC_MISS_SLOW] =
        (event_config_t){g_pmu_platform.event_llc_miss_remote, "LLC_MISS_SLOW"};

    cpu_event_configs[CPU_EVENT_LLC_MISS_SLOW] = core_event_configs[CORE_EVENT_LLC_MISS_SLOW];
    cpu_event_configs[CPU_EVENT_INSTRUCTIONS] =
        (event_config_t){g_pmu_platform.event_instructions, "INSTRUCTIONS"};
    cpu_event_configs[CPU_EVENT_CYCLES] = (event_config_t){g_pmu_platform.event_cycles, "CYCLES"};
    cpu_event_configs[CPU_EVENT_LLC_MISS_ALL] =
        (event_config_t){g_pmu_platform.event_llc_miss_all, "LLC_MISS_ALL"};
    cpu_event_configs[CPU_EVENT_OFFCORE_DD_OUT] =
        (event_config_t){g_pmu_platform.event_offcore_dd_out, "OFFCORE_DD_OUTSTANDING"};
    cpu_event_configs[CPU_EVENT_OFFCORE_DD] =
        (event_config_t){g_pmu_platform.event_offcore_dd, "OFFCORE_DD"};

    return (id == PMU_PLATFORM_UNKNOWN) ? -1 : 0;
}
