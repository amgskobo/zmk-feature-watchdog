#include "../support/init_result.h"
/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/core.h>
#include <zmk/studio/custom.h>
#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_request_exec.h>
#include <cormoran/zmk/watchdog_diagnostics.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Minimal in-RAM settings backend, same pattern as
 * tests/watchdog/'s watchdog_test.c: tests/studio/'s native_sim.conf has
 * CONFIG_SETTINGS_NONE=y (no backend registered by default), but the
 * watchdog store's zmk_watchdog_store_append() calls settings_save_one(),
 * which needs *some* registered destination to succeed.
 */

#define TEST_SETTINGS_STORAGE_CAPACITY 32

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[sizeof(struct zmk_watchdog_incident_record) + 4];
    size_t len;
};

static struct test_settings_record test_settings_storage[TEST_SETTINGS_STORAGE_CAPACITY];

static struct test_settings_record *test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        if (test_settings_storage[i].present &&
            strncmp(test_settings_storage[i].name, name, sizeof(test_settings_storage[i].name)) ==
                0) {
            return &test_settings_storage[i];
        }
    }
    return NULL;
}

static ssize_t test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);
    memcpy(data, record->data, read_len);
    return read_len;
}

static int test_settings_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        struct test_settings_record *record = &test_settings_storage[i];
        if (!record->present) {
            continue;
        }
        int ret = settings_call_set_handler(record->name, record->len, test_settings_read_cb,
                                            record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static int test_settings_save(struct settings_store *cs, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(cs);

    struct test_settings_record *record = test_settings_find_record(name);
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
        for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
            if (!test_settings_storage[i].present) {
                record = &test_settings_storage[i];
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

static const struct settings_store_itf test_settings_itf = {
    .csi_load = test_settings_load,
    .csi_save = test_settings_save,
};

static struct settings_store test_settings_store = {
    .cs_itf = &test_settings_itf,
};

static int test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    settings_src_register(&test_settings_store);
    settings_dst_register(&test_settings_store);
    return settings_load_subtree("wdg");
}

/*
 * Studio RPC tests for src/studio/watchdog_handler.c (requires
 * CONFIG_ZMK_WATCHDOG_STUDIO_RPC, which in turn requires CONFIG_ZMK_STUDIO --
 * built as part of tests/studio/, not tests/watchdog/, because enabling
 * CONFIG_ZMK_STUDIO requires a physical-layout devicetree that only
 * tests/studio/native_sim.keymap (via ../test.dtsi) provides -- see
 * zmk/app/src/physical_layouts.c's BUILD_ASSERT).
 *
 * Drives the registered handler directly (no transport), by looking it up
 * in the zmk_rpc_custom_subsystem iterable section by identifier -- the same
 * lookup the handler itself uses internally to find its own subsystem index
 * for notifications. This exercises the exact function Studio's RPC
 * dispatch would call, just without the serial transport in between.
 */

static custom_subsystem_handler *find_watchdog_rpc_handler(void) {
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);

    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "cormoran__watchdog") == 0) {
            return subsys->handler;
        }
    }
    return NULL;
}

/* Decode callback for zmk_custom_CallResponse.payload (a pb_callback_t bytes
 * field with no fixed max_size -- see custom.proto/custom.options): copies
 * the remaining raw bytes of this submessage into a fixed test buffer, the
 * standard nanopb pattern for decoding a callback-typed bytes field. */
struct call_response_payload_capture {
    /* Collect a whole wire message here; the production TX ring streams it
     * in smaller pieces and does not bound the total encoded message size. */
    uint8_t buf[MAX(cormoran_watchdog_Response_size, cormoran_watchdog_Notification_size) + 16];
    size_t size;
};

static bool decode_call_response_payload(pb_istream_t *stream, const pb_field_t *field,
                                         void **arg) {
    ARG_UNUSED(field);
    struct call_response_payload_capture *capture = *arg;

    if (stream->bytes_left > sizeof(capture->buf)) {
        LOG_ERR("CallResponse payload too large for test capture buffer: %u",
                (unsigned int)stream->bytes_left);
        return false;
    }

    capture->size = stream->bytes_left;
    return pb_read(stream, capture->buf, capture->size);
}

/* Encodes req, calls the handler, decodes the response into *out. Returns
 * true on success (handler returned true and response decoded OK). */
static bool call_watchdog_rpc(const cormoran_watchdog_Request *req,
                              cormoran_watchdog_Response *out) {
    custom_subsystem_handler *handler = find_watchdog_rpc_handler();
    if (!handler) {
        LOG_ERR("watchdog RPC subsystem not registered");
        return false;
    }

    static zmk_custom_CallRequest raw_request;
    raw_request = (zmk_custom_CallRequest){0};
    pb_ostream_t req_stream =
        pb_ostream_from_buffer(raw_request.payload.bytes, sizeof(raw_request.payload.bytes));
    if (!pb_encode(&req_stream, cormoran_watchdog_Request_fields, req)) {
        LOG_ERR("Failed to encode watchdog request: %s", PB_GET_ERROR(&req_stream));
        return false;
    }
    raw_request.payload.size = req_stream.bytes_written;

    /* Mirrors zmk/app/src/studio/custom_subsystem.c's own call() handler:
     * the handler's pb_callback_t is wired into CallResponse.payload and
     * only actually invoked when *that* message is pb_encode()'d --
     * nanopb's own field-callback machinery supplies the correct
     * pb_field_t* for CallResponse.payload at that point (calling the
     * callback directly with a NULL field descriptor segfaults inside
     * zmk_rpc_custom_subsystem_encode_response_payload(), which dereferences
     * it via pb_encode_tag_for_field()). */
    zmk_custom_CallResponse response = zmk_custom_CallResponse_init_zero;
    bool ok = handler(&raw_request, &response.payload);
    if (!ok || !response.payload.funcs.encode) {
        LOG_ERR("watchdog RPC handler did not produce a response encoder");
        return false;
    }

    static uint8_t call_resp_buf[cormoran_watchdog_Response_size + 16];
    pb_ostream_t call_resp_stream = pb_ostream_from_buffer(call_resp_buf, sizeof(call_resp_buf));
    if (!pb_encode(&call_resp_stream, zmk_custom_CallResponse_fields, &response)) {
        LOG_ERR("Failed to encode CallResponse: %s", PB_GET_ERROR(&call_resp_stream));
        return false;
    }

    static struct call_response_payload_capture capture;
    capture = (struct call_response_payload_capture){0};

    zmk_custom_CallResponse decoded_call_resp = zmk_custom_CallResponse_init_zero;
    decoded_call_resp.payload.funcs.decode = decode_call_response_payload;
    decoded_call_resp.payload.arg = &capture;

    pb_istream_t call_resp_istream =
        pb_istream_from_buffer(call_resp_buf, call_resp_stream.bytes_written);
    if (!pb_decode(&call_resp_istream, zmk_custom_CallResponse_fields, &decoded_call_resp)) {
        LOG_ERR("Failed to decode CallResponse: %s", PB_GET_ERROR(&call_resp_istream));
        return false;
    }

    *out = (cormoran_watchdog_Response)cormoran_watchdog_Response_init_zero;
    pb_istream_t resp_istream = pb_istream_from_buffer(capture.buf, capture.size);
    if (!pb_decode(&resp_istream, cormoran_watchdog_Response_fields, out)) {
        LOG_ERR("Failed to decode watchdog response: %s", PB_GET_ERROR(&resp_istream));
        return false;
    }

    return true;
}

/* Encode during event dispatch, while the production buffer remains valid.
 * Locking here models waiting for the transport before payload encoding. */
static bool notification_probe_active, notification_probe_lock_before_encode;
static bool notification_probe_lock_after_sizing, notification_probe_frame_valid;
static unsigned int notification_probe_events;
static bool notification_probe_encoded;
static cormoran_watchdog_Notification notification_probe_decoded;

static int notification_probe_listener(const zmk_event_t *eh) {
    const struct zmk_studio_custom_notification *event = as_zmk_studio_custom_notification(eh);
    if (!notification_probe_active || !event) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);
    if (event->subsystem_index >= count) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    struct zmk_rpc_custom_subsystem *subsystem;
    STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, event->subsystem_index, &subsystem);
    if (strcmp(subsystem->identifier, "cormoran__watchdog") != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    notification_probe_events++;
    if (notification_probe_lock_before_encode) {
        zmk_studio_core_lock();
    }
    zmk_custom_CallResponse response = zmk_custom_CallResponse_init_zero;
    response.payload = event->encode_payload;
    uint8_t bytes[cormoran_watchdog_Notification_size + 16];
    pb_ostream_t output = pb_ostream_from_buffer(bytes, sizeof(bytes));
    size_t expected_size;
    notification_probe_frame_valid =
        pb_get_encoded_size(&expected_size, zmk_custom_CallResponse_fields, &response);
    if (notification_probe_lock_after_sizing) {
        zmk_studio_core_lock();
    }
    notification_probe_frame_valid =
        notification_probe_frame_valid &&
        pb_encode(&output, zmk_custom_CallResponse_fields, &response) &&
        output.bytes_written == expected_size;
    notification_probe_encoded = false;
    if (notification_probe_frame_valid) {
        struct call_response_payload_capture capture = {0};
        zmk_custom_CallResponse envelope = zmk_custom_CallResponse_init_zero;
        envelope.payload.funcs.decode = decode_call_response_payload;
        envelope.payload.arg = &capture;
        pb_istream_t input = pb_istream_from_buffer(bytes, output.bytes_written);
        notification_probe_frame_valid =
            pb_decode(&input, zmk_custom_CallResponse_fields, &envelope);
        notification_probe_decoded =
            (cormoran_watchdog_Notification)cormoran_watchdog_Notification_init_zero;
        input = pb_istream_from_buffer(capture.buf, capture.size);
        notification_probe_encoded =
            notification_probe_frame_valid && capture.size > 0 &&
            pb_decode(&input, cormoran_watchdog_Notification_fields, &notification_probe_decoded);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(watchdog_notification_probe, notification_probe_listener);
ZMK_SUBSCRIPTION(watchdog_notification_probe, zmk_studio_custom_notification);

static int test_notification_authorization(void) {
    const bool unsecured = IS_ENABLED(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_UNSECURED);
    struct zmk_watchdog_incident_record record = {0};
    record.type = ZMK_WATCHDOG_INCIDENT_FATAL;
    record.uptime_s = 42;
    record.detail.fatal.pc = 0x1020;
    notification_probe_active = true;
    zmk_studio_core_lock();
    notification_probe_events = 0;
    notification_probe_encoded = false;
    notification_probe_frame_valid = false;
    if (zmk_watchdog_store_append(&record) != 0 || zmk_watchdog_store_count() != 1 ||
        notification_probe_events != (unsigned int)unsecured ||
        notification_probe_encoded != unsecured || notification_probe_frame_valid != unsecured) {
        return -EINVAL;
    }

    zmk_studio_core_unlock();
    notification_probe_events = 0;
    notification_probe_encoded = false;
    if (zmk_watchdog_store_append(&record) != 0 || notification_probe_events != 1 ||
        !notification_probe_encoded || !notification_probe_frame_valid ||
        notification_probe_decoded.which_notification_type !=
            cormoran_watchdog_Notification_incident_recorded_tag ||
        !notification_probe_decoded.notification_type.incident_recorded.has_incident ||
        notification_probe_decoded.notification_type.incident_recorded.incident.uptime_s != 42 ||
        notification_probe_decoded.notification_type.incident_recorded.incident.detail.fatal.pc !=
            0x1020) {
        return -EINVAL;
    }

    notification_probe_lock_before_encode = true;
    notification_probe_events = 0;
    notification_probe_encoded = false;
    if (zmk_watchdog_store_append(&record) != 0 || notification_probe_events != 1 ||
        !notification_probe_frame_valid || notification_probe_encoded != unsecured ||
        zmk_watchdog_store_count() != 3) {
        return -EINVAL;
    }
    notification_probe_lock_before_encode = false;
    zmk_studio_core_unlock();
    notification_probe_lock_after_sizing = true;
    notification_probe_events = 0;
    if (zmk_watchdog_store_append(&record) != 0 || notification_probe_events != 1 ||
        !notification_probe_frame_valid || !notification_probe_encoded ||
        zmk_watchdog_store_count() != 4) {
        return -EINVAL;
    }
    notification_probe_lock_after_sizing = false;
    zmk_studio_core_unlock();
    if (notification_probe_events != 1) {
        return -EINVAL;
    }
    notification_probe_active = false;
    zmk_studio_core_lock();
    LOG_INF("PASS: watchdog_notification_authorization unsecured=%d", unsecured);
    return zmk_watchdog_store_delete_all();
}

static int test_rpc_get_status_empty_store(void) {
    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_get_status_tag;

    cormoran_watchdog_Response resp;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }

    if (resp.which_response_type != cormoran_watchdog_Response_status_tag) {
        LOG_ERR("expected status response, got %d", resp.which_response_type);
        return -EINVAL;
    }
    if (resp.response_type.status.stored != 0 ||
        resp.response_type.status.recording_stopped != false) {
        LOG_ERR("unexpected status on empty store: stored=%u recording_stopped=%d",
                resp.response_type.status.stored, resp.response_type.status.recording_stopped);
        return -EINVAL;
    }
    if (resp.response_type.status.capacity != zmk_watchdog_store_capacity()) {
        LOG_ERR("status.capacity mismatch");
        return -EINVAL;
    }
    if (resp.response_type.status.build_tag != zmk_watchdog_diagnostics_build_tag() ||
        resp.response_type.status.runtime_flags != zmk_watchdog_diagnostics_runtime_flags()) {
        LOG_ERR("runtime metadata differs from running firmware");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_get_status_empty_store");
    return 0;
}

static int test_runtime_status_relay_bound(void) {
    cormoran_watchdog_RelayResponse relay = cormoran_watchdog_RelayResponse_init_zero;
    relay.request_id = UINT32_MAX;
    relay.which_relay_response_type = cormoran_watchdog_RelayResponse_response_tag;
    relay.relay_response_type.response.which_relay_non_list_response_type =
        cormoran_watchdog_RelayNonListResponse_status_tag;
    cormoran_watchdog_StatusResponse *status =
        &relay.relay_response_type.response.relay_non_list_response_type.status;
    status->capacity = UINT16_MAX;
    status->stored = UINT16_MAX;
    status->dropped_since_boot = UINT32_MAX;
    status->recording_stopped = true;
    status->build_tag = status->runtime_flags = status->reset_cause = UINT32_MAX;
    status->previous_diagnostics.size = 80;
    for (size_t i = 0; i < 80; ++i) {
        status->previous_diagnostics.bytes[i] = (uint8_t)i;
    }
    uint8_t bytes[124]; /* payload plus source/size/alignment must fit 128 */
    pb_ostream_t stream = pb_ostream_from_buffer(bytes, sizeof(bytes));
    if (!pb_encode(&stream, cormoran_watchdog_RelayResponse_fields, &relay) ||
        stream.bytes_written > 124 || cormoran_watchdog_RelayResponse_size > 124) {
        LOG_ERR("status relay exceeds existing split event bound");
        return -EINVAL;
    }
    cormoran_watchdog_RelayResponse decoded = cormoran_watchdog_RelayResponse_init_zero;
    pb_istream_t input = pb_istream_from_buffer(bytes, stream.bytes_written);
    if (!pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &decoded)) {
        return -EINVAL;
    }
    const cormoran_watchdog_StatusResponse *got =
        &decoded.relay_response_type.response.relay_non_list_response_type.status;
    if (got->capacity != UINT16_MAX || got->stored != UINT16_MAX ||
        got->dropped_since_boot != UINT32_MAX || got->runtime_flags != UINT32_MAX ||
        got->reset_cause != UINT32_MAX || got->previous_diagnostics.size != 80 ||
        memcmp(got->previous_diagnostics.bytes, status->previous_diagnostics.bytes, 80)) {
        return -EINVAL;
    }
    LOG_INF("PASS: watchdog_runtime_status_relay_bound");
    return 0;
}

static int test_rpc_list_incidents_empty_store(void) {
    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_list_incidents_tag;
    req.request_type.list_incidents.start_index = 0;

    cormoran_watchdog_Response resp;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }

    if (resp.which_response_type != cormoran_watchdog_Response_incident_page_tag) {
        LOG_ERR("expected incident_page response, got %d", resp.which_response_type);
        return -EINVAL;
    }
    if (resp.response_type.incident_page.total != 0 ||
        resp.response_type.incident_page.incidents_count != 0) {
        LOG_ERR("expected empty page, got total=%u incidents_count=%u",
                resp.response_type.incident_page.total,
                (unsigned int)resp.response_type.incident_page.incidents_count);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_list_incidents_empty_store");
    return 0;
}

static int test_rpc_list_incidents_pagination_and_delete(void) {
    /* Populate: one of each incident type, plus enough extras to force two
     * pages (page size is 4, see WATCHDOG_RPC_PAGE_SIZE in
     * src/studio/watchdog_request_exec.c). The split relay streams one incident
     * per event, so the local page size is independent of its 255-byte ceiling
     * (see DESIGN.md, Studio RPC, split relay and client). */
    struct zmk_watchdog_incident_record freeze_rec = {0};
    freeze_rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    freeze_rec.uptime_s = 10;
    freeze_rec.detail.freeze.channel_id = 2;
    freeze_rec.detail.freeze.thread_state = 2;
    freeze_rec.detail.freeze.trace_flags = ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_PENDED;
    freeze_rec.detail.freeze.trace_count = 2;
    freeze_rec.detail.freeze.wait_queue = 0x20000124;
    freeze_rec.detail.freeze.stack_pointer = 0x20001000;
    freeze_rec.detail.freeze.addresses[0] = 0x1020;
    freeze_rec.detail.freeze.addresses[1] = 0x2031;
    strncpy(freeze_rec.detail.freeze.queue_name, "sysworkq",
            sizeof(freeze_rec.detail.freeze.queue_name) - 1);
    if (zmk_watchdog_store_append(&freeze_rec) != 0) {
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record fatal_rec = {0};
    fatal_rec.type = ZMK_WATCHDOG_INCIDENT_FATAL;
    fatal_rec.uptime_s = 20;
    fatal_rec.flags = 1;
    fatal_rec.flags |= ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
    fatal_rec.capture_context[0] = 1;
    for (size_t i = 1; i < sizeof(fatal_rec.capture_context); ++i) {
        fatal_rec.capture_context[i] = (uint8_t)i;
    }
    fatal_rec.diagnostics[0] = 1;
    for (size_t i = 0; i < 6; ++i) {
        fatal_rec.detail.fatal.context[i] = 0x10000000U + i;
    }
    for (size_t i = 1; i < sizeof(fatal_rec.diagnostics); ++i) {
        fatal_rec.diagnostics[i] = (uint8_t)i;
    }
    fatal_rec.detail.fatal.reason = 4;
    fatal_rec.detail.fatal.pc = 0x1000;
    fatal_rec.detail.fatal.lr = 0x2000;
    strncpy(fatal_rec.detail.fatal.thread_name, "main",
            sizeof(fatal_rec.detail.fatal.thread_name) - 1);
    if (zmk_watchdog_store_append(&fatal_rec) != 0) {
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record reset_rec = {0};
    reset_rec.type = ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
    reset_rec.uptime_s = 30;
    reset_rec.detail.reset.cause_bits = 0x12;
    if (zmk_watchdog_store_append(&reset_rec) != 0) {
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record extra_rec = {0};
    extra_rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    extra_rec.uptime_s = 40;
    strncpy(extra_rec.detail.freeze.queue_name, "lowprio_workq",
            sizeof(extra_rec.detail.freeze.queue_name) - 1);
    if (zmk_watchdog_store_append(&extra_rec) != 0) {
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record extra_rec2 = {0};
    extra_rec2.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    extra_rec2.uptime_s = 50;
    strncpy(extra_rec2.detail.freeze.queue_name, "extra",
            sizeof(extra_rec2.detail.freeze.queue_name) - 1);
    if (zmk_watchdog_store_append(&extra_rec2) != 0) {
        return -EINVAL;
    }

    if (zmk_watchdog_store_count() != 5) {
        LOG_ERR("expected 5 incidents stored, got %u", zmk_watchdog_store_count());
        return -EINVAL;
    }

    /* First page: 4 incidents, total=5. */
    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_list_incidents_tag;
    req.request_type.list_incidents.start_index = 0;

    cormoran_watchdog_Response resp;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_watchdog_Response_incident_page_tag ||
        resp.response_type.incident_page.total != 5 ||
        resp.response_type.incident_page.incidents_count != 4 ||
        resp.response_type.incident_page.start_index != 0) {
        LOG_ERR("unexpected first page: type=%d total=%u count=%u start=%u",
                resp.which_response_type, resp.response_type.incident_page.total,
                (unsigned int)resp.response_type.incident_page.incidents_count,
                resp.response_type.incident_page.start_index);
        return -EINVAL;
    }

    const cormoran_watchdog_Incident *fatal_wire = &resp.response_type.incident_page.incidents[1];
    if (fatal_wire->diagnostics.size != 80 || fatal_wire->detail.fatal.context.size != 24 ||
        fatal_wire->capture_context.size != ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE ||
        !(fatal_wire->capture_flags & ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID) ||
        memcmp(fatal_wire->capture_context.bytes, fatal_rec.capture_context,
               ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE) ||
        memcmp(fatal_wire->diagnostics.bytes, fatal_rec.diagnostics, 80) ||
        sys_get_le32(fatal_wire->detail.fatal.context.bytes + 20) != 0x10000005U) {
        LOG_ERR("fatal context/diagnostic streaming mismatch");
        return -EINVAL;
    }

    /* Spot-check the first incident (freeze_rec): id assigned by the store,
     * source always 0 in this phase, detail decoded correctly. */
    const cormoran_watchdog_Incident *first = &resp.response_type.incident_page.incidents[0];
    if (first->source != 0 || first->type != cormoran_watchdog_IncidentType_FREEZE ||
        first->which_detail != cormoran_watchdog_Incident_freeze_tag ||
        first->detail.freeze.channel_id != 2 ||
        strcmp(first->detail.freeze.queue_name, "sysworkq") != 0 || first->uptime_s != 10) {
        LOG_ERR("first incident mismatch: source=%u type=%d channel=%u queue=%s uptime=%u",
                first->source, first->type, first->detail.freeze.channel_id,
                first->detail.freeze.queue_name, first->uptime_s);
        return -EINVAL;
    }

    const cormoran_watchdog_Incident *second = &resp.response_type.incident_page.incidents[1];
    /* Independent wire fixture: little-endian metadata, ordered PC/caller,
     * and all unused address slots zero; exercise real nanopb callbacks. */
    const uint8_t expected_trace[36] = {
        1, 2, 5, 2, 0x24, 1, 0, 0x20, 0, 0x10, 0, 0x20, 0x20, 0x10, 0, 0, 0x31, 0x20, 0, 0,
    };
    if (first->detail.freeze.trace.size != sizeof(expected_trace) ||
        memcmp(first->detail.freeze.trace.bytes, expected_trace, sizeof(expected_trace)) != 0) {
        LOG_ERR("freeze trace wire mismatch");
        return -EINVAL;
    }
    if (second->type != cormoran_watchdog_IncidentType_FATAL ||
        second->which_detail != cormoran_watchdog_Incident_fatal_tag ||
        second->detail.fatal.reason != 4 || second->detail.fatal.pc != 0x1000 ||
        second->detail.fatal.lr != 0x2000 ||
        strcmp(second->detail.fatal.thread_name, "main") != 0) {
        LOG_ERR("second incident (fatal) mismatch");
        return -EINVAL;
    }

    const cormoran_watchdog_Incident *third = &resp.response_type.incident_page.incidents[2];
    if (third->type != cormoran_watchdog_IncidentType_RESET_CAUSE ||
        third->which_detail != cormoran_watchdog_Incident_reset_tag ||
        third->detail.reset.cause_bits != 0x12) {
        LOG_ERR("third incident (reset cause) mismatch");
        return -EINVAL;
    }

    const cormoran_watchdog_Incident *fourth = &resp.response_type.incident_page.incidents[3];
    if (fourth->type != cormoran_watchdog_IncidentType_FREEZE ||
        fourth->which_detail != cormoran_watchdog_Incident_freeze_tag ||
        strcmp(fourth->detail.freeze.queue_name, "lowprio_workq") != 0 || fourth->uptime_s != 40 ||
        fourth->detail.freeze.trace.size != 0) {
        LOG_ERR("fourth incident (extra freeze) mismatch");
        return -EINVAL;
    }

    uint32_t first_id = first->id;

    /* Second page: remaining 1 incident. */
    req.request_type.list_incidents.start_index = 4;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.response_type.incident_page.total != 5 ||
        resp.response_type.incident_page.incidents_count != 1 ||
        resp.response_type.incident_page.start_index != 4) {
        LOG_ERR("unexpected second page: total=%u count=%u start=%u",
                resp.response_type.incident_page.total,
                (unsigned int)resp.response_type.incident_page.incidents_count,
                resp.response_type.incident_page.start_index);
        return -EINVAL;
    }

    /* Out-of-range page: past the end, empty but no error. */
    req.request_type.list_incidents.start_index = 10;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.response_type.incident_page.incidents_count != 0 ||
        resp.response_type.incident_page.total != 5) {
        LOG_ERR("unexpected out-of-range page: count=%u total=%u",
                (unsigned int)resp.response_type.incident_page.incidents_count,
                resp.response_type.incident_page.total);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_list_incidents_pagination");

    /* Delete by id (the first incident). */
    cormoran_watchdog_Request del_req = cormoran_watchdog_Request_init_zero;
    del_req.which_request_type = cormoran_watchdog_Request_delete_incidents_tag;
    del_req.request_type.delete_incidents.ids_count = 1;
    del_req.request_type.delete_incidents.ids[0] = first_id;
    del_req.request_type.delete_incidents.all = false;

    if (!call_watchdog_rpc(&del_req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_watchdog_Response_delete_result_tag ||
        resp.response_type.delete_result.deleted != 1) {
        LOG_ERR("unexpected delete-by-id result: type=%d deleted=%u", resp.which_response_type,
                resp.response_type.delete_result.deleted);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != 4) {
        LOG_ERR("expected 4 incidents remaining after delete-by-id, got %u",
                zmk_watchdog_store_count());
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_delete_incidents_by_id");

    /* Delete all: recording_stopped-style cleanup, deleted count matches
     * what remained. */
    cormoran_watchdog_Request del_all_req = cormoran_watchdog_Request_init_zero;
    del_all_req.which_request_type = cormoran_watchdog_Request_delete_incidents_tag;
    del_all_req.request_type.delete_incidents.all = true;

    if (!call_watchdog_rpc(&del_all_req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_watchdog_Response_delete_result_tag ||
        resp.response_type.delete_result.deleted != 4) {
        LOG_ERR("unexpected delete-all result: type=%d deleted=%u", resp.which_response_type,
                resp.response_type.delete_result.deleted);
        return -EINVAL;
    }
    if (zmk_watchdog_store_count() != 0) {
        LOG_ERR("store not empty after delete-all");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_delete_incidents_all");
    return 0;
}

static int test_rpc_recording_stopped_status(void) {
    uint16_t capacity = zmk_watchdog_store_capacity();
    for (uint16_t i = 0; i < capacity; i++) {
        struct zmk_watchdog_incident_record rec = {0};
        rec.type = ZMK_WATCHDOG_INCIDENT_RESET_CAUSE;
        rec.detail.reset.cause_bits = 1;
        if (zmk_watchdog_store_append(&rec) != 0) {
            return -EINVAL;
        }
    }

    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_get_status_tag;

    cormoran_watchdog_Response resp;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.response_type.status.stored != capacity ||
        resp.response_type.status.recording_stopped != true) {
        LOG_ERR("expected recording_stopped status: stored=%u recording_stopped=%d",
                resp.response_type.status.stored, resp.response_type.status.recording_stopped);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_status_recording_stopped");
    return zmk_watchdog_store_delete_all();
}

/*
 * InjectFreeze-via-RPC test (requires CONFIG_ZMK_WATCHDOG_TEST_INJECTION,
 * enabled in tests/studio/native_sim.conf). Mirrors tests/watchdog/'s
 * tests/ztest/watchdog_test.c::test_freeze_detect(), but drives the injection
 * through the RPC handler (InjectTestRequest) instead of calling
 * zmk_watchdog_inject_freeze() directly -- exercising the exact path a real
 * Studio client would use (src/studio/watchdog_request_exec.c's
 * handle_inject_test()).
 */

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT) && IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)

static volatile int rpc_inject_freeze_reboot_calls;

static void rpc_inject_freeze_test_reboot_override(void) { rpc_inject_freeze_reboot_calls++; }

static int test_rpc_inject_freeze(void) {
    rpc_inject_freeze_reboot_calls = 0;
    zmk_watchdog_reboot_set_override(rpc_inject_freeze_test_reboot_override);

    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = cormoran_watchdog_Request_inject_test_tag;
    req.request_type.inject_test.kind = cormoran_watchdog_InjectTestKind_FREEZE_KIND;

    cormoran_watchdog_Response resp;
    if (!call_watchdog_rpc(&req, &resp)) {
        return -EINVAL;
    }

    /* The RPC handler must respond with InjectAck *before* the freeze
     * actually happens (zmk_watchdog_inject_freeze() only submits a
     * k_work and returns immediately -- see DESIGN.md, Studio RPC, split relay and client). */
    if (resp.which_response_type != cormoran_watchdog_Response_inject_ack_tag ||
        resp.response_type.inject_ack.kind != cormoran_watchdog_InjectTestKind_FREEZE_KIND) {
        LOG_ERR("expected InjectAck(FREEZE_KIND), got response type %d", resp.which_response_type);
        return -EINVAL;
    }
    if (rpc_inject_freeze_reboot_calls != 0) {
        LOG_ERR("reboot happened synchronously from the RPC call -- injection must be async");
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_inject_freeze_ack");

    /* CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS is set small in
     * tests/studio/native_sim.conf specifically so this sleep is short. */
    k_sleep(K_MSEC(CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS * 2));

    /* See tests/ztest/watchdog_test.c::test_freeze_detect() for why the
     * "sysworkq" channel must be disarmed now, before restoring the real
     * zmk_watchdog_reboot(): the injected freeze work never returns, so
     * sysworkq stays permanently blocked and the channel would otherwise
     * keep re-firing (and re-rebooting) forever. */
    zmk_watchdog_freeze_disarm_sysworkq_channel_for_test();
    zmk_watchdog_reboot_set_override(NULL);

    if (rpc_inject_freeze_reboot_calls < 1) {
        LOG_ERR("RPC-triggered freeze did not reboot within %dms",
                CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS * 2);
        return -EINVAL;
    }

    int ret = zmk_watchdog_pending_convert();
    if (ret != 0) {
        LOG_ERR("pending_convert after RPC-triggered freeze failed: %d", ret);
        return -EINVAL;
    }

    if (zmk_watchdog_store_count() != 1) {
        LOG_ERR("RPC-triggered freeze incident did not land in store (count=%u)",
                zmk_watchdog_store_count());
        return -EINVAL;
    }

    struct zmk_watchdog_incident_record rec;
    ret = zmk_watchdog_store_get(0, &rec);
    if (ret != 0 || rec.type != ZMK_WATCHDOG_INCIDENT_FREEZE ||
        strcmp(rec.detail.freeze.queue_name, "sysworkq") != 0) {
        LOG_ERR("RPC-triggered freeze incident record mismatch: ret=%d type=%d queue='%s'", ret,
                rec.type, rec.detail.freeze.queue_name);
        return -EINVAL;
    }

    LOG_INF("PASS: watchdog_rpc_inject_freeze_recorded");

    /* Same rationale as watchdog_test.c::test_freeze_detect(): sysworkq is
     * permanently wedged from here on, so end the process now rather than
     * letting the rest of the firmware hang trying to use it. This test
     * therefore runs last (see watchdog_rpc_test_init() below). */
    printk("OWNER_TEST_RESULT watchdog_rpc_test_init=0\n");
    exit(0);
}

#endif /* CONFIG_ZMK_WATCHDOG_FREEZE_DETECT && CONFIG_ZMK_WATCHDOG_TEST_INJECTION */

/*
 * InjectTest-disabled-build error path: when CONFIG_ZMK_WATCHDOG_TEST_INJECTION
 * is off, watchdog_request_exec_handle() must answer InjectTestRequest with
 * the same ErrorResponse any other unsupported request kind gets (see the
 * #else branch in src/studio/watchdog_request_exec.c's
 * watchdog_request_exec_handle()) -- the proto tag stays wired up for a
 * stable wire format, but the handler never actually calls
 * zmk_watchdog_inject_freeze()/_fatal(). This test build always has
 * CONFIG_ZMK_WATCHDOG_TEST_INJECTION=y (see tests/studio/native_sim.conf), so
 * that #else branch cannot be exercised at runtime here -- verified instead
 * by inspection of the #if IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
 * guard in watchdog_request_exec.c, which is the only place InjectTestRequest
 * is ever acted on.
 */

static int test_bounded_names_and_capture_flags(void) {
    struct zmk_watchdog_incident_record rec;
    memset(&rec, 'A', sizeof(rec)); /* No terminator anywhere in the input. */
    rec.version = ZMK_WATCHDOG_RECORD_VERSION;
    rec.type = ZMK_WATCHDOG_INCIDENT_FATAL;
    rec.flags = ZMK_WATCHDOG_CAPTURE_INCOMPLETE;
    cormoran_watchdog_Incident out;
    watchdog_incident_record_to_proto(7, 0, &rec, &out);
    if (out.capture_flags != ZMK_WATCHDOG_CAPTURE_INCOMPLETE ||
        strcmp(out.detail.fatal.thread_name, "AAAAAAAAAAAAAAA") ||
        out.detail.fatal.context.size != 0) {
        return -EINVAL;
    }
    rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    watchdog_incident_record_to_proto(7, 0, &rec, &out);
    if (strcmp(out.detail.freeze.queue_name, "AAAAAAAAAAAAAAA")) {
        return -EINVAL;
    }
    LOG_INF("PASS: watchdog_rpc_bounded_names_and_minimal_flags");
    return 0;
}

static int watchdog_rpc_test_init(void) {
    if (test_bounded_names_and_capture_flags() < 0) {
        return -EINVAL;
    }
    int ret = test_settings_backend_init();
    if (ret < 0) {
        return ret;
    }

    ret = test_notification_authorization();
    if (ret < 0) {
        LOG_ERR("watchdog notification authorization regression");
        exit(1);
    }

    ret = test_rpc_get_status_empty_store();
    if (ret < 0) {
        return ret;
    }

    ret = test_runtime_status_relay_bound();
    if (ret < 0) {
        return ret;
    }

    ret = test_rpc_list_incidents_empty_store();
    if (ret < 0) {
        return ret;
    }

    ret = test_rpc_list_incidents_pagination_and_delete();
    if (ret < 0) {
        return ret;
    }

    ret = test_rpc_recording_stopped_status();
    if (ret < 0) {
        return ret;
    }

    /*
     * Runs last: test_rpc_inject_freeze() permanently wedges the system
     * workqueue (see its own doc comment above), so nothing else can run
     * afterwards.
     */
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT) && IS_ENABLED(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
    ret = test_rpc_inject_freeze();
    if (ret < 0) {
        return ret;
    }
#endif

    return 0;
}

/* Runs at APPLICATION level, same as the store's own settings-handler
 * registration (SETTINGS_STATIC_HANDLER_DEFINE in watchdog_store.c, which
 * registers unconditionally regardless of init order) -- registering the
 * in-RAM settings backend here first, then exercising the RPC handler, is
 * enough because settings_save_one()/settings_call_set_handler() only need
 * a registered store at call time, not at the store module's own init
 * time. */
OWNER_CHECKED_SYS_INIT(watchdog_rpc_test_init, APPLICATION, 99);
