#pragma once

#include "esp_err.h"

// Enables the onboard BQ25616 charging path and external-power detection.
// 启用板载BQ25616充电路径和外部电源检测。
esp_err_t board_charger_init();

// Returns true while USB or another external source is present.
// 当USB或其他外部电源存在时返回true。
bool board_charger_external_power_present();
