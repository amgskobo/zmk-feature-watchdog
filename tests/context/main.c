/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <nsi_main.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/byteorder.h>
#include <cormoran/zmk/watchdog_context.h>

static bool interrupt_writer;
static bool clock_forbidden;
int64_t __real_z_impl_k_uptime_ticks(void);
int64_t __wrap_z_impl_k_uptime_ticks(void) {
    __ASSERT_NO_MSG(!clock_forbidden);
    return __real_z_impl_k_uptime_ticks();
}
static struct zmk_watchdog_incident_record interrupted;
void watchdog_context_test_before_publish(void) {
    if (!interrupt_writer) {
        return;
    }
    interrupt_writer = false;
    zmk_watchdog_operation_at(0xffff, 0xfffe, 0x11111111, 0x22222222, 0x1235);
    watchdog_context_capture(&interrupted, NULL, NULL);
}

static unsigned forwarded;
#ifndef CONFIG_ASSERT_NO_FILE_INFO
static const char *forwarded_file;
static uint32_t forwarded_line;
#endif
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(esf);
    printk("WATCHDOG_CONTEXT_ASSERT_FAILURE reason=%u\n", reason);
    nsi_exit(99);
}
#ifdef CONFIG_ASSERT_NO_FILE_INFO
void __real_assert_post_action(void) { ++forwarded; }
void __wrap_assert_post_action(void);
#else
void __real_assert_post_action(const char *file, unsigned int line) {
    ++forwarded;
    forwarded_file = file;
    forwarded_line = line;
}
void __wrap_assert_post_action(const char *file, unsigned int line);
#endif

int main(void) {
    static struct zmk_watchdog_incident_record rec;
    uint8_t *c = rec.capture_context;
    watchdog_context_test_reset();
    clock_forbidden = true;
    watchdog_context_minimal(&rec);
    clock_forbidden = false;
    __ASSERT_NO_MSG(c[0] == 1 && sys_get_le32(c + 92) == 0 && (c[1] & ZMK_WDG_CONTEXT_PARTIAL));
    watchdog_context_capture(&rec, NULL, NULL);
    __ASSERT_NO_MSG(c[0] == 1 && c[2] == 0 && c[3] == 0 && (c[1] & ZMK_WDG_CONTEXT_UNSUPPORTED) &&
                    (rec.flags & ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID));
    __ASSERT_NO_MSG(sys_get_le32(c + 92) == 1);
    for (unsigned i = 1; i <= 6; ++i) {
        zmk_watchdog_operation_at(0x1234, 0xff00 + i, 0x80000000U + i, UINT32_MAX - i,
                                  0x1001 + i * 4);
    }
    watchdog_context_capture(&rec, NULL, NULL);
    __ASSERT_NO_MSG(c[3] == 4 && (c[1] & ZMK_WDG_CONTEXT_PARTIAL));
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *e = c + 96 + i * 24;
        __ASSERT_NO_MSG(sys_get_le32(e) == i + 3 && sys_get_le16(e + 12) == 0x1234 &&
                        sys_get_le16(e + 14) == 0xff03 + i &&
                        sys_get_le32(e + 16) == 0x80000003U + i &&
                        sys_get_le32(e + 20) == UINT32_MAX - i - 3);
    }
    watchdog_context_test_reset();
    interrupt_writer = true;
    zmk_watchdog_operation_at(3, 4, 5, 6, 7);
    __ASSERT_NO_MSG(interrupted.capture_context[3] == 0 &&
                    (interrupted.capture_context[1] & ZMK_WDG_CONTEXT_PARTIAL) &&
                    sys_get_le32(interrupted.capture_context + 88) == 1);
    watchdog_context_capture(&rec, NULL, NULL);
    __ASSERT_NO_MSG(c[3] == 1 && sys_get_le32(c + 112) == 5);
    clock_forbidden = true;
    __ASSERT_NO_MSG(watchdog_context_assert_begin(0x3211, 876));
    __ASSERT_NO_MSG(!watchdog_context_assert_begin(0x9999, 1));
    watchdog_context_minimal(&rec);
    __ASSERT_NO_MSG((c[1] & ZMK_WDG_CONTEXT_ASSERT) && sys_get_le32(c + 8) == 0x3211 &&
                    sys_get_le32(c + 12) == 876 && sys_get_le32(c + 92) == 0);
    clock_forbidden = false;
    watchdog_context_capture(&rec, NULL, NULL);
    __ASSERT_NO_MSG((c[1] & ZMK_WDG_CONTEXT_ASSERT) && sys_get_le32(c + 8) == 0x3211 &&
                    sys_get_le32(c + 12) == 876);
    watchdog_context_assert_end();
    watchdog_context_capture(&rec, NULL, NULL);
    __ASSERT_NO_MSG(!(c[1] & ZMK_WDG_CONTEXT_ASSERT));
    clock_forbidden = true;
#ifdef CONFIG_ASSERT_NO_FILE_INFO
    __wrap_assert_post_action();
#else
    __wrap_assert_post_action("synthetic.c", 456);
    __ASSERT_NO_MSG(strcmp(forwarded_file, "synthetic.c") == 0 && forwarded_line == 456);
#endif
    clock_forbidden = false;
    __ASSERT_NO_MSG(forwarded == 1);
    watchdog_context_capture(&rec, NULL, NULL);
    __ASSERT_NO_MSG(!(c[1] & ZMK_WDG_CONTEXT_ASSERT));
    printk("WATCHDOG_CONTEXT_COMPLETE operations=4 interrupted_writer=1 assertion_forward=1\n");
    nsi_exit(0);
    return 0;
}
