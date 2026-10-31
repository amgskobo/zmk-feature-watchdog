/* SPDX-License-Identifier: MIT */
/* DESIGN.md, Cortex-M trace. Read only the monitored thread at incident time. */
#include <zephyr/kernel.h>
#include <cormoran/zmk/watchdog_trace.h>

#if defined(CONFIG_ZMK_WATCHDOG_FREEZE_TRACE)
#include <zephyr/linker/linker-defs.h>

#define TRACE_SCAN_BYTES 1024U
#define TRACE_BASIC_WORDS 8U
#define TRACE_FP_WORDS 18U

static bool in_ram(uintptr_t address, size_t length) {
    const uintptr_t start = CONFIG_SRAM_BASE_ADDRESS;
    const size_t size = KB(CONFIG_SRAM_SIZE);
    return address >= start && length <= size && address - start <= size - length;
}

static bool in_text(uint32_t address) {
    const uintptr_t a = address & ~1U;
    return a >= (uintptr_t)__text_region_start && a < (uintptr_t)__text_region_end;
}

/* A Thumb return address must follow a BL or BLX Rm instruction. This rejects
 * ordinary function pointers; it still cannot establish stack liveness. */
static bool return_address(uint32_t address) {
    const uintptr_t a = address & ~1U;
    if (!(address & 1U) || !in_text(address) || a - (uintptr_t)__text_region_start < 4U) {
        return false;
    }
    const uint16_t last = *(const volatile uint16_t *)(a - 2U);
    if ((last & 0xFF87U) == 0x4780U) {
        return true;
    }
    const uint16_t first = *(const volatile uint16_t *)(a - 4U);
    return (first & 0xF800U) == 0xF000U && (last & 0xD000U) == 0xD000U;
}

static void add_address(struct zmk_watchdog_incident_freeze_detail *d, uint32_t a) {
    if (d->trace_count < ZMK_WATCHDOG_TRACE_ADDRS &&
        (!d->trace_count || d->addresses[d->trace_count - 1] != a)) {
        d->addresses[d->trace_count++] = a;
    }
}
#endif

void watchdog_freeze_capture_thread(struct k_thread *t,
                                    struct zmk_watchdog_incident_freeze_detail *d) {
    d->thread_state = t->base.thread_state;
    d->trace_count = 0;
    d->trace_flags = 0;
    d->wait_queue = 0;
    d->stack_pointer = 0;
    for (size_t i = 0; i < ZMK_WATCHDOG_TRACE_ADDRS; ++i) {
        d->addresses[i] = 0;
    }
#if defined(CONFIG_ZMK_WATCHDOG_FREEZE_TRACE)
    const uintptr_t wait = (uintptr_t)t->base.pended_on;
    if (wait && in_ram(wait, sizeof(_wait_q_t))) {
        d->wait_queue = (uint32_t)wait;
        d->trace_flags |= ZMK_WATCHDOG_TRACE_PENDED;
    }

    uintptr_t sp;
    const bool running = k_current_get() == t;
    if (running) {
        uint32_t psp;
        __asm__ volatile("mrs %0, psp" : "=r"(psp));
        sp = psp;
        d->trace_flags |= ZMK_WATCHDOG_TRACE_RUNNING;
    } else {
        sp = t->callee_saved.psp;
    }
    d->stack_pointer = (uint32_t)sp;

    const uintptr_t lo = t->stack_info.start;
    const size_t size = t->stack_info.size;
    if (!in_ram(lo, size) || sp < lo || sp - lo > size || (sp & 3U) ||
        size - (sp - lo) < TRACE_BASIC_WORDS * sizeof(uint32_t)) {
        d->trace_flags |= ZMK_WATCHDOG_TRACE_PARTIAL;
        return;
    }
    const volatile uint32_t *frame = (const volatile uint32_t *)sp;
    /* A thread's hardware frame must describe Thumb thread mode. */
    if (!(frame[7] & BIT(24)) || (frame[7] & 0x1FFU) || !in_text(frame[6])) {
        d->trace_flags |= ZMK_WATCHDOG_TRACE_PARTIAL;
        return;
    }
    d->trace_flags |= ZMK_WATCHDOG_TRACE_FRAME;
    add_address(d, frame[6]);
    if (return_address(frame[5])) {
        add_address(d, frame[5]);
    }

    size_t frame_words = TRACE_BASIC_WORDS + ((frame[7] >> 9) & 1U);
#if defined(CONFIG_FPU)
    if (running) {
        /* No original ISR EXC_RETURN is passed here. Do not scan potentially
         * lazy/extended FP words as callers. PC/LR remain useful. */
        d->trace_flags |= ZMK_WATCHDOG_TRACE_PARTIAL;
        return;
    }
#if defined(CONFIG_ARM_STORE_EXC_RETURN)
    if (!(t->arch.mode_exc_return & BIT(4))) {
        frame_words += TRACE_FP_WORDS;
    }
#else
    d->trace_flags |= ZMK_WATCHDOG_TRACE_PARTIAL;
    return;
#endif
#endif
    const size_t remaining = size - (sp - lo);
    const size_t scan = MIN(remaining, TRACE_SCAN_BYTES);
    for (size_t offset = frame_words * sizeof(uint32_t);
         offset + sizeof(uint32_t) <= scan && d->trace_count < ZMK_WATCHDOG_TRACE_ADDRS;
         offset += sizeof(uint32_t)) {
        const uint32_t word = *(const volatile uint32_t *)(sp + offset);
        if (return_address(word)) {
            add_address(d, word);
        }
    }
    if (scan < remaining || d->trace_count == ZMK_WATCHDOG_TRACE_ADDRS) {
        d->trace_flags |= ZMK_WATCHDOG_TRACE_PARTIAL;
    }
#else
    d->trace_flags = ZMK_WATCHDOG_TRACE_UNSUPPORTED;
#endif
}
