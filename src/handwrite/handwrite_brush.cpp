#include "handwrite_brush.h"

#include <algorithm>
#include <cstdlib>

namespace {

constexpr int kPenRadii[kPenSizeCount] = {1, 2, 4, 7};
// Pencil grain keeps roughly 70% of the tip pixels.
// 铅笔纹理大约保留笔头内70%的像素。
constexpr uint32_t kPencilDensity = 180U;  // out of 256

bool is_gray(GrayLevel level)
{
    return level == GrayLevel::DarkGray || level == GrayLevel::LightGray;
}

// A per-pixel hash keeps the grain fixed, so passing over the same spot
// again does not fill the texture in.
// 按像素位置计算的哈希让纹理固定不变，同一位置反复经过也不会被填实。
bool pencil_grain(int x, int y)
{
    uint32_t hash = static_cast<uint32_t>(x) * 73856093U ^
                    static_cast<uint32_t>(y) * 19349663U;
    hash ^= hash >> 13;
    hash *= 0x5BD1E995U;
    hash ^= hash >> 15;
    return (hash & 0xFFU) < kPencilDensity;
}

}  // namespace

int pen_radius(uint8_t size)
{
    return kPenRadii[std::min<int>(size, kPenSizeCount - 1)];
}

const char *pen_style_label(PenStyle style)
{
    switch (style) {
    case PenStyle::Round:
        return "ROUND";
    case PenStyle::Calligraphy:
        return "CALLI";
    case PenStyle::Pencil:
        return "PENCIL";
    case PenStyle::Brush:
        return "BRUSH";
    }
    return "?";
}

void BrushStroke::begin(Canvas &canvas, const PenSettings &pen,
                        const BrushClip &clip, int x, int y, uint32_t time_ms)
{
    canvas_ = &canvas;
    pen_ = pen;
    clip_ = clip;
    last_x_ = x;
    last_y_ = y;
    last_time_ms_ = time_ms;
    touched_gray_ = is_gray(pen.color);
    changed_ = false;
    // A brush stroke starts slim and swells as the finger slows down.
    // 毛笔起笔较细，手指放慢后逐渐变粗。
    brush_radius_q4_ = pen_radius(pen.size) * 16 * 6 / 10;
    stamp(x, y, pen.style == PenStyle::Brush ? (brush_radius_q4_ + 8) / 16
                                             : pen_radius(pen.size));
}

int BrushStroke::brush_target_radius(int distance, uint32_t elapsed_ms) const
{
    // Speed in pixels per 10 ms: slow writing ~1-3, fast flicks 10 and up.
    // 速度单位为像素/10毫秒：慢写约1到3，快速甩笔可达10以上。
    const int elapsed = static_cast<int>(std::max<uint32_t>(elapsed_ms, 1U));
    const int speed_x10 = distance * 100 / elapsed;
    // Scale factor in 1/16: 1.6x when nearly still, down to 0.35x when fast.
    // 缩放系数以1/16为单位：几乎静止时为1.6倍，很快时降到0.35倍。
    const int factor_q4 = std::clamp(26 - speed_x10 * 13 / 100, 6, 26);
    return pen_radius(pen_.size) * factor_q4;
}

void BrushStroke::extend(int x, int y, uint32_t time_ms)
{
    if (canvas_ == nullptr) {
        return;
    }
    const int dx = x - last_x_;
    const int dy = y - last_y_;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) {
        return;
    }

    const int fixed_radius = pen_radius(pen_.size);
    const int start_radius_q4 = brush_radius_q4_;
    int end_radius_q4 = start_radius_q4;
    if (pen_.style == PenStyle::Brush) {
        const int target_q4 = brush_target_radius(steps, time_ms - last_time_ms_);
        end_radius_q4 = (start_radius_q4 * 6 + target_q4 * 4) / 10;
    }

    for (int i = 1; i <= steps; ++i) {
        int radius = fixed_radius;
        if (pen_.style == PenStyle::Brush) {
            const int radius_q4 =
                start_radius_q4 + (end_radius_q4 - start_radius_q4) * i / steps;
            radius = std::max(1, (radius_q4 + 8) / 16);
        }
        stamp(last_x_ + dx * i / steps, last_y_ + dy * i / steps, radius);
    }

    brush_radius_q4_ = end_radius_q4;
    last_x_ = x;
    last_y_ = y;
    last_time_ms_ = time_ms;
}

void BrushStroke::stamp(int x, int y, int radius)
{
    if (pen_.style == PenStyle::Calligraphy) {
        // A two-pixel-thick nib along "/" gives thin strokes in that
        // direction and broad strokes across it.
        // 沿"/"方向、两像素厚的扁笔头：顺着该方向书写细，横穿该方向书写粗。
        const int half = radius + 1 + radius / 2;
        for (int i = -half; i <= half; ++i) {
            plot(x + i, y - i);
            plot(x + i + 1, y - i);
        }
        return;
    }

    const int limit = radius * radius + radius;
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            if (ox * ox + oy * oy <= limit) {
                plot(x + ox, y + oy);
            }
        }
    }
}

void BrushStroke::plot(int x, int y)
{
    if (x < clip_.left || x >= clip_.right || y < clip_.top || y >= clip_.bottom) {
        return;
    }
    if (x >= clip_.exclude_left && x < clip_.exclude_right &&
        y >= clip_.exclude_top && y < clip_.exclude_bottom) {
        return;
    }
    if (pen_.style == PenStyle::Pencil && !pencil_grain(x, y)) {
        return;
    }
    const GrayLevel previous = canvas_->pixel_at(x, y);
    if (previous == pen_.color) {
        return;
    }
    if (is_gray(previous)) {
        touched_gray_ = true;
    }
    canvas_->draw_pixel(x, y, pen_.color);
    if (!changed_) {
        changed_ = true;
        min_x_ = max_x_ = x;
        min_y_ = max_y_ = y;
    } else {
        min_x_ = std::min(min_x_, x);
        max_x_ = std::max(max_x_, x);
        min_y_ = std::min(min_y_, y);
        max_y_ = std::max(max_y_, y);
    }
}

bool BrushStroke::changed_bounds(int &left, int &top, int &right, int &bottom) const
{
    if (!changed_) {
        return false;
    }
    left = min_x_;
    top = min_y_;
    right = max_x_;
    bottom = max_y_;
    return true;
}
