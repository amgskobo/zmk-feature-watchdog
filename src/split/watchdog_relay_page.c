/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_relay_page.h>
static void core_to_incident(const cormoran_watchdog_RelayIncidentCore *in,
                             cormoran_watchdog_Incident *out) {
    *out = (cormoran_watchdog_Incident)cormoran_watchdog_Incident_init_zero;
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

bool watchdog_relay_page_core(struct watchdog_relay_page *state, uint32_t source,
                              uint32_t request_id,
                              const cormoran_watchdog_RelayIncidentChunk *chunk,
                              cormoran_watchdog_Response *response) {
    if (!state->active || state->request_id != request_id || state->source != source) {
        *state = (struct watchdog_relay_page){
            .active = true,
            .request_id = request_id,
            .source = source,
            .incidents_count = 0,
        };
    }

    state->total = chunk->total;
    state->start_index = chunk->start_index;

    if (chunk->has_incident) {
        /* Every Incident inside an incident_chunk was built by the
         * peripheral's own watchdog_request_exec_handle() with source == 0
         * (see the doc comment in watchdog_request_exec.c) -- stamp the
         * real source now that it is known, mirroring the top-level
         * PeripheralResponse.source convention. */
        unsigned index = chunk->has_page_index ? chunk->page_index : state->incidents_count;
        if (index >= ARRAY_SIZE(state->incidents) || (state->received & BIT(index))) {
            state->invalid = true;
        } else {
            core_to_incident(&chunk->incident, &state->incidents[index]);
            state->incidents[index].source = source;
            state->received |= BIT(index);
            if (chunk->diagnostics_expected) {
                state->diagnostic_expected |= BIT(index);
            }
            if (chunk->incident.capture_flags & ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID) {
                state->context_expected |= BIT(index);
            }
            state->incidents_count = MAX(state->incidents_count, index + 1U);
        }
    }

    if (!chunk->done) {
        return false;
    }

    *response = (cormoran_watchdog_Response)cormoran_watchdog_Response_init_zero;
    response->which_response_type = cormoran_watchdog_Response_incident_page_tag;
    unsigned count = chunk->has_expected_count ? chunk->expected_count : state->incidents_count;
    for (unsigned i = 0; i < MIN(count, ARRAY_SIZE(state->incidents)); ++i) {
        if ((state->context_expected & BIT(i)) &&
            state->context_fragments[i] != WATCHDOG_RELAY_CONTEXT_COMPLETE_MASK) {
            state->invalid = true;
        }
    }
    if (count > ARRAY_SIZE(state->incidents) || state->invalid ||
        state->received != BIT_MASK(count) ||
        state->diagnostic_received != state->diagnostic_expected) {
        response->which_response_type = cormoran_watchdog_Response_error_tag;
        snprintf(response->response_type.error.message,
                 sizeof(response->response_type.error.message),
                 "Incomplete watchdog relay page; refresh");
        state->active = false;
        return true;
    }
    cormoran_watchdog_IncidentPageResponse *page = &response->response_type.incident_page;
    page->total = state->total;
    page->start_index = state->start_index;
    page->incidents_count = state->incidents_count;
    for (size_t i = 0; i < state->incidents_count; i++) {
        page->incidents[i] = state->incidents[i];
    }

    state->active = false;
    return true;
}

void watchdog_relay_page_context(struct watchdog_relay_page *state, uint32_t source,
                                 uint32_t request_id,
                                 const cormoran_watchdog_RelayContextChunk *chunk) {
    if (!state->active || state->source != source || state->request_id != request_id ||
        chunk->page_index >= state->incidents_count ||
        !(state->received & BIT(chunk->page_index)) ||
        !(state->context_expected & BIT(chunk->page_index))) {
        return;
    }
    unsigned index = chunk->page_index;
    if (chunk->fragment_index >= WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT ||
        chunk->data.size != WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE ||
        state->incidents[index].id != chunk->incident_id ||
        state->context_fragments[index] != BIT_MASK(chunk->fragment_index)) {
        state->invalid = true;
        return;
    }
    memcpy(state->incidents[index].capture_context.bytes +
               chunk->fragment_index * WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE,
           chunk->data.bytes, WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE);
    state->context_fragments[index] |= BIT(chunk->fragment_index);
    if (state->context_fragments[index] == WATCHDOG_RELAY_CONTEXT_COMPLETE_MASK) {
        state->incidents[index].capture_context.size = ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE;
    }
}

/* Extension only attaches to an already received core in the same ordered page.
 * Reject missing/out-of-order/wrong-source/ID chunks; never guess attribution. */
void watchdog_relay_page_diagnostic(struct watchdog_relay_page *state, uint32_t source,
                                    uint32_t request_id,
                                    const cormoran_watchdog_RelayDiagnosticChunk *chunk) {
    if (!state->active || state->source != source || state->request_id != request_id ||
        chunk->page_index >= state->incidents_count ||
        !(state->received & BIT(chunk->page_index)) ||
        !(state->diagnostic_expected & BIT(chunk->page_index))) {
        return;
    }
    cormoran_watchdog_Incident *incident = &state->incidents[chunk->page_index];
    if (incident->id != chunk->incident_id || incident->diagnostics.size ||
        chunk->diagnostics.size != ZMK_WATCHDOG_DIAGNOSTIC_SIZE) {
        return;
    }
    incident->diagnostics.size = chunk->diagnostics.size;
    memcpy(incident->diagnostics.bytes, chunk->diagnostics.bytes, chunk->diagnostics.size);
    if (chunk->diagnostics.size == ZMK_WATCHDOG_DIAGNOSTIC_SIZE) {
        state->diagnostic_received |= BIT(chunk->page_index);
    }
}
