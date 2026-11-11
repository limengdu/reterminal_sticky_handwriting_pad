# SEEED_EPAPER

`SEEED_EPAPER` 是一个面向 ESP-IDF 的模块化墨水屏组件，当前先支持：

- `UC8179`：reTerminal E1001
- `SSD1677`：reTerminal E1005 / Sticky 旧屏
- `SSD2677`：Sticky 新屏
- `STICKY_AUTO`：启动读取 `0x70`，`0x07` 选择 SSD2677，其余成功读数选择 SSD1677。显式型号跳过探测。

设计目标：

- SPI 总线由外部创建，组件只接收 `spi_device_handle_t`
- `panel` 核心与 `driver` 控制器实现分层，后续加新屏只需补 driver
- 对外统一支持整屏刷新、任意区域局刷、1bpp 原生输入和 4/8/16bit 转 1bpp
- 为后续灰度 / 彩色扩展预留能力描述与像素格式枚举

## 对外 API

- `seeed_epaper_new_panel()`：创建面板句柄
- `seeed_epaper_panel_prepare()`：准备一次 full / partial 会话
- `seeed_epaper_panel_write_bitmap()`：写入一块区域到显存
- `seeed_epaper_panel_commit()`：触发物理刷新
- `seeed_epaper_panel_refresh_area()`：单次区域写入并刷新
- `seeed_epaper_panel_sleep()`：进入休眠

推荐给 LVGL9 的接法：

1. `flush_cb` 中对每个 chunk 调 `prepare + write_bitmap`
2. `lv_display_flush_is_last(display)` 为真时再 `commit`
3. 首帧走 `FULL`，后续走 `PARTIAL`
4. `LV_COLOR_DEPTH=1` 时直接传 `MONO1_MSB`
5. `LV_COLOR_DEPTH=8/16` 时传 `GRAY8` / `RGB565`，组件内部阈值转换为 1bpp

## 目录结构

- `epaper_panel.h`：统一公共接口
- `epaper_panel.c`：GPIO / SPI / 像素转换 / 会话管理
- `driver/uc8179.c`：UC8179 控制器实现
- `driver/ssd1677.c`：SSD1677 控制器实现

## Sticky 集成注意事项

- SPI 设备使用 `SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE | SPI_DEVICE_NO_DUMMY`，通过 GPIO14 双向读写屏幕 SDA；GPIO12 供 SD 卡 MISO 使用。
- 本项目统一使用 2 MHz 屏幕时钟，满足 SSD2677 读取时钟不高于 2.5 MHz 的要求。
- 通过 `seeed_epaper_panel_get_model()` 获取实际型号。
- SSD1677 BUSY 高有效；SSD2677 低有效、超时 30 秒。
- SSD2677 局刷也会发送完整 96,000 字节的旧/新像素交织数据；小区域不代表小 SPI 传输。
- SSD2677 额外需要 48,000 字节上一帧和 96,000 字节四级灰度缓冲，优先分配 PSRAM。
- 灰度全刷后，应用调用 `seeed_epaper_panel_set_partial_baseline()` 同步两份黑白比较基准，避免未修改的灰色像素被后续局刷驱动。
- 连续局刷可保留模拟电源；书写空闲时调用 `seeed_epaper_panel_power_off()`，休眠时调用 `seeed_epaper_panel_sleep()`。
- 新控制器实现来自提供的 Seeed SSD2677 示例，保留其初始化/波形数据（包括控制器扫描分辨率寄存器）。需要两种实机验收，不可用主机测试替代屏幕效果测试。
