#pragma once

#include "esp_err.h"

// Keeps the Sticky power latch active after startup.
// 在启动后保持 Sticky 的电源锁存状态。
esp_err_t board_power_init();

// Waits for the AI button to be released, retains peripheral power pins low,
// and enters deep sleep. A later AI-button press wakes (reboots) the board.
// 等待AI键松开、保持外设供电引脚为低并进入深度睡眠；之后按下AI键即可唤醒（重新启动）。
[[noreturn]] void board_power_enter_deep_sleep();
