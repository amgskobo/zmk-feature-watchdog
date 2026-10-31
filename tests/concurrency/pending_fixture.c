/* SPDX-License-Identifier: MIT */
#include "../../src/watchdog_pending.c"

void test_cancel_boot_work(void) { k_work_cancel_delayable(&pending_convert_work); }

void test_new_boot_without_pending(void) { atomic_clear(&had_incident_this_boot); }
