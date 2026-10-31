/* SPDX-License-Identifier: MIT */
#pragma once
#include <cormoran/zmk/watchdog.h>

/* Common capture format 1. Domain/event are owner-defined numeric identifiers;
 * arguments must be diagnostic metadata, never keys, input or peer identities.
 * Writes never wait for another recorder writer; interruption is counted.
 * Operation calls read Zephyr uptime: use a context where that read is safe,
 * never a clock lock holder or assertion hook. Minimal capture uses no clock.
 * Four 24-byte operations retain sequence/time/site/domain/event/two u32 args. */
#define ZMK_WDG_CONTEXT_OPERATIONS 4U
#define ZMK_WDG_CONTEXT_OPERATION_SIZE 24U
#define ZMK_WDG_CONTEXT_OPERATIONS_OFFSET 96U
#define ZMK_WDG_CONTEXT_FORMAT 1U
#define ZMK_WDG_CONTEXT_PARTIAL 0x01U
#define ZMK_WDG_CONTEXT_CALLEE 0x02U
#define ZMK_WDG_CONTEXT_FAULT_REGS 0x04U
#define ZMK_WDG_CONTEXT_STACK 0x08U
#define ZMK_WDG_CONTEXT_ASSERT 0x10U
#define ZMK_WDG_CONTEXT_UNSUPPORTED 0x20U
#define ZMK_WDG_CONTEXT_ISR 0x40U

struct arch_esf;
struct k_thread;
#ifdef __cplusplus
extern "C" {
#endif
#if defined(CONFIG_ZMK_WATCHDOG_CAPTURE_CONTEXT)
void zmk_watchdog_operation(uint16_t domain, uint16_t event, uint32_t arg0, uint32_t arg1);
void zmk_watchdog_operation_at(uint16_t domain, uint16_t event, uint32_t arg0, uint32_t arg1,
                               uint32_t site);
void watchdog_context_capture(struct zmk_watchdog_incident_record *record,
                              const struct arch_esf *esf, struct k_thread *target);
void watchdog_context_minimal(struct zmk_watchdog_incident_record *record);
bool watchdog_context_assert_begin(uint32_t site, uint32_t line);
void watchdog_context_assert_end(void);
#if defined(CONFIG_ZMK_WATCHDOG_CONTEXT_TEST)
void watchdog_context_test_before_publish(void);
void watchdog_context_test_reset(void);
#endif
#else
static inline void zmk_watchdog_operation(uint16_t d, uint16_t e, uint32_t a, uint32_t b) {
    (void)d;
    (void)e;
    (void)a;
    (void)b;
}
static inline void zmk_watchdog_operation_at(uint16_t d, uint16_t e, uint32_t a, uint32_t b,
                                             uint32_t site) {
    (void)d;
    (void)e;
    (void)a;
    (void)b;
    (void)site;
}
static inline void watchdog_context_capture(struct zmk_watchdog_incident_record *r,
                                            const struct arch_esf *e, struct k_thread *t) {
    (void)r;
    (void)e;
    (void)t;
}
static inline void watchdog_context_minimal(struct zmk_watchdog_incident_record *r) { (void)r; }
#endif
#ifdef __cplusplus
}
#endif
