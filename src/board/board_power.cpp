#include "board_power.h"

#include <cstdio>

#include "app_log.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_power";
constexpr TickType_t kPowerSettleDelay = pdMS_TO_TICKS(100);
constexpr TickType_t kReleasePollInterval = pdMS_TO_TICKS(20);
constexpr TickType_t kReleaseDebounceTime = pdMS_TO_TICKS(60);

esp_err_t set_power_level(gpio_num_t pin, uint32_t level)
{
    // Centralize GPIO writes so every failed latch operation has the same log shape.
    // 集中处理锁存引脚写入，让所有失败日志都保持相同格式。
    const esp_err_t result = gpio_set_level(pin, level);
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "step=set_level pin=%d level=%lu result=%s",
                    static_cast<int>(pin),
                    static_cast<unsigned long>(level),
                    esp_err_to_name(result));
    }
    return result;
}

esp_err_t hold_output(gpio_num_t pin, int level)
{
    gpio_hold_dis(pin);
    esp_err_t result = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    if (result != ESP_OK) {
        return result;
    }
    result = gpio_set_level(pin, level);
    return result == ESP_OK ? gpio_hold_en(pin) : result;
}

// The wake source is "AI button low", so sleeping while it is still held
// would wake the board immediately.
// 唤醒条件是AI键为低电平，按键未松开就睡眠会立即被唤醒。
void wait_for_wake_button_release()
{
    const gpio_num_t pin = static_cast<gpio_num_t>(PIN_TOP_BUTTON);
    while (gpio_get_level(pin) == 0) {
        vTaskDelay(kReleasePollInterval);
    }
    vTaskDelay(kReleaseDebounceTime);
}

}  // namespace

esp_err_t board_power_init()
{
    app_log_register_tag(kTag);

    const bool woke_from_sleep =
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
    STICKY_LOGI(kTag,
                "step=begin wake_from_sleep=%s hold_pin=%d lock_pin=%d",
                woke_from_sleep ? "true" : "false",
                PIN_POWER_HOLD,
                PIN_POWER_LOCK);

    if (woke_from_sleep) {
        // Preload retained outputs before releasing deep-sleep holds.
        // 在释放深度睡眠保持前预先恢复输出电平。
        gpio_set_direction(static_cast<gpio_num_t>(PIN_POWER_HOLD),
                           GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(PIN_POWER_HOLD), 1);
        gpio_set_direction(static_cast<gpio_num_t>(PIN_POWER_LOCK),
                           GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(PIN_POWER_LOCK), 1);
    }

    gpio_deep_sleep_hold_dis();
    constexpr int kHeldPins[] = {
        PIN_POWER_HOLD,
        PIN_POWER_LOCK,
        PIN_TOP_BUTTON,
        PIN_EPD_EN,
        PIN_TOUCH_EN,
        PIN_TOUCH_RST,
        PIN_SD_EN,
        PIN_BUZZER,
    };
    for (int held_pin : kHeldPins) {
        gpio_hold_dis(static_cast<gpio_num_t>(held_pin));
    }

    gpio_config_t config = {};
    config.pin_bit_mask =
        (1ULL << PIN_POWER_HOLD) | (1ULL << PIN_POWER_LOCK);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "step=configure_outputs result=%s",
                    esp_err_to_name(result));
        return result;
    }

    result = set_power_level(static_cast<gpio_num_t>(PIN_POWER_HOLD), 1);
    if (result != ESP_OK) {
        return result;
    }

    result = set_power_level(static_cast<gpio_num_t>(PIN_POWER_LOCK), 1);
    if (result != ESP_OK) {
        return result;
    }

    vTaskDelay(kPowerSettleDelay);
    STICKY_LOGI(kTag, "step=ready hold=high lock=high result=ok");
    return ESP_OK;
}

[[noreturn]] void board_power_enter_deep_sleep()
{
    const gpio_num_t wake_pin =
        static_cast<gpio_num_t>(PIN_TOP_BUTTON);
    wait_for_wake_button_release();

    // This sequence mirrors the hardware demo: the board latch stays high,
    // while powered peripherals are retained low throughout deep sleep.
    // 此顺序与硬件示例一致：保持板级锁存为高，同时让各外设供电在深睡期间保持低电平。
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_POWER_HOLD), 1));
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_POWER_LOCK), 1));
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_EPD_EN), 0));
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_TOUCH_EN), 0));
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_TOUCH_RST), 0));
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_SD_EN), 0));
    ESP_ERROR_CHECK(hold_output(
        static_cast<gpio_num_t>(PIN_BUZZER), 0));

    gpio_hold_dis(wake_pin);
    ESP_ERROR_CHECK(gpio_set_direction(wake_pin, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_pullup_en(wake_pin));
    ESP_ERROR_CHECK(gpio_pulldown_dis(wake_pin));
    ESP_ERROR_CHECK(gpio_hold_en(wake_pin));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        1ULL << PIN_TOP_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW));
    gpio_deep_sleep_hold_en();

    STICKY_LOGI(kTag,
                "power=deep_sleep wake_button=%d result=entering",
                PIN_TOP_BUTTON);
    std::fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}
