#pragma once

#include "esp_err.h"

// Places the MicroSD interface in its reference idle state before SPI2 starts.
// 在SPI2启动前将MicroSD接口置于参考工程使用的空闲状态。
//
// Returns ESP_OK when the SD control pins and GPIO ISR service are ready.
// SD控制引脚和GPIO中断服务准备完成时返回ESP_OK，失败时返回ESP-IDF错误码。
esp_err_t board_shared_spi_prepare();
