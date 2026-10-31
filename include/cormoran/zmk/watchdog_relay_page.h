/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/sys/util.h>
#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>
#define WATCHDOG_RELAY_INCIDENT_ACCUMULATOR_MAX_COUNT 4
#define WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE 64U
#define WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT                                                      \
    (ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE / WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE)
#define WATCHDOG_RELAY_CONTEXT_COMPLETE_MASK BIT_MASK(WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT)
#define WATCHDOG_RELAY_PAGE_MAX_PACKETS                                                            \
    (WATCHDOG_RELAY_INCIDENT_ACCUMULATOR_MAX_COUNT *                                               \
         (2U + WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT) +                                            \
     1U)

BUILD_ASSERT(ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE % WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE == 0);
BUILD_ASSERT(WATCHDOG_RELAY_CONTEXT_FRAGMENT_COUNT <= 8);
BUILD_ASSERT(WATCHDOG_RELAY_CONTEXT_FRAGMENT_SIZE ==
             sizeof(((cormoran_watchdog_RelayContextChunk *)NULL)->data.bytes));

struct watchdog_relay_page {
    bool active;
    uint32_t request_id;
    uint32_t source;
    uint32_t total;
    uint32_t start_index;
    cormoran_watchdog_Incident incidents[WATCHDOG_RELAY_INCIDENT_ACCUMULATOR_MAX_COUNT];
    size_t incidents_count;
    uint8_t received, diagnostic_expected, diagnostic_received;
    uint8_t context_expected;
    uint8_t context_fragments[WATCHDOG_RELAY_INCIDENT_ACCUMULATOR_MAX_COUNT];
    bool invalid;
};

BUILD_ASSERT(WATCHDOG_RELAY_INCIDENT_ACCUMULATOR_MAX_COUNT ==
                 ARRAY_SIZE(((cormoran_watchdog_IncidentPageResponse *)NULL)->incidents),
             "WATCHDOG_RELAY_INCIDENT_ACCUMULATOR_MAX_COUNT must match watchdog.options' "
             "IncidentPageResponse.incidents max_count");

bool watchdog_relay_page_core(struct watchdog_relay_page *state, uint32_t source,
                              uint32_t request_id,
                              const cormoran_watchdog_RelayIncidentChunk *chunk,
                              cormoran_watchdog_Response *response);
void watchdog_relay_page_diagnostic(struct watchdog_relay_page *state, uint32_t source,
                                    uint32_t request_id,
                                    const cormoran_watchdog_RelayDiagnosticChunk *chunk);
void watchdog_relay_page_context(struct watchdog_relay_page *state, uint32_t source,
                                 uint32_t request_id,
                                 const cormoran_watchdog_RelayContextChunk *chunk);
