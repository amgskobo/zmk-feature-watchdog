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

#define CHECK(test)                                                                                \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            printk("FAIL line=%d: %s\n", __LINE__, #test);                                         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &k_sys_work_q; }
int watchdog_studio_notify(struct watchdog_studio_notification *notification) {
    ARG_UNUSED(notification);
    return -EACCES;
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
    }
    size_t unused = 0;
    CHECK(k_thread_stack_space_get(k_current_get(), &unused) == 0);
    CHECK(unused >= 128);
    printk("CENTRAL capacity=%d unused=%zu failures=%u\n", PEER_COUNT, unused, failures);
    printk("RESULT: %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    exit(failures ? 99 : 0);
}
