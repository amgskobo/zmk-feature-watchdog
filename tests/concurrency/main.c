/* SPDX-License-Identifier: MIT */
/* Whole production units, real Zephyr settings/mutex/work APIs and ZMK's
 * actual low-priority queue. Only storage and reset-cause drivers are fake.
 * Save wrappers mark the interleaving before calling the real settings API. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

static int monitored_save(const char *name, const void *value, size_t size);
static int monitored_delete(const char *name);
void test_reset_audit(void);
void test_cancel_boot_work(void);
void test_cancel_reset_work(void);
void test_new_boot_without_pending(void);
#define settings_save_one monitored_save
#define settings_delete monitored_delete
#include "../../src/watchdog_store.c"
#undef settings_save_one
#undef settings_delete

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            printk("FAIL line=%d time=%lld: %s\n", __LINE__, k_uptime_get(), #condition);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

K_SEM_DEFINE(load_entered, 0, 1);
K_SEM_DEFINE(allow_load, 0, 1);
K_SEM_DEFINE(writer_at_api, 0, 16);
K_SEM_DEFINE(save_entered, 0, 1);
K_SEM_DEFINE(allow_save, 0, 1);
K_THREAD_STACK_DEFINE(load_stack, 4096);
K_THREAD_STACK_DEFINE(writer_stack, 4096);
K_THREAD_STACK_DEFINE(second_stack, 4096);
K_THREAD_STACK_DEFINE(reader_stack, 4096);
static struct k_thread loader, writer, second_writer, reader;
static struct {
    bool used;
    struct watchdog_store_slot_blob blob;
} disk[CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS];
static struct watchdog_store_meta disk_meta;
static bool pause_load, pause_save, monitor_api;
static atomic_t writes, callbacks;
static int hw_gets, hw_clears, load_result, writer_result, second_result;
static int hw_error, hw_clear_error, backend_error;
static unsigned save_attempts;
static uint32_t hw_cause = RESET_WATCHDOG;
static uint16_t target_id;

enum operation { APPEND, DELETE, DELETE_ALL, CONVERT };
static enum operation writer_operation;

static int monitored_save(const char *name, const void *value, size_t size) {
    if (monitor_api) {
        k_sem_give(&writer_at_api);
    }
    return settings_save_one(name, value, size);
}

static int monitored_delete(const char *name) {
    if (monitor_api) {
        k_sem_give(&writer_at_api);
    }
    return settings_delete(name);
}

int reset_cause_get(uint32_t *cause) {
    hw_gets++;
    *cause = hw_cause;
    return hw_error;
}

int reset_cause_clear(void) {
    hw_clears++;
    if (!hw_clear_error) {
        hw_cause = 0;
    }
    return hw_clear_error;
}

static ssize_t read_value(void *context, void *value, size_t size) {
    memcpy(value, context, size);
    return size;
}

static int backend_load(struct settings_store *store, const struct settings_load_arg *arg) {
    ARG_UNUSED(store);
    if (pause_load) {
        pause_load = false;
        k_sem_give(&load_entered);
        CHECK(k_sem_take(&allow_load, K_SECONDS(1)) == 0);
    }
    CHECK(settings_call_set_handler("wdg/meta", sizeof(disk_meta), read_value, &disk_meta, arg) ==
          0);
    for (size_t i = 0; i < ARRAY_SIZE(disk); i++) {
        if (disk[i].used) {
            char name[24];
            snprintf(name, sizeof(name), "wdg/i/%zu", i);
            CHECK(settings_call_set_handler(name, sizeof(disk[i].blob), read_value, &disk[i].blob,
                                            arg) == 0);
        }
    }
    return 0;
}

static int backend_save(struct settings_store *store, const char *name, const char *value,
                        size_t size) {
    ARG_UNUSED(store);
    ++save_attempts;
    if (backend_error) {
        return backend_error;
    }
    if (pause_save) {
        pause_save = false;
        k_sem_give(&save_entered);
        CHECK(k_sem_take(&allow_save, K_SECONDS(1)) == 0);
    }
    atomic_inc(&writes);
    if (strcmp(name, "wdg/meta") == 0) {
        CHECK(size == sizeof(disk_meta));
        memcpy(&disk_meta, value, size);
    } else {
        unsigned slot;
        CHECK(sscanf(name, "wdg/i/%u", &slot) == 1 && slot < ARRAY_SIZE(disk));
        if (value == NULL) {
            memset(&disk[slot], 0, sizeof(disk[slot]));
        } else {
            CHECK(size == sizeof(disk[slot].blob));
            memcpy(&disk[slot].blob, value, size);
            disk[slot].used = true;
        }
    }
    return 0;
}

static const struct settings_store_itf backend_itf = {
    .csi_load = backend_load,
    .csi_save = backend_save,
};
static struct settings_store backend = {.cs_itf = &backend_itf};

static void on_append(uint16_t id, const struct zmk_watchdog_incident_record *rec) {
    struct zmk_watchdog_incident_record stored;
    CHECK(zmk_watchdog_store_get_by_id(id, &stored) == 0);
    CHECK(memcmp(rec, &stored, sizeof(stored)) == 0);
    atomic_inc(&callbacks);
}

static void load_thread(void *a, void *b, void *c) { load_result = settings_load_subtree("wdg"); }

static void writer_thread(void *a, void *b, void *c) {
    struct zmk_watchdog_incident_record rec = {.uptime_s = 200};
    switch (writer_operation) {
    case APPEND:
        writer_result = zmk_watchdog_store_append(&rec);
        break;
    case DELETE:
        writer_result = zmk_watchdog_store_delete(target_id);
        break;
    case DELETE_ALL:
        writer_result = zmk_watchdog_store_delete_all();
        break;
    case CONVERT:
        writer_result = zmk_watchdog_pending_convert();
        break;
    }
}

static void second_thread(void *a, void *b, void *c) {
    struct zmk_watchdog_incident_record rec = {.uptime_s = 300};
    second_result = zmk_watchdog_store_append(&rec);
}

struct read_job {
    uint16_t id;
    int result;
    struct zmk_watchdog_incident_record record;
};

static void reader_thread(void *a, void *b, void *c) {
    struct read_job *job = a;
    job->result = zmk_watchdog_store_get_by_id(job->id, &job->record);
}

static void start_reader(struct read_job *job) {
    job->result = 99;
    k_thread_create(&reader, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack), reader_thread, job,
                    NULL, NULL, 5, 0, K_NO_WAIT);
}

static void start_load(void) {
    load_result = 99;
    k_thread_create(&loader, load_stack, K_THREAD_STACK_SIZEOF(load_stack), load_thread, NULL, NULL,
                    NULL, 5, 0, K_NO_WAIT);
}

static void start_writer(void) {
    writer_result = 99;
    k_thread_create(&writer, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack), writer_thread, NULL,
                    NULL, NULL, 5, 0, K_NO_WAIT);
}

static void verify_disk(void) {
    uint16_t n = 0;
    for (size_t i = 0; i < ARRAY_SIZE(disk); i++) {
        if (disk[i].used) {
            struct zmk_watchdog_incident_record rec;
            uint16_t id;
            CHECK(zmk_watchdog_store_get_with_id(n++, &rec, &id) == 0);
            CHECK(id == disk[i].blob.id);
            CHECK(memcmp(&rec, &disk[i].blob.rec, sizeof(rec)) == 0);
        }
    }
    CHECK(n == zmk_watchdog_store_count());
    CHECK(meta.next_id == disk_meta.next_id &&
          meta.next_boot_ordinal == disk_meta.next_boot_ordinal);
}

static void boot_readiness(void) {
    struct zmk_watchdog_incident_record rec = {.uptime_s = 99};
    struct zmk_watchdog_incident_record original = rec;
    CHECK(!zmk_watchdog_store_is_ready());
    CHECK(zmk_watchdog_store_append(&rec) == -EAGAIN);
    CHECK(memcmp(&rec, &original, sizeof(rec)) == 0);
    CHECK(zmk_watchdog_store_delete(7) == -EAGAIN);
    CHECK(zmk_watchdog_store_delete_all() == -EAGAIN);
    zmk_watchdog_pending_set(&rec);
    CHECK(zmk_watchdog_pending_convert() == -EAGAIN);
    pause_load = true;
    start_load();
    CHECK(k_sem_take(&load_entered, K_SECONDS(1)) == 0);
    k_sleep(K_MSEC(35)); /* Longer than three configured boot retry intervals. */
    CHECK(atomic_get(&writes) == 0 && !zmk_watchdog_store_is_ready());
    CHECK(!zmk_watchdog_pending_had_incident_this_boot());
    test_reset_audit();
    CHECK(hw_gets == 0 && hw_clears == 0);
    test_cancel_boot_work();
    k_sem_give(&allow_load);
    CHECK(k_thread_join(&loader, K_SECONDS(1)) == 0 && load_result == 0);
    pause_load = false;
    CHECK(!zmk_watchdog_pending_had_incident_this_boot());
    /* Unsupported/uninteresting hardware must not consume pending data. */
    hw_error = -ENOSYS;
    test_reset_audit();
    CHECK(hw_gets == 1 && hw_clears == 0 && atomic_get(&writes) == 0);
    hw_error = 0;
    hw_cause = RESET_PIN;
    test_reset_audit();
    CHECK(hw_gets == 2 && hw_clears == 1 && atomic_get(&writes) == 0);
    CHECK(!zmk_watchdog_pending_had_incident_this_boot());
    hw_cause = RESET_WATCHDOG;
    /* The audit must finish pending conversion itself if its retry runs first. */
    test_reset_audit();
    CHECK(hw_gets == 3 && hw_clears == 2);
    CHECK(zmk_watchdog_store_is_ready() && zmk_watchdog_store_count() == 2);
    CHECK(zmk_watchdog_pending_convert() == 0 && atomic_get(&callbacks) == 1);
    CHECK(zmk_watchdog_pending_had_incident_this_boot());
    CHECK(disk[0].blob.id == 7 && disk[0].blob.rec.uptime_s == 10);
    CHECK(disk[1].blob.id == 8 && disk[1].blob.rec.uptime_s == 99);
    test_cancel_boot_work();
    test_cancel_reset_work();
    verify_disk();
    printk("PASS delayed boot retains pending and orders reset audit time=%lld\n", k_uptime_get());
}

static void load_vs_operation(enum operation op) {
    if (zmk_watchdog_store_count() == 0) {
        struct zmk_watchdog_incident_record rec = {.uptime_s = 100};
        CHECK(zmk_watchdog_store_append(&rec) == 0);
    }
    struct zmk_watchdog_incident_record first;
    CHECK(zmk_watchdog_store_get_with_id(0, &first, &target_id) == 0);
    uint16_t count = zmk_watchdog_store_count();
    writer_operation = op;
    monitor_api = pause_load = true;
    k_sem_reset(&writer_at_api);
    start_load();
    CHECK(k_sem_take(&load_entered, K_SECONDS(1)) == 0);
    start_writer();
    CHECK(k_sem_take(&writer_at_api, K_SECONDS(1)) == 0);
    /* Writer is about to enter the real settings API while load owns settings_lock. */
    CHECK(k_mutex_lock(&watchdog_store_lock, K_NO_WAIT) == 0);
    k_mutex_unlock(&watchdog_store_lock);
    CHECK(zmk_watchdog_store_count() == count);
    k_sem_give(&allow_load);
    CHECK(k_thread_join(&loader, K_SECONDS(1)) == 0 && load_result == 0);
    CHECK(k_thread_join(&writer, K_SECONDS(1)) == 0 && writer_result == 0);
    monitor_api = pause_load = false;
    uint16_t expected = op == APPEND ? count + 1 : op == DELETE ? count - 1 : 0;
    CHECK(zmk_watchdog_store_count() == expected);
    verify_disk();
    printk("PASS settings load vs operation=%d count=%u time=%lld\n", op, expected, k_uptime_get());
}

static void save_vs_load_and_writer(void) {
    CHECK(zmk_watchdog_store_delete_all() == 0);
    writer_operation = APPEND;
    pause_save = true;
    start_writer();
    CHECK(k_sem_take(&save_entered, K_SECONDS(1)) == 0);
    CHECK(zmk_watchdog_store_count() == 0); /* Reader is not blocked by Flash I/O. */
    start_load();
    second_result = 99;
    k_thread_create(&second_writer, second_stack, K_THREAD_STACK_SIZEOF(second_stack),
                    second_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_sem_give(&allow_save);
    CHECK(k_thread_join(&writer, K_SECONDS(1)) == 0 && writer_result == 0);
    CHECK(k_thread_join(&loader, K_SECONDS(1)) == 0 && load_result == 0);
    CHECK(k_thread_join(&second_writer, K_SECONDS(1)) == 0 && second_result == 0);
    CHECK(zmk_watchdog_store_count() == 2);
    CHECK(disk[0].blob.id != disk[1].blob.id);
    CHECK(disk[0].blob.rec.uptime_s == 200 && disk[1].blob.rec.uptime_s == 300);
    verify_disk();
    printk("PASS save vs load and second writer time=%lld\n", k_uptime_get());
}

static void read_vs_delete(void) {
    CHECK(zmk_watchdog_store_delete_all() == 0);
    struct zmk_watchdog_incident_record rec = {.uptime_s = 111};
    CHECK(zmk_watchdog_store_append(&rec) == 0);
    struct zmk_watchdog_incident_record stored;
    CHECK(zmk_watchdog_store_get_with_id(0, &stored, &target_id) == 0);
    struct read_job job = {.id = target_id};
    pause_load = true;
    start_reader(&job);
    CHECK(k_sem_take(&load_entered, K_SECONDS(1)) == 0);
    writer_operation = DELETE;
    start_writer();
    k_sleep(K_MSEC(20));
    CHECK(writer_result == 99 && job.result == 99);
    CHECK(k_mutex_lock(&watchdog_store_lock, K_NO_WAIT) == 0);
    k_mutex_unlock(&watchdog_store_lock);
    CHECK(zmk_watchdog_store_count() == 1);
    pause_load = false;
    k_sem_give(&allow_load);
    CHECK(k_thread_join(&reader, K_SECONDS(1)) == 0 && job.result == 0);
    CHECK(k_thread_join(&writer, K_SECONDS(1)) == 0 && writer_result == 0);
    CHECK(!memcmp(&job.record, &stored, sizeof(stored)));
    CHECK(zmk_watchdog_store_count() == 0);
    printk("PASS persisted read vs delete keeps selected record time=%lld\n", k_uptime_get());
}

static void save_vs_read(void) {
    struct zmk_watchdog_incident_record rec = {.uptime_s = 222};
    CHECK(zmk_watchdog_store_append(&rec) == 0);
    struct zmk_watchdog_incident_record stored;
    CHECK(zmk_watchdog_store_get_with_id(0, &stored, &target_id) == 0);
    writer_operation = APPEND;
    pause_save = true;
    start_writer();
    CHECK(k_sem_take(&save_entered, K_SECONDS(1)) == 0);
    struct read_job job = {.id = target_id};
    start_reader(&job);
    k_sleep(K_MSEC(20));
    CHECK(job.result == 99);
    struct zmk_watchdog_store_status status;
    zmk_watchdog_store_get_status(&status);
    CHECK(status.stored == 1);
    k_sem_give(&allow_save);
    CHECK(k_thread_join(&writer, K_SECONDS(1)) == 0 && writer_result == 0);
    CHECK(k_thread_join(&reader, K_SECONDS(1)) == 0 && job.result == 0);
    CHECK(!memcmp(&job.record, &stored, sizeof(stored)));
    verify_disk();
    printk("PASS persisted read waits for save; status remains available time=%lld\n",
           k_uptime_get());
}

static void two_readers(void) {
    CHECK(zmk_watchdog_store_count() == 2);
    struct read_job first = {0}, other = {.result = 99};
    struct zmk_watchdog_incident_record expected_first, expected_other;
    CHECK(zmk_watchdog_store_get_with_id(0, &expected_first, &first.id) == 0);
    CHECK(zmk_watchdog_store_get_with_id(1, &expected_other, &other.id) == 0);
    CHECK(expected_first.uptime_s != expected_other.uptime_s);
    pause_load = true;
    start_reader(&first);
    CHECK(k_sem_take(&load_entered, K_SECONDS(1)) == 0);
    k_thread_create(&second_writer, second_stack, K_THREAD_STACK_SIZEOF(second_stack),
                    reader_thread, &other, NULL, NULL, 5, 0, K_NO_WAIT);
    k_sleep(K_MSEC(20));
    CHECK(first.result == 99 && other.result == 99);
    pause_load = false;
    k_sem_give(&allow_load);
    CHECK(k_thread_join(&reader, K_SECONDS(1)) == 0 && first.result == 0);
    CHECK(k_thread_join(&second_writer, K_SECONDS(1)) == 0 && other.result == 0);
    CHECK(!memcmp(&first.record, &expected_first, sizeof(expected_first)));
    CHECK(!memcmp(&other.record, &expected_other, sizeof(expected_other)));
    printk("PASS two persisted readers keep independent outputs time=%lld\n", k_uptime_get());
}

static void pending_failure_recovery(void) {
    CHECK(zmk_watchdog_store_delete_all() == 0);
    struct zmk_watchdog_incident_record rec = {.type = ZMK_WATCHDOG_INCIDENT_FATAL,
                                               .uptime_s = 4567};
    rec.detail.fatal.reason = 3;
    zmk_watchdog_pending_set(&rec);
    backend_error = -EIO;
    CHECK(zmk_watchdog_pending_convert() == -EIO);
    CHECK(zmk_watchdog_store_count() == 0);
    unsigned attempts = save_attempts;
    CHECK(zmk_watchdog_pending_convert() == -EAGAIN && save_attempts == attempts);
    backend_error = 0;
    k_sleep(K_MSEC(1050));
    CHECK(zmk_watchdog_pending_convert() == 0);
    CHECK(zmk_watchdog_store_count() == 1);
    struct zmk_watchdog_incident_record stored;
    CHECK(zmk_watchdog_store_get(0, &stored) == 0 && stored.uptime_s == 4567 &&
          stored.detail.fatal.reason == 3);
    attempts = save_attempts;
    CHECK(zmk_watchdog_pending_convert() == 0 && save_attempts == attempts);
    printk("PASS pending failed save retained/backoff/retry once time=%lld\n", k_uptime_get());
}

static void parallel_pending_conversion(void) {
    CHECK(zmk_watchdog_store_delete_all() == 0);
    struct zmk_watchdog_incident_record rec = {.uptime_s = 700};
    zmk_watchdog_pending_set(&rec);
    writer_operation = CONVERT;
    pause_save = true;
    start_writer();
    CHECK(k_sem_take(&save_entered, K_SECONDS(1)) == 0);
    CHECK(zmk_watchdog_pending_convert() == -EAGAIN);
    /* A newer capture can publish even while conversion is stalled in Flash. */
    rec.uptime_s = 701;
    zmk_watchdog_pending_set(&rec);
    k_sem_give(&allow_save);
    CHECK(k_thread_join(&writer, K_SECONDS(1)) == 0 && writer_result == 0);
    CHECK(zmk_watchdog_pending_convert() == 0 && zmk_watchdog_store_count() == 2);
    struct zmk_watchdog_incident_record stored;
    CHECK(zmk_watchdog_store_get(0, &stored) == 0 && stored.uptime_s == 700);
    CHECK(zmk_watchdog_store_get(1, &stored) == 0 && stored.uptime_s == 701);
    CHECK(zmk_watchdog_pending_convert() == 0 && zmk_watchdog_store_count() == 2);
    printk("PASS parallel conversion preserves newer fatal record time=%lld\n", k_uptime_get());
}

static void corrupt_and_full_pending(void) {
    CHECK(zmk_watchdog_store_delete_all() == 0);
    struct zmk_watchdog_incident_record rec = {.uptime_s = 800};
    zmk_watchdog_pending_set(&rec);
    zmk_watchdog_pending_corrupt_crc_for_test();
    unsigned attempts = save_attempts;
    CHECK(zmk_watchdog_pending_convert() == 0 && save_attempts == attempts);
    CHECK(zmk_watchdog_store_count() == 0);
    rec.type = 255;
    zmk_watchdog_pending_set(&rec);
    CHECK(zmk_watchdog_pending_convert() == 0 && save_attempts == attempts);
    rec.type = ZMK_WATCHDOG_INCIDENT_FREEZE;
    for (unsigned i = 0; i < CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS; ++i) {
        rec.uptime_s = 900 + i;
        CHECK(zmk_watchdog_store_append(&rec) == 0);
    }
    uint32_t dropped = zmk_watchdog_store_dropped_since_boot();
    attempts = save_attempts;
    zmk_watchdog_pending_set(&rec);
    CHECK(zmk_watchdog_pending_convert() == 0 && save_attempts == attempts);
    CHECK(zmk_watchdog_store_dropped_since_boot() == dropped + 1);
    CHECK(zmk_watchdog_pending_convert() == 0 && save_attempts == attempts);
    CHECK(zmk_watchdog_store_dropped_since_boot() == dropped + 1);
    CHECK(zmk_watchdog_store_count() == CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS);
    printk("PASS corrupt/unsupported pending rejected and full-store drop once time=%lld\n",
           k_uptime_get());
}

static void reset_audit_failure_recovery(void) {
    CHECK(zmk_watchdog_store_delete_all() == 0);
    /* Controlled fresh-boot handoff state; no retained incident explains this cause. */
    test_new_boot_without_pending();
    hw_cause = RESET_CPU_LOCKUP;
    hw_error = -EIO;
    unsigned clears = hw_clears;
    test_reset_audit();
    test_cancel_reset_work();
    CHECK(hw_clears == clears && zmk_watchdog_store_count() == 0);
    hw_error = 0;
    backend_error = -EIO;
    test_reset_audit();
    test_cancel_reset_work();
    CHECK(hw_clears == clears && hw_cause == RESET_CPU_LOCKUP);
    CHECK(zmk_watchdog_store_count() == 0);
    backend_error = 0;
    hw_clear_error = -EIO;
    test_reset_audit();
    test_cancel_reset_work();
    CHECK(zmk_watchdog_store_count() == 1 && hw_cause == RESET_CPU_LOCKUP);
    struct zmk_watchdog_incident_record stored;
    CHECK(zmk_watchdog_store_get(0, &stored) == 0 &&
          stored.type == ZMK_WATCHDOG_INCIDENT_RESET_CAUSE &&
          stored.detail.reset.cause_bits == RESET_CPU_LOCKUP);
    unsigned attempts = save_attempts;
    test_reset_audit();
    test_cancel_reset_work();
    CHECK(zmk_watchdog_store_count() == 1 && save_attempts == attempts);
    hw_clear_error = 0;
    test_reset_audit();
    test_cancel_reset_work();
    CHECK(hw_cause == 0 && save_attempts == attempts);
    printk("PASS reset read/save/clear failure retains cause and retries without duplicate "
           "time=%lld\n",
           k_uptime_get());
}

int main(void) {
    CHECK(settings_subsys_init() == 0);
    settings_src_register(&backend);
    settings_dst_register(&backend);
    disk_meta = (struct watchdog_store_meta){.next_id = 8, .next_boot_ordinal = 8};
    disk[0].used = true;
    disk[0].blob.id = 7;
    disk[0].blob.rec.version = ZMK_WATCHDOG_RECORD_VERSION;
    disk[0].blob.rec.uptime_s = 10;
    zmk_watchdog_store_set_appended_callback(on_append);
    boot_readiness();
    load_vs_operation(APPEND);
    load_vs_operation(DELETE);
    load_vs_operation(DELETE_ALL);
    save_vs_load_and_writer();
    pending_failure_recovery();
    parallel_pending_conversion();
    corrupt_and_full_pending();
    reset_audit_failure_recovery();
    read_vs_delete();
    save_vs_read();
    two_readers();
    printk("PASS watchdog concurrency groups=12\n");
    exit(0);
}
