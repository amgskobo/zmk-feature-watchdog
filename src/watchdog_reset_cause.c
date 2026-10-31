/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include "watchdog_timeout.h"
#include <errno.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <zmk/workqueue.h>

#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_reset_cause, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

/* Reset causes that indicate the reboot may have been an *undetected*
 * software/hardware failure (as opposed to a normal power-on, pin reset, or
 * a reboot our own freeze/fatal detectors already explained). See
 * DESIGN.md, Pending conversion and reset audit. */
#define WATCHDOG_RESET_CAUSE_OF_INTEREST (RESET_WATCHDOG | RESET_CPU_LOCKUP | RESET_BROWNOUT)

/*
 * Ordering: this audit must run *after* the pending-slot boot conversion
 * (src/watchdog_pending.c) has had a chance to run, so
 * zmk_watchdog_pending_had_incident_this_boot() reflects this boot's
 * outcome. The delay alone cannot establish ordering when settings load is
 * slow. Wait for the store's settings commit, then finish pending conversion
 * before recording an interesting reset cause. Both boot works use the low-priority queue.
 */
#define WATCHDOG_RESET_CAUSE_DELAY_MS (CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS + 100)

/* An appended/dropped explanation must not be appended again if cause clearing
 * fails. This is current-boot state, not a transaction across physical resets. */
static bool audit_explained;

static void retry_audit(struct k_work *work, int error) {
    zmk_watchdog_diagnostics_flags(ZMK_WDG_RUNTIME_RESET_AUDIT_FAILED, 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_RESET_AUDIT_ERROR, 255, (uint16_t)error);
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), k_work_delayable_from_work(work),
                                K_SECONDS(1));
}

static void clear_audited_cause(struct k_work *work) {
    int ret = hwinfo_clear_reset_cause();
    if (ret < 0) {
        retry_audit(work, ret);
    } else {
        zmk_watchdog_diagnostics_flags(0, ZMK_WDG_RUNTIME_RESET_AUDIT_FAILED);
    }
}

static void reset_cause_audit_work_handler(struct k_work *work) {
    if (!zmk_watchdog_store_is_ready()) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(),
                                    k_work_delayable_from_work(work),
                                    K_MSEC(WATCHDOG_RESET_CAUSE_DELAY_MS));
        return;
    }
    uint32_t cause = 0;
    int ret = hwinfo_get_reset_cause(&cause);
    if (ret < 0) {
        if (ret != -ENOSYS && ret != -ENOTSUP) {
            retry_audit(work, ret);
        }
        return; /* Unsupported drivers provide no reset cause. */
    }

    if ((cause & WATCHDOG_RESET_CAUSE_OF_INTEREST) == 0) {
        /* Clear anyway: nRF-style hwinfo reset cause bits accumulate across
         * resets, so leaving uninteresting bits (e.g. RESET_PIN) set would
         * make them resurface once an interesting bit eventually appears
         * alongside them. */
        clear_audited_cause(work);
        return;
    }

    int pending_ret = zmk_watchdog_pending_convert();
    if (pending_ret < 0) {
        k_work_reschedule_for_queue(
            zmk_workqueue_lowprio_work_q(), k_work_delayable_from_work(work),
            K_TICKS(watchdog_timeout_ticks(zmk_watchdog_pending_retry_delay_ms(),
                                           CONFIG_SYS_CLOCK_TICKS_PER_SEC)));
        return; /* Preserve cause bits until the pending handoff can finish. */
    }
    if (audit_explained || zmk_watchdog_pending_had_incident_this_boot()) {
        /* A freeze or fatal-error detector already recorded (or attempted
         * to record) an incident that explains this reboot -- a separate
         * RESET_CAUSE incident would just be noise. */
        clear_audited_cause(work);
        return;
    }

    /* Interesting hardware cause without a completed software explanation.
     * Capture only the raw bits in settings-ready thread context. This does
     * not imply that a hardware watchdog was configured by this module. */
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
    rec.uptime_s = k_uptime_get_32() / 1000U;
    rec.detail.reset.cause_bits = cause;

    int append_ret = zmk_watchdog_store_append(&rec);
    if (append_ret < 0 && append_ret != -ENOSPC) {
        retry_audit(work, append_ret);
        return; /* A failed save must not erase the hardware evidence. */
    }

    audit_explained = true;
    clear_audited_cause(work);
}

static K_WORK_DELAYABLE_DEFINE(reset_cause_audit_work, reset_cause_audit_work_handler);

static int watchdog_reset_cause_boot_hook(void) {
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &reset_cause_audit_work,
                              K_MSEC(WATCHDOG_RESET_CAUSE_DELAY_MS));
    return 0;
}

SYS_INIT(watchdog_reset_cause_boot_hook, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
