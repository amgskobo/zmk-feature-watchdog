/* SPDX-License-Identifier: MIT */
#pragma once
#define __packed __attribute__((packed))
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
