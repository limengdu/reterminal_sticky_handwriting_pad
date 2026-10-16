#pragma once

#include <cstdint>

#include "canvas.h"

enum class PenStyle : uint8_t {
    Round,        // Uniform round tip / 均匀圆头笔
    Calligraphy,  // Flat 45-degree nib / 45度扁头书法笔
    Pencil,       // Grainy texture / 带颗粒纹理的铅笔
    Brush,        // Width follows writing speed / 粗细随书写速度变化的毛笔
};

constexpr int kPenStyleCount = 4;
constexpr int kPenSizeCount = 4;
constexpr int kPenColorCount = 4;

struct PenSettings {
    GrayLevel color = GrayLevel::Black;
    uint8_t size = 1;
    PenStyle style = PenStyle::Round;
};

// Tip radius in pixels for size index 0..3.
// 尺寸序号0到3对应的笔头半径（像素）。
int pen_radius(uint8_t size);

// Short ASCII label shown in the toolbar.
// 在工具栏中显示的简短ASCII名称。
const char *pen_style_label(PenStyle style);

// Inclusive-exclusive rectangle that a stroke may draw into, minus an
// optional excluded rectangle (left == right means none).
// 笔画允许绘制的矩形区域（左上包含、右下不包含），可再排除一个矩形（left等于right表示不排除）。
struct BrushClip {
    int left;
    int top;
    int right;
    int bottom;
    int exclude_left = 0;
    int exclude_top = 0;
    int exclude_right = 0;
    int exclude_bottom = 0;
};

// Renders one continuous stroke sample by sample.
// 按采样点逐段绘制一条连续笔画。
class BrushStroke {
public:
    void begin(Canvas &canvas, const PenSettings &pen, const BrushClip &clip,
               int x, int y, uint32_t time_ms);
    void extend(int x, int y, uint32_t time_ms);

    // True when the stroke used a gray ink or changed an existing gray pixel;
    // such changes need a grayscale refresh to show their real tone.
    // 笔画使用了灰色墨水或修改了已有灰色像素时为true，需要灰度刷新才能显示真实灰阶。
    bool touched_gray() const { return touched_gray_; }

    // Logical bounding box (inclusive) of pixels the stroke actually changed;
    // returns false when nothing changed.
    // 笔画实际修改过的像素的逻辑包围盒（含边界）；没有修改任何像素时返回false。
    bool changed_bounds(int &left, int &top, int &right, int &bottom) const;

private:
    void stamp(int x, int y, int radius);
    void plot(int x, int y);
    int brush_target_radius(int distance, uint32_t elapsed_ms) const;

    Canvas *canvas_ = nullptr;
    PenSettings pen_;
    BrushClip clip_ = {};
    int last_x_ = 0;
    int last_y_ = 0;
    uint32_t last_time_ms_ = 0;
    // Brush radius in 1/16 pixel so speed changes blend smoothly.
    // 毛笔半径以1/16像素为单位保存，让速度变化平滑过渡。
    int brush_radius_q4_ = 0;
    bool touched_gray_ = false;
    bool changed_ = false;
    int min_x_ = 0;
    int min_y_ = 0;
    int max_x_ = 0;
    int max_y_ = 0;
};
