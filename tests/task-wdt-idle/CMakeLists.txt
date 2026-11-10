# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(watchdog_idle_timer)
target_sources(app PRIVATE main.c ../../src/watchdog_freeze.c ../../src/watchdog_trace.c
  ../../src/watchdog_capture.c "${ZMK_ROOT}/app/src/workqueue.c")
target_include_directories(app PRIVATE ../../include "${ZMK_ROOT}/app/include")
target_compile_definitions(app PRIVATE CONFIG_ZMK_LOG_LEVEL=0
  CONFIG_ZMK_WATCHDOG_FREEZE_TIMEOUT_MS=10000 CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
  CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY=10)
if(WATCHDOG_IDLE_LOWPRIO)
  target_compile_definitions(app PRIVATE CONFIG_ZMK_WATCHDOG_FREEZE_MONITOR_LOWPRIO_QUEUE=1)
endif()
zephyr_link_libraries(-Wl,--wrap=task_wdt_init -Wl,--wrap=z_impl_k_timer_start)
