# SPDX-License-Identifier: MIT
# Native simulation has a separate final host link. EXTRA_LDFLAGS instruments
# and links the guest, but does not select UBSan's runtime for that host link.
if(NOT CONFIG_BOARD_NATIVE_SIM OR NOT CONFIG_ASAN OR NOT TARGET native_simulator)
  message(FATAL_ERROR "Watchdog native sanitizer fixture requires the native ASan runner")
endif()
target_link_options(native_simulator INTERFACE
  -fsanitize=address,undefined -fno-sanitize-recover=all)
