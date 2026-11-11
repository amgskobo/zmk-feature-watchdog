/* Central production dispatch, real nanopb/event manager/kernel. Transport
 * availability and configured peer capacity are controlled boundaries. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zmk/split/central.h>
/* Model both capacities without enabling a radio or a wired driver. Real
 * role/configuration selection remains covered by the board build matrix. */
#undef ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT
#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT PEER_COUNT
#include "../../src/split/watchdog_relay_central.c"

static unsigned failures, sends;
static int transport_error;
static cormoran_watchdog_RelayRequest sent_request;
static cormoran_watchdog_Response response;
static bool fixture_notifications;
static atomic_t fixture_block_notification;
static int fixture_notification_error;
static unsigned fixture_notification_count;
static uint32_t fixture_notification_ids[8];
static struct watchdog_studio_notification *fixture_notification;
static uint32_t fixture_notification_hash;
static uint8_t fixture_notification_wire[4096];
static cormoran_watchdog_Notification fixture_decoded_notification;
static cormoran_watchdog_RelayResponse fixture_relay;
K_SEM_DEFINE(fixture_notification_entered, 0, 1);
K_SEM_DEFINE(fixture_notification_release, 0, 1);

#define CHECK(test)                                                                                \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            printk("FAIL line=%d: %s\n", __LINE__, #test);                                         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &k_sys_work_q; }
static uint32_t fixture_hash(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}
int watchdog_studio_notify(struct watchdog_studio_notification *notification) {
    if (!fixture_notifications) {
        return -EACCES;
    }
    CHECK(notification->payload.which_notification_type ==
          cormoran_watchdog_Notification_peripheral_response_tag);
    const cormoran_watchdog_PeripheralResponse *pr =
        &notification->payload.notification_type.peripheral_response;
    CHECK(pr->source == 1 && pr->has_response);
    CHECK(fixture_notification_count < ARRAY_SIZE(fixture_notification_ids));
    if (fixture_notification_count < ARRAY_SIZE(fixture_notification_ids)) {
        fixture_notification_ids[fixture_notification_count++] = pr->request_id;
    }
    if (atomic_cas(&fixture_block_notification, 1, 0)) {
        fixture_notification = notification;
        fixture_notification_hash =
            fixture_hash(&notification->payload, sizeof(notification->payload));
        k_sem_give(&fixture_notification_entered);
        CHECK(k_sem_take(&fixture_notification_release, K_SECONDS(2)) == 0);
        CHECK(fixture_hash(&notification->payload, sizeof(notification->payload)) ==
              fixture_notification_hash);
    }
    if (fixture_notification_error) {
        return fixture_notification_error;
    }
    /* Model the selected core's synchronous sizing/output use, after its
     * transport wait. The producer must own the payload until both return. */
    size_t encoded_size = 0;
    CHECK(pb_get_encoded_size(&encoded_size, cormoran_watchdog_Notification_fields,
                              &notification->payload));
    pb_ostream_t output =
        pb_ostream_from_buffer(fixture_notification_wire, sizeof(fixture_notification_wire));
    CHECK(pb_encode(&output, cormoran_watchdog_Notification_fields, &notification->payload));
    CHECK(output.bytes_written == encoded_size);
    fixture_decoded_notification =
        (cormoran_watchdog_Notification)cormoran_watchdog_Notification_init_zero;
    pb_istream_t input = pb_istream_from_buffer(fixture_notification_wire, output.bytes_written);
    CHECK(pb_decode(&input, cormoran_watchdog_Notification_fields, &fixture_decoded_notification));
    const cormoran_watchdog_PeripheralResponse *decoded =
        &fixture_decoded_notification.notification_type.peripheral_response;
    CHECK(decoded->source == 1 && decoded->has_response && decoded->request_id == pr->request_id);
    if (pr->request_id == 600) {
        CHECK(decoded->response.which_response_type ==
              cormoran_watchdog_Response_incident_page_tag);
        const cormoran_watchdog_IncidentPageResponse *page =
            &decoded->response.response_type.incident_page;
        CHECK(page->total == 16 && page->start_index == 7 && page->incidents_count == 4);
        for (unsigned i = 0; i < page->incidents_count; i++) {
            const cormoran_watchdog_Incident *incident = &page->incidents[i];
            CHECK(incident->id == 700 + i && incident->boot_ordinal == 0x20000000U + i &&
                  incident->uptime_s == 0x10000000U + i);
            CHECK(incident->diagnostics.size == ZMK_WATCHDOG_DIAGNOSTIC_SIZE &&
                  incident->capture_context.size == ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE);
            for (unsigned j = 0; j < incident->diagnostics.size; j++) {
                CHECK(incident->diagnostics.bytes[j] == (uint8_t)(17 * i + j));
            }
            for (unsigned j = 0; j < incident->capture_context.size; j++) {
                CHECK(incident->capture_context.bytes[j] == (uint8_t)(31 * i + j));
            }
        }
    } else {
        CHECK(decoded->response.which_response_type == cormoran_watchdog_Response_status_tag);
        CHECK(decoded->response.response_type.status.capacity == 16 &&
              decoded->response.response_type.status.stored == 16 &&
              decoded->response.response_type.status.build_tag == 0x12345678 &&
              decoded->response.response_type.status.previous_diagnostics.size == 0);
    }
    return 0;
}
void watchdog_response_set_error(cormoran_watchdog_Response *out, const char *message) {
    out->which_response_type = cormoran_watchdog_Response_error_tag;
    snprintf(out->response_type.error.message, sizeof(out->response_type.error.message), "%s",
             message);
}
int zmk_split_central_send_relay_event(struct zmk_split_relay_event_payload *payload) {
    sends++;
    CHECK(strcmp(payload->event_type, "wdq") == 0);
    CHECK(payload->header.event_type_size == 3);
    CHECK(payload->header.event_data_size == sizeof(struct zmk_watchdog_relay_request));
    struct zmk_watchdog_relay_request event;
    memcpy(&event, payload->event_data, sizeof(event));
    CHECK(event.source == ZMK_RELAY_EVENT_SOURCE_SELF);
    CHECK(event.size <= sizeof(event.payload));
    sent_request = (cormoran_watchdog_RelayRequest)cormoran_watchdog_RelayRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(event.payload, event.size);
    CHECK(pb_decode(&stream, cormoran_watchdog_RelayRequest_fields, &sent_request));
    return transport_error;
}
static void call(unsigned kind, uint32_t source, bool accepted) {
    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;
    req.which_request_type = kind;
    switch (kind) {
    case cormoran_watchdog_Request_get_status_tag:
        req.request_type.get_status.source = source;
        break;
    case cormoran_watchdog_Request_list_incidents_tag:
        req.request_type.list_incidents.source = source;
        req.request_type.list_incidents.start_index = 7;
        break;
    case cormoran_watchdog_Request_delete_incidents_tag:
        req.request_type.delete_incidents.source = source;
        req.request_type.delete_incidents.all = true;
        break;
    }
    sends = 0;
    memset(&response, 0xA5, sizeof(response));
    watchdog_relay_dispatch_request(&req, &response);
    if (accepted) {
        CHECK(sends == 1);
        CHECK(response.which_response_type == cormoran_watchdog_Response_deferred_tag);
        CHECK(response.response_type.deferred.request_id != 0);
        CHECK(sent_request.request_id == response.response_type.deferred.request_id);
        CHECK(sent_request.has_request && sent_request.request.which_request_type == kind);
        if (kind == cormoran_watchdog_Request_delete_incidents_tag) {
            CHECK(sent_request.request.request_type.delete_incidents.source == source);
            CHECK(sent_request.request.request_type.delete_incidents.all);
        }
    } else {
        CHECK(response.which_response_type == cormoran_watchdog_Response_error_tag);
        CHECK(response.response_type.error.message[0]);
        CHECK(sends == (transport_error && PEER_COUNT == 1 && source == 1 ? 1 : 0));
    }
}

static void fixture_enqueue_response(void) {
    struct zmk_watchdog_relay_response event = {.source = 1};
    pb_ostream_t output = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    CHECK(pb_encode(&output, cormoran_watchdog_RelayResponse_fields, &fixture_relay));
    event.size = output.bytes_written;
    raise_zmk_watchdog_relay_response(event);
}

static void fixture_flush_responses(void) {
    struct k_work_sync sync;
    k_work_flush(&watchdog_relay_response_process_work, &sync);
}

static void fixture_core(unsigned index, bool done) {
    fixture_relay = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
    fixture_relay.request_id = 600;
    fixture_relay.which_relay_response_type = cormoran_watchdog_RelayResponse_incident_chunk_tag;
    cormoran_watchdog_RelayIncidentChunk *chunk = &fixture_relay.relay_response_type.incident_chunk;
    chunk->total = 16;
    chunk->start_index = 7;
    chunk->done = done;
    if (done) {
        chunk->has_expected_count = true;
        chunk->expected_count = 4;
    } else {
        chunk->has_incident = true;
        chunk->has_page_index = true;
        chunk->page_index = index;
        chunk->diagnostics_expected = true;
        chunk->incident.id = 700 + index;
        chunk->incident.boot_ordinal = 0x20000000U + index;
        chunk->incident.uptime_s = 0x10000000U + index;
        chunk->incident.capture_flags = ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
        chunk->incident.type = cormoran_watchdog_IncidentType_FATAL;
        chunk->incident.which_detail = cormoran_watchdog_RelayIncidentCore_fatal_tag;
        chunk->incident.detail.fatal.reason = index + 1;
    }
    fixture_enqueue_response();
}

static void fixture_extensions(unsigned index) {
    fixture_relay = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
    fixture_relay.request_id = 600;
    fixture_relay.which_relay_response_type = cormoran_watchdog_RelayResponse_diagnostic_chunk_tag;
    cormoran_watchdog_RelayDiagnosticChunk *diagnostic =
        &fixture_relay.relay_response_type.diagnostic_chunk;
    diagnostic->page_index = index;
    diagnostic->incident_id = 700 + index;
    diagnostic->diagnostics.size = ZMK_WATCHDOG_DIAGNOSTIC_SIZE;
    for (unsigned j = 0; j < diagnostic->diagnostics.size; j++) {
        diagnostic->diagnostics.bytes[j] = 17 * index + j;
    }
    fixture_enqueue_response();
    for (unsigned part = 0; part < WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT; part++) {
        fixture_relay = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
        fixture_relay.request_id = 600;
        fixture_relay.which_relay_response_type = cormoran_watchdog_RelayResponse_context_chunk_tag;
        cormoran_watchdog_RelayContextChunk *context =
            &fixture_relay.relay_response_type.context_chunk;
        context->page_index = index;
        context->incident_id = 700 + index;
        context->fragment_index = part;
        context->data.size = WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE;
        for (unsigned j = 0; j < context->data.size; j++) {
            context->data.bytes[j] = 31 * index + part * WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE + j;
        }
        fixture_enqueue_response();
    }
}

static void fixture_status(unsigned request_id) {
    fixture_relay = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
    fixture_relay.request_id = request_id;
    fixture_relay.which_relay_response_type = cormoran_watchdog_RelayResponse_response_tag;
    cormoran_watchdog_RelayNonListResponse *response = &fixture_relay.relay_response_type.response;
    response->which_relay_non_list_response_type =
        cormoran_watchdog_RelayNonListResponse_status_tag;
    response->relay_non_list_response_type.status.capacity = 16;
    response->relay_non_list_response_type.status.stored = 16;
    response->relay_non_list_response_type.status.build_tag = 0x12345678;
    fixture_enqueue_response();
}

static void fixture_notification_cases(void) {
    fixture_notifications = true;
    fixture_core(0, false);
    fixture_flush_responses();
    CHECK(fixture_notification_count == 0);
    atomic_set(&fixture_block_notification, 1);
    fixture_status(601);
    CHECK(k_sem_take(&fixture_notification_entered, K_SECONDS(2)) == 0);
    int ownership = k_mutex_lock(&peripheral_response_notification_lock, K_NO_WAIT);
    CHECK(ownership == -EBUSY);
    if (ownership == 0) {
        k_mutex_unlock(&peripheral_response_notification_lock);
    }
    /* Twenty remaining packets fit the unchanged21-packet FIFO while the
     * notification encoder is blocked. A non-list reply must not replace the
     * incomplete independent page accumulator. */
    fixture_extensions(0);
    for (unsigned i = 1; i < 4; i++) {
        fixture_core(i, false);
        fixture_extensions(i);
    }
    fixture_core(0, true);
    CHECK(k_msgq_num_used_get(&watchdog_relay_response_queue) == 20);
    CHECK(fixture_hash(&fixture_notification->payload, sizeof(fixture_notification->payload)) ==
          fixture_notification_hash);
    k_sem_give(&fixture_notification_release);
    fixture_flush_responses();
    CHECK(fixture_notification_count == 2 && fixture_notification_ids[0] == 601 &&
          fixture_notification_ids[1] == 600);
    const int errors[] = {-EACCES, -EIO, 0};
    for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {
        fixture_notification_error = errors[i];
        fixture_status(602 + i);
        fixture_flush_responses();
        CHECK(k_mutex_lock(&peripheral_response_notification_lock, K_NO_WAIT) == 0);
        k_mutex_unlock(&peripheral_response_notification_lock);
        CHECK(fixture_notification_ids[2 + i] == 602 + i);
    }
    CHECK(fixture_notification_count == 5 &&
          k_msgq_num_used_get(&watchdog_relay_response_queue) == 0);
    printk("CENTRAL_NOTIFICATION PASS groups=6 notifications=5 queued=20 capacity=21\n");
}
int main(void) {
    const unsigned kinds[] = {cormoran_watchdog_Request_get_status_tag,
                              cormoran_watchdog_Request_list_incidents_tag,
                              cormoran_watchdog_Request_delete_incidents_tag};
    for (unsigned i = 0; i < ARRAY_SIZE(kinds); ++i) {
        call(kinds[i], 1, PEER_COUNT == 1);
        call(kinds[i], 0, false);
        call(kinds[i], 2, false);
        call(kinds[i], UINT32_MAX, false);
    }
    call(0, 1, false);
    call(cormoran_watchdog_Request_inject_test_tag, 1, false);
    if (PEER_COUNT == 1) {
        for (unsigned i = 0; i < ARRAY_SIZE(kinds); ++i) {
            transport_error = -ENODEV;
            call(kinds[i], 1, false);
            transport_error = -EIO;
            call(kinds[i], 1, false);
        }
        transport_error = 0;
        next_relay_request_id = UINT32_MAX;
        call(kinds[0], 1, true);
        CHECK(response.response_type.deferred.request_id == UINT32_MAX);
        call(kinds[0], 1, true);
        CHECK(response.response_type.deferred.request_id == 1);
        fixture_notification_cases();
    }
    size_t unused = 0;
    CHECK(k_thread_stack_space_get(k_current_get(), &unused) == 0);
    CHECK(unused >= 128);
    printk("CENTRAL capacity=%d unused=%zu failures=%u\n", PEER_COUNT, unused, failures);
    printk("RESULT: %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    exit(failures ? 99 : 0);
}
