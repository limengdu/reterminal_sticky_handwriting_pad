#include "handwrite_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_log.h"
#include "board_power.h"
#include "canvas.h"
#include "esp_heap_caps.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "handwrite_brush.h"
#include "handwrite_history.h"
#include "handwrite_storage.h"
#include "sticky_button.h"
#include "sticky_display.h"
#include "sticky_touch.h"

namespace {

constexpr char kTag[] = "handwrite";
// Matches the desktop-pet home orientation: portrait, buttons on the right.
// 与桌宠主界面方向一致：竖屏，按键位于右侧。
constexpr CanvasRotation kRotation = CanvasRotation::Deg90CounterClockwise;
constexpr int kMaxPages = 20;
constexpr int kStatusBarHeight = 44;
constexpr int kInkTop = kStatusBarHeight + 2;
constexpr uint8_t kStatusTextScale = 2;
// The status bar keeps the top-left corner free for the sleep moon.
// 状态栏左上角预留给睡眠月亮图标。
constexpr int kMoonLeft = 10;
constexpr int kMoonTop = 5;
constexpr int kPageLabelX = 56;
constexpr TickType_t kLoopInterval = pdMS_TO_TICKS(10);
// Partial refreshes keep the panel's analog power on for speed; it is turned
// off after this long without new ink.
// 局刷为提速保留屏幕模拟电源；超过该时长没有新笔迹后关闭。
constexpr TickType_t kIdlePowerOffDelay = pdMS_TO_TICKS(2000);
// No touch and no button for this long sends the device into deep sleep.
// 超过该时长没有触摸和按键操作，设备进入深度睡眠。
constexpr TickType_t kIdleSleepDelay = pdMS_TO_TICKS(60000);
// Gray ink is previewed as a dither pattern while writing; after this pause
// one grayscale full refresh shows its real tone.
// 书写时灰色笔迹以抖动图案预览；停笔超过该时长后执行一次灰度全刷显示真实灰阶。
constexpr TickType_t kGraySettleDelay = pdMS_TO_TICKS(1500);

struct Rect {
    int x;
    int y;
    int width;
    int height;

    bool contains(int px, int py) const
    {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// Confirmation dialog geometry in logical portrait coordinates (480x800).
// 确认弹窗在竖屏逻辑坐标（480x800）中的位置。
constexpr Rect kDialog = {40, 300, 400, 220};
constexpr Rect kCancelButton = {kDialog.x + 26, kDialog.y + 140, 160, 56};
constexpr Rect kConfirmButton = {kDialog.x + kDialog.width - 26 - 160,
                                 kDialog.y + 140, 160, 56};

// Undo and redo buttons in the status bar; taps anywhere in the bar's
// height above a button count, which makes them easier to hit.
// 状态栏中的撤销、重做按钮；在按钮所在列的整个状态栏高度内点击都算命中，更容易点中。
constexpr Rect kUndoButton = {196, 5, 52, 34};
constexpr Rect kRedoButton = {256, 5, 52, 34};
constexpr Rect kUndoHitArea = {192, 0, 60, kStatusBarHeight};
constexpr Rect kRedoHitArea = {252, 0, 60, kStatusBarHeight};

// Current pen indicator in the status bar (display only).
// 状态栏中的当前画笔指示（仅显示）。
constexpr int kPenIndicatorX = 340;

// Floating toolbar docked at the bottom. Collapsed, only the top of a small
// rounded handle peeks above the screen edge; expanded, a rounded panel
// floats above the page with color, size and style rows.
// 停靠在底部的浮动工具栏：收起时只在屏幕下沿露出圆角把手的顶部；展开后圆角面板浮在页面上方，
// 包含颜色、粗细和笔触三行。
constexpr Rect kToolbarHandle = {180, 780, 120, 40};
constexpr int kToolbarHandleRadius = 14;
// The handle is small, so taps are accepted from a wider area around it.
// 把手较小，因此在其周围更大的范围内都接受点击。
constexpr Rect kHandleHitArea = {140, 752, 200, 48};
constexpr Rect kToolbar = {12, 476, 456, 312};
constexpr int kToolbarRadius = 20;
constexpr Rect kToolbarGripArea = {kToolbar.x + 148, kToolbar.y, 160, 22};
// Swipes start in this bottom strip; a stroke starting here never inks.
// 上滑手势从底部这一条区域开始；从这里起笔不会留下笔迹。
constexpr int kSwipeZoneTop = 752;
constexpr int kSwipeDistance = 50;
constexpr int kTapSlop = 24;
constexpr int kPanelCellX = 28;
constexpr int kPanelCellWidth = 98;
constexpr int kPanelCellPitch = 108;
constexpr int kColorRowY = kToolbar.y + 44;
constexpr int kSizeRowY = kToolbar.y + 132;
constexpr int kStyleRowY = kToolbar.y + 220;
constexpr int kColorRowHeight = 56;
constexpr int kSizeRowHeight = 56;
constexpr int kStyleRowHeight = 76;
constexpr GrayLevel kPenColors[kPenColorCount] = {
    GrayLevel::Black, GrayLevel::DarkGray, GrayLevel::LightGray, GrayLevel::White,
};

enum class Overlay : uint8_t {
    None,
    ClearDialog,
    Toolbar,
};

enum class Gesture : uint8_t {
    None,
    BottomSwipe,  // Started in the bottom strip: tap handle or swipe up / 底部起手：点把手或上滑
    ToolbarTouch, // Finger is on the expanded toolbar / 手指位于展开的工具栏上
    Consumed,     // Gesture already acted; ignore until release / 手势已生效，抬手前忽略
    UndoTap,      // Started on the undo button / 从撤销按钮开始
    RedoTap,      // Started on the redo button / 从重做按钮开始
};

uint8_t *s_pages[kMaxPages] = {};
bool s_page_dirty[kMaxPages] = {};
int s_page_count = 0;
int s_current_page = 0;
Overlay s_overlay = Overlay::None;
PenSettings s_pen;
bool s_gray_settle_pending = false;

BrushStroke s_stroke;
bool s_pen_down = false;
Gesture s_gesture = Gesture::None;
int s_gesture_start_x = 0;
int s_gesture_start_y = 0;
uint32_t s_stroke_samples = 0U;
uint32_t s_stroke_started_ms = 0U;

size_t page_bytes(const Canvas &canvas)
{
    return canvas.stride() * kStickyDisplayHeight;
}

int text_width(const char *text, uint8_t scale)
{
    return static_cast<int>(std::strlen(text)) *
           (kFontWidth + kFontSpacing) * scale;
}

void draw_centered_text(Canvas &canvas, int center_x, int y, const char *text,
                        uint8_t scale, GrayLevel color = GrayLevel::Black)
{
    canvas.draw_text(center_x - text_width(text, scale) / 2, y, text, scale, color);
}

void draw_frame(Canvas &canvas, const Rect &rect, int thickness)
{
    for (int inset = 0; inset < thickness; ++inset) {
        canvas.draw_rect(rect.x + inset, rect.y + inset,
                         rect.width - inset * 2, rect.height - inset * 2);
    }
}

// UI chrome only uses pure Black/White, approximating grays with fixed
// patterns, so menus and swatches never require a grayscale refresh.
// 界面元素只使用纯黑白，用固定图案模拟灰色，菜单和色块因此不需要灰度刷新。
bool pattern_is_black(GrayLevel level, int x, int y)
{
    switch (level) {
    case GrayLevel::Black:
        return true;
    case GrayLevel::DarkGray:
        return (x & 1) != 0 || (y & 1) != 0;
    case GrayLevel::LightGray:
        return ((x + y) & 1) != 0;
    case GrayLevel::White:
        return false;
    }
    return false;
}

void fill_pattern(Canvas &canvas, const Rect &rect, GrayLevel level)
{
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            canvas.draw_pixel(x, y, pattern_is_black(level, x, y) ? GrayLevel::Black
                                                                  : GrayLevel::White);
        }
    }
}

void fill_pattern_disc(Canvas &canvas, int center_x, int center_y, int radius,
                       GrayLevel level)
{
    const int limit = radius * radius + radius;
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            if (ox * ox + oy * oy <= limit) {
                const int x = center_x + ox;
                const int y = center_y + oy;
                canvas.draw_pixel(x, y, pattern_is_black(level, x, y)
                                            ? GrayLevel::Black
                                            : GrayLevel::White);
            }
        }
    }
    if (level == GrayLevel::White || level == GrayLevel::LightGray) {
        canvas.draw_circle(center_x, center_y, radius);
    }
}

bool is_gray(GrayLevel level)
{
    return level == GrayLevel::DarkGray || level == GrayLevel::LightGray;
}

bool canvas_has_gray(const Canvas &canvas)
{
    // A packed pixel is gray (01 or 10) exactly when its two bits differ.
    // 打包像素的两位不同（01或10）时即为灰色。
    const uint8_t *data = canvas.data();
    for (size_t index = 0; index < page_bytes(canvas); ++index) {
        if (((data[index] >> 1) ^ data[index]) & 0x55U) {
            return true;
        }
    }
    return false;
}

bool region_has_gray(const Canvas &canvas, const Rect &rect)
{
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            if (is_gray(canvas.pixel_at(x, y))) {
                return true;
            }
        }
    }
    return false;
}

void draw_pen_indicator(Canvas &canvas)
{
    const int center_y = kStatusBarHeight / 2;
    fill_pattern_disc(canvas, kPenIndicatorX + 10, center_y,
                      std::clamp(pen_radius(s_pen.size), 2, 10), s_pen.color);
    canvas.draw_text(kPenIndicatorX + 28, center_y - kFontHeight,
                     pen_style_label(s_pen.style), 2);
}

bool inside_round_rect(const Rect &rect, int radius, int x, int y)
{
    if (!rect.contains(x, y)) {
        return false;
    }
    // Clamp to the inner rectangle; only corner pixels need the circle test.
    // 先夹到内部矩形，只有四个角的像素需要做圆形判断。
    const int nearest_x = std::clamp(x, rect.x + radius, rect.x + rect.width - radius - 1);
    const int nearest_y = std::clamp(y, rect.y + radius, rect.y + rect.height - radius - 1);
    const int dx = x - nearest_x;
    const int dy = y - nearest_y;
    return dx * dx + dy * dy <= radius * radius;
}

// Fills a rounded rectangle with a black border; pixels outside the rounded
// corners are left untouched so the page shows through.
// 绘制带黑色边框的圆角矩形；圆角外的像素保持不变，让下方页面透出来。
void draw_round_panel(Canvas &canvas, const Rect &rect, int radius, int border)
{
    const Rect inner = {rect.x + border, rect.y + border,
                        rect.width - border * 2, rect.height - border * 2};
    const int inner_radius = std::max(radius - border, 0);
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            if (!inside_round_rect(rect, radius, x, y)) {
                continue;
            }
            canvas.draw_pixel(x, y, inside_round_rect(inner, inner_radius, x, y)
                                        ? GrayLevel::White
                                        : GrayLevel::Black);
        }
    }
}

void draw_grip(Canvas &canvas, int center_x, int y)
{
    canvas.fill_rect(center_x - 22, y, 44, 2, GrayLevel::Black);
    canvas.fill_rect(center_x - 23, y + 2, 46, 2, GrayLevel::Black);
}

// Only the top 20 px of the handle are on screen; the canvas clips the rest.
// 把手只有顶部20像素位于屏幕内，其余部分由画布自动裁掉。
void draw_toolbar_handle(Canvas &canvas)
{
    draw_round_panel(canvas, kToolbarHandle, kToolbarHandleRadius, 2);
    draw_grip(canvas, kToolbarHandle.x + kToolbarHandle.width / 2,
              kToolbarHandle.y + 8);
}

// Curved-arrow icon; redo is the mirror image of undo. Disabled buttons
// draw the icon as a checkerboard so they read as gray.
// 弧形箭头图标，重做为撤销的镜像；不可用时图标以棋盘格绘制，看起来呈灰色。
void draw_history_button(Canvas &canvas, const Rect &rect, bool redo, bool enabled)
{
    draw_round_panel(canvas, rect, 8, 2);
    const int center_x = rect.x + rect.width / 2;
    const int base_y = rect.y + rect.height / 2 + 3;
    auto ink = [&](int dx, int y) {
        const int x = center_x + (redo ? -dx : dx);
        if (enabled || ((x + y) & 1) == 0) {
            canvas.draw_pixel(x, y, GrayLevel::Black);
        }
    };
    // Upper half ring from the left end over to the right.
    // 从左端经顶部到右端的上半圆环。
    for (int dy = -10; dy <= 0; ++dy) {
        for (int dx = -10; dx <= 10; ++dx) {
            const int distance_sq = dx * dx + dy * dy;
            if (distance_sq >= 42 && distance_sq <= 90) {
                ink(dx + 3, base_y + dy);
            }
        }
    }
    // Arrowhead pointing down at the left end of the ring.
    // 圆环左端向下的箭头。
    for (int row = 0; row <= 6; ++row) {
        const int half = 5 - row * 5 / 6;
        for (int dx = -half; dx <= half; ++dx) {
            ink(-5 + dx, base_y - 1 + row);
        }
    }
}

void draw_status_bar(Canvas &canvas)
{
    canvas.fill_rect(0, 0, canvas.width(), kStatusBarHeight + 2,
                     GrayLevel::White);

    char page_label[24];
    std::snprintf(page_label, sizeof(page_label), "PAGE %d/%d",
                  s_current_page + 1, s_page_count);
    const int text_y = (kStatusBarHeight - kFontHeight * kStatusTextScale) / 2;
    canvas.draw_text(kPageLabelX, text_y, page_label, kStatusTextScale);
    draw_history_button(canvas, kUndoButton, false,
                        handwrite_history_can_undo(s_current_page));
    draw_history_button(canvas, kRedoButton, true,
                        handwrite_history_can_redo(s_current_page));
    draw_pen_indicator(canvas);

    canvas.fill_rect(0, kStatusBarHeight, canvas.width(), 2, GrayLevel::Black);
}

// Same crescent and sparkle as the reference firmware's sleep indicator,
// placed in the logical top-left corner.
// 与参考固件睡眠指示相同的月牙和星光，放在逻辑左上角。
void draw_sleep_moon(Canvas &canvas)
{
    const int left = kMoonLeft;
    const int top = kMoonTop;
    canvas.fill_rect(left, top, 35, 35, GrayLevel::White);
    canvas.fill_circle(left + 16, top + 17, 11, GrayLevel::Black);
    canvas.fill_circle(left + 21, top + 12, 10, GrayLevel::White);
    canvas.fill_rect(left + 28, top + 25, 3, 3, GrayLevel::Black);
    canvas.draw_pixel(left + 30, top + 23, GrayLevel::Black);
    canvas.draw_pixel(left + 30, top + 29, GrayLevel::Black);
    canvas.draw_pixel(left + 26, top + 27, GrayLevel::Black);
    canvas.draw_pixel(left + 34, top + 27, GrayLevel::Black);
}

void draw_text_button(Canvas &canvas, const Rect &rect, const char *label, bool filled)
{
    if (filled) {
        canvas.fill_rect(rect.x, rect.y, rect.width, rect.height, GrayLevel::Black);
    } else {
        canvas.fill_rect(rect.x, rect.y, rect.width, rect.height, GrayLevel::White);
        draw_frame(canvas, rect, 3);
    }
    draw_centered_text(canvas,
                       rect.x + rect.width / 2,
                       rect.y + (rect.height - kFontHeight * 2) / 2,
                       label,
                       2,
                       filled ? GrayLevel::White : GrayLevel::Black);
}

void draw_clear_dialog(Canvas &canvas)
{
    canvas.fill_rect(kDialog.x, kDialog.y, kDialog.width, kDialog.height,
                     GrayLevel::White);
    draw_frame(canvas, kDialog, 4);
    const int center_x = kDialog.x + kDialog.width / 2;
    draw_centered_text(canvas, center_x, kDialog.y + 36, "CLEAR THIS PAGE?", 3);
    draw_centered_text(canvas, center_x, kDialog.y + 90, "THIS CANNOT BE UNDONE", 2);
    draw_text_button(canvas, kCancelButton, "CANCEL", false);
    draw_text_button(canvas, kConfirmButton, "CLEAR", true);
}

Rect panel_cell(int row_y, int row_height, int index)
{
    return {kPanelCellX + index * kPanelCellPitch, row_y, kPanelCellWidth, row_height};
}

void draw_panel_cell_frame(Canvas &canvas, const Rect &cell, bool selected)
{
    canvas.fill_rect(cell.x, cell.y, cell.width, cell.height, GrayLevel::White);
    draw_frame(canvas, cell, selected ? 4 : 1);
}

// Draws a short wave with the real brush so each style previews itself.
// 用真实画笔绘制一段波浪线，让每种笔触直接预览自身效果。
void draw_style_sample(Canvas &canvas, const Rect &cell, PenStyle style)
{
    const BrushClip clip = {cell.x + 5, cell.y + 26, cell.x + cell.width - 5,
                            cell.y + cell.height - 5};
    PenSettings sample_pen;
    sample_pen.color = GrayLevel::Black;
    sample_pen.size = 2;
    sample_pen.style = style;

    constexpr int kPoints = 24;
    const int left = cell.x + 14;
    const int span = cell.width - 28;
    const int center_y = cell.y + 50;
    BrushStroke stroke;
    uint32_t time_ms = 0;
    for (int i = 0; i <= kPoints; ++i) {
        const float phase = static_cast<float>(i) / kPoints;
        const int x = left + static_cast<int>(span * phase);
        const int y = center_y + static_cast<int>(10.0F * std::sin(phase * 6.2832F));
        if (i == 0) {
            stroke.begin(canvas, sample_pen, clip, x, y, time_ms);
        } else {
            stroke.extend(x, y, time_ms);
        }
        // Slow first half, fast second half shows the brush's width change.
        // 前半段慢、后半段快，用来展示毛笔的粗细变化。
        time_ms += i < kPoints / 2 ? 30U : 3U;
    }
}

void draw_toolbar(Canvas &canvas)
{
    // The collapsed handle would peek out below the panel; the area under it
    // never holds ink, so it can simply be cleared.
    // 收起状态的把手会从面板下方露出；该区域不会有笔迹，可以直接擦白。
    canvas.fill_rect(kToolbarHandle.x, kToolbarHandle.y, kToolbarHandle.width,
                     kToolbarHandle.height, GrayLevel::White);
    draw_round_panel(canvas, kToolbar, kToolbarRadius, 3);
    draw_grip(canvas, kToolbar.x + kToolbar.width / 2, kToolbar.y + 10);

    canvas.draw_text(kPanelCellX, kColorRowY - 20, "COLOR", 2);
    for (int i = 0; i < kPenColorCount; ++i) {
        const Rect cell = panel_cell(kColorRowY, kColorRowHeight, i);
        draw_panel_cell_frame(canvas, cell, s_pen.color == kPenColors[i]);
        const Rect swatch = {cell.x + 9, cell.y + 9, cell.width - 18, cell.height - 18};
        fill_pattern(canvas, swatch, kPenColors[i]);
        if (kPenColors[i] == GrayLevel::White) {
            canvas.draw_rect(swatch.x, swatch.y, swatch.width, swatch.height);
            draw_centered_text(canvas, swatch.x + swatch.width / 2,
                               swatch.y + (swatch.height - kFontHeight) / 2,
                               "ERASER", 1);
        }
    }

    canvas.draw_text(kPanelCellX, kSizeRowY - 20, "SIZE", 2);
    for (int i = 0; i < kPenSizeCount; ++i) {
        const Rect cell = panel_cell(kSizeRowY, kSizeRowHeight, i);
        draw_panel_cell_frame(canvas, cell, s_pen.size == i);
        canvas.fill_circle(cell.x + cell.width / 2, cell.y + cell.height / 2,
                           pen_radius(static_cast<uint8_t>(i)));
    }

    canvas.draw_text(kPanelCellX, kStyleRowY - 20, "STYLE", 2);
    for (int i = 0; i < kPenStyleCount; ++i) {
        const PenStyle style = static_cast<PenStyle>(i);
        const Rect cell = panel_cell(kStyleRowY, kStyleRowHeight, i);
        draw_panel_cell_frame(canvas, cell, s_pen.style == style);
        draw_centered_text(canvas, cell.x + cell.width / 2, cell.y + 8,
                           pen_style_label(style), 2);
        draw_style_sample(canvas, cell, style);
    }
}

// Converts an inclusive logical rectangle into whole bytes of the physical
// page buffer, so history steps can be copied row by row.
// 将含边界的逻辑矩形转换为物理页面缓冲区中的整字节区域，便于历史步骤逐行复制。
HistoryRegion logical_region(const Canvas &canvas, int left, int top, int right, int bottom)
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    canvas.logical_to_physical(left, top, x0, y0);
    canvas.logical_to_physical(right, bottom, x1, y1);
    HistoryRegion region;
    region.byte_x = std::min(x0, x1) / 4;
    region.byte_width = std::max(x0, x1) / 4 + 1 - region.byte_x;
    region.row = std::min(y0, y1);
    region.rows = std::max(y0, y1) + 1 - region.row;
    return region;
}

// Copies a region from the canvas into the committed page buffer.
// 将画布中的一块区域同步到已提交的页面缓冲区。
void commit_region(const Canvas &canvas, const HistoryRegion &region)
{
    for (int row = region.row; row < region.row + region.rows; ++row) {
        const size_t offset = static_cast<size_t>(row) * canvas.stride() + region.byte_x;
        std::memcpy(s_pages[s_current_page] + offset, canvas.data() + offset,
                    region.byte_width);
    }
}

void save_current_page(const Canvas &canvas)
{
    std::memcpy(s_pages[s_current_page], canvas.data(), page_bytes(canvas));
}

void load_page(Canvas &canvas, int index)
{
    s_current_page = index;
    std::memcpy(canvas.data(), s_pages[index], page_bytes(canvas));
    draw_status_bar(canvas);
    draw_toolbar_handle(canvas);
    // Keeps the committed page identical to the canvas, chrome included, so
    // undo steps never restore pixels from before the handle was drawn.
    // 让已提交页面与画布（含界面元素）完全一致，撤销时不会恢复出没有把手的旧像素。
    save_current_page(canvas);
}

uint8_t *allocate_page(const Canvas &canvas)
{
    return static_cast<uint8_t *>(heap_caps_malloc(
        page_bytes(canvas), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

bool append_blank_page(const Canvas &canvas)
{
    if (s_page_count >= kMaxPages) {
        return false;
    }
    uint8_t *buffer = allocate_page(canvas);
    if (buffer == nullptr) {
        STICKY_LOGW(kTag, "page=allocate result=no_mem count=%d", s_page_count);
        return false;
    }
    // 0xFF packs four White (3) pixels per byte.
    // 0xFF表示一个字节中的四个像素全部为白色。
    std::memset(buffer, 0xFF, page_bytes(canvas));
    s_page_dirty[s_page_count] = true;
    s_pages[s_page_count++] = buffer;
    return true;
}

// Restores the notebook and pen saved before the last deep sleep, if any.
// 恢复上一次深度睡眠前保存的笔记和画笔设置（如果有）。
void restore_notebook(const Canvas &canvas)
{
    HandwriteStorageHeader header;
    if (!handwrite_storage_load_header(header)) {
        return;
    }
    for (int index = 0; index < header.page_count; ++index) {
        uint8_t *buffer = allocate_page(canvas);
        if (buffer == nullptr ||
            handwrite_storage_load_page(index, buffer, page_bytes(canvas)) != ESP_OK) {
            heap_caps_free(buffer);
            STICKY_LOGW(kTag, "notebook=restore page=%d result=failed", index + 1);
            break;
        }
        s_pages[s_page_count] = buffer;
        s_page_dirty[s_page_count] = false;
        ++s_page_count;
    }
    s_current_page = std::min(header.current_page, std::max(s_page_count - 1, 0));
    if (header.pen_color < kPenColorCount && header.pen_size < kPenSizeCount &&
        header.pen_style < kPenStyleCount) {
        s_pen.color = static_cast<GrayLevel>(header.pen_color);
        s_pen.size = header.pen_size;
        s_pen.style = static_cast<PenStyle>(header.pen_style);
    }
    STICKY_LOGI(kTag, "notebook=restore pages=%d current=%d",
                s_page_count, s_current_page + 1);
}

// Writes only pages changed since the last save, then the header. Returns
// false when the notebook could not be saved completely.
// 只写入上次保存后有改动的页面，最后写入头信息；未能完整保存时返回false。
bool persist_notebook(const Canvas &canvas)
{
    int saved = 0;
    for (int index = 0; index < s_page_count; ++index) {
        if (!s_page_dirty[index]) {
            continue;
        }
        const esp_err_t result =
            handwrite_storage_save_page(index, s_pages[index], page_bytes(canvas));
        if (result != ESP_OK) {
            STICKY_LOGE(kTag, "notebook=save page=%d result=%s",
                        index + 1, esp_err_to_name(result));
            return false;
        }
        s_page_dirty[index] = false;
        ++saved;
    }
    HandwriteStorageHeader header;
    header.page_count = s_page_count;
    header.current_page = s_current_page;
    header.pen_color = static_cast<uint8_t>(s_pen.color);
    header.pen_size = s_pen.size;
    header.pen_style = static_cast<uint8_t>(s_pen.style);
    const esp_err_t result = handwrite_storage_save_header(header);
    STICKY_LOGI(kTag, "notebook=save pages_written=%d total=%d result=%s",
                saved, s_page_count, esp_err_to_name(result));
    return result == ESP_OK;
}

// Uses the grayscale waveform only when the canvas holds gray ink; pure
// black-and-white pages keep the faster monochrome full refresh.
// 只有画布含灰色笔迹时才使用灰度波形；纯黑白页面继续使用更快的黑白全刷。
void refresh_full(Canvas &canvas, const char *reason)
{
    const bool gray = canvas_has_gray(canvas);
    const esp_err_t result =
        gray ? sticky_display_refresh() : sticky_display_refresh_monochrome();
    s_gray_settle_pending = false;
    STICKY_LOGI(kTag, "refresh=full reason=%s mode=%s result=%s",
                reason, gray ? "gray4" : "mono", esp_err_to_name(result));
}

void turn_page(Canvas &canvas, int delta)
{
    int target = s_current_page + delta;
    if (target < 0) {
        STICKY_LOGI(kTag, "page=turn result=already_first");
        return;
    }
    save_current_page(canvas);
    if (target >= s_page_count && !append_blank_page(canvas)) {
        STICKY_LOGI(kTag, "page=turn result=limit_reached max=%d", kMaxPages);
        return;
    }
    load_page(canvas, target);
    STICKY_LOGI(kTag, "page=turn current=%d count=%d",
                s_current_page + 1, s_page_count);
    refresh_full(canvas, "page_turn");
}

void open_clear_dialog(Canvas &canvas)
{
    save_current_page(canvas);
    draw_clear_dialog(canvas);
    s_overlay = Overlay::ClearDialog;
    STICKY_LOGI(kTag, "dialog=clear state=open page=%d", s_current_page + 1);
    // A full refresh both shows the dialog and cleans partial-refresh ghosting.
    // 全刷既显示弹窗，也顺带清理局刷残影。
    refresh_full(canvas, "clear_dialog");
}

void close_clear_dialog(Canvas &canvas, bool confirmed)
{
    s_overlay = Overlay::None;
    if (confirmed) {
        canvas.clear();
        draw_toolbar_handle(canvas);
        // The committed page still holds the content from before the dialog,
        // so clearing is recorded as one undoable step over the ink area.
        // 已提交页面仍保存着弹窗前的内容，因此清空被记录为书写区域上的一个可撤销步骤。
        const HistoryRegion ink_area = logical_region(
            canvas, 0, kInkTop, canvas.width() - 1, canvas.height() - 1);
        handwrite_history_record(s_current_page, ink_area, s_pages[s_current_page],
                                 canvas.data(), canvas.stride());
        draw_status_bar(canvas);
        save_current_page(canvas);
        s_page_dirty[s_current_page] = true;
    } else {
        load_page(canvas, s_current_page);
    }
    STICKY_LOGI(kTag, "dialog=clear state=closed result=%s page=%d",
                confirmed ? "cleared" : "cancelled", s_current_page + 1);
    refresh_full(canvas, confirmed ? "clear" : "clear_cancel");
}

void expand_toolbar(Canvas &canvas)
{
    save_current_page(canvas);
    draw_toolbar(canvas);
    s_overlay = Overlay::Toolbar;
    STICKY_LOGI(kTag, "toolbar=expand");
}

void collapse_toolbar(Canvas &canvas)
{
    s_overlay = Overlay::None;
    load_page(canvas, s_current_page);
    // The toolbar's partial refreshes overwrote any real gray under it.
    // 工具栏的局刷覆盖了其下方的真实灰阶，需要重新灰度刷新。
    if (region_has_gray(canvas, kToolbar)) {
        s_gray_settle_pending = true;
    }
    STICKY_LOGI(kTag, "toolbar=collapse color=%d size=%u style=%s",
                static_cast<int>(s_pen.color),
                static_cast<unsigned>(s_pen.size),
                pen_style_label(s_pen.style));
}

void close_overlay_for_sleep(Canvas &canvas)
{
    if (s_overlay != Overlay::None) {
        // An unanswered dialog counts as cancelled; the toolbar just collapses.
        // 未确认的弹窗按取消处理；工具栏直接收起。
        s_overlay = Overlay::None;
        load_page(canvas, s_current_page);
    } else {
        save_current_page(canvas);
    }
}

// Returns true when a cell was hit and the selection changed.
// 点中格子并改变选择时返回true。
bool select_toolbar_cell(Canvas &canvas, int x, int y)
{
    // Every row has four cells.
    // 每一行都有四个格子。
    static_assert(kPenColorCount == 4 && kPenSizeCount == 4 && kPenStyleCount == 4);
    for (int i = 0; i < kPenColorCount; ++i) {
        if (panel_cell(kColorRowY, kColorRowHeight, i).contains(x, y)) {
            s_pen.color = kPenColors[i];
        } else if (panel_cell(kSizeRowY, kSizeRowHeight, i).contains(x, y)) {
            s_pen.size = static_cast<uint8_t>(i);
        } else if (panel_cell(kStyleRowY, kStyleRowHeight, i).contains(x, y)) {
            s_pen.style = static_cast<PenStyle>(i);
        } else {
            continue;
        }
        draw_toolbar(canvas);
        return true;
    }
    return false;
}

// Expanded toolbar: swipe down on it, tap its grip, or tap outside to
// collapse; tap a cell to select. Returns true when a partial refresh is due.
// 工具栏展开时：在其上下滑、点顶部把手或点工具栏外即收起；点格子进行选择。需要局刷时返回true。
bool handle_toolbar_sample(Canvas &canvas, const StickyTouchSample &sample, int x, int y)
{
    switch (sample.phase) {
    case StickyTouchPhase::Down:
        s_gesture = Gesture::ToolbarTouch;
        s_gesture_start_x = x;
        s_gesture_start_y = y;
        return false;
    case StickyTouchPhase::Move:
        if (s_gesture == Gesture::ToolbarTouch &&
            kToolbar.contains(s_gesture_start_x, s_gesture_start_y) &&
            y - s_gesture_start_y >= kSwipeDistance) {
            s_gesture = Gesture::Consumed;
            collapse_toolbar(canvas);
            return true;
        }
        return false;
    case StickyTouchPhase::Up:
        break;
    }

    const Gesture gesture = s_gesture;
    s_gesture = Gesture::None;
    if (gesture != Gesture::ToolbarTouch ||
        std::abs(x - s_gesture_start_x) > kTapSlop ||
        std::abs(y - s_gesture_start_y) > kTapSlop) {
        return false;
    }
    if (!inside_round_rect(kToolbar, kToolbarRadius, x, y) ||
        kToolbarGripArea.contains(x, y)) {
        collapse_toolbar(canvas);
        return true;
    }
    return select_toolbar_cell(canvas, x, y);
}

// Redraws the status bar when the undo/redo availability changed.
// 撤销/重做的可用状态改变时重绘状态栏。
bool refresh_history_buttons(Canvas &canvas, bool could_undo, bool could_redo)
{
    if (could_undo == handwrite_history_can_undo(s_current_page) &&
        could_redo == handwrite_history_can_redo(s_current_page)) {
        return false;
    }
    draw_status_bar(canvas);
    return true;
}

// Returns true when the canvas changed and needs a partial refresh.
// 画布发生变化、需要局刷时返回true。
bool step_history(Canvas &canvas, bool redo)
{
    uint8_t *const buffers[] = {canvas.data(), s_pages[s_current_page]};
    bool touched_gray = false;
    const bool applied =
        redo ? handwrite_history_redo(s_current_page, buffers, 2, canvas.stride(),
                                      touched_gray)
             : handwrite_history_undo(s_current_page, buffers, 2, canvas.stride(),
                                      touched_gray);
    if (!applied) {
        return false;
    }
    s_page_dirty[s_current_page] = true;
    if (touched_gray) {
        s_gray_settle_pending = true;
    }
    draw_status_bar(canvas);
    STICKY_LOGI(kTag, "history=%s page=%d gray=%d", redo ? "redo" : "undo",
                s_current_page + 1, touched_gray ? 1 : 0);
    return true;
}

// Ends the active stroke and records it as one undo step. Returns true when
// the status bar changed and needs a partial refresh.
// 结束当前笔画并记录为一个撤销步骤；状态栏因此变化、需要局刷时返回true。
bool finish_stroke(Canvas &canvas, uint32_t ended_at_ms)
{
    if (!s_pen_down) {
        return false;
    }
    s_pen_down = false;
    if (s_stroke.touched_gray()) {
        s_gray_settle_pending = true;
    }
    bool status_changed = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    if (s_stroke.changed_bounds(left, top, right, bottom)) {
        const bool could_undo = handwrite_history_can_undo(s_current_page);
        const bool could_redo = handwrite_history_can_redo(s_current_page);
        // The committed page still holds the pixels from before this stroke.
        // 已提交的页面缓冲区仍保存着这一笔之前的像素。
        const HistoryRegion region = logical_region(canvas, left, top, right, bottom);
        handwrite_history_record(s_current_page, region, s_pages[s_current_page],
                                 canvas.data(), canvas.stride());
        commit_region(canvas, region);
        status_changed = refresh_history_buttons(canvas, could_undo, could_redo);
    }
    // One summary line per stroke keeps the console readable.
    // 每一笔只输出一行汇总，避免串口刷屏。
    STICKY_LOGI(kTag, "stroke=end samples=%u duration_ms=%u gray=%d",
                static_cast<unsigned>(s_stroke_samples),
                static_cast<unsigned>(ended_at_ms - s_stroke_started_ms),
                s_stroke.touched_gray() ? 1 : 0);
    return status_changed;
}

bool in_ink_area(const Canvas &canvas, int x, int y)
{
    return x >= 0 && x < canvas.width() && y >= kInkTop && y < canvas.height();
}

// Returns true when the canvas changed and needs a partial refresh.
// 画布发生变化、需要局刷时返回true。
bool handle_page_sample(Canvas &canvas, const StickyTouchSample &sample, int x, int y)
{
    // A touch that starts in the bottom strip is a toolbar gesture, never
    // ink: swiping up or tapping the handle expands the toolbar.
    // 从底部区域开始的触摸是工具栏手势，不会画线：上滑或点击把手即展开工具栏。
    if (sample.phase == StickyTouchPhase::Down && kUndoHitArea.contains(x, y)) {
        s_gesture = Gesture::UndoTap;
        return false;
    }
    if (sample.phase == StickyTouchPhase::Down && kRedoHitArea.contains(x, y)) {
        s_gesture = Gesture::RedoTap;
        return false;
    }
    if (s_gesture == Gesture::UndoTap || s_gesture == Gesture::RedoTap) {
        // Acts on release, and only if the finger is still on the button.
        // 抬手时才执行，并且手指必须仍在按钮上。
        if (sample.phase != StickyTouchPhase::Up) {
            return false;
        }
        const bool redo = s_gesture == Gesture::RedoTap;
        s_gesture = Gesture::None;
        if ((redo ? kRedoHitArea : kUndoHitArea).contains(x, y)) {
            return step_history(canvas, redo);
        }
        return false;
    }
    if (sample.phase == StickyTouchPhase::Down && y >= kSwipeZoneTop) {
        s_gesture = Gesture::BottomSwipe;
        s_gesture_start_x = x;
        s_gesture_start_y = y;
        return false;
    }
    if (s_gesture != Gesture::None) {
        const Gesture gesture = s_gesture;
        if (sample.phase == StickyTouchPhase::Up) {
            s_gesture = Gesture::None;
        }
        if (gesture != Gesture::BottomSwipe) {
            return false;
        }
        const bool swiped_up = s_gesture_start_y - y >= kSwipeDistance;
        const bool tapped_handle =
            sample.phase == StickyTouchPhase::Up &&
            kHandleHitArea.contains(s_gesture_start_x, s_gesture_start_y) &&
            std::abs(x - s_gesture_start_x) <= kTapSlop &&
            std::abs(y - s_gesture_start_y) <= kTapSlop;
        if (swiped_up || tapped_handle) {
            if (sample.phase != StickyTouchPhase::Up) {
                s_gesture = Gesture::Consumed;
            }
            expand_toolbar(canvas);
            return true;
        }
        return false;
    }

    if (sample.phase == StickyTouchPhase::Up) {
        return finish_stroke(canvas, sample.captured_at_ms);
    }
    if (!in_ink_area(canvas, x, y)) {
        // Lifting the pen at the status bar keeps strokes out of the header.
        // 进入状态栏区域时视为抬笔，避免笔迹画进页眉。
        return finish_stroke(canvas, sample.captured_at_ms);
    }

    if (s_pen_down) {
        s_stroke.extend(x, y, sample.captured_at_ms);
    } else {
        BrushClip clip = {0, kInkTop, canvas.width(), canvas.height()};
        clip.exclude_left = kToolbarHandle.x;
        clip.exclude_top = kToolbarHandle.y;
        clip.exclude_right = kToolbarHandle.x + kToolbarHandle.width;
        clip.exclude_bottom = kToolbarHandle.y + kToolbarHandle.height;
        s_stroke.begin(canvas, s_pen, clip, x, y, sample.captured_at_ms);
        s_pen_down = true;
        s_stroke_samples = 0U;
        s_stroke_started_ms = sample.captured_at_ms;
    }
    ++s_stroke_samples;
    s_page_dirty[s_current_page] = true;
    return true;
}

bool handle_touch_sample(Canvas &canvas, const StickyTouchSample &sample)
{
    int x = 0;
    int y = 0;
    canvas.physical_to_logical(sample.x, sample.y, x, y);

    switch (s_overlay) {
    case Overlay::ClearDialog:
        // Buttons act on release, so the finger can still slide off to abort.
        // 抬手时才确定按钮，手指滑出按钮即可放弃本次点击。
        if (sample.phase == StickyTouchPhase::Up) {
            if (kConfirmButton.contains(x, y)) {
                close_clear_dialog(canvas, true);
            } else if (kCancelButton.contains(x, y)) {
                close_clear_dialog(canvas, false);
            }
        }
        return false;
    case Overlay::Toolbar:
        return handle_toolbar_sample(canvas, sample, x, y);
    case Overlay::None:
        break;
    }
    return handle_page_sample(canvas, sample, x, y);
}

[[noreturn]] void enter_sleep(Canvas &canvas, const char *reason)
{
    STICKY_LOGI(kTag, "power=sleep reason=%s", reason);
    finish_stroke(canvas, s_stroke_started_ms);
    close_overlay_for_sleep(canvas);

    // The final frame stays on the e-paper throughout sleep, so a full
    // refresh shows the moon on a clean, ghost-free page.
    // 最后一帧会在睡眠期间一直保留，因此全刷让月亮图标显示在无残影的页面上。
    draw_sleep_moon(canvas);
    refresh_full(canvas, "sleep");
    // History steps describe edits to the saved pages, so they are only
    // written once the pages themselves are safely stored.
    // 历史步骤描述的是对已保存页面的修改，因此只在页面本身保存成功后才写入。
    if (persist_notebook(canvas)) {
        handwrite_history_save();
    }

    const esp_err_t touch_result = sticky_touch_stop();
    if (touch_result != ESP_OK) {
        STICKY_LOGE(kTag, "power=sleep peripheral=touch result=%s",
                    esp_err_to_name(touch_result));
    }
    const esp_err_t display_result = sticky_display_sleep();
    if (display_result != ESP_OK) {
        STICKY_LOGE(kTag, "power=sleep peripheral=display result=%s",
                    esp_err_to_name(display_result));
    }
    board_power_enter_deep_sleep();
}

void handle_button(Canvas &canvas, StickyButtonEvent event)
{
    // Any pending stroke ends (and is recorded) when the page context changes.
    // 页面切换或清空时结束并记录当前笔画。
    finish_stroke(canvas, s_stroke_started_ms);
    s_gesture = Gesture::None;
    const bool page_visible = s_overlay == Overlay::None;
    switch (event) {
    case StickyButtonEvent::PageUp:
        if (page_visible) {
            turn_page(canvas, -1);
        }
        break;
    case StickyButtonEvent::PageDown:
        if (page_visible) {
            turn_page(canvas, 1);
        }
        break;
    case StickyButtonEvent::AiClick:
        enter_sleep(canvas, "ai_button");
    case StickyButtonEvent::AiLongPress:
        if (page_visible) {
            open_clear_dialog(canvas);
        }
        break;
    case StickyButtonEvent::None:
        break;
    }
}

}  // namespace

[[noreturn]] void handwrite_app_run(Canvas &canvas)
{
    app_log_register_tag(kTag);
    canvas.set_rotation(kRotation);

    if (handwrite_storage_init() == ESP_OK) {
        restore_notebook(canvas);
    }
    // Undo history only makes sense on top of the restored pages.
    // 撤销历史只有建立在已恢复的页面之上才有意义。
    handwrite_history_load(s_page_count);
    if (s_page_count == 0 && !append_blank_page(canvas)) {
        STICKY_LOGE(kTag, "page=allocate_first result=failed");
    }
    load_page(canvas, s_current_page);
    refresh_full(canvas, "start");
    // Drops the wake press and anything pressed during boot.
    // 丢弃唤醒时的按键以及启动期间的按键事件。
    sticky_button_clear();
    STICKY_LOGI(kTag, "app=ready width=%u height=%u pages=%d max_pages=%d",
                static_cast<unsigned>(canvas.width()),
                static_cast<unsigned>(canvas.height()),
                s_page_count,
                kMaxPages);

    bool panel_powered = false;
    TickType_t last_ink_refresh = 0;
    TickType_t last_activity = xTaskGetTickCount();
    while (true) {
        StickyButtonEvent event = StickyButtonEvent::None;
        while (sticky_button_take_event(event)) {
            last_activity = xTaskGetTickCount();
            handle_button(canvas, event);
        }

        // Drain every buffered sample before refreshing, so points recorded
        // during the previous refresh appear together in the next one.
        // 刷新前一次性处理全部缓存采样，上一轮刷新期间写下的笔迹会合并显示。
        bool dirty = false;
        StickyTouchSample sample;
        while (sticky_touch_take_sample(sample)) {
            last_activity = xTaskGetTickCount();
            dirty |= handle_touch_sample(canvas, sample);
        }

        const TickType_t now = xTaskGetTickCount();
        const TickType_t idle = now - last_activity;
        if (dirty) {
            const esp_err_t result = sticky_display_refresh_partial();
            if (result != ESP_OK) {
                STICKY_LOGW(kTag, "refresh=partial result=%s",
                            esp_err_to_name(result));
            }
            panel_powered = true;
            last_ink_refresh = xTaskGetTickCount();
        } else if (s_gray_settle_pending && s_overlay == Overlay::None &&
                   !s_pen_down && idle >= kGraySettleDelay) {
            refresh_full(canvas, "gray_settle");
            panel_powered = false;
        } else if (panel_powered && now - last_ink_refresh >= kIdlePowerOffDelay) {
            const esp_err_t result = sticky_display_power_off();
            STICKY_LOGI(kTag, "panel=power_off reason=idle result=%s",
                        esp_err_to_name(result));
            panel_powered = false;
        } else if (idle >= kIdleSleepDelay) {
            enter_sleep(canvas, "idle_timeout");
        }
        vTaskDelay(kLoopInterval);
    }
}
