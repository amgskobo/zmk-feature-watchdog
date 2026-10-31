/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Host boundary only; native checks use Zephyr's actual declaration. */
#ifdef TIMER_TIMEOUT_32
typedef uint32_t k_ticks_t;
#else
typedef int64_t k_ticks_t;
#endif
