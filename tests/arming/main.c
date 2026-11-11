/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <cormoran/zmk/watchdog.h>
K_SEM_DEFINE(stalled, 0, 1);
K_SEM_DEFINE(freeze_finished, 0, 1);
static unsigned commits;
static unsigned nested_reboots;
static bool nested;
static struct zmk_watchdog_incident_record captured;
static void stop_queue(struct k_work *w) {
    k_sem_give(&stalled);
    k_sleep(K_FOREVER);
}
static K_WORK_DEFINE(stall_work, stop_queue);
static int block_before_arm(void) {
    k_work_submit(&stall_work);
    if (k_sem_take(&stalled, K_SECONDS(1))) {
        exit(1);
    }
    return 0;
}
SYS_INIT(block_before_arm, APPLICATION, 80);
void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *r) {
    captured = *r;
    ++commits;
    if (commits == 1) {
        if (r->type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
            !(r->flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE)) {
            exit(1);
        }
        nested = true;
        k_sys_fatal_error_handler(K_ERR_KERNEL_PANIC, NULL);
        nested = false;
        /* Enrichment must still see the outer minimal record after a nested
         * fatal handler returns through the test-only reboot hook. */
        if (commits != 1 || r->type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
            !(r->flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE)) {
            exit(1);
        }
    }
}
void zmk_watchdog_reboot(void) {
    if (nested) {
        ++nested_reboots;
        return;
    }
    if (commits == 4) {
        if (k_is_in_isr() || captured.type != ZMK_WATCHDOG_INCIDENT_FATAL ||
            captured.detail.fatal.reason != K_ERR_KERNEL_OOPS ||
            (captured.flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE)) {
            exit(1);
        }
        return;
    }
    if (!k_is_in_isr() || captured.type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
        strcmp(captured.detail.freeze.queue_name, "sysworkq") || commits != 2 ||
        (captured.flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE) || k_uptime_get() < 100) {
        exit(1);
    }
    printk("PASS watchdog arms despite preblocked sysworkq time=%lld\n", k_uptime_get());
    k_sem_give(&freeze_finished);
}
int main(void) {
    if (k_sem_take(&freeze_finished, K_SECONDS(1)) || nested_reboots != 1) {
        printk("FAIL detector never armed or nested fatal replaced freeze\n");
        exit(1);
    }
    if (!zmk_watchdog_capture_begin()) {
        exit(1);
    }
    nested = true;
    k_sys_fatal_error_handler(K_ERR_KERNEL_PANIC, NULL);
    nested = false;
    zmk_watchdog_capture_end();
    if (commits != 2 || nested_reboots != 2) {
        exit(1);
    }
    k_sys_fatal_error_handler(K_ERR_KERNEL_OOPS, NULL);
    if (commits != 4 || !zmk_watchdog_capture_begin()) {
        exit(1);
    }
    zmk_watchdog_capture_end();
    printk("PASS watchdog capture ownership nested=2 sequential=2 commits=4\n");
    exit(0);
}
