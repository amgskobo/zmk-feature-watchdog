/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <cormoran/zmk/watchdog.h>
#include <cormoran/zmk/watchdog_diagnostics.h>
#include <cormoran/zmk/watchdog_context.h>
#if defined(CONFIG_CPU_CORTEX_M3) || defined(CONFIG_CPU_CORTEX_M4)
#include <cmsis_core.h>
#endif

/* Override Zephyr's weak fatal handler. Another selected strong definition
 * conflicts at link time: inspect the final ELF rather than assuming selection.
 * No logging, Flash or module lock is used in capture. Upstream fault entry,
 * optional kernel clock/name reads and the SoC reboot path remain external
 * boundaries. Commit a minimal explanation before enrichment can fail. */

static bool readable_esf(const struct arch_esf *esf) {
    if (!esf) {
        return false;
    }
#if defined(CONFIG_CPU_CORTEX_M)
    uintptr_t address = (uintptr_t)esf;
    size_t length = sizeof(esf->basic);
    const uintptr_t start = CONFIG_SRAM_BASE_ADDRESS;
    const size_t size = KB(CONFIG_SRAM_SIZE);
    return !(address & 3U) && address >= start && length <= size &&
           address - start <= size - length;
#else
    return true;
#endif
}

void watchdog_fatal_build_record(struct zmk_watchdog_incident_record *rec, unsigned int reason,
                                 const struct arch_esf *esf) {
    if (!rec) {
        return;
    }
    bool has_esf = readable_esf(esf);
    ARG_UNUSED(has_esf);
    memset(rec, 0, sizeof(*rec));
    rec->type = ZMK_WATCHDOG_INCIDENT_FATAL;
    rec->uptime_s = k_uptime_get_32() / 1000U;
    rec->detail.fatal.reason = (uint32_t)reason;

#if defined(CONFIG_ARM)
    if (has_esf) {
        rec->detail.fatal.pc = esf->basic.pc;
        rec->detail.fatal.lr = esf->basic.lr;
    }
#else
    /* Other architectures (including native_sim / ARCH_POSIX, whose
     * arch_esf carries no register info) don't expose a portable PC/LR
     * field on arch_esf -- leave them 0 rather than guess at a layout. */
    ARG_UNUSED(esf);
#endif

#if defined(CONFIG_CPU_CORTEX_M)
    if (has_esf) {
        rec->flags |= ZMK_WATCHDOG_FATAL_CONTEXT_VALID;
        rec->detail.fatal.context[0] = esf->basic.a1;
        rec->detail.fatal.context[1] = esf->basic.a2;
        rec->detail.fatal.context[2] = esf->basic.a3;
        rec->detail.fatal.context[3] = esf->basic.a4;
        rec->detail.fatal.context[4] = esf->basic.ip;
        rec->detail.fatal.context[5] = esf->basic.xpsr;
    }
#endif
    zmk_watchdog_diagnostics_snapshot(rec->diagnostics);
#if defined(CONFIG_CPU_CORTEX_M3) || defined(CONFIG_CPU_CORTEX_M4)
    if (rec->diagnostics[0]) {
        rec->diagnostics[2] |= 4U;
        sys_put_le32(SCB->CFSR, rec->diagnostics + 16);
        sys_put_le32(SCB->HFSR, rec->diagnostics + 20);
    }
#endif
    const char *thread_name = NULL;
#if defined(CONFIG_THREAD_NAME)
    thread_name = k_thread_name_get(k_current_get());
#endif
    if (thread_name == NULL || thread_name[0] == '\0') {
        thread_name = "?";
    }
    strncpy(rec->detail.fatal.thread_name, thread_name, sizeof(rec->detail.fatal.thread_name) - 1);
    watchdog_context_capture(rec, has_esf ? esf : NULL, NULL);
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    struct zmk_watchdog_incident_record *rec = zmk_watchdog_capture_begin_record();
    if (!rec) {
        zmk_watchdog_reboot();
        return;
    }
    /* Preserve a reason-only record before optional clock/name/ESF reads.
     * A fault in that enrichment path must not recursively build another trace. */
    memset(rec, 0, sizeof(*rec));
    rec->type = ZMK_WATCHDOG_INCIDENT_FATAL;
    rec->flags = ZMK_WATCHDOG_CAPTURE_INCOMPLETE;
    rec->detail.fatal.reason = reason;
    rec->detail.fatal.thread_name[0] = '?';
    watchdog_context_minimal(rec);
    zmk_watchdog_pending_set(rec);
    watchdog_fatal_build_record(rec, reason, esf);

    zmk_watchdog_pending_set(rec);
    zmk_watchdog_reboot();
    zmk_watchdog_capture_end(); /* Returning test hook only. */

    /* zmk_watchdog_reboot() never returns in production (sys_reboot() is
     * noreturn); a test override could in principle return, in which case
     * falling off the end here just means the faulting context is not
     * revived, matching upstream's "never returns" contract closely enough
     * for a test build. */
}
