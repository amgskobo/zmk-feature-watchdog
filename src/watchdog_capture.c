/* SPDX-License-Identifier: MIT */
/* Shared by fatal and freeze capture; no scheduler, clock or logging calls. */
#include <zephyr/sys/atomic.h>
#include <cormoran/zmk/watchdog.h>

static atomic_t capturing;

bool zmk_watchdog_capture_begin(void) { return atomic_cas(&capturing, 0, 1); }

void zmk_watchdog_capture_end(void) { atomic_clear(&capturing); }
