#pragma once

#include "esp_log.h"

// Keep high-frequency driver traces as compile-time options.
// 高频驱动日志采用编译期开关，发布固件可以彻底移除对应代码和文字。
#ifndef STICKY_LOG_DISPLAY_TIMING_ENABLED
#define STICKY_LOG_DISPLAY_TIMING_ENABLED 0
#endif

#ifndef STICKY_LOG_TOUCH_SAMPLES_ENABLED
#define STICKY_LOG_TOUCH_SAMPLES_ENABLED 0
#endif

#if STICKY_LOG_DISPLAY_TIMING_ENABLED
#define APP_EPAPER_DEBUG_LOGE(tag, format, ...) \
    ESP_LOGE(tag, "[epaper-debug] " format, ##__VA_ARGS__)
#define APP_EPAPER_DEBUG_LOGW(tag, format, ...) \
    ESP_LOGW(tag, "[epaper-debug] " format, ##__VA_ARGS__)
#define APP_EPAPER_DEBUG_LOGI(tag, format, ...) \
    ESP_LOGI(tag, "[epaper-debug] " format, ##__VA_ARGS__)
#define APP_EPAPER_DEBUG_LOGD(tag, format, ...) \
    ESP_LOGD(tag, "[epaper-debug] " format, ##__VA_ARGS__)
#else
#define APP_EPAPER_DEBUG_LOGE(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGE(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define APP_EPAPER_DEBUG_LOGW(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGW(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define APP_EPAPER_DEBUG_LOGI(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGI(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define APP_EPAPER_DEBUG_LOGD(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGD(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#endif

#if STICKY_LOG_TOUCH_SAMPLES_ENABLED
#define APP_TOUCH_DEBUG_LOGE(tag, format, ...) \
    ESP_LOGE(tag, "[touch-debug] " format, ##__VA_ARGS__)
#define APP_TOUCH_DEBUG_LOGW(tag, format, ...) \
    ESP_LOGW(tag, "[touch-debug] " format, ##__VA_ARGS__)
#define APP_TOUCH_DEBUG_LOGI(tag, format, ...) \
    ESP_LOGI(tag, "[touch-debug] " format, ##__VA_ARGS__)
#define APP_TOUCH_DEBUG_LOGD(tag, format, ...) \
    ESP_LOGD(tag, "[touch-debug] " format, ##__VA_ARGS__)
#else
#define APP_TOUCH_DEBUG_LOGE(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGE(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define APP_TOUCH_DEBUG_LOGW(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGW(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define APP_TOUCH_DEBUG_LOGI(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGI(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#define APP_TOUCH_DEBUG_LOGD(tag, format, ...) \
    do {                                          \
        if (0) {                                  \
            ESP_LOGD(tag, format, ##__VA_ARGS__); \
        }                                         \
    } while (0)
#endif
