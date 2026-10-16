#include "sticky_button.h"

#include <cstdint>

#include "app_log.h"
#include "button_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "iot_button.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_button";
constexpr uint16_t kLongPressTimeMs = 1000U;
constexpr uint16_t kShortPressTimeMs = 180U;
constexpr UBaseType_t kEventQueueCapacity = 8U;

button_handle_t s_ai_button = nullptr;
button_handle_t s_page_up_button = nullptr;
button_handle_t s_page_down_button = nullptr;
QueueHandle_t s_event_queue = nullptr;

void queue_event_callback(void *button_handle, void *user_data)
{
    (void)button_handle;
    const auto event = static_cast<StickyButtonEvent>(
        reinterpret_cast<uintptr_t>(user_data));
    xQueueSend(s_event_queue, &event, 0);
}

void *event_arg(StickyButtonEvent event)
{
    return reinterpret_cast<void *>(static_cast<uintptr_t>(event));
}

esp_err_t create_button(int pin, button_handle_t &button)
{
    button_config_t button_config = {};
    button_config.long_press_time = kLongPressTimeMs;
    button_config.short_press_time = kShortPressTimeMs;

    button_gpio_config_t gpio_config = {};
    gpio_config.gpio_num = pin;
    gpio_config.active_level = 0;
    gpio_config.enable_power_save = false;
    gpio_config.disable_pull = false;

    return iot_button_new_gpio_device(&button_config, &gpio_config, &button);
}

}  // namespace

esp_err_t sticky_button_init()
{
    app_log_register_tag(kTag);
    if (s_ai_button != nullptr) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreate(kEventQueueCapacity,
                                 sizeof(StickyButtonEvent));
    if (s_event_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = create_button(PIN_TOP_BUTTON, s_ai_button);
    if (result == ESP_OK) {
        result = iot_button_register_cb(s_ai_button,
                                        BUTTON_SINGLE_CLICK,
                                        nullptr,
                                        queue_event_callback,
                                        event_arg(StickyButtonEvent::AiClick));
    }
    if (result == ESP_OK) {
        result = iot_button_register_cb(
            s_ai_button,
            BUTTON_LONG_PRESS_START,
            nullptr,
            queue_event_callback,
            event_arg(StickyButtonEvent::AiLongPress));
    }

    // Side buttons react on press-down so page turns feel immediate.
    // 侧键在按下瞬间触发，让翻页没有额外等待。
    if (result == ESP_OK) {
        result = create_button(PIN_PAGE_UP_BUTTON, s_page_up_button);
    }
    if (result == ESP_OK) {
        result = iot_button_register_cb(s_page_up_button,
                                        BUTTON_PRESS_DOWN,
                                        nullptr,
                                        queue_event_callback,
                                        event_arg(StickyButtonEvent::PageUp));
    }
    if (result == ESP_OK) {
        result = create_button(PIN_PAGE_DOWN_BUTTON, s_page_down_button);
    }
    if (result == ESP_OK) {
        result = iot_button_register_cb(
            s_page_down_button,
            BUTTON_PRESS_DOWN,
            nullptr,
            queue_event_callback,
            event_arg(StickyButtonEvent::PageDown));
    }
    if (result != ESP_OK) {
        STICKY_LOGE(kTag, "button=init result=%s", esp_err_to_name(result));
        return result;
    }

    STICKY_LOGI(kTag,
                "button=ready ai_pin=%d page_up_pin=%d page_down_pin=%d long_press_ms=%u result=ok",
                PIN_TOP_BUTTON,
                PIN_PAGE_UP_BUTTON,
                PIN_PAGE_DOWN_BUTTON,
                static_cast<unsigned>(kLongPressTimeMs));
    return ESP_OK;
}

bool sticky_button_take_event(StickyButtonEvent &event)
{
    event = StickyButtonEvent::None;
    return s_event_queue != nullptr &&
           xQueueReceive(s_event_queue, &event, 0) == pdTRUE;
}

void sticky_button_clear()
{
    if (s_event_queue != nullptr) {
        xQueueReset(s_event_queue);
    }
}
