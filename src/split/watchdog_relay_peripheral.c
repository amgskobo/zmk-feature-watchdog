/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Peripheral half of the watchdog split relay: executes a relayed
 * RelayRequest ("wdq") against this half's store and relays the answer back
 * ("wdp"). A ListIncidents page goes out as one incident_chunk per incident,
 * a diagnostic_chunk after each incident that has diagnostics, and a final
 * done chunk with the expected count. Every other answer is a single
 * RelayResponse.response.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/workqueue.h>

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <cormoran/zmk/watchdog_request_exec.h>
#include <cormoran/zmk/watchdog_relay_page.h>

#include "watchdog_relay_events.h"

#include <zephyr/logging/log.h>
#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_relay, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_watchdog_relay_request);
ZMK_EVENT_IMPL(zmk_watchdog_relay_response);

/* Requests come only from the central; answers go only to it. */
ZMK_RELAY_EVENT_HANDLE(zmk_watchdog_relay_request, wdq, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_watchdog_relay_response, wdp, source);

static void incident_to_core(const cormoran_watchdog_Incident *in,
                             cormoran_watchdog_RelayIncidentCore *out) {
    *out = (cormoran_watchdog_RelayIncidentCore)cormoran_watchdog_RelayIncidentCore_init_zero;
    out->id = in->id;
    out->source = in->source;
    out->type = in->type;
    out->boot_ordinal = in->boot_ordinal;
    out->uptime_s = in->uptime_s;
    out->capture_flags = in->capture_flags;
    out->which_detail = in->which_detail;
    switch (in->which_detail) {
    case cormoran_watchdog_Incident_freeze_tag:
        out->detail.freeze = in->detail.freeze;
        break;
    case cormoran_watchdog_Incident_fatal_tag:
        out->detail.fatal = in->detail.fatal;
        break;
    case cormoran_watchdog_Incident_reset_tag:
        out->detail.reset = in->detail.reset;
        break;
    default:
        break;
    }
}

/* Decodes `payload` and executes it against the local store. A request that
 * decodes but is missing or unsupported still produces an ErrorResponse;
 * -EINVAL means the RelayRequest itself was undecodable, so there is nothing
 * to answer. */
static int watchdog_relay_exec_request(const uint8_t *payload, size_t size,
                                       uint32_t *out_request_id,
                                       cormoran_watchdog_Response *out_resp) {
    /* The size comes from the other half: never decode past the payload. */
    if (size > WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE) {
        LOG_WRN("Watchdog relay request size %u exceeds its payload", (unsigned int)size);
        return -EINVAL;
    }
    cormoran_watchdog_RelayRequest relay_req = cormoran_watchdog_RelayRequest_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(payload, size);
    if (!pb_decode(&istream, cormoran_watchdog_RelayRequest_fields, &relay_req)) {
        LOG_WRN("Failed to decode watchdog relay request: %s", PB_GET_ERROR(&istream));
        return -EINVAL;
    }

    *out_request_id = relay_req.request_id;
    *out_resp = (cormoran_watchdog_Response)cormoran_watchdog_Response_init_zero;
    if (!relay_req.has_request) {
        watchdog_response_set_error(out_resp, "missing relayed watchdog request");
    } else {
        watchdog_request_exec_handle(&relay_req.request, out_resp);
    }
    return 0;
}

/* Copies a non-page Response into the matching RelayNonListResponse variant
 * (field numbers and meaning are 1:1). A page never comes here; it is
 * streamed instead. */
static void response_to_relay_non_list_response(const cormoran_watchdog_Response *resp,
                                                cormoran_watchdog_RelayNonListResponse *out) {
    *out = (cormoran_watchdog_RelayNonListResponse)cormoran_watchdog_RelayNonListResponse_init_zero;

    switch (resp->which_response_type) {
    case cormoran_watchdog_Response_status_tag:
        out->which_relay_non_list_response_type = cormoran_watchdog_RelayNonListResponse_status_tag;
        out->relay_non_list_response_type.status = resp->response_type.status;
        break;
    case cormoran_watchdog_Response_delete_result_tag:
        out->which_relay_non_list_response_type =
            cormoran_watchdog_RelayNonListResponse_delete_result_tag;
        out->relay_non_list_response_type.delete_result = resp->response_type.delete_result;
        break;
    case cormoran_watchdog_Response_inject_ack_tag:
        out->which_relay_non_list_response_type =
            cormoran_watchdog_RelayNonListResponse_inject_ack_tag;
        out->relay_non_list_response_type.inject_ack = resp->response_type.inject_ack;
        break;
    case cormoran_watchdog_Response_incident_page_tag:
        LOG_WRN("Unexpected incident_page routed through the non-list relay response path");
        __fallthrough;
    case cormoran_watchdog_Response_error_tag:
    default:
        out->which_relay_non_list_response_type = cormoran_watchdog_RelayNonListResponse_error_tag;
        if (resp->which_response_type == cormoran_watchdog_Response_error_tag) {
            out->relay_non_list_response_type.error = resp->response_type.error;
        } else {
            snprintf(out->relay_non_list_response_type.error.message,
                     sizeof(out->relay_non_list_response_type.error.message),
                     "unsupported response kind for split relay: %d", resp->which_response_type);
        }
        break;
    }
}

/* Builds chunk `chunk_index` of `page`: indices below incidents_count carry
 * one incident each, and index incidents_count is the final done chunk.
 * Returns the number of chunks, incidents_count + 1. */
static uint32_t build_incident_chunk_response(uint32_t request_id,
                                              const cormoran_watchdog_IncidentPageResponse *page,
                                              uint32_t chunk_index,
                                              cormoran_watchdog_RelayResponse *out) {
    *out = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
    out->request_id = request_id;
    out->which_relay_response_type = cormoran_watchdog_RelayResponse_incident_chunk_tag;
    cormoran_watchdog_RelayIncidentChunk *chunk = &out->relay_response_type.incident_chunk;
    chunk->total = page->total;
    chunk->start_index = page->start_index;

    if (chunk_index < page->incidents_count) {
        chunk->done = false;
        chunk->has_incident = true;
        incident_to_core(&page->incidents[chunk_index], &chunk->incident);
        chunk->has_page_index = true;
        chunk->page_index = (uint8_t)chunk_index;
        chunk->diagnostics_expected = page->incidents[chunk_index].diagnostics.size != 0;
    } else {
        chunk->done = true;
        chunk->has_incident = false;
        chunk->has_expected_count = true;
        chunk->expected_count = (uint8_t)page->incidents_count;
    }

    return page->incidents_count + 1;
}

/* Encodes `relay_resp` and raises it as a "wdp" relay event. */
static int raise_relay_response(const cormoran_watchdog_RelayResponse *relay_resp) {
    struct zmk_watchdog_relay_response resp_event = {.source = ZMK_RELAY_EVENT_SOURCE_SELF};
    pb_ostream_t ostream = pb_ostream_from_buffer(resp_event.payload, sizeof(resp_event.payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayResponse_fields, relay_resp)) {
        LOG_WRN("Failed to encode watchdog relay response: %s", PB_GET_ERROR(&ostream));
        return -EINVAL;
    }
    resp_event.size = (uint16_t)ostream.bytes_written;

    zmk_watchdog_breadcrumb(ZMK_WDG_RELAY_SEND_BEGIN, 255, resp_event.size);
    raise_zmk_watchdog_relay_response(resp_event);
    zmk_watchdog_breadcrumb(ZMK_WDG_RELAY_SEND_END, 255, resp_event.size);
    return 0;
}

static void stream_incident_page(uint32_t request_id,
                                 const cormoran_watchdog_IncidentPageResponse *page) {
    for (uint32_t i = 0; i <= page->incidents_count; ++i) {
        cormoran_watchdog_RelayResponse relay_resp;
        build_incident_chunk_response(request_id, page, i, &relay_resp);
        if (raise_relay_response(&relay_resp) < 0) {
            return;
        }
        if (i < page->incidents_count && page->incidents[i].diagnostics.size) {
            relay_resp = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
            relay_resp.request_id = request_id;
            relay_resp.which_relay_response_type =
                cormoran_watchdog_RelayResponse_diagnostic_chunk_tag;
            cormoran_watchdog_RelayDiagnosticChunk *chunk =
                &relay_resp.relay_response_type.diagnostic_chunk;
            chunk->page_index = i;
            chunk->incident_id = page->incidents[i].id;
            chunk->diagnostics.size = page->incidents[i].diagnostics.size;
            memcpy(chunk->diagnostics.bytes, page->incidents[i].diagnostics.bytes,
                   chunk->diagnostics.size);
            if (raise_relay_response(&relay_resp) < 0) {
                return;
            }
        }
        if (i < page->incidents_count && page->incidents[i].capture_context.size) {
            for (uint32_t part = 0; part < WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT; ++part) {
                relay_resp =
                    (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
                relay_resp.request_id = request_id;
                relay_resp.which_relay_response_type =
                    cormoran_watchdog_RelayResponse_context_chunk_tag;
                cormoran_watchdog_RelayContextChunk *context =
                    &relay_resp.relay_response_type.context_chunk;
                context->page_index = i;
                context->incident_id = page->incidents[i].id;
                context->fragment_index = part;
                context->data.size = WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE;
                memcpy(context->data.bytes,
                       page->incidents[i].capture_context.bytes +
                           part * WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE,
                       WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE);
                if (raise_relay_response(&relay_resp) < 0) {
                    return;
                }
            }
        }
    }
}

/* Execution (store reads, protobuf encode) runs on the low-priority queue,
 * not on the system work queue that raises the relayed event, so the payload
 * is copied out of the listener. */
/* k_work submissions coalesce; keep each admitted request's bytes in a FIFO. */
#define WATCHDOG_RELAY_PENDING_REQUESTS 4
K_MSGQ_DEFINE(watchdog_relay_requests, sizeof(struct zmk_watchdog_relay_request),
              WATCHDOG_RELAY_PENDING_REQUESTS, 4);

static void watchdog_relay_process_work_handler(struct k_work *work) {
    /* Only the worker uses this copy while execution/transport can block. */
    static struct zmk_watchdog_relay_request request;
    /* Static, not on the 2 KiB queue stack: the response stays alive through
     * the synchronous event and BLE dispatch below. Only this serialized work
     * item uses it, and the executor resets it for every request. */
    static cormoran_watchdog_Response resp;
    if (k_msgq_get(&watchdog_relay_requests, &request, K_NO_WAIT) != 0) {
        return;
    }
    uint32_t request_id;
    zmk_watchdog_breadcrumb(ZMK_WDG_RELAY_EXEC_BEGIN, 255, request.size);
    LOG_INF("relay exec begin bytes=%u", request.size);
    int ret = watchdog_relay_exec_request(request.payload, request.size, &request_id, &resp);
    if (ret < 0) {
        LOG_WRN("relay exec decode err=%d", ret);
        goto next_request;
    }
    zmk_watchdog_breadcrumb(ZMK_WDG_RELAY_EXEC_END, 255, resp.which_response_type);
    LOG_INF("relay exec done id=%u kind=%u", request_id, resp.which_response_type);

    if (resp.which_response_type == cormoran_watchdog_Response_incident_page_tag) {
        stream_incident_page(request_id, &resp.response_type.incident_page);
        zmk_watchdog_breadcrumb(ZMK_WDG_RELAY_WORK_END, 255, resp.which_response_type);
        LOG_INF("relay page send returned id=%u", request_id);
        goto next_request;
    }

    cormoran_watchdog_RelayResponse relay_resp =
        (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
    relay_resp.request_id = request_id;
    relay_resp.which_relay_response_type = cormoran_watchdog_RelayResponse_response_tag;
    response_to_relay_non_list_response(&resp, &relay_resp.relay_response_type.response);

    raise_relay_response(&relay_resp);
    zmk_watchdog_breadcrumb(ZMK_WDG_RELAY_WORK_END, 255, resp.which_response_type);
    LOG_INF("relay response send returned id=%u", request_id);

next_request:
    /* One request per invocation lets already-queued watchdog feed work run.
     * Zephyr permits submitting a running item: it queues another invocation
     * on this same queue, without reentering the handler. Submissions coalesce.
     * If admission races this empty check, the listener submits its own wakeup;
     * otherwise our submission retains all requests already in the FIFO. */
    if (k_msgq_num_used_get(&watchdog_relay_requests) != 0) {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), work);
    }
}

K_WORK_DEFINE(watchdog_relay_process_work, watchdog_relay_process_work_handler);

/* Queue refusal must remain correlated with the central's Deferred ID. Decode
 * only that bounded envelope; no store access or large response is needed. */
static void watchdog_relay_refuse_full(const struct zmk_watchdog_relay_request *request) {
    if (request->size > sizeof(request->payload))
        return;
    pb_istream_t input = pb_istream_from_buffer(request->payload, request->size);
    uint32_t request_id = 0;
    while (input.bytes_left) {
        pb_wire_type_t wire;
        uint32_t tag;
        bool eof;
        if (!pb_decode_tag(&input, &wire, &tag, &eof))
            return;
        if (tag == cormoran_watchdog_RelayRequest_request_id_tag) {
            if (wire != PB_WT_VARINT || !pb_decode_varint32(&input, &request_id))
                return;
        } else if (!pb_skip_field(&input, wire)) {
            return;
        }
    }
    if (request_id == 0)
        return;
    cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
    response.request_id = request_id;
    response.which_relay_response_type = cormoran_watchdog_RelayResponse_response_tag;
    cormoran_watchdog_RelayNonListResponse *result = &response.relay_response_type.response;
    result->which_relay_non_list_response_type = cormoran_watchdog_RelayNonListResponse_error_tag;
    snprintf(result->relay_non_list_response_type.error.message,
             sizeof(result->relay_non_list_response_type.error.message),
             "Watchdog relay busy; retry the request");
    (void)raise_relay_response(&response);
}

static int on_watchdog_relay_request(const zmk_event_t *eh) {
    const struct zmk_watchdog_relay_request *ev = as_zmk_watchdog_relay_request(eh);
    if (!ev) {
        return 0;
    }

    if (k_msgq_put(&watchdog_relay_requests, ev, K_NO_WAIT) != 0) {
        watchdog_relay_refuse_full(ev);
        return 0;
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &watchdog_relay_process_work);
    return 0;
}

ZMK_LISTENER(watchdog_relay_request_exec, on_watchdog_relay_request);
ZMK_SUBSCRIPTION(watchdog_relay_request_exec, zmk_watchdog_relay_request);

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY_TEST)
#include "../../tests/support/watchdog_relay_assertions_0.inc"
#endif
