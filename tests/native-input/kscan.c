/* SPDX-License-Identifier: MIT */
/* Owner-only scripted input device. It uses Zephyr's public Kscan callback
 * boundary; core physical-layout, keymap, behavior and HID code are unchanged.
 * It is not a test of the upstream mock driver or physical GPIO timing. */
#define DT_DRV_COMPAT amgskobo_watchdog_test_kscan

#include <errno.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "../support/init_result.h"
#include "word.h"

struct input_config {
    const uint32_t *words;
    size_t count;
    uint32_t rows;
    uint32_t columns;
    bool exit_after;
};

struct input_data {
    const struct device *dev;
    struct k_work_delayable work;
    kscan_callback_t callback;
    size_t index;
    bool enabled;
};

static int input_configure(const struct device *dev, kscan_callback_t callback) {
    struct input_data *data = dev->data;
    if (!callback) {
        return -EINVAL;
    }
    if (data->enabled) {
        return -EBUSY;
    }
    data->callback = callback;
    data->index = 0;
    return 0;
}

static int input_disable(const struct device *dev) {
    struct input_data *data = dev->data;
    data->enabled = false;
    k_work_cancel_delayable(&data->work);
    return 0;
}

static int input_enable(const struct device *dev) {
    struct input_data *data = dev->data;
    const struct input_config *config = dev->config;
    if (!data->callback) {
        return -EINVAL;
    }
    if (!data->enabled) {
        data->enabled = true;
        uint32_t delay = data->index < config->count
                             ? watchdog_input_decode(config->words[data->index]).delay_ms
                             : 0;
        k_work_schedule(&data->work, K_MSEC(delay));
    }
    return 0;
}

/* The verifier requires this linked hook's final completion in addition to
 * every original checked initialization and every unchanged snapshot. */
static void owner_checked_watchdog_native_input_complete(struct k_work *work) {
    struct input_data *data =
        CONTAINER_OF(k_work_delayable_from_work(work), struct input_data, work);
    const struct input_config *config = data->dev->config;
    if (!data->enabled) {
        return;
    }
    if (data->index == config->count) {
        data->enabled = false;
        printk("WATCHDOG_INPUT_COMPLETE count=%u\n", (unsigned)config->count);
        printk("OWNER_TEST_RESULT watchdog_native_input_complete=0\n");
        if (config->exit_after) {
            exit(0);
        }
        return;
    }
    uint32_t raw = config->words[data->index];
    struct watchdog_input_word value = watchdog_input_decode(raw);
    printk("WATCHDOG_INPUT_EVENT index=%u word=%08x row=%u column=%u pressed=%u delay_ms=%u\n",
           (unsigned)data->index, (unsigned)raw, (unsigned)value.row, (unsigned)value.column,
           value.pressed ? 1U : 0U, (unsigned)value.delay_ms);
    data->callback(data->dev, value.row, value.column, value.pressed);
    data->index++;
    /* Keep one final configured delay for queued core position/HID processing
     * before terminating. All preserved owner vectors use 10 ms delays. */
    uint32_t delay = data->index < config->count
                         ? watchdog_input_decode(config->words[data->index]).delay_ms
                         : value.delay_ms;
    k_work_schedule(&data->work, K_MSEC(delay));
}

static int input_init(const struct device *dev) {
    struct input_data *data = dev->data;
    const struct input_config *config = dev->config;
    if (config->rows == 0 || config->rows > 256 || config->columns == 0 || config->columns > 256) {
        return -EINVAL;
    }
    for (size_t i = 0; i < config->count; i++) {
        struct watchdog_input_word word = watchdog_input_decode(config->words[i]);
        if (word.row >= config->rows || word.column >= config->columns) {
            return -EINVAL;
        }
    }
    data->dev = dev;
    k_work_init_delayable(&data->work, owner_checked_watchdog_native_input_complete);
    return 0;
}

static const struct kscan_driver_api input_api = {
    .config = input_configure,
    .enable_callback = input_enable,
    .disable_callback = input_disable,
};

#define INPUT_DEVICE(n)                                                                            \
    static const uint32_t input_words_##n[] = DT_INST_PROP(n, events);                             \
    static const struct input_config input_config_##n = {                                          \
        .words = input_words_##n,                                                                  \
        .count = ARRAY_SIZE(input_words_##n),                                                      \
        .rows = DT_INST_PROP(n, rows),                                                             \
        .columns = DT_INST_PROP(n, columns),                                                       \
        .exit_after = DT_INST_PROP(n, exit_after),                                                 \
    };                                                                                             \
    static struct input_data input_data_##n;                                                       \
    DEVICE_DT_INST_DEFINE(n, input_init, NULL, &input_data_##n, &input_config_##n, POST_KERNEL,    \
                          CONFIG_KSCAN_INIT_PRIORITY, &input_api);

DT_INST_FOREACH_STATUS_OKAY(INPUT_DEVICE)

static int watchdog_native_input_init(void) {
    return device_is_ready(DEVICE_DT_GET(DT_CHOSEN(zmk_kscan))) ? 0 : -ENODEV;
}

OWNER_CHECKED_SYS_INIT(watchdog_native_input_init, APPLICATION, 0);
