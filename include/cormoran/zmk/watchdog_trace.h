/* SPDX-License-Identifier: MIT */
#pragma once

#include <cormoran/zmk/watchdog.h>

struct k_thread;

/* Called by the watchdog timer, without locks/flash/logging. On unsupported
 * targets only state and the unsupported flag are recorded. */
void watchdog_freeze_capture_thread(struct k_thread *thread,
                                    struct zmk_watchdog_incident_freeze_detail *detail);
