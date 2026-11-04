/* Full peripheral relay/executor on a production-sized Cortex-M work stack.
 * Store and event transport are controlled boundaries; no BLE/NVS claim. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log_ctrl.h>
#include <cormoran/zmk/watchdog.h>

#include "../../src/split/watchdog_relay_peripheral.c"
#include "../support/context_relay_assertions.h"

K_THREAD_STACK_DEFINE(relay_stack, 2048);
static struct k_work_q relay_queue;
K_MSGQ_DEFINE(sent, sizeof(struct zmk_watchdog_relay_response), 21, 4);
struct zmk_watchdog_incident_record records[6];
uint16_t stored;
static int failures;
static atomic_t overflow;
static atomic_t hold_transport;
K_SEM_DEFINE(transport_entered, 0, 1);
K_SEM_DEFINE(transport_release, 0, 1);
bool store_ready, previous_available = true;
int read_error_index = -1;
unsigned reads, breadcrumbs, last_breadcrumb;
static uint8_t checkpoint[80];

/* Cortex-M basic + extended FP exception frames require up to 104 bytes.
 * Reserve 128 for those frames, alignment and a small context margin. This
 * is a sampled fixture bound, not a proof for the substituted radio path. */
#define MIN_EXCEPTION_HEADROOM 128

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            printk("FAIL line=%d: %s\n", __LINE__, #condition);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &relay_queue; }
static int fixture_queue_init(void) {
    k_work_queue_start(&relay_queue, relay_stack, K_THREAD_STACK_SIZEOF(relay_stack), 10, NULL);
    return 0;
}
SYS_INIT(fixture_queue_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
bool zmk_watchdog_store_is_ready(void) { return store_ready; }
uint32_t zmk_watchdog_diagnostics_build_tag(void) { return 0x12345678; }
uint32_t zmk_watchdog_diagnostics_runtime_flags(void) { return 0x287; }
uint32_t zmk_watchdog_diagnostics_reset_cause(void) { return 0x10; }
bool zmk_watchdog_diagnostics_previous(uint8_t out[80]) {
    if (previous_available) {
        memcpy(out, checkpoint, 80);
    }
    return previous_available;
}
void zmk_watchdog_breadcrumb(uint8_t event, uint8_t profile, uint16_t value) {
    ARG_UNUSED(value);
    if (profile != 255) {
        atomic_inc(&overflow);
    }
    breadcrumbs++;
    last_breadcrumb = event;
}
void fixture_transport(const void *event, size_t size) {
    if (size != sizeof(struct zmk_watchdog_relay_response) || k_msgq_put(&sent, event, K_NO_WAIT)) {
        atomic_inc(&overflow);
    }
#if defined(CONFIG_WATCHDOG_RELAY_FAIRNESS_TEST)
    /* A bounded send delay lets the real software watchdog timer advance.
     * A higher-priority consumer replenishes the FIFO before this returns. */
    k_sleep(K_MSEC(1));
#endif
}
/* Actual DYA event macros/dispatch run above this transport boundary. Reserve
 * 344 bytes for synchronous downstream stack use: the installed right ELF's
 * BT dispatcher (192) + sender (152), before the real kernel msgq operation.
 * This is an explicit stack-budget model, not a copied BT implementation or
 * a claim that QEMU exercises the radio. Keep it noinline and live across send. */
__attribute__((noinline)) int
zmk_split_peripheral_report_event(const struct zmk_split_transport_peripheral_event *event) {
    volatile uint8_t downstream[344];
    for (unsigned i = 0; i < sizeof(downstream); ++i) {
        downstream[i] = i;
    }
    if (atomic_cas(&hold_transport, 1, 0)) {
        k_sem_give(&transport_entered);
        if (k_sem_take(&transport_release, K_SECONDS(1))) {
            atomic_inc(&overflow);
        }
    }
    if (event->type != ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_RELAY_EVENT ||
        strcmp(event->data.relay_event.event_type, "wdp") != 0 ||
        event->data.relay_event.header.event_data_size !=
            sizeof(struct zmk_watchdog_relay_response)) {
        atomic_inc(&overflow);
        return -EINVAL;
    }
    fixture_transport(event->data.relay_event.event_data,
                      event->data.relay_event.header.event_data_size);
    for (unsigned i = 0; i < sizeof(downstream); ++i) {
        if (downstream[i] != (uint8_t)i) {
            atomic_inc(&overflow);
        }
    }
    return 0;
}
uint16_t zmk_watchdog_store_count(void) { return stored; }
void zmk_watchdog_store_get_status(struct zmk_watchdog_store_status *out) {
    *out = (struct zmk_watchdog_store_status){.capacity = 16, .stored = stored};
}
int zmk_watchdog_store_get_with_id(uint16_t index, struct zmk_watchdog_incident_record *out,
                                   uint16_t *id) {
    reads++;
    if (index == read_error_index) {
        return -EIO;
    }
    if (index >= stored) {
        return -ENOENT;
    }
    *out = records[index];
    *id = index + 100;
    return 0;
}
int zmk_watchdog_store_delete(uint16_t id) {
    ARG_UNUSED(id);
    return -ENOTSUP;
}
int zmk_watchdog_store_delete_all(void) { return -ENOTSUP; }

static void flush_requests(void) {
    /* A work-item flush waits for the invocation it observes, not every
     * future self-submission. Drain the queue so all admitted FIFO entries
     * reach the unchanged response/order assertions below. */
    CHECK(k_work_queue_drain(&relay_queue, false) >= 0);
}

static void request(unsigned sequence, unsigned kind, unsigned start, unsigned expected) {
    cormoran_watchdog_RelayRequest query = cormoran_watchdog_RelayRequest_init_zero;
    query.request_id = sequence;
    query.has_request = true;
    query.request.which_request_type = kind;
    query.request.request_type.list_incidents.start_index = start;
    struct zmk_watchdog_relay_request event = {0};
    pb_ostream_t out = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    CHECK(pb_encode(&out, cormoran_watchdog_RelayRequest_fields, &query));
    event.size = out.bytes_written;
    raise_zmk_watchdog_relay_request(event);
    flush_requests();
    CHECK(atomic_get(&overflow) == 0);
    CHECK(last_breadcrumb == ZMK_WDG_RELAY_WORK_END);
    unsigned cores = 0, extensions = 0, done = 0, responses = 0;
    struct zmk_watchdog_relay_response wire;
    while (k_msgq_get(&sent, &wire, K_NO_WAIT) == 0) {
        cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
        pb_istream_t in = pb_istream_from_buffer(wire.payload, wire.size);
        CHECK(pb_decode(&in, cormoran_watchdog_RelayResponse_fields, &response));
        CHECK(response.request_id == sequence);
        if (response.which_relay_response_type ==
            cormoran_watchdog_RelayResponse_incident_chunk_tag) {
            const cormoran_watchdog_RelayIncidentChunk *chunk =
                &response.relay_response_type.incident_chunk;
            if (chunk->done) {
                CHECK(chunk->total == stored && chunk->start_index == start);
                CHECK(chunk->has_expected_count && chunk->expected_count == expected);
                CHECK(cores == expected && extensions == expected);
                done++;
            } else {
                CHECK(chunk->has_incident && chunk->has_page_index && chunk->page_index == cores);
                CHECK(chunk->incident.id == 100 + start + cores);
                CHECK(chunk->incident.uptime_s == 10 + start + cores);
                CHECK(chunk->diagnostics_expected && done == 0 && extensions == cores);
                cores++;
            }
        } else if (response.which_relay_response_type ==
                   cormoran_watchdog_RelayResponse_diagnostic_chunk_tag) {
            const cormoran_watchdog_RelayDiagnosticChunk *chunk =
                &response.relay_response_type.diagnostic_chunk;
            CHECK(chunk->page_index == extensions &&
                  chunk->incident_id == 100 + start + extensions);
            CHECK(chunk->diagnostics.size == 80 &&
                  chunk->diagnostics.bytes[79] == start + extensions);
            CHECK(cores == extensions + 1 && done == 0);
            extensions++;
        } else {
            CHECK(kind == cormoran_watchdog_Request_get_status_tag);
            CHECK(response.which_relay_response_type ==
                  cormoran_watchdog_RelayResponse_response_tag);
            CHECK(response.relay_response_type.response.which_relay_non_list_response_type ==
                  cormoran_watchdog_RelayNonListResponse_status_tag);
            CHECK(
                response.relay_response_type.response.relay_non_list_response_type.status.stored ==
                stored);
            const cormoran_watchdog_StatusResponse *status =
                &response.relay_response_type.response.relay_non_list_response_type.status;
            CHECK(status->build_tag == 0x12345678 && status->reset_cause == 0x10);
            CHECK(status->previous_diagnostics.size == 80);
            CHECK(memcmp(status->previous_diagnostics.bytes, checkpoint, 80) == 0);
            responses++;
        }
    }
    CHECK(kind == cormoran_watchdog_Request_get_status_tag
              ? responses == 1
              : cores == expected && extensions == expected && done == 1 && responses == 0);
    size_t unused = 0;
    CHECK(k_thread_stack_space_get(&relay_queue.thread, &unused) == 0);
    printk("CASE seq=%u kind=%u start=%u records=%u unused=%u\n", sequence, kind, start, expected,
           (unsigned)unused);
    CHECK(unused >= MIN_EXCEPTION_HEADROOM);
}

static void health_request(void) {
    /* An old client's GetHealth request: Request field 5 (reserved now), with
     * page = 18 inside. The current schema no longer generates it, so the
     * relay event carries the original wire bytes. */
    static const uint8_t legacy[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0x0f, /* request_id = UINT32_MAX */
        0x12, 0x04,                         /* request, 4 bytes */
        0x2a, 0x02,                         /* field 5, 2 bytes */
        0x18, 0x12,                         /* page = 18 */
    };
    struct zmk_watchdog_relay_request event = {0};
    BUILD_ASSERT(sizeof(legacy) <= sizeof(event.payload));
    memcpy(event.payload, legacy, sizeof(legacy));
    event.size = sizeof(legacy);
    raise_zmk_watchdog_relay_request(event);
    flush_requests();
    struct zmk_watchdog_relay_response wire;
    CHECK(k_msgq_get(&sent, &wire, K_NO_WAIT) == 0 && wire.size <= 128);
    cormoran_watchdog_RelayResponse answer = cormoran_watchdog_RelayResponse_init_zero;
    pb_istream_t in = pb_istream_from_buffer(wire.payload, wire.size);
    CHECK(pb_decode(&in, cormoran_watchdog_RelayResponse_fields, &answer));
    CHECK(answer.request_id == UINT32_MAX);
    CHECK(answer.relay_response_type.response.which_relay_non_list_response_type ==
          cormoran_watchdog_RelayNonListResponse_error_tag);
    CHECK(strcmp(answer.relay_response_type.response.relay_non_list_response_type.error.message,
                 "Unsupported or missing watchdog request") == 0);
    CHECK(k_msgq_num_used_get(&sent) == 0 && atomic_get(&overflow) == 0);
    size_t unused;
    CHECK(k_thread_stack_space_get(&relay_queue.thread, &unused) == 0 && unused >= 128);
    printk("RETIRED_HEALTHCASE unsupported event=%u unused=%u\n", wire.size, (unsigned)unused);
}

int report_cases(void);

static void enqueue_status(unsigned sequence) {
    cormoran_watchdog_RelayRequest query = cormoran_watchdog_RelayRequest_init_zero;
    query.request_id = sequence;
    query.has_request = true;
    query.request.which_request_type = cormoran_watchdog_Request_get_status_tag;
    struct zmk_watchdog_relay_request event = {0};
    pb_ostream_t out = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    CHECK(pb_encode(&out, cormoran_watchdog_RelayRequest_fields, &query));
    event.size = out.bytes_written;
    raise_zmk_watchdog_relay_request(event);
}

static void concurrent_requests(void) {
    atomic_set(&hold_transport, 1);
    enqueue_status(7);
    CHECK(k_sem_take(&transport_entered, K_SECONDS(1)) == 0);
    /* The first response remains live in transport while another thread
     * admits a request to the same k_work. Kernel serialization must keep
     * their IDs/status/checkpoint bytes independent. */
    enqueue_status(8);
    k_sem_give(&transport_release);
    flush_requests();
    for (unsigned sequence = 7; sequence <= 8; ++sequence) {
        struct zmk_watchdog_relay_response wire;
        CHECK(k_msgq_get(&sent, &wire, K_NO_WAIT) == 0);
        cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
        pb_istream_t in = pb_istream_from_buffer(wire.payload, wire.size);
        CHECK(pb_decode(&in, cormoran_watchdog_RelayResponse_fields, &response));
        CHECK(response.request_id == sequence);
        CHECK(response.which_relay_response_type == cormoran_watchdog_RelayResponse_response_tag);
        const cormoran_watchdog_RelayNonListResponse *non_list =
            &response.relay_response_type.response;
        CHECK(non_list->which_relay_non_list_response_type ==
              cormoran_watchdog_RelayNonListResponse_status_tag);
        CHECK(non_list->relay_non_list_response_type.status.stored == stored);
        CHECK(non_list->relay_non_list_response_type.status.previous_diagnostics.size == 80);
        CHECK(memcmp(non_list->relay_non_list_response_type.status.previous_diagnostics.bytes,
                     checkpoint, 80) == 0);
    }
    CHECK(k_msgq_num_used_get(&sent) == 0 && atomic_get(&overflow) == 0);
    size_t unused;
    CHECK(k_thread_stack_space_get(&relay_queue.thread, &unused) == 0 && unused >= 128);
    printk("CASE seq=7 concurrent=2 ordered_ids=7,8 unused=%u\n", (unsigned)unused);
}

K_SEM_DEFINE(request_blocked, 0, 1);
K_SEM_DEFINE(request_release, 0, 1);
static void block_requests(struct k_work *work) {
    ARG_UNUSED(work);
    k_sem_give(&request_blocked);
    CHECK(k_sem_take(&request_release, K_SECONDS(1)) == 0);
}
K_WORK_DEFINE(request_blocker, block_requests);

static void expect_status(unsigned sequence) {
    struct zmk_watchdog_relay_response wire = {0};
    CHECK(k_msgq_get(&sent, &wire, K_NO_WAIT) == 0);
    cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
    pb_istream_t input = pb_istream_from_buffer(wire.payload, wire.size);
    CHECK(pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &response));
    CHECK(response.request_id == sequence);
    CHECK(response.which_relay_response_type == cormoran_watchdog_RelayResponse_response_tag);
    const cormoran_watchdog_RelayNonListResponse *result = &response.relay_response_type.response;
    CHECK(result->which_relay_non_list_response_type ==
          cormoran_watchdog_RelayNonListResponse_status_tag);
    CHECK(result->relay_non_list_response_type.status.stored == stored);
    CHECK(result->relay_non_list_response_type.status.previous_diagnostics.size == 80);
    CHECK(memcmp(result->relay_non_list_response_type.status.previous_diagnostics.bytes, checkpoint,
                 80) == 0);
}

static void queued_requests(void) {
    k_work_submit_to_queue(&relay_queue, &request_blocker);
    CHECK(k_sem_take(&request_blocked, K_SECONDS(1)) == 0);
    enqueue_status(110);
    enqueue_status(111);
    k_sem_give(&request_release);
    flush_requests();
    expect_status(110);
    expect_status(111);
    CHECK(k_msgq_num_used_get(&sent) == 0 && atomic_get(&overflow) == 0);
    printk("REQUESTCASE queued-two ordered_ids=110,111\n");
}

static void queued_failure_and_page(void) {
    k_work_submit_to_queue(&relay_queue, &request_blocker);
    CHECK(k_sem_take(&request_blocked, K_SECONDS(1)) == 0);
    struct zmk_watchdog_relay_request malformed = {.size = 1, .payload = {0xff}};
    raise_zmk_watchdog_relay_request(malformed);
    cormoran_watchdog_RelayRequest query = cormoran_watchdog_RelayRequest_init_zero;
    query.request_id = 501;
    query.has_request = true;
    query.request.which_request_type = cormoran_watchdog_Request_list_incidents_tag;
    struct zmk_watchdog_relay_request page = {0};
    pb_ostream_t output = pb_ostream_from_buffer(page.payload, sizeof(page.payload));
    CHECK(pb_encode(&output, cormoran_watchdog_RelayRequest_fields, &query));
    page.size = output.bytes_written;
    raise_zmk_watchdog_relay_request(page);
    enqueue_status(502);
    k_sem_give(&request_release);
    flush_requests();
    unsigned packets = 0, cores = 0, diagnostics = 0, done = 0;
    while (k_msgq_num_used_get(&sent) > 1) {
        struct zmk_watchdog_relay_response wire;
        CHECK(k_msgq_get(&sent, &wire, K_NO_WAIT) == 0);
        cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
        pb_istream_t input = pb_istream_from_buffer(wire.payload, wire.size);
        CHECK(pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &response));
        CHECK(response.request_id == 501);
        ++packets;
        if (response.which_relay_response_type ==
            cormoran_watchdog_RelayResponse_incident_chunk_tag) {
            if (response.relay_response_type.incident_chunk.done)
                ++done;
            else
                ++cores;
        } else if (response.which_relay_response_type ==
                   cormoran_watchdog_RelayResponse_diagnostic_chunk_tag) {
            ++diagnostics;
        } else
            CHECK(false);
    }
    CHECK(packets == 9 && cores == 4 && diagnostics == 4 && done == 1);
    expect_status(502);
    CHECK(k_msgq_num_used_get(&sent) == 0 && atomic_get(&overflow) == 0);
    printk("REQUESTCASE malformed-page-tail page=501 packets=9 status=502\n");
}

static void running_three_requests(void) {
    atomic_set(&hold_transport, 1);
    enqueue_status(201);
    CHECK(k_sem_take(&transport_entered, K_SECONDS(1)) == 0);
    enqueue_status(202);
    enqueue_status(203);
    k_sem_give(&transport_release);
    flush_requests();
    expect_status(201);
    expect_status(202);
    expect_status(203);
    CHECK(k_msgq_num_used_get(&sent) == 0 && atomic_get(&overflow) == 0);
    size_t unused;
    CHECK(k_thread_stack_space_get(&relay_queue.thread, &unused) == 0 && unused >= 128);
    printk("REQUESTCASE running-three ordered_ids=201,202,203 unused=%u\n", (unsigned)unused);
}

K_THREAD_STACK_DEFINE(request_producer_stack, 2048);
static struct k_thread request_producer;
static size_t producer_unused;
static void overflow_request(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    enqueue_status(404);
    CHECK(k_thread_stack_space_get(k_current_get(), &producer_unused) == 0);
    CHECK(producer_unused >= MIN_EXCEPTION_HEADROOM);
}

static void full_request_queue(void) {
    k_work_submit_to_queue(&relay_queue, &request_blocker);
    CHECK(k_sem_take(&request_blocked, K_SECONDS(1)) == 0);
    /* Independent public bound: four pending requests, plus the worker's own
     * immutable current input. A fifth pending request gets a correlated error. */
    for (unsigned id = 400; id < 404; ++id)
        enqueue_status(id);
    k_thread_create(&request_producer, request_producer_stack,
                    K_THREAD_STACK_SIZEOF(request_producer_stack), overflow_request, NULL, NULL,
                    NULL, 5, 0, K_NO_WAIT);
    CHECK(k_thread_join(&request_producer, K_SECONDS(1)) == 0);
    struct zmk_watchdog_relay_response wire = {0};
    CHECK(k_msgq_get(&sent, &wire, K_NO_WAIT) == 0);
    cormoran_watchdog_RelayResponse response = cormoran_watchdog_RelayResponse_init_zero;
    pb_istream_t input = pb_istream_from_buffer(wire.payload, wire.size);
    CHECK(pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &response));
    CHECK(response.request_id == 404);
    CHECK(response.which_relay_response_type == cormoran_watchdog_RelayResponse_response_tag);
    CHECK(response.relay_response_type.response.which_relay_non_list_response_type ==
          cormoran_watchdog_RelayNonListResponse_error_tag);
    CHECK(strstr(response.relay_response_type.response.relay_non_list_response_type.error.message,
                 "busy") != NULL);
    k_sem_give(&request_release);
    flush_requests();
    for (unsigned id = 400; id < 404; ++id)
        expect_status(id);
    enqueue_status(405);
    flush_requests();
    expect_status(405);
    CHECK(k_msgq_num_used_get(&sent) == 0 && atomic_get(&overflow) == 0);
    printk("REQUESTCASE full correlated_busy=404 recovered=405 producer_unused=%u\n",
           (unsigned)producer_unused);
}

static void common_context_page(void) {
    stored = 4;
    store_ready = true;
    read_error_index = -1;
    memset(records, 0, sizeof(records));
    for (unsigned i = 0; i < stored; ++i) {
        records[i].version = ZMK_WATCHDOG_RECORD_VERSION;
        records[i].type = i % 3;
        records[i].uptime_s = 10 + i;
        records[i].diagnostics[0] = 1;
        records[i].flags |= ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
        for (unsigned j = 0; j < ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE; ++j) {
            records[i].capture_context[j] = (uint8_t)(j ^ i);
        }
    }
    cormoran_watchdog_RelayRequest query = cormoran_watchdog_RelayRequest_init_zero;
    query.request_id = 91;
    query.has_request = true;
    query.request.which_request_type = cormoran_watchdog_Request_list_incidents_tag;
    struct zmk_watchdog_relay_request event = {0};
    pb_ostream_t output = pb_ostream_from_buffer(event.payload, sizeof(event.payload));
    CHECK(pb_encode(&output, cormoran_watchdog_RelayRequest_fields, &query));
    event.size = output.bytes_written;
    raise_zmk_watchdog_relay_request(event);
    flush_requests();
    CHECK(!atomic_get(&overflow));
    static struct watchdog_relay_page page;
    static cormoran_watchdog_Response assembled;
    page = (struct watchdog_relay_page){0};
    unsigned packets = 0, fragments = 0, completed = 0;
    struct zmk_watchdog_relay_response wire;
    while (k_msgq_get(&sent, &wire, K_NO_WAIT) == 0) {
        CHECK(wire.size <= 128);
        cormoran_watchdog_RelayResponse decoded = cormoran_watchdog_RelayResponse_init_zero;
        pb_istream_t input = pb_istream_from_buffer(wire.payload, wire.size);
        CHECK(pb_decode(&input, cormoran_watchdog_RelayResponse_fields, &decoded));
        CHECK(decoded.request_id == 91);
        ++packets;
        switch (decoded.which_relay_response_type) {
        case cormoran_watchdog_RelayResponse_incident_chunk_tag:
            if (watchdog_relay_page_core(&page, 1, 91, &decoded.relay_response_type.incident_chunk,
                                         &assembled)) {
                ++completed;
            }
            break;
        case cormoran_watchdog_RelayResponse_diagnostic_chunk_tag:
            watchdog_relay_page_diagnostic(&page, 1, 91,
                                           &decoded.relay_response_type.diagnostic_chunk);
            break;
        case cormoran_watchdog_RelayResponse_context_chunk_tag:
            ++fragments;
            watchdog_relay_page_context(&page, 1, 91, &decoded.relay_response_type.context_chunk);
            break;
        default:
            CHECK(false);
            break;
        }
    }
    CHECK(packets == 21 && fragments == 12 && completed == 1);
    CHECK(assembled.which_response_type == cormoran_watchdog_Response_incident_page_tag &&
          assembled.response_type.incident_page.incidents_count == 4);
    for (unsigned i = 0; i < 4; ++i) {
        const cormoran_watchdog_Incident *item =
            &assembled.response_type.incident_page.incidents[i];
        CHECK(item->id == 100 + i && item->source == 1 && item->capture_context.size == 192);
        CHECK(!memcmp(item->capture_context.bytes, records[i].capture_context, 192));
    }
    size_t unused;
    CHECK(k_thread_stack_space_get(&relay_queue.thread, &unused) == 0 &&
          unused >= MIN_EXCEPTION_HEADROOM);
    printk("CONTEXT_RELAY_COMPLETE records=4 fragments=12 packets=21 unused=%u\n",
           (unsigned)unused);
}

int report_initial_deadline(void);

int main(void) {
    failures += report_initial_deadline();
    checkpoint[0] = 1;
    checkpoint[1] = 7;
    checkpoint[2] = 4;
    sys_put_le32(0x87654321, checkpoint + 4);
    for (unsigned i = 0; i < 7; ++i) {
        sys_put_le32(100 + i, checkpoint + 24 + 8 * i);
        checkpoint[28 + 8 * i] = 30 + i;
    }
    printk("BOUNDARY Cortex-M real workqueue stack=2048; actual DYA event dispatch; downstream "
           "budget=344\n");
    request(1, cormoran_watchdog_Request_get_status_tag, 0, 0);
    request(2, cormoran_watchdog_Request_list_incidents_tag, 0, 0);
    stored = ARRAY_SIZE(records);
    for (unsigned i = 0; i < stored; i++) {
        records[i].version = 3;
        records[i].type = i % 3;
        records[i].uptime_s = 10 + i;
        memset(records[i].diagnostics, i, 80);
        records[i].diagnostics[0] = 1;
        if (records[i].type == ZMK_WATCHDOG_INCIDENT_FREEZE) {
            strcpy(records[i].detail.freeze.queue_name, "lowprio_workq");
            records[i].detail.freeze.trace_flags = ZMK_WATCHDOG_TRACE_PARTIAL;
        } else if (records[i].type == ZMK_WATCHDOG_INCIDENT_FATAL) {
            strcpy(records[i].detail.fatal.thread_name, "lowprio_workq");
            records[i].flags = 1;
        }
    }
    request(3, cormoran_watchdog_Request_get_status_tag, 0, 0);
    request(4, cormoran_watchdog_Request_list_incidents_tag, 0, 4);
    request(5, cormoran_watchdog_Request_list_incidents_tag, 4, 2);
    request(6, cormoran_watchdog_Request_list_incidents_tag, 6, 0);
    concurrent_requests();
    queued_requests();
    queued_failure_and_page();
    running_three_requests();
    full_request_queue();
    health_request();
    CHECK(breadcrumbs > 30);
    failures += report_cases();
    CHECK(test_common_context_page_assembly() == 0);
    common_context_page();
    printk("RESULT: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return 0;
}
