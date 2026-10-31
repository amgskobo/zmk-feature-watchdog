/* SPDX-License-Identifier: MIT */
/* Passive public-API adapter. Wrappers forward each operation exactly once. */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/init.h>
#include <zmk/event_manager.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <cormoran/zmk/watchdog_context.h>

#define PROFILE_HOST (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#if PROFILE_HOST
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif
static uint8_t active_profile(void) { return (uint8_t)zmk_ble_active_profile_index(); }
#else
static uint8_t active_profile(void) { return 255; }
#endif

static uint8_t conn_profile(struct bt_conn *conn) {
    if (!conn) {
        return 255;
    }
#if PROFILE_HOST
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
        int profile = zmk_ble_profile_index(info.le.dst);
        return profile < 0 ? 255 : (uint8_t)profile;
    }
#endif
    return 255;
}
static uint16_t conn_handle(struct bt_conn *conn) {
    uint16_t handle = 0xffff;
    if (conn) {
        (void)bt_hci_get_conn_handle(conn, &handle);
    }
    return handle;
}

/* One adapter of the common metadata API. No peer addresses or key material.
 * arg0: handle16, role8, matched-profile8; arg1: event-specific value32. */
static void note_connection(struct bt_conn *conn, uint16_t event, uint32_t value) {
#if defined(CONFIG_ZMK_WATCHDOG_CAPTURE_CONTEXT)
    struct bt_conn_info info;
    uint8_t role = 255;
    if (conn && bt_conn_get_info(conn, &info) == 0) {
        role = info.role;
    }
    uint32_t identity =
        conn_handle(conn) | ((uint32_t)role << 16) | ((uint32_t)conn_profile(conn) << 24);
    zmk_watchdog_operation(1, event, identity, value);
#else
    ARG_UNUSED(conn);
    ARG_UNUSED(event);
    ARG_UNUSED(value);
#endif
}

int __real_bt_unpair(uint8_t id, const bt_addr_le_t *addr);
int __wrap_bt_unpair(uint8_t id, const bt_addr_le_t *addr) {
    uint8_t profile = 255;
#if PROFILE_HOST
    if (addr && bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        int index = zmk_ble_profile_index(addr);
        if (index >= 0) {
            profile = (uint8_t)index;
        }
    }
#endif
    zmk_watchdog_breadcrumb(ZMK_WDG_UNPAIR_BEGIN, profile, id);
    int result = __real_bt_unpair(id, addr);
    zmk_watchdog_breadcrumb(ZMK_WDG_UNPAIR_END, profile, (uint16_t)result);
    return result;
}
int __real_bt_conn_disconnect(struct bt_conn *conn, uint8_t reason);
int __wrap_bt_conn_disconnect(struct bt_conn *conn, uint8_t reason) {
    uint8_t profile = conn_profile(conn);
    zmk_watchdog_breadcrumb(ZMK_WDG_DISCONNECT_REASON, profile, reason);
    zmk_watchdog_breadcrumb(ZMK_WDG_DISCONNECT_BEGIN, profile, conn_handle(conn));
    int result = __real_bt_conn_disconnect(conn, reason);
    zmk_watchdog_breadcrumb(ZMK_WDG_DISCONNECT_END, profile, (uint16_t)result);
    return result;
}
#if PROFILE_HOST
void __real_zmk_ble_clear_bonds(void);
void __wrap_zmk_ble_clear_bonds(void) {
    uint8_t profile = active_profile();
    zmk_watchdog_breadcrumb(ZMK_WDG_CLEAR_BEGIN, profile, 0);
    __real_zmk_ble_clear_bonds();
    zmk_watchdog_breadcrumb(ZMK_WDG_CLEAR_END, profile, 0);
}
static int state_changed(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *profile = as_zmk_ble_active_profile_changed(eh);
    if (profile) {
        zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, profile->index, 0);
    }
    const struct zmk_endpoint_changed *endpoint = as_zmk_endpoint_changed(eh);
    if (endpoint) {
        zmk_watchdog_breadcrumb(ZMK_WDG_ENDPOINT, active_profile(), endpoint->endpoint.transport);
    }
#if IS_ENABLED(CONFIG_ZMK_USB)
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);
    if (usb) {
        zmk_watchdog_breadcrumb(ZMK_WDG_USB, active_profile(), usb->conn_state);
    }
#endif
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(watchdog_diagnostic_state, state_changed);
ZMK_SUBSCRIPTION(watchdog_diagnostic_state, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(watchdog_diagnostic_state, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(watchdog_diagnostic_state, zmk_usb_conn_state_changed);
#endif
#endif

static void connected(struct bt_conn *conn, uint8_t err) {
    note_connection(conn, ZMK_WDG_CONNECTED, err);
    zmk_watchdog_breadcrumb(ZMK_WDG_CONNECTED, conn_profile(conn),
                            err ? (0x8000U | err) : conn_handle(conn));
}
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    note_connection(conn, ZMK_WDG_DISCONNECTED, reason);
    zmk_watchdog_breadcrumb(ZMK_WDG_DISCONNECTED, conn_profile(conn), reason);
}
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    note_connection(conn, ZMK_WDG_SECURITY, ((uint32_t)err << 16) | level);
    zmk_watchdog_breadcrumb(ZMK_WDG_SECURITY, conn_profile(conn), ((uint16_t)err << 8) | level);
}
BT_CONN_CB_DEFINE(watchdog_diagnostic_conn) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};
static int diagnostic_boot(void) {
    zmk_watchdog_breadcrumb(ZMK_WDG_BOOT, active_profile(), 0);
#if PROFILE_HOST
    zmk_watchdog_breadcrumb(ZMK_WDG_PROFILE, active_profile(), 0);
    zmk_watchdog_breadcrumb(ZMK_WDG_ENDPOINT, active_profile(),
                            zmk_endpoint_get_selected().transport);
#if IS_ENABLED(CONFIG_ZMK_USB)
    zmk_watchdog_breadcrumb(ZMK_WDG_USB, active_profile(), zmk_usb_get_conn_state());
#endif
#endif
    return 0;
}
SYS_INIT(diagnostic_boot, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
