/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Incident record shared by the store, the retained-RAM pending slot and the
 * Studio RPC / split relay layers. It is packed and fixed-size (336 bytes;
 * the split relay streams the 64-byte core and the diagnostic extension
 * separately) and has no 64-bit fields, which nanopb is built without.
 */

#define ZMK_WATCHDOG_RECORD_VERSION 4
#define ZMK_WATCHDOG_RECORD_V3_SIZE 144U
#define ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE 192U
#define ZMK_WATCHDOG_RECORD_V2_SIZE 64U
#define ZMK_WATCHDOG_DIAGNOSTIC_SIZE 80U
#define ZMK_WATCHDOG_RECORD_V1_SIZE 40
#define ZMK_WATCHDOG_TRACE_ADDRS 6
#define ZMK_WATCHDOG_TRACE_WIRE_SIZE 36
/* Record flags (context/minimal from v3; common extension from v4). */
#define ZMK_WATCHDOG_FATAL_CONTEXT_VALID 0x01 /* detail.fatal.context holds an ESF */
#define ZMK_WATCHDOG_CAPTURE_INCOMPLETE 0x02
#define ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID 0x04

/* Snapshot flags, not an assertion that stack candidates form an unwind. */
#define ZMK_WATCHDOG_TRACE_FRAME 0x01
#define ZMK_WATCHDOG_TRACE_RUNNING 0x02
#define ZMK_WATCHDOG_TRACE_PENDED 0x04
#define ZMK_WATCHDOG_TRACE_PARTIAL 0x08
#define ZMK_WATCHDOG_TRACE_UNSUPPORTED 0x10

/* Incident kind discriminator for zmk_watchdog_incident_record.type. */
enum zmk_watchdog_incident_type {
    ZMK_WATCHDOG_INCIDENT_FREEZE = 0,
    ZMK_WATCHDOG_INCIDENT_FATAL = 1,
    ZMK_WATCHDOG_INCIDENT_RESET_CAUSE = 2,
};

#define ZMK_WATCHDOG_QUEUE_NAME_LEN 16
#define ZMK_WATCHDOG_THREAD_NAME_LEN 16

struct zmk_watchdog_incident_freeze_detail {
    /* task_wdt channel id that fired. */
    uint8_t channel_id;
    uint8_t thread_state;
    uint8_t trace_flags;
    uint8_t trace_count;
    /* Name of the monitored work queue, NUL-terminated, truncated if needed. */
    char queue_name[ZMK_WATCHDOG_QUEUE_NAME_LEN];
    uint32_t wait_queue;
    uint32_t stack_pointer;
    uint32_t addresses[ZMK_WATCHDOG_TRACE_ADDRS];
} __packed;

struct zmk_watchdog_incident_fatal_detail {
    /* One of Zephyr's K_ERR_* reason codes (unsigned int truncated to u32). */
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    /* k_thread_name_get() of the faulting thread, "?" if unavailable. */
    char thread_name[ZMK_WATCHDOG_THREAD_NAME_LEN];
    uint32_t context[6]; /* r0-r3, r12, xPSR; valid with ZMK_WATCHDOG_FATAL_CONTEXT_VALID. */
} __packed;

struct zmk_watchdog_incident_reset_detail {
    /* Raw hwinfo_get_reset_cause() bit mask. */
    uint32_t cause_bits;
} __packed;

union zmk_watchdog_incident_detail {
    struct zmk_watchdog_incident_freeze_detail freeze;
    struct zmk_watchdog_incident_fatal_detail fatal;
    struct zmk_watchdog_incident_reset_detail reset;
} __packed;

struct zmk_watchdog_incident_record {
    /* Record format version, for forward compatibility. */
    uint8_t version;
    /* enum zmk_watchdog_incident_type. */
    uint8_t type;
    /* Bit 0: fatal ESF; bit 1: incomplete; bit 2: common capture extension. */
    uint8_t flags;
    uint8_t _reserved;
    /* Incremented only when an incident is actually persisted (see
     * zmk_watchdog_store_append()); it orders incidents, it does not count
     * boots. */
    uint32_t boot_ordinal;
    /* Uptime (seconds) at incident time. */
    uint32_t uptime_s;
    union zmk_watchdog_incident_detail detail;
    uint8_t diagnostics[ZMK_WATCHDOG_DIAGNOSTIC_SIZE];
    /* Optional common CPU/operation evidence, independent of incident kind. */
    uint8_t capture_context[ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE];
} __packed;

BUILD_ASSERT(offsetof(struct zmk_watchdog_incident_record, capture_context) == 144,
             "watchdog v3 prefix changed");
BUILD_ASSERT(sizeof(struct zmk_watchdog_incident_record) == 336, "watchdog v4 layout changed");

/*
 * Store (src/watchdog_store.c): one settings record per slot, "wdg/i/<slot>",
 * plus "wdg/meta" for the next ordinal and ID. Nothing is written on a normal
 * boot. The incident and its metadata are separate writes, not a transaction.
 */

/* Append a new incident to the first free slot and fill rec->boot_ordinal
 * with the ordinal assigned. Returns 0 when stored, -ENOSPC when the store is
 * full (the incident is dropped and counted, no Flash write). With
 * CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST a full store replaces the oldest
 * incident instead, unless the incident's uptime is below
 * CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S. Mutating store APIs return
 * -EAGAIN without changing anything until the first settings load has
 * committed this subtree. Assigned IDs are unique among retained records,
 * including after a metadata write failure or uint16 wrap; they are not
 * permanent identifiers for deleted records. */
int zmk_watchdog_store_append(struct zmk_watchdog_incident_record *rec);

/* True once settings_load() has loaded and committed the watchdog subtree. */
bool zmk_watchdog_store_is_ready(void);

/* Delete the incident with the given id. Returns 0 on success, -ENOENT if
 * no incident with that id is stored, or -EEXIST without deleting anything
 * if a legacy/corrupt store has more than one record with this id. */
int zmk_watchdog_store_delete(uint16_t id);

/* Delete every stored incident and resume recording. Returns 0 on success
 * (including an already empty store). On backend errors the successful
 * deletions stay deleted, every failed slot stays readable/retryable, and the
 * first error is returned. */
int zmk_watchdog_store_delete_all(void);

/* Read the stored incident at a dense 0-based enumeration index over the
 * used slots; the order is unspecified but stable between mutations. Returns
 * 0 on success, -ENOENT if index is out of range.
 * All three record getters are blocking thread-context APIs: they serialize
 * with writers and read the persisted slot without holding the RAM-index
 * mutex across settings I/O. They allocate no heap and leave all outputs
 * unchanged on failure. Missing/inconsistent persisted data returns -EIO,
 * invalid formats -EINVAL, backend read errors propagate, and an index changed
 * by a concurrent settings reload returns -EAGAIN. Status/count stay RAM-only. */
int zmk_watchdog_store_get(uint16_t index, struct zmk_watchdog_incident_record *out);

/* Look up a stored incident by id. Returns 0 on success, -ENOENT if no such
 * incident is stored, or -EEXIST with *out unchanged if the id is ambiguous. */
int zmk_watchdog_store_get_by_id(uint16_t id, struct zmk_watchdog_incident_record *out);

/* Like zmk_watchdog_store_get(), and also returns the incident's id. */
int zmk_watchdog_store_get_with_id(uint16_t index, struct zmk_watchdog_incident_record *out,
                                   uint16_t *id_out);

/* Maximum number of incidents the store can hold (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS). */
uint16_t zmk_watchdog_store_capacity(void);

struct zmk_watchdog_store_status {
    uint16_t capacity;
    uint16_t stored;
    uint32_t dropped_since_boot;
    bool recording_stopped;
};
/* One coherent RAM snapshot; never hold this lock across settings I/O. */
void zmk_watchdog_store_get_status(struct zmk_watchdog_store_status *out);

/* Number of incidents currently stored. */
uint16_t zmk_watchdog_store_count(void);

/* Incidents dropped because the store was full, since this boot (RAM only). */
uint32_t zmk_watchdog_store_dropped_since_boot(void);

/* True once the store is full and append() would drop the next incident.
 * Cleared when a delete frees a slot. Always false with
 * CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST. */
bool zmk_watchdog_store_recording_stopped(void);

/* Called after a successful zmk_watchdog_store_append() (not for a dropped
 * incident), outside the store locks and never from ISR context. One
 * listener; pass NULL to unregister. Lets the Studio RPC learn of new
 * incidents without the store depending on it. */
typedef void (*zmk_watchdog_store_appended_cb_t)(uint16_t id,
                                                 const struct zmk_watchdog_incident_record *rec);
void zmk_watchdog_store_set_appended_callback(zmk_watchdog_store_appended_cb_t cb);

/*
 * Pending slot (src/watchdog_pending.c): one retained-RAM record that carries
 * a freeze/fatal incident across the reboot that follows it.
 */

/* Fill the pending slot with rec (magic + CRC). Safe from ISR and fatal-handler
 * context: no Flash, locks or logging. The caller reboots right after. */
void zmk_watchdog_pending_set(const struct zmk_watchdog_incident_record *rec);

/* Validate the pending slot and hand a valid record to the store, then clear
 * it once persisted or dropped for capacity. Returns 0 on success or when
 * nothing is pending, -EAGAIN while the store is not ready or another
 * conversion is running, or the store's error; failures keep the record and
 * back off between retries. A reset between the Flash write and the RAM
 * clear can repeat the incident. Runs from the low-priority work queue after
 * boot, never from a SYS_INIT; exposed so tests can call it directly. */
int zmk_watchdog_pending_convert(void);
/* Current retry interval: the configured delay or a 1 s..60 s backoff. */
uint32_t zmk_watchdog_pending_retry_delay_ms(void);

/* True once a conversion this boot found a valid pending record, i.e. the
 * reboot was already explained by a freeze/fatal detector. The reset-cause
 * audit then does not record the hardware reset cause separately. */
bool zmk_watchdog_pending_had_incident_this_boot(void);

/* Shared non-blocking capture guard. A nested fatal/freeze entry must reboot
 * immediately instead of overwriting the outer capture. end() is reached only
 * when a test reboot hook returns. */
bool zmk_watchdog_capture_begin(void);
void zmk_watchdog_capture_end(void);

/* sys_reboot(SYS_REBOOT_WARM), or the test override. Production code reboots
 * only through this. Never returns in production. */
void zmk_watchdog_reboot(void);

/* Fill *rec with a FATAL incident for (reason, esf): the reason code, the
 * ESF's PC/LR and context on Cortex-M (0 elsewhere) and the current thread's
 * name ("?" if unavailable). esf may be NULL. Does not touch the pending slot
 * or reboot. Separate from the fatal handler so tests can call it. */
struct arch_esf; /* Forward-declared by zephyr/arch/arch_interface.h. */
void watchdog_fatal_build_record(struct zmk_watchdog_incident_record *rec, unsigned int reason,
                                 const struct arch_esf *esf);

/* Test/fault injection (src/watchdog_inject.c, CONFIG_ZMK_WATCHDOG_TEST_INJECTION). */
/* Blocks the system work queue forever, so its freeze channel fires after
 * CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS. */
void zmk_watchdog_inject_freeze(void);
/* Triggers a kernel oops, exercising the fatal handler. */
void zmk_watchdog_inject_fatal(void);

#if defined(CONFIG_ZMK_WATCHDOG_TEST) || defined(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
/* Test-only hooks, absent from production builds. */
typedef void (*zmk_watchdog_reboot_fn_t)(void);
/* Replace what zmk_watchdog_reboot() does; NULL restores sys_reboot(). */
void zmk_watchdog_reboot_set_override(zmk_watchdog_reboot_fn_t override);
/* Flip the pending slot's CRC so the next conversion discards it. */
void zmk_watchdog_pending_corrupt_crc_for_test(void);
/* Stop the system work queue's freeze channel, so a test that froze the
 * queue on purpose is not rebooted again and again. */
void zmk_watchdog_freeze_disarm_sysworkq_channel_for_test(void);
#endif

#ifdef __cplusplus
}
#endif
