#include "sticky_touch.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "app_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gt911.h"
#include "pin_config.h"
#include "sticky_touch_recovery_policy.h"

namespace {

constexpr char kTag[] = "sticky_touch";
constexpr uint16_t kDisplayWidth = 800;
constexpr uint16_t kDisplayHeight = 480;
constexpr uint16_t kPortraitWidth = 480;
constexpr uint16_t kPortraitHeight = 800;
// Handwriting needs denser samples than tap detection; 10 ms keeps strokes smooth.
// 手写比点击需要更密集的采样，10毫秒间隔可以让笔迹保持平滑。
constexpr TickType_t kPollInterval = pdMS_TO_TICKS(10);
constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 5;
// Large enough to buffer several seconds of writing while the panel refreshes.
// 容量足够在屏幕刷新期间缓存数秒的书写采样。
constexpr UBaseType_t kSampleQueueLength = 1024;
// Ignores sub-pixel jitter while the finger rests in place.
// 手指停留时忽略微小抖动，避免重复入队。
constexpr uint16_t kMoveThreshold = 2U;
// A capacitive contact can drop out for a frame or two under light pressure.
// Releases shorter than this are bridged so one stroke stays continuous.
// 手指轻压时电容信号可能短暂丢失一两帧；短于该时长的抬起会被视为同一笔。
constexpr uint32_t kReleaseDebounceMs = 40U;
// GT911 reports about every 10 ms while touched; this long without any fresh
// report (read errors or no data) ends the stroke.
// 按住时GT911约每10毫秒上报一次；超过该时长没有新报告则结束笔画。
constexpr uint32_t kStaleContactMs = 120U;
// A bridged gap that jumps farther than this starts a new stroke instead.
// 跨越短暂断触后若位置跳得太远，则当作新的一笔，而不是连线。
constexpr uint16_t kRejoinDistance = 40U;
constexpr uint32_t kRecoveryRetryDelayMs = 1000U;
constexpr TickType_t kStopTimeout = pdMS_TO_TICKS(1000);

i2c_master_bus_handle_t s_touch_bus = nullptr;
GT911 s_controller;
TaskHandle_t s_touch_task = nullptr;
QueueHandle_t s_sample_queue = nullptr;
bool s_touching = false;
bool s_release_pending = false;
uint32_t s_release_started_ms = 0U;
uint32_t s_last_contact_ms = 0U;
bool s_read_error_reported = false;
uint32_t s_consecutive_read_failures = 0U;
uint32_t s_next_recovery_attempt_ms = 0U;
uint32_t s_started_at_ms = 0U;
uint16_t s_last_x = 0;
uint16_t s_last_y = 0;
std::atomic<uint32_t> s_last_activity_ms{0U};
std::atomic<bool> s_stop_requested{false};

uint32_t monotonic_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000LL);
}

bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return static_cast<int32_t>(now_ms - target_ms) >= 0;
}

// Starts GT911 with the same reset and address-selection flow as the Sticky demo.
// 使用Sticky示例相同的复位和地址选择流程启动GT911。
bool start_touch_controller(const char *reason)
{
    if (!s_controller.begin(PIN_TOUCH_INT,
                            PIN_TOUCH_RST,
                            kDisplayWidth,
                            kDisplayHeight,
                            s_touch_bus)) {
        STICKY_LOGW(kTag,
                    "touch=controller_start reason=%s result=failed",
                    reason);
        return false;
    }

    uint16_t reported_width = 0U;
    uint16_t reported_height = 0U;
    const bool resolution_read =
        s_controller.readResolution(reported_width, reported_height);
    const StickyTouchSensorResolution resolution =
        sticky_touch_select_sensor_resolution(
            resolution_read, reported_width, reported_height);
    s_controller.setSensorResolution(resolution.width, resolution.height);
    s_consecutive_read_failures = 0U;
    s_read_error_reported = false;
    s_next_recovery_attempt_ms = 0U;

    STICKY_LOGI(kTag,
                "touch=controller_ready reason=%s address=0x%02X map_width=%u map_height=%u fallback=%u result=ok",
                reason,
                s_controller.address(),
                static_cast<unsigned>(resolution.width),
                static_cast<unsigned>(resolution.height),
                static_cast<unsigned>(resolution.used_fallback));
    return true;
}

void queue_sample(uint16_t x, uint16_t y, StickyTouchPhase phase)
{
    const StickyTouchSample sample = {x, y, phase, monotonic_ms()};
    if (xQueueSend(s_sample_queue, &sample, 0) != pdTRUE) {
        STICKY_LOGW(kTag,
                    "touch=sample_queue result=full capacity=%u",
                    static_cast<unsigned>(kSampleQueueLength));
    }
}

void finish_stroke()
{
    if (s_touching) {
        queue_sample(s_last_x, s_last_y, StickyTouchPhase::Up);
    }
    s_touching = false;
    s_release_pending = false;
}

bool recover_touch_controller(const char *reason, uint32_t now_ms)
{
    if (s_next_recovery_attempt_ms != 0U &&
        !time_reached(now_ms, s_next_recovery_attempt_ms)) {
        return false;
    }

    STICKY_LOGW(kTag,
                "touch=recovery reason=%s failures=%u state=started",
                reason,
                static_cast<unsigned>(s_consecutive_read_failures));
    finish_stroke();
    if (start_touch_controller(reason)) {
        return true;
    }
    s_next_recovery_attempt_ms = now_ms + kRecoveryRetryDelayMs;
    return false;
}

uint16_t scale_coordinate(uint16_t value,
                          uint16_t source_max,
                          uint16_t target_max)
{
    const uint16_t clamped = std::min(value, source_max);
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(clamped) * target_max + source_max / 2U) /
        source_max);
}

void transform_touch_coordinate(uint16_t controller_x,
                                uint16_t controller_y,
                                uint16_t &screen_x,
                                uint16_t &screen_y)
{
    // Converts the portrait-oriented touch axes into physical screen axes.
    // 将竖向排列的触摸坐标转换为屏幕的物理坐标。
    const uint16_t portrait_x =
        scale_coordinate(controller_x, kDisplayWidth, kPortraitWidth - 1U);
    const uint16_t clamped_y = std::min(controller_y, kDisplayHeight);
    const uint16_t portrait_y =
        scale_coordinate(kDisplayHeight - clamped_y,
                         kDisplayHeight,
                         kPortraitHeight - 1U);

    const uint16_t framebuffer_x = kDisplayWidth - portrait_y - 1U;
    const uint16_t framebuffer_y = portrait_x;

    // The display layer transmits a 180-degree rotated framebuffer.
    // 显示层会把画布旋转180度后再发送到屏幕。
    screen_x = kDisplayWidth - framebuffer_x - 1U;
    screen_y = kDisplayHeight - framebuffer_y - 1U;
}

uint16_t coordinate_distance(uint16_t first, uint16_t second)
{
    return first >= second ? first - second : second - first;
}

void handle_contact(uint16_t x, uint16_t y, uint32_t now_ms)
{
    s_last_contact_ms = now_ms;
    s_last_activity_ms.store(now_ms, std::memory_order_release);

    if (!s_touching) {
        s_touching = true;
        s_started_at_ms = now_ms;
        s_last_x = x;
        s_last_y = y;
        queue_sample(x, y, StickyTouchPhase::Down);
        return;
    }

    if (s_release_pending) {
        s_release_pending = false;
        if (coordinate_distance(x, s_last_x) > kRejoinDistance ||
            coordinate_distance(y, s_last_y) > kRejoinDistance) {
            finish_stroke();
            handle_contact(x, y, now_ms);
            return;
        }
        // Always emit after a bridged gap so the segment reconnects.
        // 断触被桥接后总是输出一个点，让线段重新接上。
    } else if (coordinate_distance(x, s_last_x) < kMoveThreshold &&
               coordinate_distance(y, s_last_y) < kMoveThreshold) {
        return;
    }
    s_last_x = x;
    s_last_y = y;
    queue_sample(x, y, StickyTouchPhase::Move);
}

// Ends a stroke only after the release persisted, or reports went stale.
// 只有抬起状态持续足够久，或长时间没有新报告时才结束笔画。
void settle_release(uint32_t now_ms)
{
    if (!s_touching) {
        return;
    }
    if (s_release_pending) {
        if (now_ms - s_release_started_ms >= kReleaseDebounceMs) {
            s_last_activity_ms.store(now_ms, std::memory_order_release);
            finish_stroke();
        }
    } else if (now_ms - s_last_contact_ms >= kStaleContactMs) {
        finish_stroke();
    }
}

void touch_task(void *)
{
    TickType_t next_poll = xTaskGetTickCount();
    while (!s_stop_requested.load(std::memory_order_acquire)) {
        GTPoint point = {};
        const int8_t count = s_controller.read_points(&point, 1);
        const uint32_t now_ms = monotonic_ms();
        if (count > 0) {
            s_consecutive_read_failures = 0U;
            s_read_error_reported = false;
            if (s_touching &&
                sticky_touch_contact_stuck(s_started_at_ms, now_ms)) {
                (void)recover_touch_controller("contact_stuck", now_ms);
                vTaskDelayUntil(&next_poll, kPollInterval);
                continue;
            }

            uint16_t x = 0U;
            uint16_t y = 0U;
            transform_touch_coordinate(point.x, point.y, x, y);
            handle_contact(x, y, now_ms);
        } else if (count == 0) {
            s_consecutive_read_failures = 0U;
            s_read_error_reported = false;
            // A zero count is a real release only when GT911 had fresh data;
            // otherwise the controller simply has not produced a new frame yet.
            // 只有GT911给出新数据且点数为0才是真正抬起；否则只是新一帧尚未生成。
            if (s_controller.lastReadHadData() && s_touching &&
                !s_release_pending) {
                s_release_pending = true;
                s_release_started_ms = now_ms;
            }
        } else {
            // A single failed I2C read must not split the stroke; the stale
            // timeout below ends it if failures persist.
            // 单次I2C读取失败不应打断笔画；持续失败时由下方超时结束。
            ++s_consecutive_read_failures;
            if (!s_read_error_reported) {
                STICKY_LOGW(kTag,
                            "touch=read failures=%u result=failed",
                            static_cast<unsigned>(s_consecutive_read_failures));
                s_read_error_reported = true;
            }
            if (sticky_touch_recovery_required(
                    s_consecutive_read_failures)) {
                (void)recover_touch_controller("read_failures", now_ms);
            }
        }

        settle_release(now_ms);
        vTaskDelayUntil(&next_poll, kPollInterval);
    }
    s_touch_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t sticky_touch_init()
{
    app_log_register_tag(kTag);
    app_log_register_tag("GT911");

    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << PIN_TOUCH_EN;
    power_config.mode = GPIO_MODE_OUTPUT;
    power_config.pull_up_en = GPIO_PULLUP_DISABLE;
    power_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t result = gpio_config(&power_config);
    if (result != ESP_OK) {
        return result;
    }
    gpio_hold_dis(static_cast<gpio_num_t>(PIN_TOUCH_EN));
    result = gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_EN), 1);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SDA);
    bus_config.scl_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SCL);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    result = i2c_new_master_bus(&bus_config, &s_touch_bus);
    if (result != ESP_OK) {
        return result;
    }

    if (!start_touch_controller("boot")) {
        return ESP_FAIL;
    }

    s_sample_queue =
        xQueueCreate(kSampleQueueLength, sizeof(StickyTouchSample));
    if (s_sample_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

#if !STICKY_LOG_TOUCH_DRIVER_OUTPUT_ENABLED
    esp_log_level_set("GT911", ESP_LOG_ERROR);
#endif

    if (xTaskCreate(touch_task,
                    "sticky_touch",
                    kTaskStackSize,
                    nullptr,
                    kTaskPriority,
                    &s_touch_task) != pdPASS) {
        vQueueDelete(s_sample_queue);
        s_sample_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }
    STICKY_LOGI(kTag,
                "touch=polling_ready interval_ms=10 sample_queue_capacity=%u result=ok",
                static_cast<unsigned>(kSampleQueueLength));
    return ESP_OK;
}

esp_err_t sticky_touch_stop()
{
    if (s_touch_task == nullptr) {
        return ESP_OK;
    }

    s_stop_requested.store(true, std::memory_order_release);
    const TickType_t deadline = xTaskGetTickCount() + kStopTimeout;
    while (s_touch_task != nullptr) {
        if (static_cast<int32_t>(xTaskGetTickCount() - deadline) >= 0) {
            STICKY_LOGE(kTag, "touch=stop result=timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xQueueReset(s_sample_queue);
    STICKY_LOGI(kTag, "touch=monitoring state=stopped result=ok");
    return ESP_OK;
}

bool sticky_touch_take_sample(StickyTouchSample &sample)
{
    return s_sample_queue != nullptr &&
           xQueueReceive(s_sample_queue, &sample, 0) == pdTRUE;
}

uint32_t sticky_touch_last_activity_ms()
{
    return s_last_activity_ms.load(std::memory_order_acquire);
}
