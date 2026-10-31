/* SPDX-License-Identifier: MIT */
#pragma once
#include <assert.h>
struct k_mutex {
    int held;
};
#define K_MUTEX_DEFINE(name) struct k_mutex name = {0}
#define K_FOREVER 0
static inline int k_mutex_lock(struct k_mutex *mutex, int timeout) {
    (void)timeout;
    assert(mutex->held == 0);
    mutex->held = 1;
    return 0;
}
static inline int k_mutex_unlock(struct k_mutex *mutex) {
    assert(mutex->held == 1);
    mutex->held = 0;
    return 0;
}
