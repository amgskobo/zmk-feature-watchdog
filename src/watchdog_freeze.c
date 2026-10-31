/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_trace.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <cormoran/zmk/watchdog_context.h>
#include <zmk/workqueue.h>

#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_freeze, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

/* Each queue is fed twice per timeout. */
#define WATCHDOG_FREEZE_FEED_MS (CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS / 2)

/* The fault may already have exhausted the interrupted thread/ISR stack. */
static struct zmk_watchdog_incident_record freeze_record;

/* One task_wdt channel per monitored work queue, fed by delayable work on
 * that same queue. If the queue is blocked longer than the timeout, the feed
 * never runs and the channel fires. */
struct watchdog_freeze_monitor {
    struct k_work_q *(*get_queue)(void);
    /* Recorded in the incident, truncated to ZMK_WATCHDOG_QUEUE_NAME_LEN - 1. */
    const char *name;

    /* Filled in when armed. */
    int channel_id;
    struct k_work_delayable feed_work;
};

static struct k_work_q *get_sys_work_q(void) { return &k_sys_work_q; }

/* The system work queue is always entry 0. */
static struct watchdog_freeze_monitor watchdog_freeze_monitors[] = {
    {
        .get_queue = get_sys_work_q,
        .name = "sysworkq",
    },
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_MONITOR_LOWPRIO_QUEUE)
    {
        /* CONFIG_ZMK_WATCHDOG selects ZMK_LOW_PRIORITY_WORK_QUEUE. */
        .get_queue = zmk_workqueue_lowprio_work_q,
        .name = "lowprio_workq",
    },
#endif
};

/* task_wdt callback, in timer ISR context: no Flash, locks or logging. */
static void watchdog_freeze_channel_fired(int channel_id, void *user_data) {
    struct watchdog_freeze_monitor *mon = user_data;

    if (!zmk_watchdog_capture_begin()) {
        zmk_watchdog_reboot();
        return;
    }
    memset(&freeze_record, 0, sizeof(freeze_record));
    freeze_record.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    freeze_record.flags = ZMK_WATCHDOG_CAPTURE_INCOMPLETE;
    freeze_record.detail.freeze.trace_flags = ZMK_WATCHDOG_TRACE_PARTIAL;
    freeze_record.detail.freeze.channel_id = (uint8_t)channel_id;
    strncpy(freeze_record.detail.freeze.queue_name, mon->name,
            sizeof(freeze_record.detail.freeze.queue_name) - 1);
    /* Commit a minimal explanation before optional clock/stack capture. */
    watchdog_context_minimal(&freeze_record);
    zmk_watchdog_pending_set(&freeze_record);
    freeze_record.uptime_s = k_uptime_get_32() / 1000U;
    watchdog_freeze_capture_thread(&mon->get_queue()->thread, &freeze_record.detail.freeze);
    watchdog_context_capture(&freeze_record, NULL, &mon->get_queue()->thread);
    freeze_record.flags &= ~ZMK_WATCHDOG_CAPTURE_INCOMPLETE;

    zmk_watchdog_pending_set(&freeze_record);
    zmk_watchdog_reboot();
    zmk_watchdog_capture_end(); /* Only reached by a returning test reboot hook. */
}

static void watchdog_freeze_feed_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct watchdog_freeze_monitor *mon =
        CONTAINER_OF(dwork, struct watchdog_freeze_monitor, feed_work);

    int ret = task_wdt_feed(mon->channel_id);
    if (ret < 0) {
        zmk_watchdog_diagnostics_monitor_armed(mon != &watchdog_freeze_monitors[0], ret);
        return;
    }

    ret = k_work_reschedule_for_queue(mon->get_queue(), &mon->feed_work,
                                      K_MSEC(WATCHDOG_FREEZE_FEED_MS));
    if (ret < 0) {
        zmk_watchdog_diagnostics_monitor_armed(mon != &watchdog_freeze_monitors[0], ret);
    }
}

/* Runs during APPLICATION initialization, after both queues have started,
 * so a queue that is already stopped cannot prevent arming. */
static int watchdog_freeze_init(void) {
    /* NULL selects software-only monitoring; runtime status reports that. */
    int ret = task_wdt_init(NULL);
    if (ret < 0) {
        zmk_watchdog_diagnostics_monitor_armed(false, ret);
        LOG_ERR("task_wdt_init failed: %d", ret);
        return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(watchdog_freeze_monitors); i++) {
        struct watchdog_freeze_monitor *mon = &watchdog_freeze_monitors[i];

        int channel_id =
            task_wdt_add(CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS, watchdog_freeze_channel_fired, mon);
        if (channel_id < 0) {
            zmk_watchdog_diagnostics_monitor_armed(i != 0, channel_id);
            LOG_ERR("task_wdt_add failed for queue '%s': %d", mon->name, channel_id);
            return 0;
        }
        mon->channel_id = channel_id;

        k_work_init_delayable(&mon->feed_work, watchdog_freeze_feed_work_handler);
        int scheduled = k_work_schedule_for_queue(mon->get_queue(), &mon->feed_work,
                                                  K_MSEC(WATCHDOG_FREEZE_FEED_MS));
        zmk_watchdog_diagnostics_monitor_armed(i != 0, scheduled < 0 ? scheduled : 0);

        LOG_INF("Watchdog freeze monitor armed: queue='%s' channel=%d timeout=%dms", mon->name,
                channel_id, CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS);
    }
    return 0;
}

SYS_INIT(watchdog_freeze_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_ZMK_WATCHDOG_TEST) || defined(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
void zmk_watchdog_freeze_disarm_sysworkq_channel_for_test(void) {
    task_wdt_delete(watchdog_freeze_monitors[0].channel_id);
}
#endif
