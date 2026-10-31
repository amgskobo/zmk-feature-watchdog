/* SPDX-License-Identifier: MIT */
/* Real Cortex-M4 kernel/context layout; no board/radio/reset acceptance. */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cormoran/zmk/watchdog_trace.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <cormoran/zmk/watchdog_context.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/byteorder.h>

static unsigned failures;
#define CHECK(c, name)                                                                             \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            ++failures;                                                                            \
            printk("FAIL %s\n", name);                                                             \
        }                                                                                          \
    } while (0)
static volatile int progress, stop;
K_MUTEX_DEFINE(trace_mutex);
K_SEM_DEFINE(timer_done, 0, 1);

static void owner_entry(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    k_mutex_lock(&trace_mutex, K_FOREVER);
    k_sleep(K_FOREVER);
}
static int __noinline wait_for_mutex(void) {
    int err = k_mutex_lock(&trace_mutex, K_FOREVER);
    ++progress;
    return err;
}
static void waiter_entry(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    volatile float x = 1.5f;
    x *= 2.0f;
    progress = wait_for_mutex() + (int)x;
}
static void __noinline spin(void) {
    while (!stop) {
    }
}
static void spinner_entry(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    volatile float x = 2.5f;
    x *= 2.0f;
    spin();
    progress = (int)x;
}
K_THREAD_DEFINE(owner_thread, 1024, owner_entry, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(waiter_thread, 1024, waiter_entry, NULL, NULL, NULL, 4, K_FP_REGS, 50);
K_THREAD_DEFINE(spinner_thread, 1024, spinner_entry, NULL, NULL, NULL, 6, K_FP_REGS, 200);

static struct zmk_watchdog_incident_freeze_detail snapshots[3];
static bool from_isr;
static void sample(struct k_timer *timer) {
    ARG_UNUSED(timer);
    from_isr = k_is_in_isr();
    watchdog_freeze_capture_thread(waiter_thread, &snapshots[0]);
    watchdog_freeze_capture_thread(owner_thread, &snapshots[1]);
    watchdog_freeze_capture_thread(spinner_thread, &snapshots[2]);
    k_sem_give(&timer_done);
}
static K_TIMER_DEFINE(timer, sample, NULL);
static bool interrupt_writer;
void watchdog_diagnostics_test_before_publish(void) {
    if (!interrupt_writer) {
        return;
    }
    interrupt_writer = false;
    uint8_t data[80];
    zmk_watchdog_breadcrumb(ZMK_WDG_UNPAIR_BEGIN, 3, 1); /* nested writer must drop */
    zmk_watchdog_diagnostics_snapshot(data);             /* interrupted writer must not deadlock */
    CHECK(data[0] == 1 && (data[2] & 2) && sys_get_le32(data + 12) == 1,
          "interrupted writer/drop recorded");
    CHECK(data[1] == 0, "unpublished event never copied");
}
static struct zmk_watchdog_incident_record fatal_record;
static bool nested_capture_test;
static unsigned reboot_calls, minimal_commits;
#if defined(CONFIG_ZMK_WATCHDOG_ASSERT_CASE)
static uint32_t assertion_line;
#endif
void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *rec) {
    fatal_record = *rec;
    if (rec->flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE) {
        ++minimal_commits;
    }
}
void zmk_watchdog_reboot(void) {
    if (nested_capture_test) {
        if (++reboot_calls == 1) {
            k_sys_fatal_error_handler(K_ERR_KERNEL_PANIC, NULL);
        }
        return;
    }
#if defined(CONFIG_ZMK_WATCHDOG_ASSERT_CASE)
    const unsigned expected_reason = K_ERR_KERNEL_PANIC;
    CHECK((fatal_record.capture_context[1] & ZMK_WDG_CONTEXT_ASSERT) &&
              sys_get_le32(fatal_record.capture_context + 12) == assertion_line &&
              sys_get_le32(fatal_record.capture_context + 8) != 0,
          "real Zephyr assertion retains original call site and source line");
#else
    const unsigned expected_reason = K_ERR_KERNEL_OOPS;
#endif
    CHECK(fatal_record.type == ZMK_WATCHDOG_INCIDENT_FATAL &&
              fatal_record.detail.fatal.reason == expected_reason,
          "real kernel oops capture");
    CHECK((fatal_record.flags & 1) && fatal_record.detail.fatal.pc && fatal_record.detail.fatal.lr,
          "real Cortex-M frame");
    CHECK((fatal_record.detail.fatal.context[5] & 511) != 0, "fatal interrupted ISR identified");
    CHECK(fatal_record.diagnostics[0] == 1 && fatal_record.diagnostics[1] == 7,
          "breadcrumbs survive fatal builder");
    CHECK((fatal_record.flags & ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID) &&
              fatal_record.capture_context[0] == 1 &&
              (fatal_record.capture_context[1] & ZMK_WDG_CONTEXT_CALLEE) &&
              (fatal_record.capture_context[1] & ZMK_WDG_CONTEXT_ISR),
          "generic exception captures preserved callee registers and ISR context");
    CHECK(fatal_record.capture_context[3] >= 1 &&
              sys_get_le32(fatal_record.capture_context + 112) == 0x81234567U &&
              sys_get_le32(fatal_record.capture_context + 116) == 0xfedcba98U,
          "generic asynchronous callback metadata survives real fatal");
    printk("FATAL {\"pc\":%u,\"lr\":%u,\"xpsr\":%u,\"build\":%u,\"origin\":%u}\n",
           fatal_record.detail.fatal.pc, fatal_record.detail.fatal.lr,
           fatal_record.detail.fatal.context[5], sys_get_le32(fatal_record.diagnostics + 4),
           sys_get_le32(fatal_record.capture_context + 8));
    printk("RESULT: %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    for (;;) {
    }
}
static void fatal_timer_handler(struct k_timer *t) {
    ARG_UNUSED(t);
    zmk_watchdog_operation(0x42, 0x1234, 0x81234567U, 0xfedcba98U);
#if defined(CONFIG_ZMK_WATCHDOG_ASSERT_CASE)
    assertion_line = __LINE__ + 1;
    __ASSERT_NO_MSG(false);
#else
    k_oops();
#endif
}
static K_TIMER_DEFINE(fatal_timer, fatal_timer_handler, NULL);
static void checkpoint_bounds(void) {
    uint8_t data[80], live[80];
    watchdog_diagnostics_test_boot();
    CHECK(!zmk_watchdog_diagnostics_previous(data), "empty boot has no previous checkpoint");
    CHECK(zmk_watchdog_diagnostics_build_tag() == 0x12345678,
          "live build identity is independent of incidents");
    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 2, 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_CLEAR_BEGIN, 2, 73);
    zmk_watchdog_diagnostics_snapshot(live);
    watchdog_diagnostics_test_boot(); /* same RAM, simulated startup; not a physical reset */
    CHECK(zmk_watchdog_diagnostics_previous(data) && !memcmp(data, live, 80),
          "last completed checkpoint and byte order recovered");
    CHECK(zmk_watchdog_diagnostics_runtime_flags() & ZMK_WDG_RUNTIME_PREVIOUS,
          "runtime reports recovered checkpoint");
    zmk_watchdog_diagnostics_snapshot(live);
    CHECK(live[1] == 0 && live[8] == 255, "new boot starts independent history");
    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 1, 0);
    watchdog_diagnostics_test_boot();
    CHECK(zmk_watchdog_diagnostics_previous(data) && data[1] == 1 && data[8] == 1,
          "second reset recovers only the immediately previous boot");
    watchdog_diagnostics_test_boot();
    CHECK(!zmk_watchdog_diagnostics_previous(data), "empty reset does not replay older boot");

    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 5, 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_UNPAIR_BEGIN, 5, 7);
    watchdog_diagnostics_test_invalidate_slot(0); /* CPU reset before inactive commit */
    watchdog_diagnostics_test_boot();
    CHECK(zmk_watchdog_diagnostics_previous(data) && data[1] == 1 && data[8] == 5 && (data[2] & 2),
          "interrupted commit falls back to completed slot, marked partial");

    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 3, 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_CLEAR_END, 3, 0);
    watchdog_diagnostics_test_corrupt_slot(0);
    watchdog_diagnostics_test_boot();
    CHECK(zmk_watchdog_diagnostics_previous(data) && data[1] == 1 && (data[2] & 2) &&
              (zmk_watchdog_diagnostics_runtime_flags() & ZMK_WDG_RUNTIME_CORRUPT),
          "invalid CRC rejected with valid older-slot recovery");
    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 4, 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_CLEAR_END, 4, 0);
    watchdog_diagnostics_test_corrupt_slot(0);
    watchdog_diagnostics_test_corrupt_slot(1);
    watchdog_diagnostics_test_boot();
    CHECK(!zmk_watchdog_diagnostics_previous(data) &&
              (zmk_watchdog_diagnostics_runtime_flags() & ZMK_WDG_RUNTIME_CORRUPT),
          "two corrupt slots never exported as valid evidence");

    watchdog_diagnostics_test_sequence(0xfffffffeU);
    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 2, 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_CLEAR_END, 2, 123);
    watchdog_diagnostics_test_boot();
    CHECK(zmk_watchdog_diagnostics_previous(data) && data[1] == 2 && sys_get_le16(data + 38) == 123,
          "checkpoint sequence wrap preserves newest order");
    watchdog_diagnostics_test_boot();
    zmk_watchdog_diagnostics_monitor_armed(false, -12);
    zmk_watchdog_diagnostics_monitor_armed(false, 0);
    zmk_watchdog_diagnostics_monitor_armed(true, 0);
    CHECK((zmk_watchdog_diagnostics_runtime_flags() &
           (ZMK_WDG_RUNTIME_SYS_ARMED | ZMK_WDG_RUNTIME_LOW_ARMED | ZMK_WDG_RUNTIME_ARM_FAILED)) ==
              (ZMK_WDG_RUNTIME_SYS_ARMED | ZMK_WDG_RUNTIME_LOW_ARMED | ZMK_WDG_RUNTIME_ARM_FAILED),
          "monitor registration and failure visible in status");
    watchdog_diagnostics_test_boot(); /* leave original diagnostic/fatal checks independent */
}
static void diagnostic_bounds(void) {
    interrupt_writer = true;
    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, 3, 0);
    for (unsigned i = 0; i < 10; ++i) {
        zmk_watchdog_breadcrumb(ZMK_WDG_DISCONNECT_BEGIN, 3, i);
    }
    uint8_t data[80];
    zmk_watchdog_diagnostics_snapshot(data);
    CHECK(data[1] == 7 && (data[2] & 1) && data[8] == 3, "bounded history/cached profile");
    for (unsigned i = 0; i < 7; ++i) {
        CHECK(sys_get_le16(data + 30 + i * 8) == i + 3, "chronological values after eviction");
    }
    struct arch_esf esf = {0};
    esf.basic.a1 = 1;
    esf.basic.a2 = 2;
    esf.basic.a3 = 3;
    esf.basic.a4 = 4;
    esf.basic.ip = 12;
    esf.basic.xpsr = BIT(24) | 17;
    struct zmk_watchdog_incident_record rec;
    watchdog_fatal_build_record(&rec, 3, &esf);
    CHECK((rec.flags & 1) && rec.detail.fatal.context[4] == 12 &&
              rec.detail.fatal.context[5] == (BIT(24) | 17),
          "all exception register words");
    watchdog_fatal_build_record(&rec, 3, (const struct arch_esf *)0xffffffffU);
    CHECK(!(rec.flags & 1) && !rec.detail.fatal.pc, "invalid ESF rejected without dereference");
    watchdog_fatal_build_record(&rec, 3, NULL);
    CHECK(!(rec.flags & 1) && !rec.detail.fatal.context[5] && !rec.detail.fatal.pc,
          "NULL ESF stays unavailable");
}
static uint32_t __noinline poison_value(void) {
    return (uint32_t)(uintptr_t)__builtin_return_address(0) | 1U;
}
static void poison_stack(struct k_thread *t, uint32_t poison) {
    for (uintptr_t p = t->stack_info.start; p + 4U <= t->callee_saved.psp; p += 4U) {
        *(volatile uint32_t *)p = poison;
    }
}
static bool contains(const struct zmk_watchdog_incident_freeze_detail *d, uint32_t value) {
    for (unsigned i = 0; i < d->trace_count; ++i) {
        if (d->addresses[i] == value) {
            return true;
        }
    }
    return false;
}
static void print_trace(const char *name, const struct zmk_watchdog_incident_freeze_detail *d) {
    printk("TRACE {\"name\":\"%s\",\"state\":%u,\"flags\":%u,\"wait\":%u,\"sp\":%u,\"addresses\":[",
           name, d->thread_state, d->trace_flags, d->wait_queue, d->stack_pointer);
    for (unsigned i = 0; i < d->trace_count; ++i) {
        printk("%s%u", i ? "," : "", d->addresses[i]);
    }
    printk("]}\n");
}

static void synthetic_bounds(uint32_t poison) {
    static struct k_thread fake;
    static uint32_t words[32];
    fake = *waiter_thread;
    fake.base.pended_on = NULL;
    fake.stack_info.start = (uintptr_t)words;
    fake.stack_info.size = sizeof(words);
    fake.callee_saved.psp = (uintptr_t)words;
    fake.arch.mode_exc_return = 0xFD; /* basic frame */
    memset(words, 0, sizeof(words));
    words[6] = (uintptr_t)spin & ~1U;
    words[7] = BIT(24) | BIT(9);
    words[8] = poison; /* alignment pad must not be a caller */
    struct zmk_watchdog_incident_freeze_detail d = {0};
    watchdog_freeze_capture_thread(&fake, &d);
    CHECK(d.trace_count == 1 && !contains(&d, poison), "alignment pad skipped");
    fake.arch.mode_exc_return = 0xED; /* extended FP frame */
    words[7] = BIT(24);
    for (unsigned i = 8; i < 26; ++i) {
        words[i] = poison;
    }
    watchdog_freeze_capture_thread(&fake, &d);
    CHECK(d.trace_count == 1 && !contains(&d, poison), "FP words skipped");
    words[26] = poison;
    watchdog_freeze_capture_thread(&fake, &d);
    CHECK(d.trace_count == 2 && contains(&d, poison), "scan starts after full FP frame");
    fake.callee_saved.psp = (uintptr_t)words + sizeof(words) - 4;
    watchdog_freeze_capture_thread(&fake, &d);
    CHECK(d.trace_count == 0 && (d.trace_flags & ZMK_WATCHDOG_TRACE_PARTIAL),
          "short frame rejected");
    fake.callee_saved.psp = 0xFFFFFFFF;
    fake.base.pended_on = (void *)0xFFFFFFFF;
    watchdog_freeze_capture_thread(&fake, &d);
    CHECK(d.trace_count == 0 && !d.wait_queue, "invalid pointers rejected without reads");
    fake.stack_info.start = 0xFFFFFFF0;
    fake.stack_info.size = 0xFFFFFFFF;
    watchdog_freeze_capture_thread(&fake, &d);
    CHECK(d.trace_count == 0, "overflowing stack bounds rejected");
}

static void common_context_bounds(uint32_t poison) {
    static struct arch_esf frame;
    static _callee_saved_t callee;
    static struct zmk_watchdog_incident_record rec;
    memset(&frame, 0, sizeof(frame));
    frame.basic.pc = (uintptr_t)spin & ~1U;
    frame.basic.lr = poison;
    frame.basic.xpsr = BIT(24) | 27U;
    callee = (_callee_saved_t){.v1 = 0x12345678, .v8 = 0x87654321};
    frame.extra_info.callee = &callee;
    watchdog_context_capture(&rec, &frame, NULL);
    CHECK((rec.capture_context[1] & ZMK_WDG_CONTEXT_CALLEE) &&
              sys_get_le32(rec.capture_context + 32) == 0x12345678 &&
              sys_get_le32(rec.capture_context + 60) == 0x87654321,
          "common context preserves full-width callee registers");
    CHECK((rec.capture_context[1] & ZMK_WDG_CONTEXT_PARTIAL) &&
              !(rec.capture_context[1] & ZMK_WDG_CONTEXT_STACK),
          "missing original ISR stack is explicitly partial");
    frame.extra_info.callee = (void *)0xfffffffd;
    watchdog_context_capture(&rec, &frame, NULL);
    CHECK(!(rec.capture_context[1] & ZMK_WDG_CONTEXT_CALLEE),
          "invalid callee pointer rejected without reads");
    watchdog_context_capture(&rec, (void *)0xfffffffd, NULL);
    CHECK(rec.capture_context[2] == 0 && (rec.capture_context[1] & ZMK_WDG_CONTEXT_PARTIAL),
          "invalid extended ESF rejected without reads");
    rec.detail.freeze = snapshots[0];
    watchdog_context_capture(&rec, NULL, waiter_thread);
    CHECK(rec.capture_context[2] >= 3 && (rec.capture_context[1] & ZMK_WDG_CONTEXT_STACK),
          "freeze uses the same common context envelope");
}

int main(void) {
    const uint32_t poison = poison_value();
    poison_stack(owner_thread, poison);
    poison_stack(waiter_thread, poison);
    poison_stack(spinner_thread, poison);
    k_timer_start(&timer, K_MSEC(300), K_NO_WAIT);
    CHECK(k_sem_take(&timer_done, K_SECONDS(5)) == 0, "timer callback completes");
    stop = 1;
    CHECK(from_isr, "capture in ISR");
    CHECK((snapshots[0].trace_flags & (ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_PENDED)) ==
              (ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_PENDED),
          "blocked target frame/wait");
    CHECK(!(snapshots[0].trace_flags & ZMK_WATCHDOG_TRACE_RUNNING),
          "blocked target is not current thread");
    CHECK(snapshots[0].wait_queue == (uintptr_t)&trace_mutex, "exact wait queue");
    CHECK((snapshots[0].thread_state & 2U) != 0 && snapshots[0].trace_count >= 3,
          "blocked state/callers");
    CHECK(!(snapshots[1].trace_flags & (ZMK_WATCHDOG_TRACE_RUNNING | ZMK_WATCHDOG_TRACE_PENDED)),
          "sleeping target");
    CHECK((snapshots[2].trace_flags & (ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_RUNNING)) ==
              (ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_RUNNING),
          "running target frame");
    CHECK(snapshots[2].trace_count == 2 && (snapshots[2].trace_flags & ZMK_WATCHDOG_TRACE_PARTIAL),
          "running FP scan explicitly partial");
    for (unsigned j = 0; j < 3; ++j) {
        CHECK(snapshots[j].trace_count <= 6 && !contains(&snapshots[j], poison),
              "bounded trace rejects unwritten poison");
    }
    synthetic_bounds(poison);
    common_context_bounds(poison);
    print_trace("pended", &snapshots[0]);
    print_trace("asleep", &snapshots[1]);
    print_trace("running", &snapshots[2]);
    nested_capture_test = true;
    k_sys_fatal_error_handler(K_ERR_KERNEL_OOPS, NULL);
    CHECK(reboot_calls == 2 && minimal_commits == 1 &&
              fatal_record.detail.fatal.reason == K_ERR_KERNEL_OOPS &&
              !(fatal_record.flags & ZMK_WATCHDOG_CAPTURE_INCOMPLETE),
          "nested fatal reboot bypasses capture and preserves outer record");
    CHECK(zmk_watchdog_capture_begin(), "returning test hook releases capture guard");
    zmk_watchdog_capture_end();
    nested_capture_test = false;
    checkpoint_bounds();
    diagnostic_bounds();
    k_timer_start(&fatal_timer, K_MSEC(10), K_NO_WAIT);
    k_sleep(K_FOREVER);
    return 0;
}
