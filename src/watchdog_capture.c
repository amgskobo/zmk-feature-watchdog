/* SPDX-License-Identifier: MIT */
/* Shared by fatal and freeze capture; no scheduler, clock or logging calls. */
#include <zephyr/sys/atomic.h>
#include <cormoran/zmk/watchdog.h>

static atomic_t capturing;
/* Both detectors publish only after acquiring capturing. A nested fault goes
 * directly to reboot, so the fault paths need one off-stack record together. */
static struct zmk_watchdog_incident_record capture_record;

bool zmk_watchdog_capture_begin(void) { return atomic_cas(&capturing, 0, 1); }

struct zmk_watchdog_incident_record *zmk_watchdog_capture_begin_record(void) {
    return zmk_watchdog_capture_begin() ? &capture_record : NULL;
}

void zmk_watchdog_capture_end(void) { atomic_clear(&capturing); }
