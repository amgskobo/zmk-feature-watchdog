/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Central half of the watchdog split relay: watchdog_relay_dispatch_request()
 * relays a request with a nonzero source ("wdq") and returns a
 * DeferredResponse. Each relayed answer ("wdp") becomes a PeripheralResponse
 * Studio notification; a streamed page is reassembled first
 * (watchdog_relay_page.c) and notified once complete.
 */

#include <errno.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
/* ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL calls
 * zmk_split_central_send_relay_event(), which event_manager.h does not declare.
 */
#include <zmk/split/central.h>
#include <zmk/workqueue.h>

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog_relay.h>
#include <cormoran/zmk/watchdog_relay_page.h>
#include <cormoran/zmk/watchdog_request_exec.h>
#include <cormoran/zmk/watchdog_studio.h>

#include "watchdog_relay_events.h"

#include <zephyr/logging/log.h>
#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_relay, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_watchdog_relay_request);
ZMK_EVENT_IMPL(zmk_watchdog_relay_response);

/* Requests are encoded below so transport failure reaches the RPC caller. */
ZMK_RELAY_EVENT_HANDLE(zmk_watchdog_relay_response, wdp, source);

static atomic_t next_relay_request_id = 1;

/* Preserve the existing wdq event envelope, with a nonzero correlation ID.
 * Send directly through the selected core transport API: the event macro
 * discards its return value and would acknowledge a failed send as deferred. */
static int send_relay_request(const cormoran_watchdog_Request *req, uint32_t *out_id) {
    uint32_t request_id = (uint32_t)atomic_inc(&next_relay_request_id);
    if (request_id == 0) {
        request_id = (uint32_t)atomic_inc(&next_relay_request_id);
    }

    cormoran_watchdog_RelayRequest relay_req = cormoran_watchdog_RelayRequest_init_zero;
    relay_req.request_id = request_id;
    relay_req.has_request = true;
    relay_req.request = *req;

    struct zmk_watchdog_relay_request event;
    memset(&event, 0, sizeof(event));
    event.source = ZMK_RELAY_EVENT_SOURCE_SELF;
    pb_ostream_t ostream = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to encode watchdog relay request: %s", PB_GET_ERROR(&ostream));
        return -EINVAL;
    }
    event.size = (uint16_t)ostream.bytes_written;

    struct zmk_split_relay_event_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.header.event_data_size = sizeof(event);
    payload.header.event_type_size = 3;
    memcpy(payload.event_type, "wdq", 4);
    memcpy(payload.event_data, &event, sizeof(event));
    *out_id = request_id;
    return zmk_split_central_send_relay_event(&payload);
}

void watchdog_relay_dispatch_request(const cormoran_watchdog_Request *req,
                                     cormoran_watchdog_Response *resp) {
    /* The selected core only exposes broadcast relay. A peripheral cannot
     * identify its central-side slot, and the local executor ignores source.
     * Never broadcast a targeted deletion to an unrelated peer, or acknowledge
     * a nonexistent target. Multiple peers also interleave the page assembler.
     * Local requests remain independent of this transport limitation. */
    if (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT != 1) {
        watchdog_response_set_error(resp, "Watchdog relay requires one configured peripheral");
        return;
    }
    if (watchdog_request_source(req) != 1) {
        watchdog_response_set_error(resp, "Invalid watchdog peripheral source");
        return;
    }
    uint32_t request_id;
    int ret = send_relay_request(req, &request_id);
    if (ret < 0) {
        watchdog_response_set_error(resp, "Failed to send watchdog relay request");
        return;
    }

    cormoran_watchdog_DeferredResponse deferred = cormoran_watchdog_DeferredResponse_init_zero;
    deferred.request_id = request_id;
    resp->which_response_type = cormoran_watchdog_Response_deferred_tag;
    resp->response_type.deferred = deferred;
}

static K_MUTEX_DEFINE(peripheral_response_notification_lock);
static struct watchdog_studio_notification peripheral_response_notification;

/* Wraps `resp` in PeripheralResponse{source, request_id, response} and
 * notifies it, under the same lock rule as every watchdog notification. */
static void raise_peripheral_response_notification(uint32_t source, uint32_t request_id,
                                                   const cormoran_watchdog_Response *resp) {
    k_mutex_lock(&peripheral_response_notification_lock, K_FOREVER);

    cormoran_watchdog_Notification *notification = &peripheral_response_notification.payload;
    *notification = (cormoran_watchdog_Notification)cormoran_watchdog_Notification_init_zero;
    notification->which_notification_type = cormoran_watchdog_Notification_peripheral_response_tag;
    cormoran_watchdog_PeripheralResponse *pr = &notification->notification_type.peripheral_response;
    pr->source = source;
    pr->request_id = request_id;
    pr->has_response = true;
    pr->response = *resp;

    int ret = watchdog_studio_notify(&peripheral_response_notification);
    if (ret < 0 && ret != -EACCES) {
        LOG_WRN("Failed to raise watchdog PeripheralResponse notification: %d", ret);
    }

    k_mutex_unlock(&peripheral_response_notification_lock);
}

static struct watchdog_relay_page chunk_accumulator;

static void handle_incident_chunk(uint32_t source, uint32_t request_id,
                                  const cormoran_watchdog_RelayIncidentChunk *chunk) {
    /* Only the serialized relay worker calls this; keep the full page off its
     * stack. */
    static cormoran_watchdog_Response response;
    if (watchdog_relay_page_core(&chunk_accumulator, source, request_id, chunk, &response)) {
        raise_peripheral_response_notification(source, request_id, &response);
    }
}

static void handle_diagnostic_chunk(uint32_t source, uint32_t request_id,
                                    const cormoran_watchdog_RelayDiagnosticChunk *chunk) {
    watchdog_relay_page_diagnostic(&chunk_accumulator, source, request_id, chunk);
}

/* Decoding and notification run on the low-priority queue, not on the system
 * work queue that raises the relayed event, so each payload is copied out of
 * the listener. A page is up to 21 datagrams (four cores, four diagnostics,
 * twelve common-context fragments and done); a dropped one fails the page
 * closed through its completion and extension checks, or the client's timeout.
 */
K_MSGQ_DEFINE(watchdog_relay_response_queue, sizeof(struct zmk_watchdog_relay_response),
              WATCHDOG_RELAY_PAGE_MAX_PACKETS, 4);

static void watchdog_relay_response_process_work_handler(struct k_work *work) {
    struct zmk_watchdog_relay_response watchdog_relay_pending_response;
    while (k_msgq_get(&watchdog_relay_response_queue, &watchdog_relay_pending_response,
                      K_NO_WAIT) == 0) {
        /* The size comes from the other half: never decode past the payload. */
        if (watchdog_relay_pending_response.size >
            sizeof(watchdog_relay_pending_response.payload)) {
            LOG_WRN("Watchdog relay response size %u exceeds its payload",
                    watchdog_relay_pending_response.size);
            continue;
        }
        cormoran_watchdog_RelayResponse relay_resp = cormoran_watchdog_RelayResponse_init_zero;
        pb_istream_t istream = pb_istream_from_buffer(watchdog_relay_pending_response.payload,
                                                      watchdog_relay_pending_response.size);
        if (!pb_decode(&istream, cormoran_watchdog_RelayResponse_fields, &relay_resp)) {
            LOG_WRN("Failed to decode watchdog relay response: %s", PB_GET_ERROR(&istream));
            continue;
        }

        /* ZMK_RELAY_EVENT_HANDLE rewrote the source to the peripheral's slot + 1,
         * this module's `source` convention (0 is the central itself). */
        uint32_t source = watchdog_relay_pending_response.source;

        switch (relay_resp.which_relay_response_type) {
        case cormoran_watchdog_RelayResponse_context_chunk_tag:
            watchdog_relay_page_context(&chunk_accumulator, source, relay_resp.request_id,
                                        &relay_resp.relay_response_type.context_chunk);
            break;
        case cormoran_watchdog_RelayResponse_diagnostic_chunk_tag:
            handle_diagnostic_chunk(source, relay_resp.request_id,
                                    &relay_resp.relay_response_type.diagnostic_chunk);
            break;
        case cormoran_watchdog_RelayResponse_incident_chunk_tag:
            handle_incident_chunk(source, relay_resp.request_id,
                                  &relay_resp.relay_response_type.incident_chunk);
            break;
        case cormoran_watchdog_RelayResponse_response_tag: {
            static cormoran_watchdog_Response resp;
            resp = (cormoran_watchdog_Response)cormoran_watchdog_Response_init_zero;
            const cormoran_watchdog_RelayNonListResponse *rnl =
                &relay_resp.relay_response_type.response;
            switch (rnl->which_relay_non_list_response_type) {
            case cormoran_watchdog_RelayNonListResponse_status_tag:
                resp.which_response_type = cormoran_watchdog_Response_status_tag;
                resp.response_type.status = rnl->relay_non_list_response_type.status;
                break;
            case cormoran_watchdog_RelayNonListResponse_delete_result_tag:
                resp.which_response_type = cormoran_watchdog_Response_delete_result_tag;
                resp.response_type.delete_result = rnl->relay_non_list_response_type.delete_result;
                break;
            case cormoran_watchdog_RelayNonListResponse_inject_ack_tag:
                resp.which_response_type = cormoran_watchdog_Response_inject_ack_tag;
                resp.response_type.inject_ack = rnl->relay_non_list_response_type.inject_ack;
                break;
            case cormoran_watchdog_RelayNonListResponse_error_tag:
            default:
                resp.which_response_type = cormoran_watchdog_Response_error_tag;
                resp.response_type.error = rnl->relay_non_list_response_type.error;
                break;
            }
            raise_peripheral_response_notification(source, relay_resp.request_id, &resp);
            break;
        }
        default:
            LOG_WRN("Watchdog relay response with no relay_response_type set");
            break;
        }
    }
}

K_WORK_DEFINE(watchdog_relay_response_process_work, watchdog_relay_response_process_work_handler);

static int on_watchdog_relay_response(const zmk_event_t *eh) {
    const struct zmk_watchdog_relay_response *ev = as_zmk_watchdog_relay_response(eh);
    if (!ev) {
        return 0;
    }

    if (k_msgq_put(&watchdog_relay_response_queue, ev, K_NO_WAIT) < 0) {
        LOG_WRN("Watchdog relay response FIFO full; page must be retried");
        return 0;
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &watchdog_relay_response_process_work);
    return 0;
}

ZMK_LISTENER(watchdog_relay_response_notify, on_watchdog_relay_response);
ZMK_SUBSCRIPTION(watchdog_relay_response_notify, zmk_watchdog_relay_response);

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST)
#include "../../tests/support/watchdog_relay_assertions_1.inc"
#endif
