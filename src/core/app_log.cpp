#include "app_log.h"

namespace {

esp_log_level_t application_log_level()
{
    // Convert the project compile level into ESP-IDF's runtime level.
    // 将项目的编译日志等级转换为ESP-IDF运行时日志等级。
#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_TRACE
    return ESP_LOG_VERBOSE;
#elif STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_DEBUG
    return ESP_LOG_DEBUG;
#elif STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_INFO
    return ESP_LOG_INFO;
#elif STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_WARN
    return ESP_LOG_WARN;
#elif STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_ERROR
    return ESP_LOG_ERROR;
#else
    return ESP_LOG_NONE;
#endif
}

}  // namespace

void app_log_init()
{
    // Framework logs stay concise; application tags are raised independently below.
    // 系统框架默认保持简洁，各Sticky模块再单独提升到需要的详细等级。
#if STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_DEBUG
    esp_log_level_set("*", ESP_LOG_INFO);
#elif STICKY_LOG_COMPILE_LEVEL >= STICKY_LOG_LEVEL_INFO
    esp_log_level_set("*", ESP_LOG_WARN);
#else
    esp_log_level_set("*", application_log_level());
#endif
}

void app_log_register_tag(const char *tag)
{
    // Registering per module avoids enabling verbose output for every ESP-IDF component.
    // 按模块登记可以保留Sticky详细日志，同时避免打开全部ESP-IDF底层日志。
    if (tag != nullptr) {
        esp_log_level_set(tag, application_log_level());
    }
}
