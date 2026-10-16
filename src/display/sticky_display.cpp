#include "sticky_display.h"

#include <cstring>
#include <new>

#include "app_log.h"
#include "canvas.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper_panel.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_display";
constexpr size_t kFramebufferStride = kStickyDisplayWidth / 4U;
constexpr size_t kFramebufferSize = kFramebufferStride * kStickyDisplayHeight;
constexpr size_t kMonochromeStride = kStickyDisplayWidth / 8U;
constexpr int kSpiClockHz = 20 * 1000 * 1000;

seeed_epaper_panel_handle_t s_panel = nullptr;
spi_device_handle_t s_spi_device = nullptr;
uint8_t *s_framebuffer = nullptr;
uint8_t *s_rotated_framebuffer = nullptr;
Canvas *s_canvas = nullptr;

uint8_t reverse_pixel_order(uint8_t packed_pixels)
{
    return static_cast<uint8_t>(((packed_pixels & 0x03U) << 6) |
                                ((packed_pixels & 0x0CU) << 2) |
                                ((packed_pixels & 0x30U) >> 2) |
                                ((packed_pixels & 0xC0U) >> 6));
}

void rotate_framebuffer_180(const uint8_t *source, uint8_t *destination)
{
    // Four 2-bit pixels are packed into each byte. Reversing both the byte
    // order and the four pixel groups produces a complete 180-degree rotation.
    // 每个字节保存四个2位像素；同时反转字节顺序和字节内像素顺序即可旋转180度。
    for (size_t source_index = 0; source_index < kFramebufferSize; ++source_index) {
        const size_t destination_index = kFramebufferSize - 1U - source_index;
        destination[destination_index] = reverse_pixel_order(source[source_index]);
    }
}

// Maps one packed 2bpp byte (four pixels, MSB first) to a 4-bit monochrome
// nibble with the pixel order reversed. Black/White map directly; the two
// grays become fixed ordered-dither patterns so gray ink is visible during
// fast partial refreshes before a gray4 refresh shows the real tone:
// DarkGray keeps one white pixel per 2x2 cell, LightGray is a checkerboard.
// The pattern depends on canvas row parity, so there is one table per parity.
// 将一个2位灰度字节（四个像素）映射为像素顺序反转的4位黑白值。黑白直接映射；两种灰色使用
// 固定的有序抖动图案，让灰色笔迹在快速局刷时也可见，之后由灰度全刷显示真实灰阶：
// 深灰在每个2x2格中保留一个白点，浅灰为棋盘格。图案与画布行奇偶有关，因此每种奇偶各一张表。
struct ReversedMonoNibbleTable {
    uint8_t values[2][256];

    constexpr ReversedMonoNibbleTable() : values()
    {
        for (int row_parity = 0; row_parity < 2; ++row_parity) {
            for (int packed = 0; packed < 256; ++packed) {
                uint8_t nibble = 0;
                for (int pixel = 0; pixel < 4; ++pixel) {
                    // Every byte starts at an x divisible by four, so the
                    // pixel index gives the column parity.
                    // 每个字节都从x为4的倍数处开始，像素序号即可给出列奇偶。
                    const int column_parity = pixel & 1;
                    const int gray = (packed >> ((3 - pixel) * 2)) & 0x03;
                    bool white = gray == 3;
                    if (gray == 1) {
                        white = column_parity == 0 && row_parity == 0;
                    } else if (gray == 2) {
                        white = ((column_parity + row_parity) & 1) == 0;
                    }
                    if (white) {
                        nibble |= static_cast<uint8_t>(1U << pixel);
                    }
                }
                values[row_parity][packed] = nibble;
            }
        }
    }
};

constexpr ReversedMonoNibbleTable kReversedMonoNibble;

void rotate_180_to_monochrome(const uint8_t *source, uint8_t *destination)
{
    // A 180-degree rotation reverses the linear pixel order because rows have
    // no padding. Each 1bpp output byte comes from two 2bpp source bytes, so
    // one table-driven pass replaces separate rotation and conversion loops.
    // 行内没有填充字节，旋转180度等于反转整个像素序列；每个1位输出字节来自两个2位输入字节，
    // 用一次查表完成原来的旋转和转换两遍循环。
    constexpr size_t kMonochromeSize = kMonochromeStride * kStickyDisplayHeight;
    for (size_t index = 0; index < kMonochromeSize; ++index) {
        const uint8_t(&table)[256] =
            kReversedMonoNibble.values[(index / kMonochromeStride) & 1U];
        const uint8_t first = source[index * 2U];
        const uint8_t second = source[index * 2U + 1U];
        destination[kMonochromeSize - 1U - index] =
            static_cast<uint8_t>((table[second] << 4) | table[first]);
    }
}

int64_t elapsed_ms(int64_t started_us)
{
    return (esp_timer_get_time() - started_us) / 1000;
}

}  // namespace

esp_err_t sticky_display_init()
{
    app_log_register_tag(kTag);
    app_log_register_tag("seeed_epaper");
    app_log_register_tag("ssd1677");

    if (s_panel != nullptr && s_canvas != nullptr) {
        STICKY_LOGD(kTag, "display=init state=already_ready");
        return ESP_OK;
    }

    const int64_t init_started_us = esp_timer_get_time();
    STICKY_LOGI(kTag,
                "display=init_begin width=%u height=%u",
                static_cast<unsigned>(kStickyDisplayWidth),
                static_cast<unsigned>(kStickyDisplayHeight));

    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << PIN_EPD_EN;
    power_config.mode = GPIO_MODE_OUTPUT;
    power_config.pull_up_en = GPIO_PULLUP_DISABLE;
    power_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&power_config), "sticky_display", "configure display power");
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_EN), 1),
                        "sticky_display", "enable display power");
    vTaskDelay(pdMS_TO_TICKS(100));
    STICKY_LOGD(kTag,
                "display=power_on pin=%d settle_ms=100 result=ok",
                PIN_EPD_EN);

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = PIN_EPD_MOSI;
    bus_config.miso_io_num = PIN_EPD_MISO;
    bus_config.sclk_io_num = PIN_EPD_CLK;
    // ESP-IDF requires every unused SPI data pin to be -1. Leaving these
    // zero-initialized assigns GPIO0 to the SPI bus, but GPIO0 is also the
    // sensor I2C SCL pin. That made SHT40 work before display init and fail
    // immediately afterwards.
    // ESP-IDF要求未使用的SPI数据脚显式设为-1，否则零初始化会错误占用GPIO0。
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    bus_config.max_transfer_sz = kStickyDisplayWidth * kStickyDisplayHeight / 8;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        "sticky_display", "initialize SPI2");
    STICKY_LOGD(kTag,
                "display=spi_bus_ready host=%d mosi=%d miso=%d clk=%d result=ok",
                static_cast<int>(SPI2_HOST),
                PIN_EPD_MOSI,
                PIN_EPD_MISO,
                PIN_EPD_CLK);

    spi_device_interface_config_t device_config = {};
    // SSD1677 accepts writes up to 20 MHz (50 ns cycle); doubling the
    // reference 10 MHz halves the ~77 ms transfer of each partial refresh.
    // SSD1677写入时钟最高20MHz；相比参考工程的10MHz，局刷传输时间减半。
    device_config.clock_speed_hz = kSpiClockHz;
    device_config.mode = 0;
    device_config.spics_io_num = PIN_EPD_CS;
    device_config.queue_size = 1;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &device_config, &s_spi_device),
                        "sticky_display", "add display to SPI2");
    STICKY_LOGD(kTag,
                "display=spi_device_ready cs=%d clock_hz=%d mode=0 result=ok",
                PIN_EPD_CS,
                device_config.clock_speed_hz);

    seeed_epaper_panel_config_t panel_config = {};
    panel_config.spi_handle = s_spi_device;
    panel_config.pin_dc = static_cast<gpio_num_t>(PIN_EPD_DC);
    panel_config.pin_rst = static_cast<gpio_num_t>(PIN_EPD_RST);
    panel_config.pin_busy = static_cast<gpio_num_t>(PIN_EPD_BUSY);
    panel_config.pin_enable = GPIO_NUM_NC;
    panel_config.busy_timeout_ms = 10000;
    panel_config.reset_low_ms = 10;
    panel_config.reset_high_ms = 10;
    panel_config.busy_level = 1;
    panel_config.enable_level = 1;
    panel_config.mirror_x = true;
    ESP_RETURN_ON_ERROR(
        seeed_epaper_new_panel(SEEED_EPAPER_PANEL_SSD1677, &panel_config, &s_panel),
        "sticky_display", "create SSD1677 panel");
    STICKY_LOGI(kTag,
                "display=panel_ready controller=SSD1677 dc=%d rst=%d busy=%d result=ok",
                PIN_EPD_DC,
                PIN_EPD_RST,
                PIN_EPD_BUSY);

    s_framebuffer = static_cast<uint8_t *>(
        heap_caps_malloc(kFramebufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_rotated_framebuffer = static_cast<uint8_t *>(
        heap_caps_malloc(kFramebufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_framebuffer == nullptr || s_rotated_framebuffer == nullptr) {
        heap_caps_free(s_framebuffer);
        heap_caps_free(s_rotated_framebuffer);
        s_framebuffer = nullptr;
        s_rotated_framebuffer = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_canvas = new (std::nothrow)
        Canvas(kStickyDisplayWidth, kStickyDisplayHeight, s_framebuffer, kFramebufferSize);
    if (s_canvas == nullptr) {
        heap_caps_free(s_framebuffer);
        heap_caps_free(s_rotated_framebuffer);
        s_framebuffer = nullptr;
        s_rotated_framebuffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_canvas->clear();
    STICKY_LOGI(kTag,
                "display=framebuffer_ready buffers=2 bytes_each=%u memory=psram result=ok",
                static_cast<unsigned>(kFramebufferSize));
    STICKY_LOGI(kTag,
                "display=init_done elapsed_ms=%lld result=ok",
                static_cast<long long>((esp_timer_get_time() - init_started_us) / 1000));
    return ESP_OK;
}

Canvas *sticky_display_canvas()
{
    return s_canvas;
}

esp_err_t sticky_display_refresh()
{
    if (s_panel == nullptr || s_canvas == nullptr || s_rotated_framebuffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t refresh_started_us = esp_timer_get_time();
    STICKY_LOGI(kTag, "display=refresh_begin mode=gray4");
    rotate_framebuffer_180(s_canvas->data(), s_rotated_framebuffer);

    const seeed_epaper_area_t full_screen = {
        0, 0, kStickyDisplayWidth, kStickyDisplayHeight,
    };
    ESP_RETURN_ON_ERROR(
        seeed_epaper_panel_write_bitmap_gray4(
            s_panel, &full_screen, s_rotated_framebuffer, s_canvas->stride()),
        "sticky_display", "write gray4 framebuffer");
    esp_err_t result =
        seeed_epaper_panel_commit(s_panel, &full_screen, SEEED_EPAPER_REFRESH_GRAY4);
    if (result == ESP_OK) {
        // The driver records a thresholded frame as the partial baseline.
        // Replace it with the dithered frame the partial path produces, so
        // unchanged gray pixels compare equal and keep their real gray tone.
        // 驱动默认把阈值化的黑白帧作为局刷基准；改为局刷路径生成的抖动帧，
        // 使未变化的灰色像素前后相同，从而保持真实灰阶。
        rotate_180_to_monochrome(s_canvas->data(), s_rotated_framebuffer);
        result = seeed_epaper_panel_set_partial_baseline(
            s_panel, &full_screen, s_rotated_framebuffer, kMonochromeStride,
            SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB);
    }
    STICKY_LOGI(kTag,
                "display=refresh_done mode=gray4 elapsed_ms=%lld result=%s",
                static_cast<long long>((esp_timer_get_time() - refresh_started_us) / 1000),
                esp_err_to_name(result));
    return result;
}

esp_err_t sticky_display_refresh_partial()
{
    if (s_panel == nullptr || s_canvas == nullptr ||
        s_rotated_framebuffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // SSD1677 applies its partial waveform across the panel even when only a
    // small RAM window is written. Send a complete monochrome comparison frame
    // so unchanged pixels have identical previous/current values and remain
    // visually untouched. Callers should change only the intended Canvas area.
    // SSD1677局刷仍会应用全屏波形，因此发送完整对比帧来保护没有变化的像素。
    const int64_t started_us = esp_timer_get_time();
    rotate_180_to_monochrome(s_canvas->data(), s_rotated_framebuffer);
    const int64_t converted_us = esp_timer_get_time();

    const seeed_epaper_area_t full_screen = {
        0, 0, kStickyDisplayWidth, kStickyDisplayHeight,
    };
    ESP_RETURN_ON_ERROR(
        seeed_epaper_panel_write_bitmap(s_panel,
                                        &full_screen,
                                        s_rotated_framebuffer,
                                        kMonochromeStride,
                                        SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,
                                        SEEED_EPAPER_REFRESH_PARTIAL),
        "sticky_display", "write partial framebuffer");
    const int64_t written_us = esp_timer_get_time();
    const esp_err_t result = seeed_epaper_panel_commit(
        s_panel, &full_screen, SEEED_EPAPER_REFRESH_PARTIAL);

    // convert = CPU frame preparation, write = SPI transfer of both RAM
    // planes, waveform = the panel's own update time (BUSY high).
    // convert为CPU准备帧的耗时，write为SPI传输两份显存，waveform为屏幕自身波形耗时（BUSY为高）。
    STICKY_LOGI(kTag,
                "display=refresh_partial convert_ms=%lld write_ms=%lld waveform_ms=%lld total_ms=%lld result=%s",
                static_cast<long long>((converted_us - started_us) / 1000),
                static_cast<long long>((written_us - converted_us) / 1000),
                static_cast<long long>(elapsed_ms(written_us)),
                static_cast<long long>(elapsed_ms(started_us)),
                esp_err_to_name(result));
    return result;
}

esp_err_t sticky_display_refresh_monochrome()
{
    if (s_panel == nullptr || s_canvas == nullptr ||
        s_rotated_framebuffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t refresh_started_us = esp_timer_get_time();
    STICKY_LOGI(kTag, "display=refresh_begin mode=monochrome_full");
    rotate_180_to_monochrome(s_canvas->data(), s_rotated_framebuffer);

    const seeed_epaper_area_t full_screen = {
        0, 0, kStickyDisplayWidth, kStickyDisplayHeight,
    };
    const esp_err_t result = seeed_epaper_panel_refresh_area(
        s_panel,
        &full_screen,
        s_rotated_framebuffer,
        kMonochromeStride,
        SEEED_EPAPER_PIXEL_FORMAT_MONO1_MSB,
        SEEED_EPAPER_REFRESH_FULL);
    STICKY_LOGI(kTag,
                "display=refresh_done mode=monochrome_full elapsed_ms=%lld result=%s",
                static_cast<long long>((esp_timer_get_time() - refresh_started_us) / 1000),
                esp_err_to_name(result));
    return result;
}

esp_err_t sticky_display_power_off()
{
    if (s_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return seeed_epaper_panel_power_off(s_panel);
}

esp_err_t sticky_display_clear()
{
    if (s_panel == nullptr || s_canvas == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t clear_started_us = esp_timer_get_time();
    STICKY_LOGI(kTag, "display=clear_begin color=white mode=full");
    s_canvas->clear();
    const esp_err_t result =
        seeed_epaper_panel_clear(s_panel, true, SEEED_EPAPER_REFRESH_FULL);
    STICKY_LOGI(kTag,
                "display=clear_done color=white mode=full elapsed_ms=%lld result=%s",
                static_cast<long long>(
                    (esp_timer_get_time() - clear_started_us) / 1000),
                esp_err_to_name(result));
    return result;
}

esp_err_t sticky_display_sleep()
{
    if (s_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        seeed_epaper_panel_sleep(s_panel),
        "sticky_display", "put panel to sleep");
    return gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_EN), 0);
}
