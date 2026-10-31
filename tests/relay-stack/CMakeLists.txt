cmake_minimum_required(VERSION 3.20.0)
set(WEST OFF CACHE BOOL "Isolate this fixture from containing west modules")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(watchdog_relay_stack)
get_filename_component(OWNER ../.. ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${OWNER}
    ${OWNER}/proto/cormoran/watchdog/watchdog.proto)
if(NOT ZMK_ROOT)
    message(FATAL_ERROR "ZMK_ROOT must identify the resolved unchanged DYA checkout")
endif()
target_sources(app PRIVATE main.c report_fixture.c ${OWNER}/src/studio/watchdog_request_exec.c
    ${OWNER}/src/split/watchdog_relay_page.c ${ZMK_ROOT}/app/src/event_manager.c ${proto_srcs})
zephyr_linker_sources(RODATA ${ZMK_ROOT}/app/include/linker/zmk-events.ld)
target_include_directories(app PRIVATE ${ZMK_ROOT}/app/include ${OWNER}/include ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/proto)
target_compile_definitions(app PRIVATE CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=128
    CONFIG_ZMK_SPLIT_RELAY_EVENT=1 CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN=4
    CONFIG_ZMK_LOG_LEVEL=0 CONFIG_ZMK_WATCHDOG_DIAGNOSTICS=1)
if(DEFINED WATCHDOG_LOG_LEVEL)
    target_compile_definitions(app PRIVATE CONFIG_ZMK_WATCHDOG_FORK_LOG_LEVEL=${WATCHDOG_LOG_LEVEL})
endif()
