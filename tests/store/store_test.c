/* Copyright (c) 2026. SPDX-License-Identifier: MIT */
/* Compile the entire production store and its real public record header.
 * The backend injects failures; it is not NVS or a real Zephyr scheduler. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "store tests require assertions enabled"
#endif
#include "../../src/watchdog_store.c"

#ifdef CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST
#define WATCHDOG_STORE_OVERWRITES 1
#else
#define WATCHDOG_STORE_OVERWRITES 0
#endif

static struct watchdog_store_slot flash_slots[CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS];
static struct watchdog_store_meta flash_meta;
static bool flash_has_meta;
static int save_failure, meta_failure;
static int delete_failure[CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS];
static unsigned saves, deletes, notifications;

static unsigned slot_number(const char *name) {
    unsigned slot = UINT32_MAX;
    assert(sscanf(name, "wdg/i/%u", &slot) == 1);
    assert(slot < ARRAY_SIZE(flash_slots));
    return slot;
}

int settings_save_one(const char *name, const void *value, size_t length) {
    assert(!watchdog_store_lock.held && watchdog_store_write_lock.held);
    saves++;
    if (strcmp(name, "wdg/meta") == 0) {
        if (meta_failure) {
            return meta_failure;
        }
        assert(length == sizeof(flash_meta));
        memcpy(&flash_meta, value, length);
        flash_has_meta = true;
    } else {
        if (save_failure) {
            return save_failure;
        }
        unsigned slot = slot_number(name);
        assert(length == sizeof(flash_slots[slot].blob));
        flash_slots[slot].used = true;
        memcpy(&flash_slots[slot].blob, value, length);
    }
    return 0;
}

int settings_delete(const char *name) {
    assert(!watchdog_store_lock.held && watchdog_store_write_lock.held);
    deletes++;
    unsigned slot = slot_number(name);
    if (delete_failure[slot]) {
        return delete_failure[slot];
    }
    if (!flash_slots[slot].used) {
        return -ENOENT;
    }
    memset(&flash_slots[slot], 0, sizeof(flash_slots[slot]));
    return 0;
}

int settings_name_next(const char *name, const char **next) {
    const char *slash = strchr(name, '/');
    *next = slash ? slash + 1 : NULL;
    return (int)(slash ? slash - name : (ptrdiff_t)strlen(name));
}

static void notify(uint16_t id, const struct zmk_watchdog_incident_record *rec) {
    assert(!watchdog_store_lock.held && !watchdog_store_write_lock.held);
    struct zmk_watchdog_incident_record stored;
    assert(zmk_watchdog_store_get_by_id(id, &stored) == 0);
    assert(memcmp(rec, &stored, sizeof(stored)) == 0);
    notifications++;
}

static void reset_ram(void) {
    assert(!watchdog_store_lock.held && !watchdog_store_write_lock.held);
    memset(slots, 0, sizeof(slots));
    meta = (struct watchdog_store_meta){.next_boot_ordinal = 1, .next_id = 1};
    dropped_since_boot = 0;
    appended_cb = NULL;
    settings_loaded = false;
}

static void reset(void) {
    reset_ram();
    memset(flash_slots, 0, sizeof(flash_slots));
    memset(delete_failure, 0, sizeof(delete_failure));
    flash_has_meta = false;
    saves = deletes = notifications = 0;
    save_failure = meta_failure = 0;
    zmk_watchdog_store_set_appended_callback(notify);
    assert(watchdog_settings_commit() == 0);
}

struct read_value {
    const void *data;
    ssize_t result;
};
static ssize_t read_value(void *context, void *value, size_t length) {
    struct read_value *read = context;
    if (read->result > 0) {
        assert((size_t)read->result <= length);
        memcpy(value, read->data, (size_t)read->result);
    }
    return read->result;
}

static void reload(void) {
    reset_ram();
    if (flash_has_meta) {
        struct read_value value = {&flash_meta, sizeof(flash_meta)};
        assert(watchdog_settings_set("meta", sizeof(flash_meta), read_value, &value) == 0);
    }
    for (size_t i = 0; i < ARRAY_SIZE(flash_slots); i++) {
        if (!flash_slots[i].used) {
            continue;
        }
        char name[24];
        snprintf(name, sizeof(name), "i/%zu", i);
        struct read_value value = {&flash_slots[i].blob, sizeof(flash_slots[i].blob)};
        assert(watchdog_settings_set(name, sizeof(flash_slots[i].blob), read_value, &value) == 0);
    }
    assert(watchdog_settings_commit() == 0);
}

static void readiness(void) {
    reset();
    reset_ram();
    struct zmk_watchdog_incident_record record = {.uptime_s = 99};
    struct zmk_watchdog_incident_record before = record;
    assert(!zmk_watchdog_store_is_ready());
    assert(zmk_watchdog_store_append(&record) == -EAGAIN);
    assert(memcmp(&before, &record, sizeof(record)) == 0);
    assert(zmk_watchdog_store_delete(1) == -EAGAIN);
    assert(zmk_watchdog_store_delete_all() == -EAGAIN);
    assert(saves == 0 && deletes == 0 && zmk_watchdog_store_count() == 0);
    assert(watchdog_settings_commit() == 0 && zmk_watchdog_store_is_ready());
    assert(zmk_watchdog_store_append(&record) == 0);
}

static void append(unsigned uptime) {
    struct zmk_watchdog_incident_record record = {.type = ZMK_WATCHDOG_INCIDENT_FREEZE,
                                                  .uptime_s = uptime};
    assert(zmk_watchdog_store_append(&record) == 0);
}

static void capacity_and_reload(void) {
    reset();
    assert(zmk_watchdog_store_delete_all() == 0 && deletes == 0);
    assert(zmk_watchdog_store_capacity() == CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS);
    assert(zmk_watchdog_store_append(NULL) == -EINVAL);
    assert(zmk_watchdog_store_get(0, NULL) == -EINVAL);
    assert(zmk_watchdog_store_get_by_id(1, NULL) == -EINVAL);
    struct zmk_watchdog_incident_record record;
    uint16_t id;
    assert(zmk_watchdog_store_get_with_id(0, &record, NULL) == -EINVAL);
    for (unsigned i = 0; i < CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS; i++) {
        append(i + 10);
        assert(zmk_watchdog_store_get_with_id(i, &record, &id) == 0);
        assert(id == i + 1 && record.boot_ordinal == i + 1 && record.uptime_s == i + 10);
        assert(record.version == ZMK_WATCHDOG_RECORD_VERSION);
    }
    assert(notifications == CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS);
#ifndef CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST
    unsigned writes = saves;
    assert(zmk_watchdog_store_recording_stopped());
    assert(zmk_watchdog_store_append(&record) == -ENOSPC && saves == writes);
    assert(zmk_watchdog_store_dropped_since_boot() == 1);
    assert(zmk_watchdog_store_get(CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS, &record) == -ENOENT);
#endif
    reload();
    assert(zmk_watchdog_store_count() == CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS);
    assert(zmk_watchdog_store_dropped_since_boot() == 0);
    assert(zmk_watchdog_store_delete(1) == 0);
    assert(!zmk_watchdog_store_recording_stopped());
    append(99);
    assert(zmk_watchdog_store_get_with_id(0, &record, &id) == 0);
    assert(id == CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS + 1 && record.uptime_s == 99);
    assert(zmk_watchdog_store_delete_all() == 0);
    reload();
    assert(zmk_watchdog_store_count() == 0);
}

static void backend_failures(void) {
    reset();
    save_failure = -EIO;
    struct zmk_watchdog_incident_record record = {0};
    assert(zmk_watchdog_store_append(&record) == -EIO);
    assert(zmk_watchdog_store_count() == 0 && notifications == 0);
    save_failure = 0;
    meta_failure = -EIO;
    append(42);
    assert(zmk_watchdog_store_count() == 1 && notifications == 1);
    /* Metadata failure is documented as non-fatal; the incident survives. */
    reload();
    assert(zmk_watchdog_store_get_by_id(1, &record) == 0 && record.uptime_s == 42);
    delete_failure[0] = -EIO;
    assert(zmk_watchdog_store_delete(1) == -EIO);
    assert(zmk_watchdog_store_get_by_id(1, &record) == 0);
    delete_failure[0] = 0;
    flash_slots[0].used = false;
    assert(zmk_watchdog_store_delete(1) == 0); /* Already absent on flash. */
    assert(zmk_watchdog_store_delete(1) == -ENOENT);
}

static void delete_all_failures(void) {
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS < 3) {
        return;
    }
    reset();
    append(10);
    append(20);
    append(30);
    delete_failure[0] = -EIO;
    delete_failure[1] = -ENOSPC;
    assert(zmk_watchdog_store_delete_all() == -EIO); /* First error returned. */
    assert(zmk_watchdog_store_count() == 2);         /* EVERY failed delete retained. */
    struct zmk_watchdog_incident_record record;
    assert(zmk_watchdog_store_get_by_id(1, &record) == 0 && record.uptime_s == 10);
    assert(zmk_watchdog_store_get_by_id(2, &record) == 0 && record.uptime_s == 20);
    assert(zmk_watchdog_store_get_by_id(3, &record) == -ENOENT);
    reload();
    assert(zmk_watchdog_store_count() == 2);
    memset(delete_failure, 0, sizeof(delete_failure));
    assert(zmk_watchdog_store_delete_all() == 0);
    reload();
    assert(zmk_watchdog_store_count() == 0);
}

static void id_metadata_failures(void) {
    reset();
    append(10); /* A valid persisted cursor exists before metadata fails. */
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS == 1) {
        assert(zmk_watchdog_store_delete(1) == 0);
    }
    meta_failure = -EIO;
    append(20);
    struct zmk_watchdog_incident_record record;
    assert(zmk_watchdog_store_get_by_id(2, &record) == 0 && record.uptime_s == 20);
    assert(meta.next_id == 3);
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS > 2) {
        append(30); /* Consecutive metadata failures must not repeat ID 2. */
        assert(zmk_watchdog_store_get_by_id(3, &record) == 0 && record.uptime_s == 30);
    }
    reload(); /* Flash's cursor still points to the occupied ID 2. */
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS == 1) {
        assert(zmk_watchdog_store_delete(2) == 0);
    }
    meta_failure = 0;
    unsigned notifications_before = notifications;
    save_failure = -EIO;
    record = (struct zmk_watchdog_incident_record){.uptime_s = 40};
    uint16_t next = meta.next_id;
    assert(zmk_watchdog_store_append(&record) == -EIO);
    assert(meta.next_id == next && notifications == notifications_before);
    save_failure = 0;
    append(40);
    assert(zmk_watchdog_store_get_by_id(next, &record) == 0 && record.uptime_s == 40);
    assert(next != 2);
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS > 2) {
        assert(next != 3);
        assert(zmk_watchdog_store_delete(2) == 0);
        assert(zmk_watchdog_store_get_by_id(1, &record) == 0 && record.uptime_s == 10);
        assert(zmk_watchdog_store_get_by_id(3, &record) == 0 && record.uptime_s == 30);
    }
    reload();
    assert(zmk_watchdog_store_get_by_id(next, &record) == 0 && record.uptime_s == 40);

    reset(); /* Missing metadata, not just an old saved cursor. */
    meta_failure = -EIO;
    append(50);
    assert(!flash_has_meta);
    reload();
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS == 1) {
        assert(zmk_watchdog_store_delete(1) == 0);
    }
    append(60);
    assert(zmk_watchdog_store_get_by_id(2, &record) == 0 && record.uptime_s == 60);
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS > 1) {
        assert(zmk_watchdog_store_get_by_id(1, &record) == 0 && record.uptime_s == 50);
    }
}

static void id_wrap(void) {
    reset();
    flash_has_meta = true;
    flash_meta = (struct watchdog_store_meta){.next_boot_ordinal = 1, .next_id = UINT16_MAX};
    reload();
    meta_failure = -EIO;
    append(10);
    struct zmk_watchdog_incident_record record;
    assert(zmk_watchdog_store_get_by_id(UINT16_MAX, &record) == 0 && record.uptime_s == 10);
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS == 1) {
        reload();
        assert(zmk_watchdog_store_delete(UINT16_MAX) == 0);
    }
    append(20);
    assert(zmk_watchdog_store_get_by_id(0, &record) == 0 && record.uptime_s == 20);
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS > 2) {
        append(30);
        assert(zmk_watchdog_store_get_by_id(1, &record) == 0 && record.uptime_s == 30);
        reload(); /* Cursor at 65535 must skip live 65535, 0 and 1. */
        append(40);
        assert(zmk_watchdog_store_get_by_id(2, &record) == 0 && record.uptime_s == 40);
        assert(zmk_watchdog_store_delete(0) == 0);
        assert(zmk_watchdog_store_get_by_id(UINT16_MAX, &record) == 0);
        assert(zmk_watchdog_store_get_by_id(1, &record) == 0);
    }
}

static void legacy_duplicate_ids(void) {
    if (CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS < 3) {
        return;
    }
    reset();
    for (unsigned i = 0; i < 2; i++) {
        flash_slots[i].used = true;
        flash_slots[i].blob.id = 9;
        flash_slots[i].blob.rec.version = ZMK_WATCHDOG_RECORD_VERSION;
        flash_slots[i].blob.rec.uptime_s = (i + 1) * 10;
    }
    flash_has_meta = true;
    flash_meta = (struct watchdog_store_meta){.next_boot_ordinal = 3, .next_id = 9};
    reload();
    struct zmk_watchdog_incident_record record = {.uptime_s = 77};
    struct zmk_watchdog_incident_record before = record;
    assert(zmk_watchdog_store_get_by_id(9, &record) == -EEXIST);
    assert(memcmp(&before, &record, sizeof(record)) == 0);
    unsigned deletes_before = deletes;
    assert(zmk_watchdog_store_delete(9) == -EEXIST && deletes == deletes_before);
    assert(zmk_watchdog_store_count() == 2);
    for (unsigned i = 0; i < 2; i++) {
        uint16_t id;
        assert(zmk_watchdog_store_get_with_id(i, &record, &id) == 0);
        assert(id == 9 && record.uptime_s == (i + 1) * 10);
        assert(flash_slots[i].used && flash_slots[i].blob.id == 9);
    }
    append(30);
    assert(zmk_watchdog_store_get_by_id(10, &record) == 0 && record.uptime_s == 30);
    assert(zmk_watchdog_store_delete(10) == 0 && zmk_watchdog_store_count() == 2);
    assert(zmk_watchdog_store_delete_all() == 0);
    reload();
    assert(zmk_watchdog_store_count() == 0);
}

static void settings_read_failures(void) {
    reset();
    struct watchdog_store_meta proposed = {.next_boot_ordinal = 88, .next_id = 42};
    struct read_value value = {&proposed, sizeof(proposed)};
    assert(watchdog_settings_set("unknown", sizeof(proposed), read_value, &value) == -ENOENT);
    assert(watchdog_settings_set("meta", sizeof(proposed) - 1, read_value, &value) == -EINVAL);
    for (ssize_t result = 0; result < (ssize_t)sizeof(proposed); result++) {
        value.result = result;
        assert(watchdog_settings_set("meta", sizeof(proposed), read_value, &value) == -EIO);
        assert(meta.next_boot_ordinal == 1 && meta.next_id == 1);
    }
    value.result = -ENOMEM;
    assert(watchdog_settings_set("meta", sizeof(proposed), read_value, &value) == -ENOMEM);
    struct watchdog_store_slot_blob blob = {
        .id = 55, .rec = {.version = ZMK_WATCHDOG_RECORD_VERSION, .uptime_s = 77}};
    value = (struct read_value){&blob, sizeof(blob)};
    assert(watchdog_settings_set("i/bad", sizeof(blob), read_value, &value) == -ENOENT);
    assert(watchdog_settings_set("i/999", sizeof(blob), read_value, &value) == -ENOENT);
    for (ssize_t result = 0; result < (ssize_t)sizeof(blob); result++) {
        value.result = result;
        assert(watchdog_settings_set("i/0", sizeof(blob), read_value, &value) == -EIO);
        assert(zmk_watchdog_store_count() == 0);
    }
    value.result = sizeof(blob);
    assert(watchdog_settings_set("i/0", sizeof(blob), read_value, &value) == 0);
    struct zmk_watchdog_incident_record record;
    assert(zmk_watchdog_store_get_by_id(55, &record) == 0 && record.uptime_s == 77);
}

static unsigned slot_key_reads;
static ssize_t read_slot_key(void *context, void *value, size_t length) {
    slot_key_reads++;
    return read_value(context, value, length);
}

static void slot_key_validation(void) {
    reset();
    append(10);
    struct watchdog_store_slot before[CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS];
    memcpy(before, slots, sizeof(before));
    struct watchdog_store_meta before_meta = meta;
    struct watchdog_store_slot_blob blob = {
        .id = 55, .rec = {.version = ZMK_WATCHDOG_RECORD_VERSION, .uptime_s = 77}};
    struct read_value value = {&blob, sizeof(blob)};
    const char *invalid[] = {"i/",
                             "i",
                             "i/bad",
                             "i/0/",
                             "i/0junk",
                             "i/-1",
                             "i/9999999999999999999999999999999999999999"};
    slot_key_reads = 0;
    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
        assert(watchdog_settings_set(invalid[i], sizeof(blob), read_slot_key, &value) == -ENOENT);
        assert(slot_key_reads == 0);
        assert(memcmp(before, slots, sizeof(before)) == 0);
        assert(memcmp(&before_meta, &meta, sizeof(meta)) == 0);
    }
    char name[24];
    snprintf(name, sizeof(name), "i/%u", CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS);
    assert(watchdog_settings_set(name, sizeof(blob), read_slot_key, &value) == -ENOENT);
    assert(slot_key_reads == 0 && memcmp(before, slots, sizeof(before)) == 0);
    snprintf(name, sizeof(name), "i/%u", CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS - 1);
    assert(watchdog_settings_set(name, sizeof(blob), read_slot_key, &value) == 0);
    assert(slot_key_reads == 1);
    assert(slots[CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS - 1].blob.id == 55);
    blob.id = 56;
    assert(watchdog_settings_set("i/0", sizeof(blob), read_slot_key, &value) == 0);
    assert(slot_key_reads == 2 && slots[0].blob.id == 56 && slots[0].blob.rec.uptime_s == 77);
}

static void record_formats(void) {
    /* Independent v1 wire fixture: the on-flash prefix is 4 bytes, record
     * header 12 and old detail union 28. Preserve every real payload field. */
    for (unsigned kind = 0; kind < 3; ++kind) {
        reset();
        uint8_t legacy[44] = {0};
        legacy[0] = 53;
        legacy[4] = 1;
        legacy[5] = kind;
        legacy[8] = 7;
        legacy[12] = 91;
        for (unsigned i = 16; i < sizeof(legacy); ++i) {
            legacy[i] = (uint8_t)i;
        }
        if (kind == ZMK_WATCHDOG_INCIDENT_FREEZE) {
            legacy[16] = 3;
            memcpy(legacy + 20, "sysworkq", 9);
        }
        struct read_value value = {legacy, sizeof(legacy)};
        for (ssize_t n = 0; n < (ssize_t)sizeof(legacy); ++n) {
            value.result = n;
            assert(watchdog_settings_set("i/0", sizeof(legacy), read_value, &value) == -EIO);
            assert(zmk_watchdog_store_count() == 0);
        }
        value.result = sizeof(legacy);
        assert(watchdog_settings_set("i/0", sizeof(legacy), read_value, &value) == 0);
        struct zmk_watchdog_incident_record rec;
        assert(zmk_watchdog_store_get_by_id(53, &rec) == 0);
        assert(rec.version == 1 && rec.type == kind && rec.boot_ordinal == 7 && rec.uptime_s == 91);
        assert(saves == 0 && deletes == 0);
        if (kind == ZMK_WATCHDOG_INCIDENT_FREEZE) {
            assert(rec.detail.freeze.channel_id == 3);
            assert(strcmp(rec.detail.freeze.queue_name, "sysworkq") == 0);
            assert(rec.detail.freeze.trace_count == 0 && rec.detail.freeze.trace_flags == 0);
            assert(rec.detail.freeze.thread_state == 0 && rec.detail.freeze.wait_queue == 0);
            assert(rec.detail.freeze.stack_pointer == 0);
            for (unsigned i = 0; i < ZMK_WATCHDOG_TRACE_ADDRS; ++i) {
                assert(rec.detail.freeze.addresses[i] == 0);
            }
        } else {
            assert(memcmp(&rec.detail, legacy + 16, 28) == 0);
        }
        struct watchdog_store_slot unchanged = slots[0];
        legacy[4] = 2;
        assert(watchdog_settings_set("i/0", sizeof(legacy), read_value, &value) == -EINVAL);
        assert(memcmp(&unchanged, &slots[0], sizeof(unchanged)) == 0);
    }
    reset();
    struct zmk_watchdog_incident_record trace = {
        .type = ZMK_WATCHDOG_INCIDENT_FREEZE,
        .uptime_s = 975,
        .detail.freeze = {.channel_id = 1,
                          .thread_state = 2,
                          .trace_flags = ZMK_WATCHDOG_TRACE_FRAME | ZMK_WATCHDOG_TRACE_PENDED,
                          .trace_count = 6,
                          .queue_name = "sysworkq",
                          .wait_queue = 0x20000120,
                          .stack_pointer = 0x20001000,
                          .addresses = {0x1010, 0x2011, 0x3011, 0x4011, 0x5011, 0x6011}}};
    trace.diagnostics[0] = 1;
    trace.flags = ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
    trace.capture_context[0] = 1;
    for (size_t i = 1; i < sizeof(trace.capture_context); ++i) {
        trace.capture_context[i] = (uint8_t)(i ^ 0xa5);
    }
    for (size_t i = 1; i < sizeof(trace.diagnostics); ++i) {
        trace.diagnostics[i] = (uint8_t)i;
    }
    assert(zmk_watchdog_store_append(&trace) == 0);
    assert(trace.version == 4 && saves == 2);
    reload();
    struct zmk_watchdog_incident_record reloaded;
    assert(zmk_watchdog_store_get_by_id(1, &reloaded) == 0);
    assert(memcmp(&trace, &reloaded, sizeof(trace)) == 0 && saves == 2);
    uint8_t v3[148];
    memcpy(v3, &slots[0].blob, sizeof(v3));
    v3[4] = 3;
    /* An old record cannot claim the new extension, even with a reserved bit set. */
    struct read_value v3_value = {v3, sizeof(v3)};
    assert(watchdog_settings_set("i/0", sizeof(v3), read_value, &v3_value) == 0);
    assert(zmk_watchdog_store_get_by_id(1, &reloaded) == 0 && reloaded.version == 3);
    assert(!(reloaded.flags & ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID));
    assert(!memcmp(reloaded.diagnostics, trace.diagnostics, sizeof(trace.diagnostics)));
    for (size_t i = 0; i < sizeof(reloaded.capture_context); ++i) {
        assert(reloaded.capture_context[i] == 0);
    }
    uint8_t v2[68];
    memcpy(v2, &slots[0].blob, sizeof(v2));
    v2[4] = 2;
    struct read_value old_value = {v2, sizeof(v2)};
    unsigned before_saves = saves;
    assert(watchdog_settings_set("i/0", sizeof(v2), read_value, &old_value) == 0);
    assert(zmk_watchdog_store_get_by_id(1, &reloaded) == 0 && reloaded.version == 2);
    assert(memcmp(&reloaded.detail.freeze, &trace.detail.freeze, sizeof(trace.detail.freeze)) == 0);
    for (size_t i = 0; i < sizeof(reloaded.diagnostics); ++i) {
        assert(reloaded.diagnostics[i] == 0);
    }
    assert(saves == before_saves);
    assert(watchdog_settings_set(
               "i/0", sizeof(slots[0].blob), read_value,
               &(struct read_value){&flash_slots[0].blob, sizeof(slots[0].blob)}) == 0);
    struct watchdog_store_slot_blob bad = slots[0].blob;
    struct watchdog_store_slot unchanged = slots[0];
    struct read_value value = {&bad, sizeof(bad)};
    for (unsigned version = 0; version < 6; ++version) {
        if (version == 4) {
            continue;
        }
        bad.rec.version = version;
        assert(watchdog_settings_set("i/0", sizeof(bad), read_value, &value) == -EINVAL);
        assert(memcmp(&unchanged, &slots[0], sizeof(unchanged)) == 0);
    }
}

static void unsupported_record_types(void) {
    reset();
    struct zmk_watchdog_incident_record rec = {.type = 255, .uptime_s = 17};
    struct zmk_watchdog_incident_record original = rec;
    assert(zmk_watchdog_store_append(&rec) == -EINVAL && saves == 0);
    assert(!memcmp(&rec, &original, sizeof(rec)) && zmk_watchdog_store_count() == 0);
    rec.type = ZMK_WATCHDOG_INCIDENT_FATAL;
    assert(zmk_watchdog_store_append(&rec) == 0);
    struct watchdog_store_slot unchanged = slots[0];
    struct watchdog_store_slot_blob bad = slots[0].blob;
    bad.rec.type = 3;
    struct read_value value = {&bad, sizeof(bad)};
    assert(watchdog_settings_set("i/0", sizeof(bad), read_value, &value) == -EINVAL);
    assert(!memcmp(&unchanged, &slots[0], sizeof(unchanged)));
    struct zmk_watchdog_store_status status;
    zmk_watchdog_store_get_status(&status);
    assert(status.capacity == CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS && status.stored == 1 &&
           status.dropped_since_boot == 0 &&
           status.recording_stopped == (status.capacity == 1 && !WATCHDOG_STORE_OVERWRITES));
}

#ifdef CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST
#define WATCHDOG_STORE_GROUPS 12
/* Present ids (1-based) must be exactly the newest `cap` after `last` appends. */
static void assert_newest_ids(unsigned last) {
    const unsigned cap = CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS;
    struct zmk_watchdog_incident_record record;
    assert(zmk_watchdog_store_count() == (last < cap ? last : cap));
    for (unsigned id = 1; id <= last; id++) {
        int found = zmk_watchdog_store_get_by_id((uint16_t)id, &record);
        if (id + cap > last) {
            assert(found == 0 && record.boot_ordinal == id);
        } else {
            assert(found == -ENOENT);
        }
    }
}

/* A full store replaces the oldest incident (lowest persisted ordinal) with an incident that
 * happened at least CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S after boot; an earlier one (a boot
 * loop) is dropped and counted, as in a store without the option. */
static void overwrite_oldest(void) {
    const unsigned cap = CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS;
    const unsigned min_up = CONFIG_ZMK_WATCHDOG_OVERWRITE_MIN_UPTIME_S;
    struct zmk_watchdog_incident_record record;
    reset();
    for (unsigned i = 0; i < cap; i++) {
        append(1000 + i);
    }
    struct zmk_watchdog_store_status status;
    zmk_watchdog_store_get_status(&status);
    assert(status.stored == cap && !status.recording_stopped && status.dropped_since_boot == 0);
    assert(!zmk_watchdog_store_recording_stopped());

    if (min_up > 0) {
        unsigned writes = saves, calls = notifications;
        struct zmk_watchdog_incident_record early = {.type = ZMK_WATCHDOG_INCIDENT_FREEZE,
                                                     .uptime_s = min_up - 1};
        assert(zmk_watchdog_store_append(&early) == -ENOSPC);
        assert(saves == writes && notifications == calls);
        assert(zmk_watchdog_store_dropped_since_boot() == 1);
        assert_newest_ids(cap);
    }

    /* The first replacement: slot, then meta, one notification; the old id is gone. */
    unsigned writes = saves, calls = notifications;
    append(min_up);
    assert(saves == writes + 2 && notifications == calls + 1);
    assert_newest_ids(cap + 1);
    assert(flash_slots[0].used && flash_slots[0].blob.id == cap + 1 &&
           flash_slots[0].blob.rec.boot_ordinal == cap + 1 &&
           flash_slots[0].blob.rec.uptime_s == min_up);
    assert(!zmk_watchdog_store_recording_stopped() && zmk_watchdog_store_count() == cap);
    /* A replacement is not a drop: only the early incident above was counted. */
    const unsigned dropped_expected = min_up > 0 ? 1U : 0U;
    assert(zmk_watchdog_store_dropped_since_boot() == dropped_expected);

    /* Two more laps: always the newest `cap` remain, in any slot order. */
    unsigned last = cap + 1;
    for (unsigned i = 0; i < 2 * cap; i++) {
        append(min_up + 10 + i);
        last++;
        assert_newest_ids(last);
    }
    assert(notifications == last && zmk_watchdog_store_dropped_since_boot() == dropped_expected);

    /* The order is the persisted ordinal, not the slot index, and survives a reload. */
    reload();
    assert_newest_ids(last);
    append(min_up + 5000);
    last++;
    assert_newest_ids(last);

    /* A failed write keeps the old incident: no change, no notification, and it is not a drop. */
    calls = notifications;
    save_failure = -EIO;
    struct zmk_watchdog_incident_record late = {.type = ZMK_WATCHDOG_INCIDENT_FREEZE,
                                                .uptime_s = min_up + 7};
    assert(zmk_watchdog_store_append(&late) == -EIO);
    save_failure = 0;
    assert_newest_ids(last);
    reload();
    assert_newest_ids(last);
    assert(notifications == calls && zmk_watchdog_store_dropped_since_boot() == 0);

    /* A failed metadata write is non-fatal (documented): the incident replaced the oldest one. */
    meta_failure = -EIO;
    append(min_up + 6000);
    meta_failure = 0;
    last++;
    reload();
    assert(zmk_watchdog_store_count() == cap);
    assert(zmk_watchdog_store_get_by_id((uint16_t)last, &record) == 0 &&
           record.uptime_s == min_up + 6000);
    assert(zmk_watchdog_store_get_by_id((uint16_t)(last - cap), &record) == -ENOENT);

    /* A stale/missing metadata cursor must not reuse the latest retained ordinal after reboot.
     * Repeated failures used to create ties, then evict a new record while older ones survived. */
    for (unsigned missing = 0; missing < 2; missing++) {
        reset();
        for (unsigned i = 0; i < cap; i++) {
            append(min_up + 100 + i);
        }
        last = cap;
        if (missing) {
            flash_has_meta = false;
        }
        meta_failure = -EIO;
        for (unsigned i = 0; i < 3 * cap + 2; i++) {
            reload();
            append(min_up + 7000 + i);
            last++;
            /* IDs may be reused after eviction; ordinals define chronology. */
            assert(zmk_watchdog_store_count() == cap);
            for (unsigned expected = last - cap + 1; expected <= last; expected++) {
                unsigned found = 0;
                for (unsigned index = 0; index < cap; index++) {
                    assert(zmk_watchdog_store_get(index, &record) == 0);
                    if (record.boot_ordinal == expected) {
                        found++;
                        unsigned uptime = expected <= cap ? min_up + 99 + expected
                                                          : min_up + 6999 + expected - cap;
                        assert(record.uptime_s == uptime);
                    }
                }
                assert(found == 1);
            }
        }
        meta_failure = 0;
        reload();
        struct zmk_watchdog_incident_record recovered = {.type = ZMK_WATCHDOG_INCIDENT_FREEZE,
                                                         .uptime_s = min_up + 8000};
        assert(zmk_watchdog_store_append(&recovered) == 0);
        assert(recovered.boot_ordinal == ++last);
    }

    /* The ordinal counter wraps modulo 2^32: the oldest is still the first appended, not the lowest
     * value. */
    reset();
    flash_has_meta = true;
    flash_meta = (struct watchdog_store_meta){.next_boot_ordinal = UINT32_MAX - 1U, .next_id = 1};
    reload();
    for (unsigned i = 0; i < cap; i++) {
        append(2000 + i);
    }
    assert(zmk_watchdog_store_get_by_id(1, &record) == 0 && record.boot_ordinal == UINT32_MAX - 1U);
    append(min_up + 3000);
    assert(zmk_watchdog_store_get_by_id(1, &record) == -ENOENT);
    for (unsigned i = 2; i <= cap; i++) {
        assert(zmk_watchdog_store_get_by_id((uint16_t)i, &record) == 0);
    }
    assert(zmk_watchdog_store_get_by_id((uint16_t)(cap + 1), &record) == 0 &&
           record.uptime_s == min_up + 3000);
    uint32_t ordinal = record.boot_ordinal;
    meta_failure = -EIO;
    for (unsigned i = 0; i < 2 * cap + 2; i++) {
        reload();
        struct zmk_watchdog_incident_record wrapped = {.type = ZMK_WATCHDOG_INCIDENT_FREEZE,
                                                       .uptime_s = min_up + 9000 + i};
        assert(zmk_watchdog_store_append(&wrapped) == 0);
        assert(wrapped.boot_ordinal == ++ordinal);
        for (unsigned age = 0; age < cap; age++) {
            unsigned found = 0;
            for (unsigned index = 0; index < cap; index++) {
                assert(zmk_watchdog_store_get(index, &record) == 0);
                found += record.boot_ordinal == ordinal - age;
            }
            assert(found == 1);
        }
    }
    meta_failure = 0;
}
#else
#define WATCHDOG_STORE_GROUPS 11
#endif

int main(void) {
#if defined(CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST) && defined(WATCHDOG_STORE_OVERWRITE_ONLY)
    /* Capacities 2 and 3 run only the overwrite scenario: the other scenarios were written for
     * capacities 1 and 16. */
    overwrite_oldest();
    assert(!watchdog_store_lock.held && !watchdog_store_write_lock.held);
    printf("watchdog store: PASS capacity=%d groups=1 overwrite=on (overwrite scenario only)\n",
           CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS);
    return 0;
#endif
    readiness();
    capacity_and_reload();
    backend_failures();
    delete_all_failures();
    settings_read_failures();
    id_metadata_failures();
    id_wrap();
    legacy_duplicate_ids();
    slot_key_validation();
    record_formats();
    unsupported_record_types();
#ifdef CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST
    overwrite_oldest();
#endif
    assert(!watchdog_store_lock.held && !watchdog_store_write_lock.held);
    printf("watchdog store: PASS capacity=%d groups=%d overwrite=%s\n",
           CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS, WATCHDOG_STORE_GROUPS,
#ifdef CONFIG_ZMK_WATCHDOG_OVERWRITE_OLDEST
           "on"
#else
           "off"
#endif
    );
    return 0;
}
