/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <cormoran/zmk/watchdog_context.h>
#ifdef CONFIG_ASSERT_NO_FILE_INFO
void __real_assert_post_action(void);
void __wrap_assert_post_action(void) {
    uintptr_t address = (uintptr_t)__builtin_return_address(0);
    bool owner = watchdog_context_assert_begin(address <= UINT32_MAX ? (uint32_t)address : 0, 0);
    __real_assert_post_action();
    if (owner) {
        watchdog_context_assert_end();
    }
}
#else
void __real_assert_post_action(const char *file, unsigned int line);
void __wrap_assert_post_action(const char *file, unsigned int line) {
    uintptr_t address = (uintptr_t)__builtin_return_address(0);
    bool owner = watchdog_context_assert_begin(address <= UINT32_MAX ? (uint32_t)address : 0, line);
    __real_assert_post_action(file, line);
    if (owner) {
        watchdog_context_assert_end();
    }
}
#endif
