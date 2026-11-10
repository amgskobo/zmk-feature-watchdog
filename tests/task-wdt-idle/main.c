/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <cormoran/zmk/watchdog.h>

static bool initializing;
static struct k_timer *observed_timer;
static atomic_t starts;

/* Probes record the actual Task WDT kernel timer; all calls reach Zephyr. */
void __real_z_impl_k_timer_start(struct k_timer *, k_timeout_t, k_timeout_t);
void __wrap_z_impl_k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period) {
    if (initializing) {
        observed_timer = timer;
    }
    if (timer == observed_timer) {
        atomic_inc(&starts);
    }
    __real_z_impl_k_timer_start(timer, duration, period);
}

int __real_task_wdt_init(const struct device *device);
int __wrap_task_wdt_init(const struct device *device) {
    if (device != NULL) {
        printk("FAIL watchdog idle unexpected hardware device\n");
        exit(1);
    }
    initializing = true;
    int result = __real_task_wdt_init(device);
    initializing = false;
    return result;
}

void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *record) {
    ARG_UNUSED(record);
}

void zmk_watchdog_reboot(void) {
    printk("FAIL watchdog idle unexpected freeze\n");
    exit(1);
}

int main(void) {
    const unsigned channels = 1 + IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_MONITOR_LOWPRIO_QUEUE);
    k_msleep(1200);
    unsigned idle_starts = atomic_get(&starts);
    uint32_t remaining = observed_timer ? k_timer_remaining_get(observed_timer) : 0;
    if (idle_starts != 1 + channels || remaining < 8000 || remaining > 10000) {
        printk("FAIL watchdog idle background timer starts=%u remaining_ms=%u\n", idle_starts,
               remaining);
        exit(1);
    }
    /* The real queue-owned feeds run at 5 s; no 100-ms timer is needed. */
    k_msleep(4000);
    unsigned fed_starts = atomic_get(&starts);
    remaining = k_timer_remaining_get(observed_timer);
    if (fed_starts != 1 + 2 * channels || remaining < 9000 || remaining > 10000) {
        printk("FAIL watchdog idle feed cadence starts=%u remaining_ms=%u\n", fed_starts,
               remaining);
        exit(1);
    }
    printk("PASS watchdog idle timer channels=%u idle_starts=%u fed_starts=%u time_ms=%lld\n",
           channels, idle_starts, fed_starts, k_uptime_get());
    exit(0);
}
