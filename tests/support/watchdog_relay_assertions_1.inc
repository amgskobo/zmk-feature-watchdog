/* SPDX-License-Identifier: MIT */
#include "init_result.h"
#include <zephyr/init.h>

/* Central-side counterpart: asserts watchdog_relay_dispatch_request()
 * assigns distinct, nonzero request_ids after transport acceptance, or
 * reports an unavailable transport explicitly. Only reachable from a
 * build test (module_watchdog_split_central in tests/zmk-config/build.yaml),
 * not a native_sim test: raise_zmk_watchdog_relay_request() needs
 * zmk_split_central_send_relay_event(), only implemented by the real BLE
 * split transport (CONFIG_ZMK_SPLIT_BLE) -- native_sim would need a working
 * BT stack to link it. This only proves the central dispatch test code
 * compiles/links and is logically correct in isolation; it does not (and
 * cannot, without a real peripheral) assert that a relay actually reaches
 * anyone -- that is exactly what native_sim cannot simulate.
 *
 * resp1/resp2 are `static`, not stack locals: two 688-byte responses on top
 * of the dispatch's own locals once overflowed a boot-time stack on real
 * hardware (nRF52840), and a stack guard fault there silently stops the
 * rest of boot, USB enumeration included. */
static int watchdog_split_relay_central_test_init(void) {
    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_get_status_tag;
    req.request_type.get_status.source = 1;

    static cormoran_watchdog_Response resp1;
    watchdog_relay_dispatch_request(&req, &resp1);
    /* Startup need not have a connected transport. The executable central
     * fixture covers positive IDs, rejected targets and transport errors. */
    if (resp1.which_response_type == cormoran_watchdog_Response_error_tag &&
        strcmp(resp1.response_type.error.message, "Failed to send watchdog relay request") == 0) {
        printk("PASS: watchdog_split_relay_central_unavailable\n");
        return 0;
    }
    if (resp1.which_response_type != cormoran_watchdog_Response_deferred_tag ||
        resp1.response_type.deferred.request_id == 0) {
        LOG_ERR("Split relay central test: expected a nonzero DeferredResponse, "
                "got type=%d "
                "request_id=%u",
                resp1.which_response_type, resp1.response_type.deferred.request_id);
        return -EINVAL;
    }

    static cormoran_watchdog_Response resp2;
    watchdog_relay_dispatch_request(&req, &resp2);
    if (resp2.which_response_type != cormoran_watchdog_Response_deferred_tag ||
        resp2.response_type.deferred.request_id == 0 ||
        resp2.response_type.deferred.request_id == resp1.response_type.deferred.request_id) {
        LOG_ERR("Split relay central test: request_ids did not increment (%u, %u)",
                resp1.response_type.deferred.request_id, resp2.response_type.deferred.request_id);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_central_dispatch\n");
    return 0;
}

OWNER_CHECKED_SYSWORKQ(watchdog_split_relay_central_test_init, APPLICATION,
                       CONFIG_APPLICATION_INIT_PRIORITY);
