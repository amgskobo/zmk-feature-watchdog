/* SPDX-License-Identifier: MIT */
#pragma once
#include <cormoran/zmk/watchdog_relay_page.h>
#include <cormoran/zmk/watchdog.h>

/* Real production assembler and nanopb envelope, including all missing/invalid
 * extension controls. Static buffers preserve the production workqueue budget. */
static int test_common_context_page_assembly(void) {
    static struct watchdog_relay_page state;
    static cormoran_watchdog_Response response;
    cormoran_watchdog_RelayIncidentChunk core = cormoran_watchdog_RelayIncidentChunk_init_zero;
    core.has_incident = core.has_page_index = true;
    core.incident.id = 7;
    core.incident.capture_flags = ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
    cormoran_watchdog_RelayIncidentChunk done = cormoran_watchdog_RelayIncidentChunk_init_zero;
    done.done = done.has_expected_count = true;
    done.expected_count = 1;
    for (unsigned failure = 0; failure < 9; ++failure) {
        state = (struct watchdog_relay_page){0};
        watchdog_relay_page_core(&state, 1, 42, &core, &response);
        for (unsigned part = 0; part < 3; ++part) {
            cormoran_watchdog_RelayContextChunk chunk =
                cormoran_watchdog_RelayContextChunk_init_zero;
            chunk.incident_id = 7;
            chunk.fragment_index = part;
            chunk.data.size = 64;
            for (unsigned i = 0; i < 64; ++i) {
                chunk.data.bytes[i] = (uint8_t)(part * 64 + i);
            }
            if (failure == 1 && part == 1) {
                continue;
            }
            if (failure == 2 && part == 0) {
                chunk.incident_id = 8;
            }
            if (failure == 3 && part == 0) {
                chunk.page_index = 1;
            }
            if (failure == 4 && part == 0) {
                chunk.fragment_index = 2;
            }
            if (failure == 5 && part == 0) {
                chunk.data.size = 63;
            }
            cormoran_watchdog_RelayResponse envelope = cormoran_watchdog_RelayResponse_init_zero;
            envelope.request_id = 42;
            envelope.which_relay_response_type = cormoran_watchdog_RelayResponse_context_chunk_tag;
            envelope.relay_response_type.context_chunk = chunk;
            uint8_t wire[128];
            pb_ostream_t out = pb_ostream_from_buffer(wire, sizeof(wire));
            if (!pb_encode(&out, cormoran_watchdog_RelayResponse_fields, &envelope)) {
                return -EIO;
            }
            envelope = (cormoran_watchdog_RelayResponse)cormoran_watchdog_RelayResponse_init_zero;
            pb_istream_t in = pb_istream_from_buffer(wire, out.bytes_written);
            if (!pb_decode(&in, cormoran_watchdog_RelayResponse_fields, &envelope)) {
                return -EIO;
            }
            chunk = envelope.relay_response_type.context_chunk;
            watchdog_relay_page_context(&state, failure == 6 && part == 0 ? 2 : 1,
                                        failure == 7 && part == 0 ? 43 : 42, &chunk);
            if (failure == 8 && part == 0) {
                watchdog_relay_page_context(&state, 1, 42, &chunk);
            }
        }
        if (!watchdog_relay_page_core(&state, 1, 42, &done, &response)) {
            return -EINVAL;
        }
        if (failure) {
            if (response.which_response_type != cormoran_watchdog_Response_error_tag) {
                return -EINVAL;
            }
        } else {
            if (response.which_response_type != cormoran_watchdog_Response_incident_page_tag ||
                response.response_type.incident_page.incidents[0].capture_context.size != 192) {
                return -EINVAL;
            }
            for (unsigned i = 0; i < 192; ++i) {
                if (response.response_type.incident_page.incidents[0].capture_context.bytes[i] !=
                    (uint8_t)i) {
                    return -EINVAL;
                }
            }
        }
    }
    printk("PASS: watchdog_common_context_relay_controls=8\n");
    return 0;
}
