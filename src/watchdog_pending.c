/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include "watchdog_timeout.h"
#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/atomic.h>

#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <zmk/workqueue.h>

#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_pending, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

#define WATCHDOG_PENDING_MAGIC 0x57444741u /* 'WDGA' */

struct zmk_watchdog_pending {
    atomic_t magic;
    struct zmk_watchdog_incident_record record;
    uint32_t crc;
    uint32_t sequence;
};

/* Retained RAM candidate, validated by magic/version/type/CRC at startup.
 * Physical retention depends on the SoC/bootloader/reset path; it is not a
 * power-loss guarantee. Native fixtures inject records without relying on
 * retention across process starts. */
static struct zmk_watchdog_pending pending __noinit;

/* Set by zmk_watchdog_pending_convert() the first time it finds a valid
 * pending record this boot. Deliberately "sticky" (never cleared) so it
 * still reflects the truth even after the slot itself has been zeroed. Not
 * __noinit: this only needs to be valid within a single boot. */
static atomic_t had_incident_this_boot;
static atomic_t pending_access;
static uint32_t converted_sequence;
static bool converted_valid;
static K_MUTEX_DEFINE(convert_lock);
static uint32_t retry_started_ms;
static atomic_t retry_delay;
static uint32_t next_backoff = 1000;

uint32_t zmk_watchdog_pending_retry_delay_ms(void) {
    return MAX(MAX(1, CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS),
               (uint32_t)atomic_get(&retry_delay));
}

bool zmk_watchdog_pending_had_incident_this_boot(void) {
    return atomic_get(&had_incident_this_boot) != 0;
}

static uint32_t pending_crc(const struct zmk_watchdog_incident_record *rec) {
    uint32_t crc = crc32_ieee((const uint8_t *)rec, sizeof(*rec));
    return crc32_ieee_update(crc, (const uint8_t *)&pending.sequence, sizeof(pending.sequence));
}

void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *rec) {
    if (!rec || !atomic_cas(&pending_access, 0, 1)) {
        return; /* A nested ISR never waits for an interrupted producer. */
    }
    uint32_t sequence = pending.sequence + 1U;
    atomic_clear(&pending.magic);
    pending.record = *rec;
    pending.record.version = ZMK_WATCHDOG_RECORD_VERSION;
    if (!pending.record.diagnostics[0]) {
        zmk_watchdog_diagnostics_snapshot(pending.record.diagnostics);
    }
    pending.sequence = sequence;
    pending.crc = pending_crc(&pending.record);
    atomic_set(&pending.magic, WATCHDOG_PENDING_MAGIC);
    zmk_watchdog_diagnostics_flags(ZMK_WDG_RUNTIME_PENDING, 0);
    atomic_clear(&pending_access);
}

static void pending_clear(void) {
    atomic_clear(&pending.magic); /* Invalidate before touching retained bytes. */
    memset(&pending.record, 0, sizeof(pending.record));
    pending.crc = 0;
    zmk_watchdog_diagnostics_flags(0, ZMK_WDG_RUNTIME_PENDING);
}

#if defined(CONFIG_ZMK_WATCHDOG_TEST) || defined(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
void zmk_watchdog_pending_corrupt_crc_for_test(void) {
    if ((uint32_t)atomic_get(&pending.magic) == WATCHDOG_PENDING_MAGIC) {
        pending.crc ^= 0xFFFFFFFFu;
    }
}
#endif

int zmk_watchdog_pending_convert(void) {
    if (k_mutex_lock(&convert_lock, K_NO_WAIT) != 0) {
        return -EAGAIN; /* Another converter owns the handoff, not a second append. */
    }
    int ret = 0;
    if (!atomic_cas(&pending_access, 0, 1)) {
        ret = -EAGAIN;
        goto done;
    }
    if ((uint32_t)atomic_get(&pending.magic) != WATCHDOG_PENDING_MAGIC) {
        atomic_clear(&pending_access);
        goto done;
    }
    if (pending_crc(&pending.record) != pending.crc ||
        pending.record.version != ZMK_WATCHDOG_RECORD_VERSION ||
        pending.record.type > ZMK_WATCHDOG_INCIDENT_RESET_CAUSE) {
        pending_clear();
        atomic_clear(&pending_access);
        retry_started_ms = 0;
        next_backoff = 1000;
        atomic_clear(&retry_delay);
        zmk_watchdog_diagnostics_flags(ZMK_WDG_RUNTIME_PENDING_INVALID,
                                       ZMK_WDG_RUNTIME_PERSIST_FAILED);
        goto done;
    }
    struct zmk_watchdog_incident_record rec = pending.record;
    uint32_t sequence = pending.sequence;
    if (converted_valid && sequence == converted_sequence) {
        pending_clear();
        atomic_clear(&pending_access);
        goto done;
    }
    atomic_clear(&pending_access); /* Never hold a capture guard during Flash I/O. */
    zmk_watchdog_diagnostics_flags(ZMK_WDG_RUNTIME_PENDING, 0);
    if (!zmk_watchdog_store_is_ready() ||
        watchdog_retry_waiting(k_uptime_get_32(), retry_started_ms,
                               (uint32_t)atomic_get(&retry_delay))) {
        ret = -EAGAIN;
        goto done;
    }
    atomic_set(&had_incident_this_boot, 1);
    ret = zmk_watchdog_store_append(&rec);
    if (ret == 0 || ret == -ENOSPC) {
        converted_sequence = sequence;
        converted_valid = true;
        if (atomic_cas(&pending_access, 0, 1)) {
            /* A newer fatal record must not be erased by an older converter. */
            if ((uint32_t)atomic_get(&pending.magic) == WATCHDOG_PENDING_MAGIC &&
                pending.sequence == sequence) {
                pending_clear();
            }
            atomic_clear(&pending_access);
        }
        retry_started_ms = 0;
        next_backoff = 1000;
        atomic_clear(&retry_delay);
        zmk_watchdog_diagnostics_flags(0, ZMK_WDG_RUNTIME_PERSIST_FAILED);
        ret = 0;
    } else {
        atomic_set(&retry_delay, next_backoff);
        retry_started_ms = k_uptime_get_32();
        next_backoff = MIN(next_backoff * 2U, 60000U);
        zmk_watchdog_diagnostics_flags(ZMK_WDG_RUNTIME_PERSIST_FAILED, 0);
    }
    zmk_watchdog_breadcrumb(ZMK_WDG_PENDING_CONVERT, 255, (uint16_t)ret);
done:
    k_mutex_unlock(&convert_lock);
    return ret;
}

/*
 * Boot hook: settings_load() runs from main() *after* every SYS_INIT level
 * (see zmk/app/src/main.c), so a SYS_INIT callback must not call
 * zmk_watchdog_pending_convert() directly -- the store's RAM index would
 * not be populated yet. Instead this SYS_INIT only *schedules* the
 * conversion on the low-priority workqueue. The store's h_commit callback
 * establishes readiness; the delay is only a retry interval, never evidence
 * that settings_load has finished. An early attempt retains the pending slot.
 */

static void pending_convert_work_handler(struct k_work *work) {
    if (zmk_watchdog_pending_convert() < 0) {
        k_work_reschedule_for_queue(
            zmk_workqueue_lowprio_work_q(), k_work_delayable_from_work(work),
            K_TICKS(watchdog_timeout_ticks(zmk_watchdog_pending_retry_delay_ms(),
                                           CONFIG_SYS_CLOCK_TICKS_PER_SEC)));
    }
}

static K_WORK_DELAYABLE_DEFINE(pending_convert_work, pending_convert_work_handler);

static int watchdog_pending_boot_hook(void) {
    /* Expose retained evidence before conversion work needs the low-prio queue.
     * Do not write Flash or mark a pending incident as persisted here. */
    if (atomic_cas(&pending_access, 0, 1)) {
        if ((uint32_t)atomic_get(&pending.magic) == WATCHDOG_PENDING_MAGIC) {
            bool valid = pending_crc(&pending.record) == pending.crc &&
                         pending.record.version == ZMK_WATCHDOG_RECORD_VERSION &&
                         pending.record.type <= ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
            zmk_watchdog_diagnostics_flags(
                valid ? ZMK_WDG_RUNTIME_PENDING : ZMK_WDG_RUNTIME_PENDING_INVALID, 0);
        }
        atomic_clear(&pending_access);
    }
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &pending_convert_work,
                              K_MSEC(CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS));
    return 0;
}

SYS_INIT(watchdog_pending_boot_hook, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* --------------------------------------------------------------------
 * Reboot wrapper: testable indirection around sys_reboot().
 * -------------------------------------------------------------------- */

#if defined(CONFIG_ZMK_WATCHDOG_TEST) || defined(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
static zmk_watchdog_reboot_fn_t reboot_override;

void zmk_watchdog_reboot_set_override(zmk_watchdog_reboot_fn_t override) {
    reboot_override = override;
}
#endif

void zmk_watchdog_reboot(void) {
#if defined(CONFIG_ZMK_WATCHDOG_TEST) || defined(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
    if (reboot_override) {
        reboot_override();
        return;
    }
#endif

    sys_reboot(SYS_REBOOT_WARM);
}
