cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(watchdog_store_concurrency)
if(NOT EXISTS "${ZMK_ROOT}/app/src/workqueue.c")
  message(FATAL_ERROR "ZMK_ROOT must select the resolved ZMK checkout")
endif()
target_sources(app PRIVATE main.c pending_fixture.c reset_fixture.c
  "${ZMK_ROOT}/app/src/workqueue.c")
target_include_directories(app PRIVATE ../../include "${ZMK_ROOT}/app/include")
target_compile_definitions(app PRIVATE
  CONFIG_ZMK_WATCHDOG_MAX_INCIDENTS=16 CONFIG_ZMK_LOG_LEVEL=0 CONFIG_ZMK_WATCHDOG_TEST=1
  CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=4096
  CONFIG_ZMK_LOW_PRIORITY_THREAD_PRIORITY=10
  CONFIG_ZMK_WATCHDOG_PENDING_CONVERT_DELAY_MS=10)
