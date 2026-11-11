/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Test input format: state:1, relative milliseconds:15, column:8, row:8.
 * These are value fields, not a reproduction of a physical GPIO scanner. */
struct watchdog_input_word {
    uint32_t row;
    uint32_t column;
    uint32_t delay_ms;
    bool pressed;
};

static inline struct watchdog_input_word watchdog_input_decode(uint32_t word) {
    return (struct watchdog_input_word){
        .row = word & UINT32_C(0xff),
        .column = (word >> 8) & UINT32_C(0xff),
        .delay_ms = (word >> 16) & UINT32_C(0x7fff),
        .pressed = (word & UINT32_C(0x80000000)) != 0,
    };
}
