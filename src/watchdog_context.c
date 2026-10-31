/* SPDX-License-Identifier: MIT */
/* Common evidence only: no subsystem policy, allocation or Flash. Recorder
 * writers never wait for each other; timed operations require safe uptime access. */
#include <string.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <cormoran/zmk/watchdog_context.h>
#if defined(CONFIG_CPU_CORTEX_M3) || defined(CONFIG_CPU_CORTEX_M4)
#include <cmsis_core.h>
#include <zephyr/linker/linker-defs.h>
#endif

struct operation_buffer {
    uint8_t count;
    uint8_t reserved[3];
    uint8_t entries[ZMK_WDG_CONTEXT_OPERATIONS][ZMK_WDG_CONTEXT_OPERATION_SIZE];
};
BUILD_ASSERT(ZMK_WDG_CONTEXT_OPERATIONS_OFFSET +
                 sizeof(((struct operation_buffer *)NULL)->entries) ==
             ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE);
static struct operation_buffer operations[2];
static atomic_t writer, generation, dropped, assertion;
static uint32_t assertion_site, assertion_line;

void zmk_watchdog_operation_at(uint16_t domain, uint16_t event, uint32_t arg0, uint32_t arg1,
                               uint32_t site) {
    if (!atomic_cas(&writer, 0, 1)) {
        atomic_inc(&dropped);
        return;
    }
    const uint32_t gen = (uint32_t)atomic_get(&generation);
    struct operation_buffer *next = &operations[(gen + 1U) & 1U];
    *next = operations[gen & 1U];
    if (next->count == ZMK_WDG_CONTEXT_OPERATIONS) {
        memmove(next->entries[0], next->entries[1],
                sizeof(next->entries) - sizeof(next->entries[0]));
        next->reserved[0] = 1; /* eviction survives the sequence counter wrapping */
    } else {
        ++next->count;
    }
    uint8_t *entry = next->entries[next->count - 1U];
    sys_put_le32(gen + 1U, entry);
    sys_put_le32(k_uptime_get_32(), entry + 4);
    sys_put_le32(site, entry + 8);
    sys_put_le16(domain, entry + 12);
    sys_put_le16(event, entry + 14);
    sys_put_le32(arg0, entry + 16);
    sys_put_le32(arg1, entry + 20);
#if defined(CONFIG_ZMK_WATCHDOG_CONTEXT_TEST)
    watchdog_context_test_before_publish();
#endif
    atomic_set(&generation, (atomic_val_t)(gen + 1U));
    atomic_clear(&writer);
}

__attribute__((noinline)) void zmk_watchdog_operation(uint16_t domain, uint16_t event,
                                                      uint32_t arg0, uint32_t arg1) {
    uintptr_t address = (uintptr_t)__builtin_return_address(0);
    zmk_watchdog_operation_at(domain, event, arg0, arg1,
                              address <= UINT32_MAX ? (uint32_t)address : 0);
}

bool watchdog_context_assert_begin(uint32_t site, uint32_t line) {
    if (!atomic_cas(&assertion, 0, 1)) {
        return false;
    }
    assertion_site = site;
    assertion_line = line;
    atomic_set(&assertion, 2);
    /* Forward the original assert before any clock/lock-taking metadata path. */
    return true;
}
void watchdog_context_assert_end(void) { atomic_clear(&assertion); }

#if defined(CONFIG_ZMK_WATCHDOG_CONTEXT_TEST)
void watchdog_context_test_reset(void) {
    memset(operations, 0, sizeof(operations));
    atomic_clear(&writer);
    atomic_clear(&generation);
    atomic_clear(&dropped);
    atomic_clear(&assertion);
}
#endif

static void snapshot_operations(uint8_t *out) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        uint32_t gen = (uint32_t)atomic_get(&generation);
        const struct operation_buffer *buffer = &operations[gen & 1U];
        out[3] = MIN(buffer->count, ZMK_WDG_CONTEXT_OPERATIONS);
        memcpy(out + ZMK_WDG_CONTEXT_OPERATIONS_OFFSET, buffer->entries, sizeof(buffer->entries));
        if (gen == (uint32_t)atomic_get(&generation)) {
            if (atomic_get(&writer) || atomic_get(&dropped)) {
                out[1] |= ZMK_WDG_CONTEXT_PARTIAL;
            }
            if (buffer->reserved[0]) {
                out[1] |= ZMK_WDG_CONTEXT_PARTIAL;
            }
            sys_put_le32((uint32_t)atomic_get(&dropped), out + 88);
            return;
        }
    }
    out[1] |= ZMK_WDG_CONTEXT_PARTIAL;
    out[3] = 0;
    memset(out + ZMK_WDG_CONTEXT_OPERATIONS_OFFSET, 0, sizeof(operations[0].entries));
    sys_put_le32((uint32_t)atomic_get(&dropped), out + 88);
}

static void snapshot_assertion(uint8_t *out) {
    if (atomic_get(&assertion) == 2) {
        out[1] |= ZMK_WDG_CONTEXT_ASSERT;
        sys_put_le32(assertion_site, out + 8);
        sys_put_le32(assertion_line, out + 12);
    }
}

/* One envelope initializer for minimal and enriched captures. Clock access
 * stays explicit: the minimal publication path must remain clock-free. */
static uint8_t *prepare_context(struct zmk_watchdog_incident_record *record, bool timed) {
    uint8_t *out = record->capture_context;
    memset(out, 0, ZMK_WATCHDOG_CAPTURE_CONTEXT_SIZE);
    out[0] = ZMK_WDG_CONTEXT_FORMAT;
    out[1] = timed ? 0 : ZMK_WDG_CONTEXT_PARTIAL;
    if (timed) {
        sys_put_le32(k_uptime_get_32(), out + 4);
        sys_put_le32(1, out + 92); /* capture-time available */
    }
    snapshot_operations(out);
    snapshot_assertion(out);
    return out;
}

void watchdog_context_minimal(struct zmk_watchdog_incident_record *record) {
    if (!record) {
        return;
    }
    prepare_context(record, false);
    record->flags |= ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
}

#if defined(CONFIG_CPU_CORTEX_M3) || defined(CONFIG_CPU_CORTEX_M4)
static bool in_ram(uintptr_t address, size_t bytes) {
    const uintptr_t start = CONFIG_SRAM_BASE_ADDRESS;
    const size_t size = KB(CONFIG_SRAM_SIZE);
    return !(address & 3U) && address >= start && bytes <= size && address - start <= size - bytes;
}
static bool in_text(uint32_t address) {
    uintptr_t a = address & ~1U;
    return a >= (uintptr_t)__text_region_start && a < (uintptr_t)__text_region_end;
}
static bool return_address(uint32_t address) {
    uintptr_t a = address & ~1U;
    if (!(address & 1U) || !in_text(address) || a - (uintptr_t)__text_region_start < 4U) {
        return false;
    }
    uint16_t last = *(const volatile uint16_t *)(a - 2U);
    if ((last & 0xff87U) == 0x4780U) {
        return true;
    }
    uint16_t first = *(const volatile uint16_t *)(a - 4U);
    return (first & 0xf800U) == 0xf000U && (last & 0xd000U) == 0xd000U;
}
static void add_address(uint8_t *out, uint32_t address, bool is_return) {
    if (!in_text(address) || (is_return && !return_address(address)) || out[2] >= 6) {
        return;
    }
    for (unsigned i = 0; i < out[2]; ++i) {
        if (sys_get_le32(out + 64 + i * 4U) == address) {
            return;
        }
    }
    sys_put_le32(address, out + 64 + out[2]++ * 4U);
}
#endif

void watchdog_context_capture(struct zmk_watchdog_incident_record *record,
                              const struct arch_esf *esf, struct k_thread *target) {
    if (!record) {
        return;
    }
    uint8_t *out = prepare_context(record, true);
#if defined(CONFIG_CPU_CORTEX_M3) || defined(CONFIG_CPU_CORTEX_M4)
    out[1] |= ZMK_WDG_CONTEXT_FAULT_REGS;
    sys_put_le32(SCB->MMFAR, out + 20);
    sys_put_le32(SCB->BFAR, out + 24);
    sys_put_le32(SCB->ICSR, out + 28);
    if (target) {
        /* The existing freeze collector already validates target/stack bounds. */
        const struct zmk_watchdog_incident_freeze_detail *d = &record->detail.freeze;
        sys_put_le32(d->stack_pointer, out + 16);
        for (unsigned i = 0; i < MIN(d->trace_count, 6); ++i) {
            add_address(out, d->addresses[i], i != 0);
        }
        if (d->trace_flags & ZMK_WATCHDOG_TRACE_PARTIAL) {
            out[1] |= ZMK_WDG_CONTEXT_PARTIAL;
        }
        if (d->trace_flags & ZMK_WATCHDOG_TRACE_FRAME) {
            out[1] |= ZMK_WDG_CONTEXT_STACK;
        }
    } else if (esf && in_ram((uintptr_t)esf, sizeof(*esf))) {
        add_address(out, esf->basic.pc, false);
        add_address(out, esf->basic.lr, true);
        if (esf->basic.xpsr & 511U) {
            out[1] |= ZMK_WDG_CONTEXT_ISR;
        }
#if defined(CONFIG_EXTRA_EXCEPTION_INFO)
        const _callee_saved_t *callee = esf->extra_info.callee;
        if (callee && in_ram((uintptr_t)callee, sizeof(*callee))) {
            const uint32_t registers[8] = {callee->v1, callee->v2, callee->v3, callee->v4,
                                           callee->v5, callee->v6, callee->v7, callee->v8};
            for (unsigned i = 0; i < 8; ++i) {
                sys_put_le32(registers[i], out + 32 + i * 4U);
            }
            out[1] |= ZMK_WDG_CONTEXT_CALLEE;
            /* A software-oops ESF copy has no original MSP/EXC_RETURN on this
             * core. Never guess a stack from the address of that copied ESF.
             * A validated thread PSP still permits a bounded candidate scan. */
            bool scan_safe = !(esf->basic.xpsr & 511U);
#if defined(CONFIG_FPU)
            scan_safe =
                scan_safe && esf->extra_info.exc_return && (esf->extra_info.exc_return & BIT(4));
#endif
            if (scan_safe) {
                struct k_thread *thread = k_current_get();
                uintptr_t sp = callee->psp;
                uintptr_t lo = thread->stack_info.start;
                size_t size = thread->stack_info.size;
                if (in_ram(lo, size) && in_ram(sp, 4) && sp >= lo && sp - lo <= size) {
                    size_t limit = MIN(size - (sp - lo), 1024U);
                    sys_put_le32((uint32_t)sp, out + 16);
                    out[1] |= ZMK_WDG_CONTEXT_STACK | ZMK_WDG_CONTEXT_PARTIAL;
                    /* Only call-compatible code words, never raw stack data. */
                    for (size_t at = 0; at + 4U <= limit && out[2] < 6; at += 4U) {
                        add_address(out, *(const volatile uint32_t *)(sp + at), true);
                    }
                }
            }
        }
#endif
        if (!(out[1] & ZMK_WDG_CONTEXT_STACK)) {
            out[1] |= ZMK_WDG_CONTEXT_PARTIAL;
        }
    } else {
        out[1] |= ZMK_WDG_CONTEXT_PARTIAL;
    }
#else
    ARG_UNUSED(esf);
    ARG_UNUSED(target);
    out[1] |= ZMK_WDG_CONTEXT_UNSUPPORTED;
#endif
    record->flags |= ZMK_WATCHDOG_CAPTURE_CONTEXT_VALID;
}
