/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <cormoran/zmk/watchdog.h>
K_SEM_DEFINE(stalled, 0, 1);
static unsigned commits;
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
}
void zmk_watchdog_reboot(void) {
    if (!k_is_in_isr() || captured.type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
        strcmp(captured.detail.freeze.queue_name, "sysworkq") || commits != 2 ||
        (captured.flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE) || k_uptime_get() < 100) {
        exit(1);
    }
    printk("PASS watchdog arms despite preblocked sysworkq time=%lld\n", k_uptime_get());
    exit(0);
}
int main(void) {
    k_sleep(K_SECONDS(1));
    printk("FAIL detector never armed\n");
    exit(1);
}
