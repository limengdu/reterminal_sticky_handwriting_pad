#pragma once

#include <cstddef>
#include <cstdint>

enum class GrayLevel : uint8_t {
    Black = 0,
    DarkGray = 1,
    LightGray = 2,
    White = 3,
};

enum class CanvasRotation : uint8_t {
    Deg0,
    Deg90Clockwise,
    Deg180,
    Deg90CounterClockwise,
};

class Canvas {
public:
    // Wraps a caller-owned 2-bit grayscale buffer as an 800x480 drawing surface.
    // 将调用方提供的2位灰度缓冲区包装成可绘图画布，画布本身不负责释放内存。
    Canvas(uint16_t width, uint16_t height, uint8_t *buffer, size_t buffer_size);

    uint16_t width() const;
    uint16_t height() const;
    size_t stride() const { return stride_; }
    uint8_t *data() { return buffer_; }
    const uint8_t *data() const { return buffer_; }

    // Rotates logical drawing coordinates while preserving the physical buffer.
    // 旋转逻辑绘图坐标，底层物理缓冲区保持不变。
    void set_rotation(CanvasRotation rotation) { rotation_ = rotation; }
    CanvasRotation rotation() const { return rotation_; }

    void clear(GrayLevel color = GrayLevel::White);

    // Drawing primitives clip coordinates at the canvas boundary.
    // 以下绘图函数会自动裁剪超出画布边界的坐标。
    GrayLevel pixel_at(int x, int y) const;
    void draw_pixel(int x, int y, GrayLevel color = GrayLevel::Black);
    void draw_line(int x0, int y0, int x1, int y1,
                   GrayLevel color = GrayLevel::Black);
    void draw_rect(int x, int y, int width, int height,
                   GrayLevel color = GrayLevel::Black);
    void fill_rect(int x, int y, int width, int height, GrayLevel color);
    void draw_circle(int center_x, int center_y, int radius,
                     GrayLevel color = GrayLevel::Black);
    void fill_circle(int center_x, int center_y, int radius,
                     GrayLevel color = GrayLevel::Black);
    void draw_text(int x, int y, const char *text, uint8_t scale = 1,
                   GrayLevel color = GrayLevel::Black);

    // Converts a physical touch coordinate into the current logical page axes.
    // 将物理触摸坐标转换为当前旋转页面使用的逻辑坐标。
    void physical_to_logical(int physical_x,
                             int physical_y,
                             int &logical_x,
                             int &logical_y) const;

    // Converts a logical coordinate into the physical buffer axes.
    // 将当前旋转下的逻辑坐标转换为物理缓冲区坐标。
    void logical_to_physical(int logical_x,
                             int logical_y,
                             int &physical_x,
                             int &physical_y) const;

private:
    uint16_t physical_width_;
    uint16_t physical_height_;
    size_t stride_;
    uint8_t *buffer_;
    size_t buffer_size_;
    CanvasRotation rotation_ = CanvasRotation::Deg0;
};
