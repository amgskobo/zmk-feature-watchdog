/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "word.h"

int main(void) {
    /* Literal independent values, including both state bits and field limits. */
    const struct {
        uint32_t raw, row, column, delay;
        bool pressed;
    } cases[] = {
        {UINT32_C(0x800a0000), 0, 0, 10, true},
        {UINT32_C(0x000a0000), 0, 0, 10, false},
        {UINT32_C(0xffffffff), 255, 255, 32767, true},
        {UINT32_C(0x7fffffff), 255, 255, 32767, false},
        {UINT32_C(0x00000000), 0, 0, 0, false},
        {UINT32_C(0x80000000), 0, 0, 0, true},
        {UINT32_C(0x12345678), 120, 86, 4660, false},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct watchdog_input_word word = watchdog_input_decode(cases[i].raw);
        assert(word.row == cases[i].row);
        assert(word.column == cases[i].column);
        assert(word.delay_ms == cases[i].delay);
        assert(word.pressed == cases[i].pressed);
    }
    puts("WATCHDOG_INPUT_WORD_PASS cases=7");
    return 0;
}
