/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include "../../src/watchdog_boot_report.c"
extern struct zmk_watchdog_incident_record records[6];
extern uint16_t stored;
extern bool store_ready, previous_available;
extern int read_error_index;
extern unsigned reads;
static int failures;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            printk("FAIL line=%d: %s\n", __LINE__, #condition);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
static void report_fixture_work(struct k_work *work) {
    ARG_UNUSED(work);
    boot_report_work_handler(&boot_report_work.work);
}
K_WORK_DEFINE(report_fixture, report_fixture_work);
static int report_tick(void) {
    struct k_work_sync sync;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &report_fixture);
    k_work_flush(&report_fixture, &sync);
    int remaining = k_ticks_to_ms_floor32(k_work_delayable_remaining_get(&boot_report_work));
    k_work_cancel_delayable_sync(&boot_report_work, &sync);
    while (log_process()) {
    }
    return remaining;
}

int report_initial_deadline(void) {
    CHECK(k_ticks_to_ms_floor32(k_work_delayable_remaining_get(&boot_report_work)) >= 4900);
    return failures;
}

int report_cases(void) {
    struct k_work_sync sync;
    k_work_cancel_delayable_sync(&boot_report_work, &sync);
    reads = 0;
    store_ready = false;
    for (unsigned i = 0; i < 30; ++i) {
        int remaining = report_tick();
        /* Zephyr relative timeouts round up to the next tick. */
        CHECK(i < 29 ? remaining >= 900 && remaining <= 1000 + k_ticks_to_ms_ceil32(1)
                     : remaining == 0);
    }
    CHECK(reads == 0 && !report_started && ready_attempts == 30);
    CHECK(!k_work_delayable_is_pending(&boot_report_work));
    printk("REPORTCASE unavailable retries=30 reads=0\n");
    ready_attempts = 0;
    store_ready = true;
    stored = 0;
    CHECK(report_tick() == 0);
    CHECK(report_started && report_index == 0 && report_total == 0 && reads == 0);
    printk("REPORTCASE empty reads=0\n");
    report_started = false;
    previous_available = false;
    stored = 6;
    records[0].version = 2; /* Legacy diagnostic bytes must not be exported. */
    records[0].diagnostics[0] = 1;
    records[0].diagnostics[1] = 255;
    records[1].detail.fatal.reason = 2;
    records[1].detail.fatal.pc = 0x1234;
    records[1].detail.fatal.lr = 0x5678;
    memset(records[1].detail.fatal.thread_name, 'S', 16);
    memset(records[1].detail.fatal.context, 0xab, 24);
    records[1].diagnostics[0] = 1;
    records[1].diagnostics[1] = 255; /* Bound the report to seven events. */
    records[1].diagnostics[2] = 4;
    records[2].diagnostics[0] = 9;           /* Unknown format is unavailable. */
    read_error_index = 3;                    /* Mutating/erroring enumeration remains visible. */
    records[4].detail.fatal.pc = 0x69726576; /* Corrupt non-code PC/LR must be omitted. */
    records[4].detail.fatal.lr = 0x20686375;
    struct zmk_watchdog_incident_record before[6];
    memcpy(before, records, sizeof(before));
    for (unsigned i = 0; i < 7; ++i) {
        int remaining = report_tick();
        CHECK(i < 6 ? remaining >= 900 && remaining <= 1000 + k_ticks_to_ms_ceil32(1)
                    : remaining == 0);
    }
    CHECK(memcmp(before, records, sizeof(before)) == 0);
    size_t unused;
    CHECK(k_thread_stack_space_get(&zmk_workqueue_lowprio_work_q()->thread, &unused) == 0);
    CHECK(unused >= 128);
    CHECK(report_index == 6 && report_total == 6 && reads == 6);
    CHECK(!k_work_delayable_is_pending(&boot_report_work));
    printk("REPORTCASE populated attempted=6 failed_index=3\n");
    return failures;
}
