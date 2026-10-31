/* SPDX-License-Identifier: MIT */
#pragma once
static inline void watchdog_test_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static inline void watchdog_test_log(const char *format, ...) { (void)format; }
#define LOG_MODULE_DECLARE(...)
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...) watchdog_test_log(__VA_ARGS__)
#define LOG_WRN(...) watchdog_test_log(__VA_ARGS__)
#define LOG_ERR(...) watchdog_test_log(__VA_ARGS__)
