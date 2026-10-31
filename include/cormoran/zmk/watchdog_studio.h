#pragma once

/**
 * @file watchdog_studio.h
 *
 * @brief Studio notifications of the cormoran__watchdog subsystem
 * (src/studio/watchdog_handler.c). Shared by the IncidentRecorded
 * notification and the split relay's PeripheralResponse, so both follow the
 * same lock rule.
 */

#include <stdbool.h>

#include <cormoran/watchdog/watchdog.pb.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A notification payload and the authorization decided at its first encoder
 * pass. Every pass of one frame then encodes the same thing, so a lock change
 * mid-frame cannot leave the Studio frame unterminated. */
struct watchdog_studio_notification {
    cormoran_watchdog_Notification payload;
    bool decided;
    bool allowed;
};

/** @brief Raises `notification` on this module's Studio subsystem. Returns
 * -EACCES without sending while Studio is locked (unless
 * CONFIG_ZMK_WATCHDOG_STUDIO_RPC_UNSECURED), -ENOENT if the subsystem is not
 * registered, or the raise result. ZMK encodes the payload before this
 * returns; the caller owns the storage and serializes its use. */
int watchdog_studio_notify(struct watchdog_studio_notification *notification);

#ifdef __cplusplus
}
#endif
