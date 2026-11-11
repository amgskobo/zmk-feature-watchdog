/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/watchdog.h>

#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_store, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

#define SETTINGS_SUBTREE "wdg"
#define SETTINGS_META_KEY SETTINGS_SUBTREE "/meta"

/*
 * On-flash slot blob: the incident record plus the RPC-visible id it was
 * assigned when stored. Keeping id in the blob (rather than deriving it
 * from the slot index) means ids survive a delete-then-append that reuses
 * the freed slot with a *different* id, and survive a settings reload.
 */
struct watchdog_store_slot_blob {
    uint16_t id;
    uint16_t _reserved;
    struct zmk_watchdog_incident_record rec;
} __packed;

#define WATCHDOG_STORE_V1_BLOB_SIZE (4U + ZMK_WATCHDOG_RECORD_V1_SIZE)
#define WATCHDOG_STORE_V2_BLOB_SIZE (4U + ZMK_WATCHDOG_RECORD_V2_SIZE)
#define WATCHDOG_STORE_V3_BLOB_SIZE (4U + ZMK_WATCHDOG_RECORD_V3_SIZE)
BUILD_ASSERT(sizeof(struct watchdog_store_slot_blob) == 340, "watchdog v4 slot layout changed");

struct watchdog_store_slot {
    uint32_t boot_ordinal;
    uint16_t id;
    uint8_t version;
    bool used;
};
BUILD_ASSERT(sizeof(struct watchdog_store_slot) == 8, "watchdog RAM index grew");

struct watchdog_store_meta {
    uint32_t next_boot_ordinal;
    uint16_t next_id;
    uint16_t _reserved;
} __packed;

static K_MUTEX_DEFINE(watchdog_store_lock);
/* Serialize persisted reads and writers, but never acquire this from a settings callback.
 * settings_load holds Zephyr's settings lock while invoking h_set/h_commit.
 * No settings I/O may run with watchdog_store_lock held. */
static K_MUTEX_DEFINE(watchdog_store_write_lock);
static struct watchdog_store_slot slots[CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS];
/* One bounded I/O buffer, owned by watchdog_store_write_lock. Append also
 * uses it while holding that mutex; settings-load callbacks need their own
 * temporary record because they cannot take this mutex. History stays in
 * settings; do not put another full record on the peripheral worker stack. */
static struct watchdog_store_slot_blob read_blob;
static struct watchdog_store_meta meta = {.next_boot_ordinal = 1, .next_id = 1};
static uint32_t dropped_since_boot;
static zmk_watchdog_store_appended_cb_t appended_cb;
static bool settings_loaded;

static int slot_settings_name(uint16_t slot, char *name, size_t name_size) {
    int ret = snprintf(name, name_size, SETTINGS_SUBTREE "/i/%u", (unsigned int)slot);
    if (ret < 0 || (size_t)ret >= name_size) {
        return -ENAMETOOLONG;
    }
    return 0;
}

static int find_free_slot_locked(void) {
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (!slots[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static int find_slot_by_id_locked(uint16_t id) {
    int found = -ENOENT;
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (slots[i].used && slots[i].id == id) {
            if (found >= 0) {
                return -EEXIST;
            }
            found = (int)i;
        }
    }
    return found;
}

static int find_unused_id_locked(void) {
    uint16_t candidate = meta.next_id;
    /* At most slot count + 1 candidates are needed, capped at the whole
     * uint16 namespace. Duplicate legacy IDs also count as occupied. */
    for (uint32_t tried = 0; tried <= ARRAY_SIZE(slots) && tried <= UINT16_MAX; tried++) {
        if (find_slot_by_id_locked(candidate) == -ENOENT) {
            return candidate;
        }
        candidate++;
    }
    return -ENOSPC;
}

/* Dense (0-based) enumeration over currently-used slots, in slot-index
 * order. Order is unspecified by the API contract but stable between
 * mutations, which is all a later RPC "list" phase needs. */
static int find_slot_by_enum_index_locked(uint16_t index) {
    uint16_t seen = 0;
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (!slots[i].used) {
            continue;
        }
        if (seen == index) {
            return (int)i;
        }
        seen++;
    }
    return -1;
}

static uint16_t count_used_locked(void) {
    uint16_t count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (slots[i].used) {
            count++;
        }
    }
    return count;
}

#if defined(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST)
#ifndef CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S
#define CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S 10
#endif

/* The used slot holding the oldest incident: the lowest persisted ordinal, compared modulo 2^32 so
 * that a wrapped ordinal counter still orders correctly. Of equal ordinals (legacy duplicates) the
 * lowest slot wins. -1 when no slot is used. */
static int find_oldest_slot_locked(void) {
    int oldest = -1;
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (!slots[i].used) {
            continue;
        }
        if (oldest < 0 || (int32_t)(slots[i].boot_ordinal - slots[oldest].boot_ordinal) < 0) {
            oldest = (int)i;
        }
    }
    return oldest;
}
#endif

int zmk_watchdog_store_append(struct zmk_watchdog_incident_record *rec) {
    if (!rec || rec->type > ZMK_WATCHDOG_INCIDENT_RESET_CAUSE) {
        return -EINVAL;
    }

    k_mutex_lock(&watchdog_store_write_lock, K_FOREVER);
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    if (!settings_loaded) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return -EAGAIN;
    }

    int slot = find_free_slot_locked();
#if defined(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST)
    /* Full store: an incident that happened late enough in its boot replaces the oldest one. A
     * failed write below leaves the old incident in place (the slot is only updated after the
     * write succeeded). Early incidents (a boot loop) are dropped as in a store without the option,
     * which bounds the Flash writes of a crash loop. */
    int replaced_id = -1;
    uint32_t replaced_ordinal = 0;
    const uint32_t min_uptime_s = CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S;
    if (slot < 0 && rec->uptime_s >= min_uptime_s) {
        slot = find_oldest_slot_locked();
        if (slot >= 0) {
            replaced_id = slots[slot].id;
            replaced_ordinal = slots[slot].boot_ordinal;
        }
    }
#endif
    if (slot < 0) {
        uint32_t dropped = ++dropped_since_boot;
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        LOG_WRN("Watchdog store full (cap=%d); dropping incident (dropped_since_boot=%u)",
                CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS, dropped);
        return -ENOSPC;
    }

    int id = find_unused_id_locked();
    if (id < 0) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return id;
    }

    rec->version = ZMK_WATCHDOG_RECORD_VERSION;
    rec->boot_ordinal = meta.next_boot_ordinal;

    /* An aggregate initializer materializes a full blob on the ARM stack at
     * -Os. Copy directly into the mutex-owned I/O storage instead. */
    memcpy(&read_blob.rec, rec, sizeof(read_blob.rec));
    read_blob.id = (uint16_t)id;
    read_blob._reserved = 0;

    struct watchdog_store_meta new_meta = {
        .next_boot_ordinal = meta.next_boot_ordinal + 1,
        .next_id = (uint16_t)(id + 1),
    };

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = slot_settings_name((uint16_t)slot, name, sizeof(name));
    if (ret < 0) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return ret;
    }
    k_mutex_unlock(&watchdog_store_lock);

    /* Persist the incident first, then the bumped meta (ordinal/id)
     * bookkeeping. A missing/stale cursor cannot reuse a retained ID:
     * allocation always checks the loaded slots. This is not a transaction;
     * settings commit also reconciles a stale ordinal against retained records. */
    ret = settings_save_one(name, &read_blob, sizeof(read_blob));
    if (ret < 0) {
        k_mutex_unlock(&watchdog_store_write_lock);
        LOG_ERR("Failed to persist watchdog incident: %d", ret);
        return ret;
    }

    ret = settings_save_one(SETTINGS_META_KEY, &new_meta, sizeof(new_meta));
    if (ret < 0) {
        /* Non-fatal: the incident is safely stored. Advance the RAM cursor
         * regardless; a subsequent settings commit reconciles loaded IDs. */
        LOG_WRN("Failed to persist watchdog meta (ordinal bookkeeping): %d", ret);
    }

    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    meta = new_meta;
    slots[slot] = (struct watchdog_store_slot){.boot_ordinal = read_blob.rec.boot_ordinal,
                                               .id = read_blob.id,
                                               .version = read_blob.rec.version,
                                               .used = true};
    zmk_watchdog_store_appended_cb_t cb = appended_cb;

    k_mutex_unlock(&watchdog_store_lock);
    k_mutex_unlock(&watchdog_store_write_lock);

    LOG_INF("Watchdog incident stored: id=%u slot=%d type=%u ordinal=%u", (uint16_t)id, slot,
            rec->type, rec->boot_ordinal);
#if defined(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST)
    if (replaced_id >= 0) {
        LOG_WRN("Watchdog store full (cap=%d); replaced the oldest incident id=%d ordinal=%u",
                CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS, replaced_id, replaced_ordinal);
    }
#endif

    if (cb) {
        cb((uint16_t)id, rec);
    }

    return 0;
}

int zmk_watchdog_store_delete(uint16_t id) {
    k_mutex_lock(&watchdog_store_write_lock, K_FOREVER);
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    if (!settings_loaded) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return -EAGAIN;
    }

    int slot = find_slot_by_id_locked(id);
    if (slot < 0) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return slot;
    }

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = slot_settings_name((uint16_t)slot, name, sizeof(name));
    if (ret < 0) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return ret;
    }
    k_mutex_unlock(&watchdog_store_lock);

    ret = settings_delete(name);
    if (ret < 0 && ret != -ENOENT) {
        k_mutex_unlock(&watchdog_store_write_lock);
        LOG_ERR("Failed to delete watchdog incident id=%u: %d", id, ret);
        return ret;
    }

    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    memset(&slots[slot], 0, sizeof(slots[slot]));

    k_mutex_unlock(&watchdog_store_lock);
    k_mutex_unlock(&watchdog_store_write_lock);

    LOG_INF("Watchdog incident deleted: id=%u slot=%d", id, slot);
    return 0;
}

int zmk_watchdog_store_delete_all(void) {
    k_mutex_lock(&watchdog_store_write_lock, K_FOREVER);
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    if (!settings_loaded) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return -EAGAIN;
    }
    k_mutex_unlock(&watchdog_store_lock);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        k_mutex_lock(&watchdog_store_lock, K_FOREVER);
        bool used = slots[i].used;
        k_mutex_unlock(&watchdog_store_lock);
        if (!used) {
            continue;
        }

        char name[SETTINGS_MAX_NAME_LEN];
        int ret = slot_settings_name((uint16_t)i, name, sizeof(name));
        if (ret == 0) {
            ret = settings_delete(name);
        }
        if (ret < 0 && ret != -ENOENT) {
            if (first_error == 0) {
                first_error = ret;
            }
            LOG_ERR("Failed to delete watchdog incident slot=%zu: %d", i, ret);
            continue;
        }

        k_mutex_lock(&watchdog_store_lock, K_FOREVER);
        memset(&slots[i], 0, sizeof(slots[i]));
        k_mutex_unlock(&watchdog_store_lock);
    }

    k_mutex_unlock(&watchdog_store_write_lock);

    LOG_INF("Watchdog store cleared");
    return first_error;
}

/* Both initial indexing and later reads validate the unchanged v1-v4 layouts. */
static int decode_slot_blob(size_t len, settings_read_cb read_cb, void *cb_arg,
                            struct watchdog_store_slot_blob *blob) {
    if (len != sizeof(*blob) && len != WATCHDOG_STORE_V1_BLOB_SIZE &&
        len != WATCHDOG_STORE_V2_BLOB_SIZE && len != WATCHDOG_STORE_V3_BLOB_SIZE) {
        return -EINVAL;
    }
    memset(blob, 0, sizeof(*blob));
    ssize_t read = read_cb(cb_arg, blob, len);
    if (read < 0) {
        return (int)read;
    }
    if ((size_t)read != len) {
        return -EIO;
    }
    if (blob->rec.type > ZMK_WATCHDOG_INCIDENT_RESET_CAUSE ||
        (len == WATCHDOG_STORE_V1_BLOB_SIZE && blob->rec.version != 1) ||
        (len == WATCHDOG_STORE_V2_BLOB_SIZE && blob->rec.version != 2) ||
        (len == WATCHDOG_STORE_V3_BLOB_SIZE && blob->rec.version != 3) ||
        (len == sizeof(*blob) && blob->rec.version != ZMK_WATCHDOG_RECORD_VERSION)) {
        return -EINVAL;
    }
    if (blob->rec.version == 1 && blob->rec.type == ZMK_WATCHDOG_INCIDENT_FREEZE) {
        /* Legacy reserved bytes are not diagnostic state or pointers. */
        blob->rec.detail.freeze.thread_state = 0;
        blob->rec.detail.freeze.trace_flags = 0;
        blob->rec.detail.freeze.trace_count = 0;
        blob->rec.detail.freeze.wait_queue = 0;
        blob->rec.detail.freeze.stack_pointer = 0;
        memset(blob->rec.detail.freeze.addresses, 0, sizeof(blob->rec.detail.freeze.addresses));
    }
    if (blob->rec.version < 4) {
        blob->rec.flags &= ~ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
    }
    if (blob->rec.version < 3) {
        blob->rec.flags = 0;
        if (blob->rec.type == ZMK_WATCHDOG_INCIDENT_FATAL) {
            memset(blob->rec.detail.fatal.context, 0, sizeof(blob->rec.detail.fatal.context));
        }
    }
    return 0;
}

struct watchdog_store_read {
    bool found;
    int result;
};

static int read_slot_setting(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                             void *param) {
    struct watchdog_store_read *read = param;
    if (key && key[0] != '\0') {
        return 0; /* An unexpected child key is not this exact slot. */
    }
    if (read->found) {
        read->result = -EEXIST;
    } else {
        read->found = true;
        read->result = decode_slot_blob(len, read_cb, cb_arg, &read_blob);
    }
    /* This core discards csi_load's return value. Retain every callback error
     * in our context instead of treating a successful traversal as valid data. */
    return 0;
}

static int get_record(uint16_t selector, bool by_id, struct zmk_watchdog_incident_record *out,
                      uint16_t *id_out) {
    k_mutex_lock(&watchdog_store_write_lock, K_FOREVER);
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    int slot = by_id ? find_slot_by_id_locked(selector) : find_slot_by_enum_index_locked(selector);
    if (slot < 0) {
        k_mutex_unlock(&watchdog_store_lock);
        k_mutex_unlock(&watchdog_store_write_lock);
        return by_id ? slot : -ENOENT;
    }
    struct watchdog_store_slot expected = slots[slot];
    k_mutex_unlock(&watchdog_store_lock);

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = slot_settings_name((uint16_t)slot, name, sizeof(name));
    if (ret == 0) {
        struct watchdog_store_read read = {.result = -EIO};
        ret = settings_load_subtree_direct(name, read_slot_setting, &read);
        if (ret == 0) {
            ret = read.result; /* Missing persisted data is an error, not an empty index. */
        }
    }

    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    if (ret == 0) {
        const struct watchdog_store_slot *current = &slots[slot];
        if (!current->used || current->id != expected.id ||
            current->boot_ordinal != expected.boot_ordinal ||
            current->version != expected.version) {
            ret = -EAGAIN; /* A settings reload changed the index during traversal. */
        } else if (read_blob.id != expected.id ||
                   read_blob.rec.boot_ordinal != expected.boot_ordinal ||
                   read_blob.rec.version != expected.version) {
            ret = -EIO; /* Never return a reused/corrupt slot as the requested incident. */
        } else {
            *out = read_blob.rec;
            if (id_out) {
                *id_out = read_blob.id;
            }
        }
    }
    k_mutex_unlock(&watchdog_store_lock);
    k_mutex_unlock(&watchdog_store_write_lock);
    return ret;
}

int zmk_watchdog_store_get(uint16_t index, struct zmk_watchdog_incident_record *out) {
    return out ? get_record(index, false, out, NULL) : -EINVAL;
}

int zmk_watchdog_store_get_by_id(uint16_t id, struct zmk_watchdog_incident_record *out) {
    return out ? get_record(id, true, out, NULL) : -EINVAL;
}

int zmk_watchdog_store_get_with_id(uint16_t index, struct zmk_watchdog_incident_record *out,
                                   uint16_t *id_out) {
    return out && id_out ? get_record(index, false, out, id_out) : -EINVAL;
}

uint16_t zmk_watchdog_store_capacity(void) { return CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS; }

void zmk_watchdog_store_get_status(struct zmk_watchdog_store_status *out) {
    if (!out) {
        return;
    }
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    out->capacity = CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS;
    out->stored = count_used_locked();
    out->dropped_since_boot = dropped_since_boot;
#if defined(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST)
    out->recording_stopped = false;
#else
    out->recording_stopped = out->stored >= out->capacity;
#endif
    k_mutex_unlock(&watchdog_store_lock);
}

uint16_t zmk_watchdog_store_count(void) {
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    uint16_t count = count_used_locked();
    k_mutex_unlock(&watchdog_store_lock);
    return count;
}

uint32_t zmk_watchdog_store_dropped_since_boot(void) {
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    uint32_t count = dropped_since_boot;
    k_mutex_unlock(&watchdog_store_lock);
    return count;
}

bool zmk_watchdog_store_is_ready(void) {
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    bool ready = settings_loaded;
    k_mutex_unlock(&watchdog_store_lock);
    return ready;
}

bool zmk_watchdog_store_recording_stopped(void) {
#if defined(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST)
    return false;
#else
    return zmk_watchdog_store_count() >= CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS;
#endif
}

void zmk_watchdog_store_set_appended_callback(zmk_watchdog_store_appended_cb_t cb) {
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    appended_cb = cb;
    k_mutex_unlock(&watchdog_store_lock);
}

/* --------------------------------------------------------------------
 * settings subsystem wiring: rebuild the RAM index (slots[] + meta) from
 * flash during the single global settings_load() call in main(). No
 * separate settings_load_subtree("wdg") call is needed here -- the
 * top-level settings_load() already walks every registered subtree
 * handler, this one included.
 * -------------------------------------------------------------------- */

static int watchdog_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    const char *next;
    int name_len = settings_name_next(name, &next);

    if (name_len == 4 && strncmp(name, "meta", 4) == 0) {
        struct watchdog_store_meta loaded;
        if (len != sizeof(loaded)) {
            return -EINVAL;
        }
        ssize_t read = read_cb(cb_arg, &loaded, sizeof(loaded));
        if (read < 0) {
            return (int)read;
        }
        if ((size_t)read != sizeof(loaded)) {
            return -EIO;
        }
        k_mutex_lock(&watchdog_store_lock, K_FOREVER);
        meta = loaded;
        k_mutex_unlock(&watchdog_store_lock);
        return 0;
    }

    if (name_len == 1 && strncmp(name, "i", 1) == 0 && next) {
        char *endptr;
        unsigned long slot_index = strtoul(next, &endptr, 10);
        if (endptr == next || *endptr != '\0' || slot_index >= ARRAY_SIZE(slots)) {
            return -ENOENT;
        }

        struct watchdog_store_slot_blob blob;
        int ret = decode_slot_blob(len, read_cb, cb_arg, &blob);
        if (ret < 0) {
            return ret;
        }

        k_mutex_lock(&watchdog_store_lock, K_FOREVER);
        slots[slot_index] = (struct watchdog_store_slot){.boot_ordinal = blob.rec.boot_ordinal,
                                                         .id = blob.id,
                                                         .version = blob.rec.version,
                                                         .used = true};
        k_mutex_unlock(&watchdog_store_lock);
        return 0;
    }

    return -ENOENT;
}

static int watchdog_settings_commit(void) {
    /* Even an empty backend commits this subtree. Never wait for a writer:
     * this callback runs under Zephyr's settings lock. */
    k_mutex_lock(&watchdog_store_lock, K_FOREVER);
    int id = find_unused_id_locked();
    if (id < 0) {
        k_mutex_unlock(&watchdog_store_lock);
        return id;
    }
    /* Reconcile before a delete can remove evidence of a stale cursor.
     * This is RAM-only: h_commit must never write settings. */
    meta.next_id = (uint16_t)id;
    /* A saved incident can be newer than metadata after its separate write failed.
     * Reconcile the ordinal too, or reboot can create ties and overwrite newer history.
     * Preserve an advanced cursor after deletion, and compare modulo the counter width. */
    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        if (slots[i].used && (int32_t)(slots[i].boot_ordinal - meta.next_boot_ordinal) >= 0) {
            meta.next_boot_ordinal = slots[i].boot_ordinal + 1U;
        }
    }
    settings_loaded = true;
    k_mutex_unlock(&watchdog_store_lock);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(watchdog, SETTINGS_SUBTREE, NULL, watchdog_settings_set,
                               watchdog_settings_commit, NULL);
