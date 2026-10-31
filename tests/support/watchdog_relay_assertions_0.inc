/* SPDX-License-Identifier: MIT */
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <cormoran/zmk/watchdog_relay_page.h>
#include "init_result.h"
#include "context_relay_assertions.h"

static int test_diagnostic_page_assembly(void) {
    static struct watchdog_relay_page state;
    static cormoran_watchdog_Response response;
    state = (struct watchdog_relay_page){0};
    cormoran_watchdog_RelayIncidentChunk core = cormoran_watchdog_RelayIncidentChunk_init_zero;
    core.has_incident = core.has_page_index = true;
    core.diagnostics_expected = true;
    core.incident.id = 7;
    cormoran_watchdog_RelayDiagnosticChunk diagnostic =
        cormoran_watchdog_RelayDiagnosticChunk_init_zero;
    diagnostic.incident_id = 7;
    diagnostic.diagnostics.size = 80;
    diagnostic.diagnostics.bytes[0] = 1;
    cormoran_watchdog_RelayIncidentChunk done = cormoran_watchdog_RelayIncidentChunk_init_zero;
    done.done = done.has_expected_count = true;
    done.expected_count = 2;
    for (unsigned i = 0; i < 2; ++i) {
        core.page_index = i;
        diagnostic.page_index = i;
        diagnostic.diagnostics.bytes[1] = i;
        if (watchdog_relay_page_core(&state, 1, 42, &core, &response)) {
            return -EINVAL;
        }
        /* Actual protobuf encode/decode extension, including its bounded envelope. */
        cormoran_watchdog_RelayResponse sent = cormoran_watchdog_RelayResponse_init_zero;
        sent.request_id = 42;
        sent.which_relay_response_type = cormoran_watchdog_RelayResponse_diagnostic_chunk_tag;
        sent.relay_response_type.diagnostic_chunk = diagnostic;
        uint8_t wire[WATCHDOG_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
        pb_ostream_t out = pb_ostream_from_buffer(wire, sizeof(wire));
        cormoran_watchdog_RelayResponse received = cormoran_watchdog_RelayResponse_init_zero;
        if (!pb_encode(&out, cormoran_watchdog_RelayResponse_fields, &sent)) {
            return -EMSGSIZE;
        }
        pb_istream_t in = pb_istream_from_buffer(wire, out.bytes_written);
        if (!pb_decode(&in, cormoran_watchdog_RelayResponse_fields, &received)) {
            return -EINVAL;
        }
        watchdog_relay_page_diagnostic(&state, 1, 42,
                                       &received.relay_response_type.diagnostic_chunk);
        /* A duplicate must not overwrite the first accepted payload. */
        diagnostic.diagnostics.bytes[1] = 99;
        watchdog_relay_page_diagnostic(&state, 1, 42, &diagnostic);
    }
    if (!watchdog_relay_page_core(&state, 1, 42, &done, &response) ||
        response.which_response_type != cormoran_watchdog_Response_incident_page_tag ||
        response.response_type.incident_page.incidents_count != 2 ||
        response.response_type.incident_page.incidents[0].diagnostics.bytes[1] != 0 ||
        response.response_type.incident_page.incidents[1].diagnostics.bytes[1] != 1) {
        return -EINVAL;
    }
    for (unsigned failure = 0; failure < 6; ++failure) {
        state = (struct watchdog_relay_page){0};
        core.page_index = 0;
        diagnostic.page_index = 0;
        diagnostic.incident_id = 7;
        diagnostic.diagnostics.size = 80;
        done.expected_count = 1;
        if (failure == 0) {
            watchdog_relay_page_diagnostic(&state, 1, 42, &diagnostic);
        }
        if (failure == 5) {
            core.page_index = 1;
            done.expected_count = 2;
        }
        watchdog_relay_page_core(&state, 1, 42, &core, &response);
        if (failure == 1) {
            watchdog_relay_page_diagnostic(&state, 2, 42, &diagnostic);
        }
        if (failure == 2) {
            watchdog_relay_page_diagnostic(&state, 1, 43, &diagnostic);
        }
        if (failure == 3) {
            diagnostic.incident_id = 8;
            watchdog_relay_page_diagnostic(&state, 1, 42, &diagnostic);
        }
        if (failure == 4) {
            diagnostic.diagnostics.size = 81;
            watchdog_relay_page_diagnostic(&state, 1, 42, &diagnostic);
        }
        if (!watchdog_relay_page_core(&state, 1, 42, &done, &response) ||
            response.which_response_type != cormoran_watchdog_Response_error_tag) {
            return -EINVAL;
        }
    }
    state = (struct watchdog_relay_page){0};
    core.has_page_index = false;
    core.diagnostics_expected = false;
    done.has_expected_count = false;
    watchdog_relay_page_core(&state, 1, 42, &core, &response);
    watchdog_relay_page_core(&state, 1, 42, &core, &response);
    if (!watchdog_relay_page_core(&state, 1, 42, &done, &response) ||
        response.which_response_type != cormoran_watchdog_Response_incident_page_tag ||
        response.response_type.incident_page.incidents_count != 2) {
        return -EINVAL;
    }
    return 0;
}

/*
 * Minimal in-RAM settings backend, same pattern as tests/watchdog/'s
 * watchdog_test.c and tests/ztest/watchdog_rpc_test.c: this test build has
 * CONFIG_SETTINGS=y but no backend registered by default, and
 * zmk_watchdog_store_append() calls settings_save_one(), which needs *some*
 * registered destination to succeed.
 */

#define WATCHDOG_RELAY_TEST_SETTINGS_STORAGE_CAPACITY 8

struct watchdog_relay_test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[sizeof(struct zmk_watchdog_incident_record) + 4];
    size_t len;
};

static struct watchdog_relay_test_settings_record
    watchdog_relay_test_settings_storage[WATCHDOG_RELAY_TEST_SETTINGS_STORAGE_CAPACITY];

static struct watchdog_relay_test_settings_record *
watchdog_relay_test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(watchdog_relay_test_settings_storage); i++) {
        if (watchdog_relay_test_settings_storage[i].present &&
            strncmp(watchdog_relay_test_settings_storage[i].name, name,
                    sizeof(watchdog_relay_test_settings_storage[i].name)) == 0) {
            return &watchdog_relay_test_settings_storage[i];
        }
    }
    return NULL;
}

static ssize_t watchdog_relay_test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct watchdog_relay_test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);
    memcpy(data, record->data, read_len);
    return read_len;
}

static int watchdog_relay_test_settings_load(struct settings_store *cs,
                                             const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(watchdog_relay_test_settings_storage); i++) {
        struct watchdog_relay_test_settings_record *record =
            &watchdog_relay_test_settings_storage[i];
        if (!record->present) {
            continue;
        }
        int ret = settings_call_set_handler(record->name, record->len,
                                            watchdog_relay_test_settings_read_cb, record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static int watchdog_relay_test_settings_save(struct settings_store *cs, const char *name,
                                             const char *value, size_t val_len) {
    ARG_UNUSED(cs);

    struct watchdog_relay_test_settings_record *record =
        watchdog_relay_test_settings_find_record(name);
    if (value == NULL) {
        if (record) {
            record->present = false;
        }
        return 0;
    }

    if (val_len > sizeof(record->data)) {
        return -EMSGSIZE;
    }
    if (strlen(name) >= SETTINGS_MAX_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    if (!record) {
        for (size_t i = 0; i < ARRAY_SIZE(watchdog_relay_test_settings_storage); i++) {
            if (!watchdog_relay_test_settings_storage[i].present) {
                record = &watchdog_relay_test_settings_storage[i];
                break;
            }
        }
    }
    if (!record) {
        return -ENOMEM;
    }

    record->present = true;
    strcpy(record->name, name);
    memcpy(record->data, value, val_len);
    record->len = val_len;
    return 0;
}

static const struct settings_store_itf watchdog_relay_test_settings_itf = {
    .csi_load = watchdog_relay_test_settings_load,
    .csi_save = watchdog_relay_test_settings_save,
};

static struct settings_store watchdog_relay_test_settings_store = {
    .cs_itf = &watchdog_relay_test_settings_itf,
};

static int watchdog_relay_test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&watchdog_relay_test_settings_store);
    settings_dst_register(&watchdog_relay_test_settings_store);
    return settings_load_subtree("wdg");
}

static int watchdog_split_relay_test_init(void) {
    if (test_common_context_page_assembly() < 0) {
        return -EINVAL;
    }
    if (test_diagnostic_page_assembly() < 0) {
        LOG_ERR("Watchdog diagnostic page assembly failed");
        return -EINVAL;
    }
    int backend_ret = watchdog_relay_test_settings_backend_init();
    if (backend_ret < 0) {
        LOG_ERR("Split relay test: failed to init settings backend: %d", backend_ret);
        return backend_ret;
    }

    /* Populate the local store with one incident so a relayed ListIncidents
     * has something to report -- exercises the same
     * watchdog_request_exec_handle() code path a real relayed request would
     * take. */
    struct zmk_watchdog_incident_record rec = {0};
    rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    rec.uptime_s = 5;
    rec.detail.freeze.trace_flags = ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_PARTIAL;
    rec.detail.freeze.trace_count = ZMK_WATCHDOG_TRACE_ADDRS;
    for (size_t i = 0; i < ZMK_WATCHDOG_TRACE_ADDRS; ++i) {
        rec.detail.freeze.addresses[i] = 0x1020 + 16 * i;
    }
    if (zmk_watchdog_store_append(&rec) != 0) {
        LOG_ERR("Split relay test: failed to seed local store");
        return -EIO;
    }

    cormoran_watchdog_RelayRequest relay_req = cormoran_watchdog_RelayRequest_init_zero;
    relay_req.request_id = 42;
    relay_req.has_request = true;
    relay_req.request.which_request_type = cormoran_watchdog_Request_list_incidents_tag;
    relay_req.request.request_type.list_incidents.start_index = 0;

    uint8_t payload[WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
    pb_ostream_t ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &relay_req)) {
        LOG_ERR("Split relay test: failed to encode synthetic request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }

    uint32_t request_id;
    cormoran_watchdog_Response resp;
    int ret = watchdog_relay_exec_request(payload, ostream.bytes_written, &request_id, &resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: exec failed: %d", ret);
        return ret;
    }

    if (request_id != 42) {
        LOG_ERR("Split relay test: request_id mismatch: got %u", request_id);
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_watchdog_Response_incident_page_tag) {
        LOG_ERR("Split relay test: expected an IncidentPage, got response type %d",
                resp.which_response_type);
        return -EINVAL;
    }
    if (resp.response_type.incident_page.incidents_count != 1 ||
        resp.response_type.incident_page.incidents[0].source != 0) {
        LOG_ERR("Split relay test: expected 1 incident with source 0 (stamped later by the "
                "central), got count=%u source=%u",
                (unsigned int)resp.response_type.incident_page.incidents_count,
                resp.response_type.incident_page.incidents[0].source);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_list_incidents\n");

    /* build_incident_chunk_response() (the same helper
     * on_watchdog_relay_request() -> stream_incident_page() calls to build
     * each event it raises) must produce exactly N+1 chunks for an N-incident
     * page -- one incident_chunk{done:false, incident:incidents[i]} per
     * incident, then a final incident_chunk{done:true} with no incident, all
     * sharing this request's request_id/total/start_index (DESIGN.md, Studio RPC, split relay and
     * client). Exercised directly (rather than via stream_incident_page()'s
     * raise_zmk_watchdog_relay_response(), which needs the real split
     * transport) since this is exactly the seam the peripheral responder
     * itself is built from. */
    {
        const cormoran_watchdog_IncidentPageResponse *page = &resp.response_type.incident_page;
        uint32_t total_chunks = 0;
        for (uint32_t i = 0;; i++) {
            cormoran_watchdog_RelayResponse chunk_resp;
            total_chunks = build_incident_chunk_response(request_id, page, i, &chunk_resp);
            if (chunk_resp.which_relay_response_type !=
                cormoran_watchdog_RelayResponse_incident_chunk_tag) {
                LOG_ERR("Split relay test: chunk %u has no incident_chunk set", i);
                return -EINVAL;
            }
            const cormoran_watchdog_RelayIncidentChunk *chunk =
                &chunk_resp.relay_response_type.incident_chunk;
            if (chunk_resp.request_id != request_id || chunk->total != page->total ||
                chunk->start_index != page->start_index) {
                LOG_ERR("Split relay test: chunk %u envelope mismatch", i);
                return -EINVAL;
            }
            if (i < page->incidents_count) {
                uint8_t wire[WATCHDOG_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
                pb_ostream_t encoded = pb_ostream_from_buffer(wire, sizeof(wire));
                cormoran_watchdog_RelayResponse decoded = cormoran_watchdog_RelayResponse_init_zero;
                if (!pb_encode(&encoded, cormoran_watchdog_RelayResponse_fields, &chunk_resp)) {
                    return -EMSGSIZE;
                }
                pb_istream_t input = pb_istream_from_buffer(wire, encoded.bytes_written);
                if (!pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &decoded) ||
                    decoded.relay_response_type.incident_chunk.incident.detail.freeze.trace.size !=
                        ZMK_WATCHDOG_TRACE_WIRE_SIZE ||
                    memcmp(decoded.relay_response_type.incident_chunk.incident.detail.freeze.trace
                               .bytes,
                           chunk->incident.detail.freeze.trace.bytes,
                           ZMK_WATCHDOG_TRACE_WIRE_SIZE)) {
                    return -EINVAL;
                }
                if (chunk->done || !chunk->has_incident ||
                    chunk->incident.id != page->incidents[i].id) {
                    LOG_ERR("Split relay test: chunk %u expected incident %u, got done=%d "
                            "has_incident=%d",
                            i, page->incidents[i].id, chunk->done, chunk->has_incident);
                    return -EINVAL;
                }
            } else {
                if (!chunk->done || chunk->has_incident) {
                    LOG_ERR(
                        "Split relay test: final chunk %u expected done=true has_incident=false, "
                        "got done=%d has_incident=%d",
                        i, chunk->done, chunk->has_incident);
                    return -EINVAL;
                }
                break;
            }
        }
        if (total_chunks != page->incidents_count + 1) {
            LOG_ERR("Split relay test: expected %u total chunks (N incidents + 1 done), got %u",
                    (unsigned int)page->incidents_count + 1, total_chunks);
            return -EINVAL;
        }
    }

    printk("PASS: watchdog_split_relay_incident_chunk_stream\n");

    /* DeleteIncidents relayed the same way. */
    cormoran_watchdog_RelayRequest del_relay_req = cormoran_watchdog_RelayRequest_init_zero;
    del_relay_req.request_id = 43;
    del_relay_req.has_request = true;
    del_relay_req.request.which_request_type = cormoran_watchdog_Request_delete_incidents_tag;
    del_relay_req.request.request_type.delete_incidents.all = true;

    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &del_relay_req)) {
        LOG_ERR("Split relay test: failed to encode delete request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = watchdog_relay_exec_request(payload, ostream.bytes_written, &request_id, &resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: delete exec failed: %d", ret);
        return ret;
    }
    if (resp.which_response_type != cormoran_watchdog_Response_delete_result_tag ||
        resp.response_type.delete_result.deleted != 1) {
        LOG_ERR("Split relay test: expected delete_result.deleted=1, got type=%d deleted=%u",
                resp.which_response_type, resp.response_type.delete_result.deleted);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_delete_incidents\n");

    /* Genuinely unsupported/malformed relayed request (no request_type set
     * at all): watchdog_request_exec_handle() must still produce an
     * ErrorResponse, not a crash or an unfilled response. */
    cormoran_watchdog_RelayRequest empty_relay_req = cormoran_watchdog_RelayRequest_init_zero;
    empty_relay_req.request_id = 44;
    empty_relay_req.has_request = true;
    ostream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &empty_relay_req)) {
        LOG_ERR("Split relay test: failed to encode empty request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    ret = watchdog_relay_exec_request(payload, ostream.bytes_written, &request_id, &resp);
    if (ret < 0) {
        LOG_ERR("Split relay test: empty-request exec failed: %d", ret);
        return ret;
    }
    if (resp.which_response_type != cormoran_watchdog_Response_error_tag) {
        LOG_ERR("Split relay test: expected an ErrorResponse for an unset request kind, got "
                "response type %d",
                resp.which_response_type);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_unsupported_kind\n");

    /* A size larger than the payload it travels in is refused before
     * decoding. The bytes are a valid request followed by unknown
     * length-delimited fields, so they would decode if the size were trusted. */
    static uint8_t oversized[WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE + 1];
    cormoran_watchdog_RelayRequest status_relay_req = cormoran_watchdog_RelayRequest_init_zero;
    status_relay_req.request_id = 45;
    status_relay_req.has_request = true;
    status_relay_req.request.which_request_type = cormoran_watchdog_Request_get_status_tag;
    ostream = pb_ostream_from_buffer(oversized, sizeof(oversized));
    if (!pb_encode(&ostream, cormoran_watchdog_RelayRequest_fields, &status_relay_req)) {
        LOG_ERR("Split relay test: failed to encode oversized request: %s", PB_GET_ERROR(&ostream));
        return -EIO;
    }
    size_t used = ostream.bytes_written;
    while (sizeof(oversized) - used >= 2) {
        size_t len = MIN(sizeof(oversized) - used - 2, 127);
        if (sizeof(oversized) - used - 2 - len == 1) {
            len--; /* never leave a one-byte tail */
        }
        oversized[used++] = (15 << 3) | 2; /* unknown field 15, length-delimited */
        oversized[used++] = (uint8_t)len;
        memset(&oversized[used], 0, len);
        used += len;
    }
    if (used != sizeof(oversized)) {
        LOG_ERR("Split relay test: oversized request padding left %u bytes",
                (unsigned int)(sizeof(oversized) - used));
        return -EINVAL;
    }
    ret = watchdog_relay_exec_request(oversized, sizeof(oversized), &request_id, &resp);
    if (ret != -EINVAL) {
        LOG_ERR("Split relay test: an oversized request was not refused: %d", ret);
        return -EINVAL;
    }

    printk("PASS: watchdog_split_relay_oversized_request\n");
    return 0;
}

OWNER_CHECKED_SYSWORKQ(watchdog_split_relay_test_init, APPLICATION,
                       CONFIG_APPLICATION_INIT_PRIORITY);
