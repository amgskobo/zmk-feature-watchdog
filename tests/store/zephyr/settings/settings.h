/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define SETTINGS_MAX_NAME_LEN 64
typedef ssize_t (*settings_read_cb)(void *, void *, size_t);
typedef int (*settings_load_direct_cb)(const char *, size_t, settings_read_cb, void *, void *);
int settings_load_subtree_direct(const char *, settings_load_direct_cb, void *);
int settings_save_one(const char *, const void *, size_t);
int settings_delete(const char *);
int settings_name_next(const char *, const char **);
#define SETTINGS_STATIC_HANDLER_DEFINE(...)
