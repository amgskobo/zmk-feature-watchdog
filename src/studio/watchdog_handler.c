/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <cormoran/watchdog/watchdog.pb.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_request_exec.h>
#include <cormoran/zmk/watchdog_studio.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/core.h>
#include <zmk/studio/custom.h>

#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
#include <cormoran/zmk/watchdog_relay.h>
#endif

#include <zephyr/logging/log.h>
#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_handler, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

#define WATCHDOG_SUBSYSTEM_IDENTIFIER "cormoran__watchdog"

static struct zmk_rpc_custom_subsystem_meta watchdog_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-watchdog/"),
/* The incident log requires an unlocked device unless configured otherwise. */
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_UNSECURED)
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
#else
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
#endif
};

static bool watchdog_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                        pb_callback_t *encode_response);

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__watchdog, &watchdog_feature_meta, watchdog_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__watchdog, cormoran_watchdog_Response);

/* --- Notifications --- */

static bool notifications_allowed(void) {
    return IS_ENABLED(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_UNSECURED) ||
           zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
}

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    struct watchdog_studio_notification *notification = *arg;
    /* The first pass runs after the transport wait. Failing a later pass would
     * leave the Studio frame unterminated, so the decision is kept. */
    if (!notification->decided) {
        notification->allowed = notifications_allowed();
        notification->decided = true;
    }
    if (!notification->allowed) {
        return true; /* suppress the content, not the completion of the frame */
    }
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_watchdog_Notification_fields, &notification->payload);
}

/* The registry is fixed at link time, so the index is looked up once. */
static int subsystem_index(void) {
    static int index = -ENOENT;
    if (index >= 0) {
        return index;
    }

    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);
    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsystem;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsystem);
        if (strcmp(subsystem->identifier, WATCHDOG_SUBSYSTEM_IDENTIFIER) == 0) {
            index = (int)i;
            return index;
        }
    }
    return -ENOENT;
}

int watchdog_studio_notify(struct watchdog_studio_notification *notification) {
    if (!notifications_allowed()) {
        return -EACCES;
    }
    int index = subsystem_index();
    if (index < 0) {
        return index;
    }

    notification->decided = false;
    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload =
            {
                .funcs.encode = encode_notification_payload,
                .arg = notification,
            },
    });
}

/* IncidentRecorded: every persisted incident, including the boot-time
 * reset-cause audit and the pending conversion after a freeze/fatal reboot,
 * so the client sees new incidents without polling. */
static K_MUTEX_DEFINE(incident_notification_lock);
static struct watchdog_studio_notification incident_notification;

static void on_incident_appended(uint16_t id, const struct zmk_watchdog_incident_record *rec) {
    k_mutex_lock(&incident_notification_lock, K_FOREVER);

    cormoran_watchdog_Notification *notification = &incident_notification.payload;
    *notification = (cormoran_watchdog_Notification)cormoran_watchdog_Notification_init_zero;
    notification->which_notification_type = cormoran_watchdog_Notification_incident_recorded_tag;
    notification->notification_type.incident_recorded.has_incident = true;
    /* Source 0: the callback fires only for this half's own store. */
    watchdog_incident_record_to_proto(id, 0, rec,
                                      &notification->notification_type.incident_recorded.incident);

    int ret = watchdog_studio_notify(&incident_notification);
    if (ret < 0 && ret != -EACCES) {
        LOG_WRN("Failed to raise watchdog IncidentRecorded notification: %d", ret);
    }

    k_mutex_unlock(&incident_notification_lock);
}

static int watchdog_studio_rpc_init(void) {
    zmk_watchdog_store_set_appended_callback(on_incident_appended);
    return 0;
}

SYS_INIT(watchdog_studio_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* --- Requests --- */

static bool watchdog_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                        pb_callback_t *encode_response) {
    cormoran_watchdog_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__watchdog, encode_response);

    cormoran_watchdog_Request req = cormoran_watchdog_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_watchdog_Request_fields, &req)) {
        LOG_WRN("Failed to decode watchdog request: %s", PB_GET_ERROR(&req_stream));
        watchdog_response_set_error(resp, "Failed to decode request");
        return true;
    }

    if (watchdog_request_source(&req) != 0) {
#if IS_ENABLED(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
        watchdog_relay_dispatch_request(&req, resp);
#else
        watchdog_response_set_error(resp, "Split relay not enabled in this firmware");
#endif
        return true;
    }

    watchdog_request_exec_handle(&req, resp);
    return true;
}
