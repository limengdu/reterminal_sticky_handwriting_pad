#pragma once

#include <cstdint>

#include "esp_err.h"

enum class StickyButtonEvent : uint8_t {
    None,
    PageUp,
    PageDown,
    AiClick,
    AiLongPress,
};

// Initializes the top AI button and the two side page buttons (active low).
// 初始化顶部AI键和两个侧面翻页键（低电平有效）。
esp_err_t sticky_button_init();

// Consumes one queued button event; returns false when nothing is pending.
// 取出一个排队的按键事件；没有事件时返回false。
bool sticky_button_take_event(StickyButtonEvent &event);

// Discards events queued before the app was ready, such as the wake press.
// 丢弃APP就绪前排队的事件，例如用于唤醒的那次按键。
void sticky_button_clear();
