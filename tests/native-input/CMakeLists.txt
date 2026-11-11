# SPDX-License-Identifier: MIT
if(NOT CONFIG_BOARD_NATIVE_SIM)
  message(FATAL_ERROR "Watchdog native input fixture requires native_sim")
endif()
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/kscan.c)
