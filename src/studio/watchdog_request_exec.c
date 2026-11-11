/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <cormoran/zmk/watchdog_request_exec.h>

/* One page is as many incidents as IncidentPageResponse holds (watchdog.options). */
#define WATCHDOG_RPC_PAGE_SIZE                                                                     \
    ARRAY_SIZE(((cormoran_watchdog_IncidentPageResponse *)NULL)->incidents)

void watchdog_incident_record_to_proto(uint16_t id, uint32_t source,
                                       const struct zmk_watchdog_incident_record *rec,
                                       cormoran_watchdog_Incident *out) {
    *out = (cormoran_watchdog_Incident)cormoran_watchdog_Incident_init_zero;

    out->id = id;
    out->source = source;
    out->boot_ordinal = rec->boot_ordinal;
    out->uptime_s = rec->uptime_s;
    out->capture_flags =
        rec->version >= 3
            ? rec->flags & (ZMK_WATCHDOG_CAPTURE_INCOMPLETE | ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID)
            : 0;
    if (rec->version >= 4 && (rec->flags & ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID)) {
        out->capture_context.size = ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE;
        memcpy(out->capture_context.bytes, rec->capture_context, ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE);
    } else {
        out->capture_flags &= ~ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
    }

    if (rec->version >= 3 && rec->diagnostics[0]) {
        out->diagnostics.size = ZMK_WATCHDOG_DIAGNOSTIC_SIZE;
        memcpy(out->diagnostics.bytes, rec->diagnostics, ZMK_WATCHDOG_DIAGNOSTIC_SIZE);
    }
    switch (rec->type) {
    case ZMK_WATCHDOG_INCIDENT_FREEZE:
        out->type = cormoran_watchdog_IncidentType_FREEZE;
        out->which_detail = cormoran_watchdog_Incident_freeze_tag;
        out->detail.freeze.channel_id = rec->detail.freeze.channel_id;
        snprintf(out->detail.freeze.queue_name, sizeof(out->detail.freeze.queue_name), "%.*s",
                 (int)sizeof(rec->detail.freeze.queue_name), rec->detail.freeze.queue_name);
        if (rec->version >= 2 && rec->detail.freeze.trace_flags != 0) {
            const struct zmk_watchdog_incident_freeze_detail *d = &rec->detail.freeze;
            uint8_t *trace = out->detail.freeze.trace.bytes;
            out->detail.freeze.trace.size = ZMK_WATCHDOG_TRACE_WIRE_SIZE;
            trace[0] = 1;
            trace[1] = d->thread_state;
            trace[2] = d->trace_flags;
            trace[3] = MIN(d->trace_count, ZMK_WATCHDOG_TRACE_ADDRS);
            sys_put_le32(d->wait_queue, trace + 4);
            sys_put_le32(d->stack_pointer, trace + 8);
            for (size_t i = 0; i < ZMK_WATCHDOG_TRACE_ADDRS; ++i) {
                sys_put_le32(i < trace[3] ? d->addresses[i] : 0, trace + 12 + 4 * i);
            }
        }
        break;
    case ZMK_WATCHDOG_INCIDENT_FATAL:
        out->type = cormoran_watchdog_IncidentType_FATAL;
        out->which_detail = cormoran_watchdog_Incident_fatal_tag;
        out->detail.fatal.reason = rec->detail.fatal.reason;
        out->detail.fatal.pc = rec->detail.fatal.pc;
        out->detail.fatal.lr = rec->detail.fatal.lr;
        if (rec->version >= 3 && (rec->flags & ZMK_WATCHDOG_FATAL_CONTEXT_VALID)) {
            out->detail.fatal.context.size = 24;
            for (size_t i = 0; i < 6; ++i) {
                sys_put_le32(rec->detail.fatal.context[i], out->detail.fatal.context.bytes + 4 * i);
            }
        }
        snprintf(out->detail.fatal.thread_name, sizeof(out->detail.fatal.thread_name), "%.*s",
                 (int)sizeof(rec->detail.fatal.thread_name), rec->detail.fatal.thread_name);
        break;
    case ZMK_WATCHDOG_INCIDENT_RESET_CAUSE:
    default:
        out->type = cormoran_watchdog_IncidentType_RESET_CAUSE;
        out->which_detail = cormoran_watchdog_Incident_reset_tag;
        out->detail.reset.cause_bits = rec->detail.reset.cause_bits;
        break;
    }
}

void watchdog_response_set_error(cormoran_watchdog_Response *resp, const char *message) {
    cormoran_watchdog_ErrorResponse err = cormoran_watchdog_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_watchdog_Response_error_tag;
    resp->response_type.error = err;
}

static void handle_get_status(cormoran_watchdog_Response *resp) {
    cormoran_watchdog_StatusResponse status = cormoran_watchdog_StatusResponse_init_zero;
    struct zmk_watchdog_store_status stored;
    zmk_watchdog_store_get_status(&stored);
    status.capacity = stored.capacity;
    status.stored = stored.stored;
    status.dropped_since_boot = stored.dropped_since_boot;
    status.recording_stopped = stored.recording_stopped;
    status.build_tag = zmk_watchdog_diagnostics_build_tag();
    status.runtime_flags = zmk_watchdog_diagnostics_runtime_flags();
    status.reset_cause = zmk_watchdog_diagnostics_reset_cause();
    if (zmk_watchdog_diagnostics_previous(status.previous_diagnostics.bytes)) {
        status.previous_diagnostics.size = ZMK_WATCHDOG_DIAGNOSTIC_SIZE;
    }

    resp->which_response_type = cormoran_watchdog_Response_status_tag;
    resp->response_type.status = status;
}

static void handle_list_incidents(const cormoran_watchdog_ListIncidentsRequest *req,
                                  cormoran_watchdog_Response *resp) {
    /* The caller already owns the full response. A second page on this stack
     * leaves almost no exception-frame headroom on the peripheral's 2 KiB
     * low-priority queue. Build directly in the caller's response storage. */
    cormoran_watchdog_IncidentPageResponse *page = &resp->response_type.incident_page;
    memset(page, 0, sizeof(*page));

    uint16_t total = zmk_watchdog_store_count();
    uint32_t start = req->start_index;

    page->total = total;
    page->start_index = start;

    for (uint32_t i = start; i < (uint32_t)total && page->incidents_count < WATCHDOG_RPC_PAGE_SIZE;
         i++) {
        struct zmk_watchdog_incident_record rec;
        uint16_t id;
        int ret = zmk_watchdog_store_get_with_id((uint16_t)i, &rec, &id);
        if (ret == -ENOENT) {
            /* Store mutated concurrently (delete) between count() and
             * get_with_id() -- stop the page here rather than erroring the
             * whole request; the client will see a shorter page than
             * `total` implied and can re-request if it cares. */
            break;
        }
        if (ret < 0) {
            watchdog_response_set_error(resp, "Failed to read incident history");
            return;
        }

        /* Source 0: this always reads this half's own store. The central
         * stamps a relayed page with the peripheral's source on arrival. */
        watchdog_incident_record_to_proto(id, 0, &rec, &page->incidents[page->incidents_count]);
        page->incidents_count++;
    }

    resp->which_response_type = cormoran_watchdog_Response_incident_page_tag;
}

static void handle_delete_incidents(const cormoran_watchdog_DeleteIncidentsRequest *req,
                                    cormoran_watchdog_Response *resp) {
    uint32_t deleted = 0;

    if (req->all) {
        uint16_t count_before = zmk_watchdog_store_count();
        int ret = zmk_watchdog_store_delete_all();
        if (ret < 0) {
            watchdog_response_set_error(resp, "Failed to delete all incidents");
            return;
        }
        deleted = count_before;
    } else {
        for (size_t i = 0; i < req->ids_count; i++) {
            uint32_t id = req->ids[i];
            if (id > UINT16_MAX) {
                continue;
            }
            int ret = zmk_watchdog_store_delete((uint16_t)id);
            if (ret == 0) {
                deleted++;
            }
        }
    }

    cormoran_watchdog_DeleteResultResponse result =
        cormoran_watchdog_DeleteResultResponse_init_zero;
    result.deleted = deleted;

    resp->which_response_type = cormoran_watchdog_Response_delete_result_tag;
    resp->response_type.delete_result = result;
}

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
/*
 * Dangerous by design -- only compiled in when CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y
 * (default n). See DESIGN.md, Studio RPC, split relay and client and src/watchdog_inject.c.
 *
 *  - FREEZE_KIND: zmk_watchdog_inject_freeze() only submits a k_work to the
 *    system workqueue and returns immediately -- the actual freeze (and the
 *    eventual reboot once CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS elapses)
 *    happens asynchronously, well after this function returns and the
 *    InjectAckResponse below has been handed back to the RPC transport. Safe
 *    to call from the RPC handler's own thread context.
 *  - FATAL_KIND: zmk_watchdog_inject_fatal() calls k_oops() synchronously, in
 *    this same call stack -- it does not return, and the device reboots
 *    almost immediately. The InjectAckResponse this function fills in is
 *    typically never actually encoded/sent: the fatal-error handler's own
 *    zmk_watchdog_reboot() call happens first. That is expected, not a bug
 *    (see the proto doc comment on InjectTestRequest).
 */
static void handle_inject_test(const cormoran_watchdog_InjectTestRequest *req,
                               cormoran_watchdog_Response *resp) {
    switch (req->kind) {
    case cormoran_watchdog_InjectTestKind_FATAL_KIND:
        zmk_watchdog_inject_fatal();
        break;
    case cormoran_watchdog_InjectTestKind_FREEZE_KIND:
    default:
        zmk_watchdog_inject_freeze();
        break;
    }

    cormoran_watchdog_InjectAckResponse ack = cormoran_watchdog_InjectAckResponse_init_zero;
    ack.kind = req->kind;
    resp->which_response_type = cormoran_watchdog_Response_inject_ack_tag;
    resp->response_type.inject_ack = ack;
}
#endif /* CONFIG_ZMK_WATCHDOG_TEST_INJECTION */

void watchdog_request_exec_handle(const cormoran_watchdog_Request *req,
                                  cormoran_watchdog_Response *resp) {
    *resp = (cormoran_watchdog_Response)cormoran_watchdog_Response_init_zero;

    switch (req->which_request_type) {
    case cormoran_watchdog_Request_get_status_tag:
        handle_get_status(resp);
        break;
    case cormoran_watchdog_Request_list_incidents_tag:
        handle_list_incidents(&req->request_type.list_incidents, resp);
        break;
    case cormoran_watchdog_Request_delete_incidents_tag:
        handle_delete_incidents(&req->request_type.delete_incidents, resp);
        break;
    case cormoran_watchdog_Request_inject_test_tag:
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
        handle_inject_test(&req->request_type.inject_test, resp);
#else
        /* The proto tag exists unconditionally for a stable wire format
         * (DESIGN.md, Studio RPC, split relay and client), but a non-test-injection build must
         * never actually freeze/fault itself just because a client asked -- answer with the same
         * error a genuinely unsupported request kind would get. */
        watchdog_response_set_error(resp, "Unsupported or missing watchdog request");
#endif
        break;
    default:
        watchdog_response_set_error(resp, "Unsupported or missing watchdog request");
        break;
    }
}
