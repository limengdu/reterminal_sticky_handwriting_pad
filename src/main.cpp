#include "app_log.h"
#include "board_charger.h"
#include "board_power.h"
#include "board_shared_spi.h"
#include "canvas.h"
#include "handwrite_app.h"
#include "sticky_button.h"
#include "sticky_display.h"
#include "sticky_touch.h"

#include "esp_err.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef STICKY_FIRMWARE_VERSION
#define STICKY_FIRMWARE_VERSION "unknown"
#endif

namespace {

constexpr char kTag[] = "sticky_boot";

// Keeps the device alive after a required startup component fails.
// 必要组件启动失败后保持设备运行，方便串口持续保留最后一条错误信息。
[[noreturn]] void halt_after_error(const char *component, esp_err_t result)
{
    STICKY_LOGE(kTag,
                "phase=halt component=%s result=%s",
                component,
                esp_err_to_name(result));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void require_ok(const char *component, esp_err_t result)
{
    if (result != ESP_OK) {
        halt_after_error(component, result);
    }
}

}  // namespace

extern "C" void app_main()
{
    app_log_init();
    app_log_register_tag(kTag);
    STICKY_LOGI(kTag, "phase=start firmware=%s app=handwrite wake=%s",
                STICKY_FIRMWARE_VERSION,
                esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED
                    ? "cold_boot"
                    : "deep_sleep");

    require_ok("board_power", board_power_init());
    require_ok("board_charger", board_charger_init());
    // Isolate the SD card before the e-paper driver owns the shared SPI2 bus.
    // 在电子纸驱动接管SPI2前，先让共享总线上的SD卡进入确定的空闲状态。
    require_ok("board_shared_spi", board_shared_spi_prepare());
    require_ok("sticky_display_init", sticky_display_init());

    Canvas *canvas = sticky_display_canvas();
    if (canvas == nullptr) {
        halt_after_error("sticky_display_canvas", ESP_ERR_INVALID_STATE);
    }
    // After a cold boot a white full refresh removes whatever image the
    // e-paper retained. On deep-sleep wake the app's first full refresh
    // replaces the sleep frame directly, saving one flash of the panel.
    // 冷启动时先白屏全刷清除旧画面；深睡唤醒时由APP首帧全刷直接覆盖睡眠画面，少闪一次。
    const bool woke_from_deep_sleep =
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
    if (!woke_from_deep_sleep) {
        require_ok("sticky_display_clear", sticky_display_clear());
    }

    require_ok("sticky_touch_init", sticky_touch_init());
    require_ok("sticky_button_init", sticky_button_init());

    STICKY_LOGI(kTag, "phase=ready result=ok");
    handwrite_app_run(*canvas);
}
