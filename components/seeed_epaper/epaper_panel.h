#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifndef GPIO_NUM_NC
#define GPIO_NUM_NC ((gpio_num_t)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct seeed_epaper_panel_t *seeed_epaper_panel_handle_t;

typedef enum {
    SEEED_EPAPER_PANEL_UC8179 = 0,
    SEEED_EPAPER_PANEL_SSD1677,
} seeed_epaper_panel_model_t;

typedef enum {
    SEEED_EPAPER_REFRESH_FULL = 0,
    SEEED_EPAPER_REFRESH_PARTIAL,
    SEEED_EPAPER_REFRESH_GRAY4,
} seeed_epaper_refresh_mode_t;

typedef enum {
    SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB = 0,
    SEEED_EPAPER_PIXEL_FORMAT_GRAY4,
    SEEED_EPAPER_PIXEL_FORMAT_GRAY8,
    SEEED_EPAPER_PIXEL_FORMAT_RGB565,
} seeed_epaper_pixel_format_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} seeed_epaper_area_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t native_bpp;
    bool supports_partial;
    bool supports_gray4;
    bool supports_color;
} seeed_epaper_panel_info_t;

typedef struct {
    // SPI handle is created by the board layer so shared-bus ownership remains explicit.
    // SPI句柄由板级代码创建，便于后续明确管理屏幕与SD卡的共享总线。
    spi_device_handle_t spi_handle;
    gpio_num_t pin_dc;
    gpio_num_t pin_rst;
    gpio_num_t pin_busy;
    gpio_num_t pin_enable;
    uint32_t busy_timeout_ms;
    uint16_t reset_low_ms;
    uint16_t reset_high_ms;
    uint8_t busy_level;
    uint8_t enable_level;
    bool mirror_x;
} seeed_epaper_panel_config_t;

/**
 * Creates a controller instance without taking ownership of the SPI handle.
 * 创建屏幕控制器实例，但不接管SPI句柄的释放责任。
 *
 * @param model Controller model selected from seeed_epaper_panel_model_t.
 *              从seeed_epaper_panel_model_t中选择的控制器型号。
 * @param config GPIO, timing and SPI configuration; must remain valid for this call.
 *               GPIO、时序和SPI配置，本次调用期间必须有效。
 * @param ret_panel Receives the created handle when ESP_OK is returned.
 *                  返回ESP_OK时，通过该参数带回创建好的句柄。
 * @return ESP_OK on success, otherwise an ESP-IDF error code.
 *         成功返回ESP_OK，失败返回对应的ESP-IDF错误码。
 */
esp_err_t seeed_epaper_new_panel(seeed_epaper_panel_model_t model,
                                 const seeed_epaper_panel_config_t *config,
                                 seeed_epaper_panel_handle_t *ret_panel);

esp_err_t seeed_epaper_panel_del(seeed_epaper_panel_handle_t panel);

esp_err_t seeed_epaper_panel_get_info(seeed_epaper_panel_handle_t panel,
                                      seeed_epaper_panel_info_t *ret_info);

esp_err_t seeed_epaper_panel_prepare(seeed_epaper_panel_handle_t panel,
                                     seeed_epaper_refresh_mode_t mode);

/**
 * Converts one source image into the panel-native format and writes panel RAM.
 * 将一张源图转换为屏幕原生格式，并写入控制器显存；此函数本身不触发物理刷新。
 */
esp_err_t seeed_epaper_panel_write_bitmap(seeed_epaper_panel_handle_t panel,
                                          const seeed_epaper_area_t *area,
                                          const void *pixels,
                                          size_t src_stride_bytes,
                                          seeed_epaper_pixel_format_t input_format,
                                          seeed_epaper_refresh_mode_t mode);

esp_err_t seeed_epaper_panel_write_bitmap_diff(seeed_epaper_panel_handle_t panel,
                                               const seeed_epaper_area_t *area,
                                               const void *previous_pixels,
                                               const void *current_pixels,
                                               size_t src_stride_bytes,
                                               seeed_epaper_pixel_format_t input_format,
                                               seeed_epaper_refresh_mode_t mode);

// Replaces the stored "previous frame" used by the next partial refresh
// without touching the physical panel. After a gray4 refresh this lets the
// caller record exactly the monochrome frame its partial path will produce,
// so later partial refreshes leave the real gray pixels undriven.
// 只替换下一次局刷使用的"上一帧"基准，不改动实体屏幕。灰度刷新后调用方可写入其局刷路径
// 将生成的同一黑白帧，使之后的局刷不再驱动已显示为真灰的像素。
esp_err_t seeed_epaper_panel_set_partial_baseline(seeed_epaper_panel_handle_t panel,
                                                  const seeed_epaper_area_t *area,
                                                  const void *pixels,
                                                  size_t src_stride_bytes,
                                                  seeed_epaper_pixel_format_t input_format);

// Writes packed 2-bit grayscale pixels before a gray4 commit.
// 写入打包后的2位灰度像素，随后需要调用commit触发四级灰度刷新。
esp_err_t seeed_epaper_panel_write_bitmap_gray4(
    seeed_epaper_panel_handle_t panel,
    const seeed_epaper_area_t *area,
    const uint8_t *pixels,
    size_t src_stride_bytes);

/**
 * Triggers the physical refresh for image data already written into panel RAM.
 * 对已经写入控制器显存的数据触发实体屏幕刷新。
 */
esp_err_t seeed_epaper_panel_commit(seeed_epaper_panel_handle_t panel,
                                    const seeed_epaper_area_t *area,
                                    seeed_epaper_refresh_mode_t mode);

/**
 * Convenience operation that prepares, writes and refreshes one image area.
 * 组合操作：依次完成刷新准备、图像写入和实体屏幕刷新。
 */
esp_err_t seeed_epaper_panel_refresh_area(seeed_epaper_panel_handle_t panel,
                                          const seeed_epaper_area_t *area,
                                          const void *pixels,
                                          size_t src_stride_bytes,
                                          seeed_epaper_pixel_format_t input_format,
                                          seeed_epaper_refresh_mode_t mode);

esp_err_t seeed_epaper_panel_clear(seeed_epaper_panel_handle_t panel,
                                   bool white,
                                   seeed_epaper_refresh_mode_t mode);

// Turns off panel analog power kept on between fast partial refreshes.
// Call it once writing goes idle; it is a no-op when power is already off.
// 关闭快速局刷之间保留的屏幕模拟电源；书写空闲后调用一次即可，已断电时不做任何操作。
esp_err_t seeed_epaper_panel_power_off(seeed_epaper_panel_handle_t panel);

// Enters controller deep sleep; the e-paper image remains visible without power.
// 让控制器进入深度睡眠；电子纸断电后仍会保留当前画面。
esp_err_t seeed_epaper_panel_sleep(seeed_epaper_panel_handle_t panel);

esp_err_t seeed_epaper_panel_wakeup(seeed_epaper_panel_handle_t panel,
                                    seeed_epaper_refresh_mode_t mode);

bool seeed_epaper_panel_busy_wait_active(void);

#ifdef __cplusplus
}
#endif
