/* SPDX-License-Identifier: MIT */
#include <zephyr/drivers/hwinfo.h>
int reset_cause_get(uint32_t *cause);
int reset_cause_clear(void);
#define hwinfo_get_reset_cause reset_cause_get
#define hwinfo_clear_reset_cause reset_cause_clear
#include "../../src/watchdog_reset_cause.c"

void test_reset_audit(void) { reset_cause_audit_work_handler(&reset_cause_audit_work.work); }
void test_cancel_reset_work(void) { k_work_cancel_delayable(&reset_cause_audit_work); }
