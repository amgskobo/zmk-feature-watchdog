#pragma once

/**
 * @file watchdog_relay.h
 *
 * @brief Central entry point of the watchdog split relay
 * (CONFIG_ZMK_WATCHDOG_SPLIT_RELAY, src/split/watchdog_relay_central.c). The
 * peripheral responder needs no public entry point.
 */

#include <cormoran/watchdog/watchdog.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Relays `req`, a GetStatus/ListIncidents/DeleteIncidents request
 * with source 1 to a single configured peripheral and fills `resp` with a
 * DeferredResponse after transport acceptance. Other sources, multiple-peer
 * configurations and encode/send failures produce an ErrorResponse before
 * acknowledging a request. The selected core API broadcasts, so targeted
 * execution cannot be guaranteed with multiple peers. The answer arrives
 * later as a PeripheralResponse Studio notification for the nonzero ID. */
void watchdog_relay_dispatch_request(const cormoran_watchdog_Request *req,
                                     cormoran_watchdog_Response *resp);

#ifdef __cplusplus
}
#endif
