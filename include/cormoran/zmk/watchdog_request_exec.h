#pragma once

/**
 * @file watchdog_request_exec.h
 *
 * @brief Transport-independent execution of watchdog Request messages against
 * this half's own incident store. Shared by the local Studio RPC handler
 * (source 0) and the split relay peripheral responder, which answers a
 * relayed request against its own store.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Source routing shared by Studio and the relay. Injection is always local. */
static inline uint32_t watchdog_request_source(const cormoran_watchdog_Request *req) {
    switch (req->which_request_type) {
    case cormoran_watchdog_Request_get_status_tag:
        return req->request_type.get_status.source;
    case cormoran_watchdog_Request_list_incidents_tag:
        return req->request_type.list_incidents.source;
    case cormoran_watchdog_Request_delete_incidents_tag:
        return req->request_type.delete_incidents.source;
    default:
        return 0;
    }
}

/** @brief Converts an incident record and its store id into the wire Incident.
 * The caller stamps `source`: 0 for this half, the peripheral slot for a
 * relayed answer. */
void watchdog_incident_record_to_proto(uint16_t id, uint32_t source,
                                       const struct zmk_watchdog_incident_record *rec,
                                       cormoran_watchdog_Incident *out);

/** @brief Replaces `resp` with an ErrorResponse carrying `message`. */
void watchdog_response_set_error(cormoran_watchdog_Response *resp, const char *message);

/** @brief Executes `req` against this half's store and always fills `resp`:
 * a Status/IncidentPage/DeleteResult/InjectAck, or an ErrorResponse for an
 * unset or unsupported request kind. The request's `source` is ignored; the
 * caller routes a nonzero source to the relay instead. InjectTest is honored
 * only with CONFIG_ZMK_WATCHDOG_TEST_INJECTION and always targets this half. */
void watchdog_request_exec_handle(const cormoran_watchdog_Request *req,
                                  cormoran_watchdog_Response *resp);

#ifdef __cplusplus
}
#endif
