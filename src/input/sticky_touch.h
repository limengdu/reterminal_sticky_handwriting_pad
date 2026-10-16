#pragma once

#include <cstdint>

#include "esp_err.h"

enum class StickyTouchPhase : uint8_t {
    Down,
    Move,
    Up,
};

// One raw contact sample in physical framebuffer coordinates (800x480).
// 一个原始触摸采样点，坐标为物理帧缓冲区坐标（800x480）。
struct StickyTouchSample {
    uint16_t x = 0;
    uint16_t y = 0;
    StickyTouchPhase phase = StickyTouchPhase::Down;
    uint32_t captured_at_ms = 0;
};

// Powers the GT911 and starts a fast sampling task for continuous strokes.
// 为GT911上电，并启动用于连续笔迹的高频采样任务。
esp_err_t sticky_touch_init();

// Stops the polling task before touch power is removed for deep sleep.
// 在深度睡眠关闭触摸供电前，安全停止轮询任务。
esp_err_t sticky_touch_stop();

// Consumes the oldest queued sample; returns false when the queue is empty.
// 按顺序取出最早的采样点；队列为空时返回false。
bool sticky_touch_take_sample(StickyTouchSample &sample);

// Returns the most recent physical touch timestamp in milliseconds.
// 返回最近一次实体触摸发生的毫秒时间戳。
uint32_t sticky_touch_last_activity_ms();
