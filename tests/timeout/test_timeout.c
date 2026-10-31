/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "watchdog_timeout.h"

#ifdef TIMER_TIMEOUT_WIDTH_PROBE
k_ticks_t probe_timeout(uint32_t ms, uint32_t hz);
k_ticks_t probe_timeout(uint32_t ms, uint32_t hz) { return watchdog_timeout_ticks(ms, hz); }
#else
int main(void) {
    assert(!watchdog_retry_waiting(0, 0, 0));
    assert(watchdog_retry_waiting(0, 0, 1));
    assert(!watchdog_retry_waiting(1, 0, 1));
    assert(!watchdog_retry_waiting(47U * 86400000U, 0, 1000));
    assert(watchdog_retry_waiting(0, UINT32_MAX - 999U, 1001));
    assert(!watchdog_retry_waiting(1, UINT32_MAX - 999U, 1001));
    assert(watchdog_retry_waiting(UINT32_MAX, UINT32_MAX, 60000));

    static const uint32_t rates[] = {
        0, 1, 10, 100, 128, 1000, 1024, 32768, 65535, 65536, 65537, 1000000, INT32_MAX, UINT32_MAX,
    };
    for (unsigned h = 0; h < sizeof(rates) / sizeof(rates[0]); ++h) {
        const uint32_t hz = rates[h];
        static const uint32_t edges[] = {65536,     131070,          131071,    131072,
                                         INT32_MAX, UINT32_MAX - 1U, UINT32_MAX};
        for (unsigned j = 0; j < sizeof(edges) / sizeof(edges[0]); ++j) {
            const uint32_t ms = edges[j];
            assert(watchdog_timeout_ticks(ms, hz) ==
                   (k_ticks_t)(((uint64_t)ms * hz + 999U) / 1000U));
        }
        uint32_t sample = 0x73219a4bU;
        for (unsigned j = 0; j < 65536U; ++j) {
            sample = sample * 1664525U + 1013904223U;
            assert(watchdog_timeout_ticks(sample, hz) ==
                   (k_ticks_t)(((uint64_t)sample * hz + 999U) / 1000U));
        }
        for (uint32_t ms = 0; ms <= UINT16_MAX; ++ms) {
            const k_ticks_t expected = (k_ticks_t)(((uint64_t)ms * hz + 999U) / 1000U);
            assert(watchdog_timeout_ticks((uint32_t)ms, hz) == expected);
        }
    }
    puts("zmk-watchdog-fork timeout all 65536 short waits, full uint32 edges/samples at 14 clock "
         "rates, exact ceil and kernel width: PASS");
    return 0;
}
#endif
