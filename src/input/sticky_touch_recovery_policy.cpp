#include "sticky_touch_recovery_policy.h"

StickyTouchSensorResolution sticky_touch_select_sensor_resolution(
    bool read_succeeded,
    uint16_t reported_width,
    uint16_t reported_height)
{
    const bool reported_geometry_valid =
        read_succeeded &&
        reported_width == kStickyTouchSensorWidth &&
        reported_height == kStickyTouchSensorHeight;
    return {
        kStickyTouchSensorWidth,
        kStickyTouchSensorHeight,
        !reported_geometry_valid,
    };
}

bool sticky_touch_recovery_required(uint32_t consecutive_failures)
{
    return consecutive_failures >= kStickyTouchReadFailureThreshold;
}

bool sticky_touch_contact_stuck(uint32_t started_at_ms, uint32_t now_ms)
{
    return now_ms - started_at_ms >= kStickyTouchMaximumContactMs;
}
