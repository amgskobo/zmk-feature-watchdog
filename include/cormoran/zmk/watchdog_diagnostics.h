/* SPDX-License-Identifier: MIT */
#pragma once
#include <cormoran/zmk/watchdog.h>

/* Stable wire event IDs. Values are event-specific; never key material. */
enum zmk_watchdog_event {
    ZMK_WDG_BOOT = 1,
    ZMK_WDG_PROFILE = 2,
    ZMK_WDG_ENDPOINT = 3,
    ZMK_WDG_USB = 4,
    ZMK_WDG_CLEAR_BEGIN = 10,
    ZMK_WDG_CLEAR_END = 11,
    ZMK_WDG_UNPAIR_BEGIN = 12,
    ZMK_WDG_UNPAIR_END = 13,
    ZMK_WDG_DISCONNECT_BEGIN = 14,
    ZMK_WDG_DISCONNECT_END = 15,
    ZMK_WDG_CONNECTED = 16,
    ZMK_WDG_DISCONNECTED = 17,
    ZMK_WDG_SECURITY = 18,
    ZMK_WDG_DISCONNECT_REASON = 19,
    ZMK_WDG_MONITOR_ARM = 20,
    ZMK_WDG_PENDING_CONVERT = 21,
    ZMK_WDG_RESET_AUDIT_ERROR = 22,
    ZMK_WDG_RELAY_EXEC_BEGIN = 30,
    ZMK_WDG_RELAY_EXEC_END = 31,
    ZMK_WDG_RELAY_SEND_BEGIN = 32,
    ZMK_WDG_RELAY_SEND_END = 33,
    ZMK_WDG_RELAY_WORK_END = 34,
};

/* StatusResponse runtime_flags, stable wire bits (DESIGN.md, Live runtime flags). */
#define ZMK_WDG_RUNTIME_SUPPORTED (1U << 0)
#define ZMK_WDG_RUNTIME_FATAL (1U << 1)
#define ZMK_WDG_RUNTIME_FREEZE (1U << 2)
#define ZMK_WDG_RUNTIME_SYS_ARMED (1U << 3)
#define ZMK_WDG_RUNTIME_LOW_ARMED (1U << 4)
#define ZMK_WDG_RUNTIME_ARM_FAILED (1U << 5)
#define ZMK_WDG_RUNTIME_RESET_CAUSE (1U << 6)
#define ZMK_WDG_RUNTIME_PREVIOUS (1U << 7)
#define ZMK_WDG_RUNTIME_CORRUPT (1U << 8)
#define ZMK_WDG_RUNTIME_SOFTWARE_ONLY (1U << 9)
#define ZMK_WDG_RUNTIME_PENDING (1U << 10)
#define ZMK_WDG_RUNTIME_PENDING_INVALID (1U << 11)
#define ZMK_WDG_RUNTIME_PERSIST_FAILED (1U << 12)
#define ZMK_WDG_RUNTIME_RESET_AUDIT_FAILED (1U << 13)
#define ZMK_WDG_RUNTIME_CAPTURE_CONTEXT (1U << 14)
#define ZMK_WDG_RUNTIME_ASSERT_CONTEXT (1U << 15)

#if defined(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS)
/* Non-blocking, bounded. Unknown profile is 255; profile indices are zero-based. */
void zmk_watchdog_breadcrumb(uint8_t event, uint8_t profile, uint16_t value);
/* Only PROFILE/ENDPOINT/USB update cached state; no live getters at fault time. */
void zmk_watchdog_diagnostics_snapshot(uint8_t out[ZMK_WATCHDOG_DIAGNOSTIC_SIZE]);
uint32_t zmk_watchdog_diagnostics_build_tag(void);
uint32_t zmk_watchdog_diagnostics_runtime_flags(void);
uint32_t zmk_watchdog_diagnostics_reset_cause(void);
bool zmk_watchdog_diagnostics_previous(uint8_t out[ZMK_WATCHDOG_DIAGNOSTIC_SIZE]);
void zmk_watchdog_diagnostics_monitor_armed(bool lowprio, int result);
void zmk_watchdog_diagnostics_flags(uint32_t set, uint32_t clear);
#if defined(CONFIG_ZMK_WATCHDOG_DIAGNOSTICS_TEST)
void watchdog_diagnostics_test_boot(void);
void watchdog_diagnostics_test_corrupt_slot(unsigned slot);
void watchdog_diagnostics_test_invalidate_slot(unsigned slot);
void watchdog_diagnostics_test_sequence(uint32_t generation);
#endif
#else
static inline void zmk_watchdog_breadcrumb(uint8_t event, uint8_t profile, uint16_t value) {
    (void)event;
    (void)profile;
    (void)value;
}
static inline void zmk_watchdog_diagnostics_snapshot(uint8_t out[ZMK_WATCHDOG_DIAGNOSTIC_SIZE]) {
    for (size_t i = 0; i < ZMK_WATCHDOG_DIAGNOSTIC_SIZE; ++i) {
        out[i] = 0;
    }
}
static inline uint32_t zmk_watchdog_diagnostics_build_tag(void) { return 0; }
static inline uint32_t zmk_watchdog_diagnostics_runtime_flags(void) { return 0; }
static inline uint32_t zmk_watchdog_diagnostics_reset_cause(void) { return 0; }
static inline bool zmk_watchdog_diagnostics_previous(uint8_t out[ZMK_WATCHDOG_DIAGNOSTIC_SIZE]) {
    (void)out;
    return false;
}
static inline void zmk_watchdog_diagnostics_monitor_armed(bool lowprio, int result) {
    (void)lowprio;
    (void)result;
}
static inline void zmk_watchdog_diagnostics_flags(uint32_t set, uint32_t clear) {
    (void)set;
    (void)clear;
}
#endif
