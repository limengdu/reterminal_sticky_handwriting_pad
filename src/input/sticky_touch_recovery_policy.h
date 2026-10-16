#pragma once

#include <cstdint>

constexpr uint16_t kStickyTouchSensorWidth = 480U;
constexpr uint16_t kStickyTouchSensorHeight = 800U;
constexpr uint32_t kStickyTouchReadFailureThreshold = 5U;
constexpr uint32_t kStickyTouchMaximumContactMs = 10000U;

struct StickyTouchSensorResolution {
    uint16_t width;
    uint16_t height;
    bool used_fallback;
};

// Selects the verified Sticky sensor geometry when a register read is unusable.
// 当寄存器读取结果不可用时，选用Sticky已验证的触摸分辨率。
StickyTouchSensorResolution sticky_touch_select_sensor_resolution(
    bool read_succeeded,
    uint16_t reported_width,
    uint16_t reported_height);

// Reports when repeated polling failures require the reference reset flow.
// 判断连续轮询失败是否需要重新执行示例复位流程。
bool sticky_touch_recovery_required(uint32_t consecutive_failures);

// Reports when one contact has remained active beyond normal UI interaction.
// 判断一次触摸是否已持续超过正常界面交互时间。
bool sticky_touch_contact_stuck(uint32_t started_at_ms, uint32_t now_ms);
