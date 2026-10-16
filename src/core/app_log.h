#pragma once

#include "esp_log.h"

#define STICKY_LOG_LEVEL_NONE 0
#define STICKY_LOG_LEVEL_ERROR 1
#define STICKY_LOG_LEVEL_WARN 2
#define STICKY_LOG_LEVEL_INFO 3
#define STICKY_LOG_LEVEL_DEBUG 4
#define STICKY_LOG_LEVEL_TRACE 5

#ifndef STICKY_LOG_COMPILE_LEVEL
#define STICKY_LOG_COMPILE_LEVEL STICKY_LOG_LEVEL_INFO
#endif

// High-volume categories stay independently switchable.
// 高频日志分类保持独立开关，便于调试和发布裁剪。
#ifndef STICKY_LOG_BOOT_DETAILS_ENABLED
#define STICKY_LOG_BOOT_DETAILS_ENABLED 0
#endif

#ifndef STICKY_LOG_POWER_DETAILS_ENABLED
#define STICKY_LOG_POWER_DETAILS_ENABLED 0
#endif

#ifndef STICKY_LOG_HEARTBEAT_ENABLED
#define STICKY_LOG_HEARTBEAT_ENABLED 0
#endif

#ifndef STICKY_LOG_DISPLAY_TIMING_ENABLED
#define STICKY_LOG_DISPLAY_TIMING_ENABLED 0
#endif

#ifndef STICKY_LOG_TOUCH_SAMPLES_ENABLED
#define STICKY_LOG_TOUCH_SAMPLES_ENABLED 0
#endif

#ifndef STICKY_LOG_TOUCH_DRIVER_OUTPUT_ENABLED
#define STICKY_LOG_TOUCH_DRIVER_OUTPUT_ENABLED 0
#endif

#ifndef STICKY_LOG_MOTION_SAMPLES_ENABLED
#define STICKY_LOG_MOTION_SAMPLES_ENABLED 0
#endif

#ifndef STICKY_LOG_PET_ANIMATION_ENABLED
#define STICKY_LOG_PET_ANIMATION_ENABLED 0
#endif

#ifndef STICKY_LOG_STATUS_ANIMATION_ENABLED
#define STICKY_LOG_STATUS_ANIMATION_ENABLED 0
#endif

#ifndef STICKY_LOG_BOOK_ANIMATION_ENABLED
#define STICKY_LOG_BOOK_ANIMATION_ENABLED 0
#endif

#ifndef STICKY_LOG_TIMER_TICKS_ENABLED
#define STICKY_LOG_TIMER_TICKS_ENABLED 0
#endif

#ifndef STICKY_LOG_RTC_READS_ENABLED
#define STICKY_LOG_RTC_READS_ENABLED 0
#endif

#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_ERROR
#define STICKY_LOGE(tag, format, ...) \
    ESP_LOGE(tag, format, ##__VA_ARGS__)
#else
#define STICKY_LOGE(tag, format, ...) \
    do {                                \
    } while (0)
#endif

#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_WARN
#define STICKY_LOGW(tag, format, ...) \
    ESP_LOGW(tag, format, ##__VA_ARGS__)
#else
#define STICKY_LOGW(tag, format, ...) \
    do {                                \
    } while (0)
#endif

#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_INFO
#define STICKY_LOGI(tag, format, ...) \
    ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
#define STICKY_LOGI(tag, format, ...) \
    do {                                \
    } while (0)
#endif

#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_DEBUG
#define STICKY_LOGD(tag, format, ...) \
    ESP_LOGD(tag, format, ##__VA_ARGS__)
#else
#define STICKY_LOGD(tag, format, ...) \
    do {                                \
    } while (0)
#endif

#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_TRACE
#define STICKY_LOGT(tag, format, ...) \
    ESP_LOGV(tag, format, ##__VA_ARGS__)
#else
#define STICKY_LOGT(tag, format, ...) \
    do {                                \
    } while (0)
#endif

// Configures the global runtime level for the selected build profile.
// 根据当前构建配置设置全局运行时日志等级。
void app_log_init();

// Enables the selected application log level for one module tag.
// 为一个模块标签启用当前应用日志等级。
void app_log_register_tag(const char *tag);
