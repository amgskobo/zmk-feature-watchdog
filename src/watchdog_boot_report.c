/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <zmk/workqueue.h>

#ifndef CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL
#define CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_watchdog_fork_watchdog_boot_report, CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL);

/* One bounded pass per boot, independent of Studio/event/radio delivery.
 * Enumerations can change during a pass; an error is reported, never hidden.
 * No names, register payloads, keys, peer identifiers or input values are logged.
 * One record per second limits deferred log pressure. Reading never clears it. */
static uint16_t report_index, report_total;
static uint8_t ready_attempts;
static bool report_started;

static uint32_t report_code_address(uint32_t address) {
    /* A corrupted exception frame can contain arbitrary data in PC/LR.
     * Emit only addresses in this image's executable text, not arbitrary
     * saved words, settings/storage addresses or bootloader data. */
#if defined(CONFIG_CPU_CORTEX_M)
    const uintptr_t start = (uintptr_t)__text_region_start;
    const uintptr_t end = (uintptr_t)__text_region_end;
    if (address && address >= start && address < end) {
        return address;
    }
#else
    /* Native host linkers do not define the Cortex-M text-region symbols.
     * This uint32 wire address is not a native host pointer; fail closed. */
    ARG_UNUSED(address);
#endif
    return 0;
}

static void report_diagnostics(unsigned source, const uint8_t *bytes) {
    if (bytes[0] != 1) {
        LOG_INF("diag source=%u available=0", source);
        return;
    }
    LOG_INF("diag source=%u tag=%08x flags=%u count=%u", source, sys_get_le32(bytes + 4), bytes[2],
            bytes[1]);
    if (bytes[2] & 4U) {
        LOG_INF("fault source=%u cfsr=%08x hfsr=%08x", source, sys_get_le32(bytes + 16),
                sys_get_le32(bytes + 20));
    }
    for (unsigned i = 0; i < MIN(bytes[1], 7U); ++i) {
        const uint8_t *entry = bytes + 24 + 8 * i;
        LOG_INF("event source=%u t_ms=%u kind=%u value=%u", source, sys_get_le32(entry), entry[4],
                sys_get_le16(entry + 6));
    }
}

static void boot_report_work_handler(struct k_work *work) {
    if (!zmk_watchdog_store_is_ready()) {
        if (++ready_attempts < 30) {
            k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(),
                                        k_work_delayable_from_work(work), K_SECONDS(1));
        } else {
            LOG_WRN("boot history unavailable: settings not ready after %u attempts",
                    ready_attempts);
        }
        return;
    }
    if (!report_started) {
        struct zmk_watchdog_store_status status;
        uint8_t previous[ZMK_WATCHDOG_DIAGNOSTIC_SIZE];
        zmk_watchdog_store_get_status(&status);
        report_total = MIN(status.stored, status.capacity);
        report_started = true;
        LOG_INF("boot tag=%08x reset=%08x flags=%08x", zmk_watchdog_diagnostics_build_tag(),
                zmk_watchdog_diagnostics_reset_cause(), zmk_watchdog_diagnostics_runtime_flags());
        LOG_INF("history stored=%u capacity=%u dropped=%u stopped=%u", status.stored,
                status.capacity, status.dropped_since_boot, status.recording_stopped);
        if (zmk_watchdog_diagnostics_previous(previous)) {
            report_diagnostics(0, previous);
        }
    }
    if (report_index >= report_total) {
        LOG_INF("history complete attempted=%u", report_index);
        return;
    }
    struct zmk_watchdog_incident_record record;
    uint16_t id;
    unsigned index = report_index++;
    int ret = zmk_watchdog_store_get_with_id(index, &record, &id);
    if (ret < 0) {
        LOG_WRN("history index=%u err=%d", index, ret);
    } else {
        /* source=index+1 keeps the prior checkpoint (source=0) distinct. */
        unsigned source = index + 1;
        LOG_INF("incident source=%u id=%u type=%u ordinal=%u", source, id, record.type,
                record.boot_ordinal);
        LOG_INF("incident source=%u version=%u t_s=%u flags=%u", source, record.version,
                record.uptime_s, record.flags);
        switch (record.type) {
        case ZMK_WATCHDOG_INCIDENT_FATAL:
            LOG_INF("fatal source=%u reason=%u pc=%08x lr=%08x", source, record.detail.fatal.reason,
                    report_code_address(record.detail.fatal.pc),
                    report_code_address(record.detail.fatal.lr));
            break;
        case ZMK_WATCHDOG_INCIDENT_FREEZE:
            LOG_INF("freeze source=%u channel=%u state=%u trace_flags=%u", source,
                    record.detail.freeze.channel_id, record.detail.freeze.thread_state,
                    record.detail.freeze.trace_flags);
            break;
        case ZMK_WATCHDOG_INCIDENT_RESET_CAUSE:
            LOG_INF("reset source=%u bits=%08x", source, record.detail.reset.cause_bits);
            break;
        default:
            LOG_WRN("incident source=%u unsupported type=%u", source, record.type);
            break;
        }
        if (record.version >= 3) {
            report_diagnostics(source, record.diagnostics);
        }
    }
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), k_work_delayable_from_work(work),
                                K_SECONDS(1));
}

static K_WORK_DELAYABLE_DEFINE(boot_report_work, boot_report_work_handler);

static int boot_report_init(void) {
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &boot_report_work, K_SECONDS(5));
    return 0;
}
SYS_INIT(boot_report_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
