#include "board_shared_spi.h"

#include "app_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_shared_spi";
constexpr uint32_t kReferenceIdleLevel = 1;
constexpr TickType_t kControlPinSettleDelay = pdMS_TO_TICKS(10);

// Configures one output and verifies that ESP-IDF accepted the requested level.
// 配置一个输出引脚，并确认ESP-IDF成功写入目标电平。
esp_err_t configure_output(gpio_num_t pin, uint32_t level)
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t result = gpio_config(&config);
    if (result == ESP_OK) {
        result = gpio_set_level(pin, level);
    }
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "shared_spi=configure_output pin=%d level=%lu result=%s",
                    static_cast<int>(pin),
                    static_cast<unsigned long>(level),
                    esp_err_to_name(result));
    }
    return result;
}

}  // namespace

esp_err_t board_shared_spi_prepare()
{
    app_log_register_tag(kTag);
    STICKY_LOGI(kTag,
                "shared_spi=prepare_begin host=%d sd_cs_pin=%d sd_en_pin=%d",
                static_cast<int>(SPI2_HOST),
                PIN_SD_CS,
                PIN_SD_EN);

    // SD_EN may be retained by a future deep-sleep flow. Release that hold
    // before restoring the same idle level used by the hardware demo.
    // 后续深度睡眠流程可能保持SD_EN电平，因此先解除保持，再恢复示例工程的空闲电平。
    gpio_hold_dis(static_cast<gpio_num_t>(PIN_SD_EN));

    esp_err_t result = configure_output(
        static_cast<gpio_num_t>(PIN_SD_CS), kReferenceIdleLevel);
    if (result != ESP_OK) {
        return result;
    }

    result = configure_output(
        static_cast<gpio_num_t>(PIN_SD_EN), kReferenceIdleLevel);
    if (result != ESP_OK) {
        return result;
    }

    gpio_config_t detect_config = {};
    detect_config.pin_bit_mask = 1ULL << PIN_SD_DETECT;
    detect_config.mode = GPIO_MODE_INPUT;
    detect_config.pull_up_en = GPIO_PULLUP_ENABLE;
    detect_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    detect_config.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&detect_config);
    if (result != ESP_OK) {
        STICKY_LOGE(kTag,
                    "shared_spi=configure_detect pin=%d result=%s",
                    PIN_SD_DETECT,
                    esp_err_to_name(result));
        return result;
    }

    STICKY_LOGI(kTag,
                "shared_spi=sd_idle cs_level=%d en_level=%d detect_level=%d",
                gpio_get_level(static_cast<gpio_num_t>(PIN_SD_CS)),
                gpio_get_level(static_cast<gpio_num_t>(PIN_SD_EN)),
                gpio_get_level(static_cast<gpio_num_t>(PIN_SD_DETECT)));

    // Installs the GPIO interrupt service before the panel registers BUSY.
    // 在屏幕驱动注册BUSY脚之前安装GPIO中断服务。
    result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        STICKY_LOGE(kTag,
                    "shared_spi=gpio_isr result=%s",
                    esp_err_to_name(result));
        return result;
    }

    STICKY_LOGI(kTag,
                "shared_spi=gpio_isr_ready state=%s",
                result == ESP_OK ? "installed" : "already_installed");
    vTaskDelay(kControlPinSettleDelay);
    STICKY_LOGI(kTag, "shared_spi=prepare_done result=ok");
    return ESP_OK;
}
