/* SPDX-License-Identifier: MIT */
/* Reuse the complete real-event/codec/store boundary, not a second worker. */
#define main relay_stack_scenarios_main
#include "main.c"
#undef main
#include <zephyr/task_wdt/task_wdt.h>

static atomic_t producing, completed, lowprio_feeds, freeze_events, capture_commits;
static int lowprio_channel = -1;
static unsigned registered_channels;
static struct zmk_watchdog_incident_record captured;
K_SEM_DEFINE(freeze_observed, 0, 1);

int __real_task_wdt_add(uint32_t timeout, task_wdt_callback_t callback, void *data);
int __wrap_task_wdt_add(uint32_t timeout, task_wdt_callback_t callback, void *data) {
    int channel = __real_task_wdt_add(timeout, callback, data);
    if (++registered_channels == 2)
        lowprio_channel = channel;
    return channel;
}
int __real_task_wdt_feed(int channel);
int __wrap_task_wdt_feed(int channel) {
    int result = __real_task_wdt_feed(channel);
    if (channel == lowprio_channel) {
        CHECK(!k_is_in_isr() && k_current_get() == &relay_queue.thread && result == 0);
        atomic_inc(&lowprio_feeds);
    }
    return result;
}
void zmk_watchdog_diagnostics_monitor_armed(bool lowprio, int result) {
    ARG_UNUSED(lowprio);
    CHECK(result == 0);
}
void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *record) {
    CHECK(k_is_in_isr());
    captured = *record;
    atomic_inc(&capture_commits);
}
void zmk_watchdog_reboot(void) {
    CHECK(k_is_in_isr());
    CHECK(captured.type == ZMK_WATCHDOG_INCIDENT_FREEZE);
    CHECK(strcmp(captured.detail.freeze.queue_name, "lowprio_workq") == 0);
    CHECK(!(captured.flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE));
    atomic_inc(&freeze_events);
    k_sem_give(&freeze_observed);
}
/* The unused original entry point still links all its unchanged assertions. */
int report_cases(void) { return 0; }
int report_initial_deadline(void) { return 0; }

K_THREAD_STACK_DEFINE(replenisher_stack, 4096);
static struct k_thread replenisher_thread;
static void replenish(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    unsigned expected = 1000, next = 1004;
    while (atomic_get(&producing) || k_msgq_num_used_get(&sent) ||
           k_msgq_num_used_get(&watchdog_relay_requests) ||
           k_work_busy_get(&watchdog_relay_process_work)) {
        struct zmk_watchdog_relay_response wire;
        if (k_msgq_get(&sent, &wire, K_MSEC(20)) != 0)
            continue;
        cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
        pb_istream_t input = pb_istream_from_buffer(wire.payload, wire.size);
        CHECK(pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &response));
        CHECK(response.request_id == expected++);
        CHECK(response.which_relay_response_type == cormoran_watchdog_RelayResponse_response_tag);
        CHECK(response.relay_response_type.response.which_relay_non_list_response_type ==
              cormoran_watchdog_RelayNonListResponse_status_tag);
        atomic_inc(&completed);
        if (atomic_get(&producing))
            enqueue_status(next++);
    }
}

int main(void) {
    CHECK(registered_channels == 2 && lowprio_channel >= 0);
    store_ready = true;
    k_work_submit_to_queue(&relay_queue, &request_blocker);
    CHECK(k_sem_take(&request_blocked, K_MSEC(50)) == 0);
    for (unsigned id = 1000; id < 1004; ++id)
        enqueue_status(id);
    atomic_set(&producing, 1);
    k_thread_create(&replenisher_thread, replenisher_stack,
                    K_THREAD_STACK_SIZEOF(replenisher_stack), replenish, NULL, NULL, NULL, 5, 0,
                    K_NO_WAIT);
    k_sem_give(&request_release);
    /* Four software-watchdog periods, while a higher-priority producer puts
     * a replacement in the FIFO before every bounded transport send returns. */
    k_sleep(K_MSEC(400));
    atomic_set(&producing, 0);
    CHECK(k_thread_join(&replenisher_thread, K_SECONDS(1)) == 0);
    flush_requests();
    CHECK(atomic_get(&completed) >= 20);
    CHECK(atomic_get(&lowprio_feeds) >= 4);
    CHECK(atomic_get(&freeze_events) == 0);
    CHECK(atomic_get(&capture_commits) == 0);
    CHECK(atomic_get(&overflow) == 0);
    printk("FAIRNESSCASE replenished responses=%ld feeds=%ld freezes=%ld\n",
           (long)atomic_get(&completed), (long)atomic_get(&lowprio_feeds),
           (long)atomic_get(&freeze_events));

    /* Positive detector control: a truly stopped queue must still time out.
     * Capture/reboot are controlled observers; task_wdt, ISR, queue and feed
     * are the production implementation, and no monitor is suspended. */
    int previous_freezes = atomic_get(&freeze_events);
    int previous_commits = atomic_get(&capture_commits);
    k_sem_reset(&freeze_observed);
    k_work_submit_to_queue(&relay_queue, &request_blocker);
    CHECK(k_sem_take(&request_blocked, K_MSEC(50)) == 0);
    int previous_feeds = atomic_get(&lowprio_feeds);
    CHECK(k_sem_take(&freeze_observed, K_MSEC(250)) == 0);
    CHECK(atomic_get(&freeze_events) == previous_freezes + 1);
    CHECK(atomic_get(&capture_commits) == previous_commits + 2);
    CHECK(captured.detail.freeze.channel_id == lowprio_channel);
    CHECK(atomic_get(&lowprio_feeds) == previous_feeds);
    k_sem_give(&request_release);
    flush_requests();
    k_sleep(K_MSEC(60));
    CHECK(atomic_get(&lowprio_feeds) > previous_feeds);
    size_t unused;
    CHECK(k_thread_stack_space_get(&relay_queue.thread, &unused) == 0);
    CHECK(unused >= MIN_EXCEPTION_HEADROOM);
    printk("FAIRNESSCASE real-stall detected=1 recovered_feed=1 unused=%u\n", (unsigned)unused);
    printk("RESULT: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return 0;
}
