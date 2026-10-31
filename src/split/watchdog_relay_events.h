/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Split relay events of the watchdog subsystem. Each half compiles one of
 * watchdog_relay_central.c and watchdog_relay_peripheral.c. */

#include <stdint.h>

#include <zephyr/toolchain.h>
#include <zmk/event_manager.h>

#include <cormoran/watchdog/watchdog.pb.h>

/* Payloads are sized by nanopb's worst-case encoded sizes: every string and
 * repeated field has a max_size/max_count in watchdog.options. */
#define WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE cormoran_watchdog_RelayRequest_size
#define WATCHDOG_RELAY_RESPONSE_PAYLOAD_MAX_SIZE cormoran_watchdog_RelayResponse_size

struct zmk_watchdog_relay_request {
    uint8_t source;
    uint16_t size;
    uint8_t payload[WATCHDOG_RELAY_REQUEST_PAYLOAD_MAX_SIZE];
};

struct zmk_watchdog_relay_response {
    uint8_t source;
    uint16_t size;
    uint8_t payload[WATCHDOG_RELAY_RESPONSE_PAYLOAD_MAX_SIZE];
};

/* A relayed event's size travels in a uint8_t (struct relay_event_header), so
 * 255 bytes is a hard ceiling whatever CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN
 * says. A page is streamed one incident per event to stay under both. */
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_request) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the watchdog relay request");
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_response) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for the watchdog relay response");
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_request) <= 255,
             "the watchdog relay request exceeds the split relay's 255-byte ceiling");
BUILD_ASSERT(sizeof(struct zmk_watchdog_relay_response) <= 255,
             "the watchdog relay response exceeds the split relay's 255-byte ceiling");

ZMK_EVENT_DECLARE(zmk_watchdog_relay_request);
ZMK_EVENT_DECLARE(zmk_watchdog_relay_response);
