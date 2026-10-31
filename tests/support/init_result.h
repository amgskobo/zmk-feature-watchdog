/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/sys/printk.h>
/* SYS_INIT can return an error without failing the native process. The owner
 * verifier requires every linked wrapper's completion and rejects nonzero rc. */
#define OWNER_CHECKED_SYS_INIT(fn, level, priority)                                                \
    static int owner_checked_##fn(void) {                                                          \
        int result = fn();                                                                         \
        printk("OWNER_TEST_RESULT " #fn "=%d\n", result);                                          \
        return result;                                                                             \
    }                                                                                              \
    SYS_INIT(owner_checked_##fn, level, priority)

/* The same check for a test that runs on the system work queue: the SYS_INIT
 * only submits it, and the work reports the test's own result. */
#define OWNER_CHECKED_SYSWORKQ(fn, level, priority)                                                \
    static void owner_checked_##fn(struct k_work *work) {                                          \
        ARG_UNUSED(work);                                                                          \
        printk("OWNER_TEST_RESULT " #fn "=%d\n", fn());                                            \
    }                                                                                              \
    static K_WORK_DEFINE(fn##_work, owner_checked_##fn);                                           \
    static int fn##_submit(void) {                                                                 \
        k_work_submit(&fn##_work);                                                                 \
        return 0;                                                                                  \
    }                                                                                              \
    SYS_INIT(fn##_submit, level, priority)
