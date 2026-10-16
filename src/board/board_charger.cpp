#include "board_charger.h"

#include "app_log.h"
#include "driver/gpio.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_charger";
constexpr uint32_t kChargingEnabledLevel = 0U;

}  // namespace

esp_err_t board_charger_init()
{
    app_log_register_tag(kTag);

#if STICKY_LOG_POWER_DETAILS_ENABLED
    STICKY_LOGD(kTag,
                "charger=init_begin enable_pin=%d active_level=%lu external_power_pin=%d",
                PIN_BAT_CHG_EN,
                static_cast<unsigned long>(kChargingEnabledLevel),
                PIN_EXTERNAL_POWER);
#endif

    // EN_BAT_CHGn is active low in the hardware reference implementation.
    // 硬件参考实现中EN_BAT_CHGn为低电平有效。
    gpio_config_t enable_config = {};
    enable_config.pin_bit_mask = 1ULL << PIN_BAT_CHG_EN;
    enable_config.mode = GPIO_MODE_OUTPUT;
    enable_config.pull_up_en = GPIO_PULLUP_DISABLE;
    enable_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    enable_config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t result = gpio_config(&enable_config);
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "charger=configure_enable pin=%d result=%s",
                    PIN_BAT_CHG_EN,
                    esp_err_to_name(result));
        return result;
    }

    result = gpio_set_level(
        static_cast<gpio_num_t>(PIN_BAT_CHG_EN),
        kChargingEnabledLevel);
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "charger=set_enable pin=%d level=%lu result=%s",
                    PIN_BAT_CHG_EN,
                    static_cast<unsigned long>(kChargingEnabledLevel),
                    esp_err_to_name(result));
        return result;
    }

    gpio_config_t detect_config = {};
    detect_config.pin_bit_mask = 1ULL << PIN_EXTERNAL_POWER;
    detect_config.mode = GPIO_MODE_INPUT;
    detect_config.pull_up_en = GPIO_PULLUP_DISABLE;
    detect_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    detect_config.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&detect_config);
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "charger=configure_external_power pin=%d result=%s",
                    PIN_EXTERNAL_POWER,
                    esp_err_to_name(result));
        return result;
    }

    STICKY_LOGI(kTag,
                "charger=ready enable_pin=%d enable_level=%d external_power_pin=%d external_power=%d result=ok",
                PIN_BAT_CHG_EN,
                gpio_get_level(static_cast<gpio_num_t>(PIN_BAT_CHG_EN)),
                PIN_EXTERNAL_POWER,
                gpio_get_level(static_cast<gpio_num_t>(PIN_EXTERNAL_POWER)));
    return ESP_OK;
}

bool board_charger_external_power_present()
{
    return gpio_get_level(
               static_cast<gpio_num_t>(PIN_EXTERNAL_POWER)) != 0;
}
