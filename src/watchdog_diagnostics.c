/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/crc.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include "watchdog_build_id.h"

/* Format 1: header 16, CFSR/HFSR 8, seven chronological 8-byte events.
 * Single-core only. Publish a complete inactive buffer; an interrupt never
 * waits for the writer it interrupted. Thread snapshots use bounded retries. */
struct retained_snapshot {
    uint8_t bytes[ZMK_WATCHDOG_DIAGNOSTIC_SIZE];
    uint32_t sequence;
    uint32_t crc;
    atomic_t magic;
};
#define CHECKPOINT_MAGIC 0x57444742U /* WDGB; deliberately distinct from pending */
BUILD_ASSERT(offsetof(struct retained_snapshot, crc) == 84, "checkpoint CRC input layout changed");
#if defined(CONFIG_CPU_CORTEX_M)
BUILD_ASSERT(sizeof(struct retained_snapshot) == 92, "Cortex-M checkpoint layout changed");
#endif
static struct retained_snapshot snapshots[2] __noinit;
static uint8_t previous[ZMK_WATCHDOG_DIAGNOSTIC_SIZE];
static atomic_t writer, generation, dropped;
static atomic_t runtime_flags;
static uint32_t boot_reset_cause;

static uint32_t checkpoint_crc(const struct retained_snapshot *slot) {
    return crc32_ieee((const uint8_t *)slot, offsetof(struct retained_snapshot, crc));
}

static bool checkpoint_valid(const struct retained_snapshot *slot) {
    return (uint32_t)atomic_get(&slot->magic) == CHECKPOINT_MAGIC && slot->bytes[0] == 1 &&
           slot->bytes[1] <= 7 && checkpoint_crc(slot) == slot->crc;
}

static int watchdog_diagnostics_boot(void) {
    bool valid[2] = {checkpoint_valid(&snapshots[0]), checkpoint_valid(&snapshots[1])};
    uint32_t flags = ZMK_WDG_RUNTIME_SUPPORTED;
    if (IS_ENABLED(CONFIG_ZMK_WATCHDOG_CAPTURE_CONTEXT)) {
        flags |= ZMK_WDG_RUNTIME_CAPTURE_CONTEXT;
    }
    if (IS_ENABLED(CONFIG_ZMK_WATCHDOG_ASSERT_CONTEXT)) {
        flags |= ZMK_WDG_RUNTIME_ASSERT_CONTEXT;
    }
    if (IS_ENABLED(CONFIG_ZMK_WATCHDOG_FATAL_DETECT)) {
        flags |= ZMK_WDG_RUNTIME_FATAL;
    }
    if (IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT)) {
        flags |= ZMK_WDG_RUNTIME_FREEZE | ZMK_WDG_RUNTIME_SOFTWARE_ONLY;
    }
    memset(previous, 0, sizeof(previous));
    boot_reset_cause = 0;
#if defined(CONFIG_HWINFO)
    if (hwinfo_get_reset_cause(&boot_reset_cause) == 0) {
        flags |= ZMK_WDG_RUNTIME_RESET_CAUSE;
    }
#endif
    if (valid[0] || valid[1]) {
        unsigned last = valid[1] ? 1U : 0U;
        if (valid[0] && valid[1]) {
            uint32_t distance = snapshots[0].sequence - snapshots[1].sequence;
            last = distance != 0 && distance < 0x80000000U ? 0U : 1U;
        }
        memcpy(previous, snapshots[last].bytes, sizeof(previous));
        flags |= ZMK_WDG_RUNTIME_PREVIOUS;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if ((uint32_t)atomic_get(&snapshots[i].magic) == CHECKPOINT_MAGIC && !valid[i]) {
            flags |= ZMK_WDG_RUNTIME_CORRUPT;
        }
    }
    if (flags & ZMK_WDG_RUNTIME_CORRUPT) {
        previous[2] |= 2U;
    }
    if ((valid[0] && !valid[1] && snapshots[1].bytes[0]) ||
        (valid[1] && !valid[0] && snapshots[0].bytes[0])) {
        previous[2] |= 2U; /* interrupted/corrupt inactive update */
    }
    /* Before any APPLICATION-level BLE callbacks; no flash, live getters,
     * clocks or lock acquisition. A new boot must not append to old history. */
    memset(snapshots, 0, sizeof(snapshots));
    atomic_clear(&writer);
    atomic_clear(&generation);
    atomic_clear(&dropped);
    atomic_set(&runtime_flags, (atomic_val_t)flags);
    return 0;
}
SYS_INIT(watchdog_diagnostics_boot, PRE_KERNEL_2, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

uint32_t zmk_watchdog_diagnostics_build_tag(void) { return WATCHDOG_BUILD_TAG; }
uint32_t zmk_watchdog_diagnostics_runtime_flags(void) {
    return (uint32_t)atomic_get(&runtime_flags);
}
uint32_t zmk_watchdog_diagnostics_reset_cause(void) { return boot_reset_cause; }
bool zmk_watchdog_diagnostics_previous(uint8_t out[ZMK_WATCHDOG_DIAGNOSTIC_SIZE]) {
    if (!(atomic_get(&runtime_flags) & ZMK_WDG_RUNTIME_PREVIOUS)) {
        return false;
    }
    memcpy(out, previous, sizeof(previous));
    return true;
}
void zmk_watchdog_diagnostics_monitor_armed(bool lowprio, int result) {
    uint32_t armed = lowprio ? ZMK_WDG_RUNTIME_LOW_ARMED : ZMK_WDG_RUNTIME_SYS_ARMED;
    zmk_watchdog_diagnostics_flags(result < 0 ? ZMK_WDG_RUNTIME_ARM_FAILED : armed,
                                   result < 0 ? armed : 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_MONITOR_ARM, lowprio ? 1 : 0, (uint16_t)result);
}

void zmk_watchdog_diagnostics_flags(uint32_t set, uint32_t clear) {
    atomic_val_t old, next;
    do {
        old = atomic_get(&runtime_flags);
        next = (old & ~(atomic_val_t)clear) | (atomic_val_t)set;
    } while (!atomic_cas(&runtime_flags, old, next));
}

#if defined(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS_TEST)
void watchdog_diagnostics_test_before_publish(void);
void watchdog_diagnostics_test_boot(void) { (void)watchdog_diagnostics_boot(); }
void watchdog_diagnostics_test_corrupt_slot(unsigned slot) {
    if (slot < 2) {
        snapshots[slot].crc ^= 1U;
    }
}
void watchdog_diagnostics_test_invalidate_slot(unsigned slot) {
    if (slot < 2) {
        atomic_clear(&snapshots[slot].magic);
    }
}
void watchdog_diagnostics_test_sequence(uint32_t next_generation) {
    atomic_set(&generation, (atomic_val_t)next_generation);
}
#endif

void zmk_watchdog_breadcrumb(uint8_t event, uint8_t profile, uint16_t value) {
    if (!atomic_cas(&writer, 0, 1)) {
        atomic_inc(&dropped);
        return;
    }
    unsigned gen = (unsigned)atomic_get(&generation);
    struct retained_snapshot *slot = &snapshots[(gen + 1U) & 1U];
    atomic_clear(&slot->magic);
    uint8_t *next = slot->bytes;
    memcpy(next, snapshots[gen & 1U].bytes, ZMK_WATCHDOG_DIAGNOSTIC_SIZE);
    if (!next[0]) {
        next[0] = 1;
        next[8] = next[9] = next[10] = 255;
        sys_put_le32(WATCHDOG_BUILD_TAG, next + 4);
    }
    if (event == ZMK_WDG_PROFILE) {
        next[8] = profile;
    }
    if (event == ZMK_WDG_ENDPOINT) {
        next[9] = (uint8_t)value;
    }
    if (event == ZMK_WDG_USB) {
        next[10] = (uint8_t)value;
    }
    if (event == ZMK_WDG_BOOT) {
        next[3] |= 1U;
    }
    if (event == ZMK_WDG_CONNECTED && !(value & 0x8000U) && next[11] < 255) {
        next[11]++;
    }
    if (event == ZMK_WDG_DISCONNECTED && next[11]) {
        next[11]--;
    }
    if (next[1] == 7) {
        memmove(next + 24, next + 32, 6U * 8U);
        next[2] |= 1U; /* earlier entries evicted, not a complete boot log */
    } else {
        next[1]++;
    }
    uint8_t *entry = next + 24 + (next[1] - 1U) * 8U;
    sys_put_le32(k_uptime_get_32(), entry);
    entry[4] = event;
    entry[5] = profile;
    sys_put_le16(value, entry + 6);
    sys_put_le32((uint32_t)atomic_get(&dropped), next + 12);
    slot->sequence = gen + 1U;
    slot->crc = checkpoint_crc(slot);
#if defined(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS_TEST)
    watchdog_diagnostics_test_before_publish();
#endif
    /* atomic commit includes a full memory barrier. An interrupted producer
     * cannot publish its inactive slot as a completed reset checkpoint. */
    atomic_set(&slot->magic, (atomic_val_t)CHECKPOINT_MAGIC);
    atomic_set(&generation, (atomic_val_t)(gen + 1U));
    atomic_clear(&writer);
}

void zmk_watchdog_diagnostics_snapshot(uint8_t out[ZMK_WATCHDOG_DIAGNOSTIC_SIZE]) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        unsigned gen = (unsigned)atomic_get(&generation);
        memcpy(out, snapshots[gen & 1U].bytes, ZMK_WATCHDOG_DIAGNOSTIC_SIZE);
        if (gen == (unsigned)atomic_get(&generation)) {
            if (!out[0]) {
                out[0] = 1;
                out[8] = out[9] = out[10] = 255;
                sys_put_le32(WATCHDOG_BUILD_TAG, out + 4);
            }
            if (atomic_get(&writer)) {
                out[2] |= 2U;
            }
            sys_put_le32((uint32_t)atomic_get(&dropped), out + 12);
            return;
        }
    }
    /* Never export a torn event stream as if it were valid. */
    memset(out, 0, ZMK_WATCHDOG_DIAGNOSTIC_SIZE);
    out[0] = 1;
    out[2] = 2;
    out[8] = out[9] = out[10] = 255;
    sys_put_le32(WATCHDOG_BUILD_TAG, out + 4);
    sys_put_le32((uint32_t)atomic_get(&dropped), out + 12);
}
