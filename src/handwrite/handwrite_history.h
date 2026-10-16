#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// A region of the physical 2bpp page buffer, in whole bytes horizontally
// (four pixels per byte) and rows vertically.
// 物理2位页面缓冲区中的一块区域：水平方向以整字节（每字节四个像素）为单位，垂直方向以行为单位。
struct HistoryRegion {
    int byte_x = 0;
    int row = 0;
    int byte_width = 0;
    int rows = 0;
};

// Per-page undo/redo stacks. Each step stores only the changed region's
// pixels before and after, kept in PSRAM under a global memory budget.
// 每页独立的撤销/重做栈。每一步只保存变化区域修改前后的像素，存放在PSRAM中并受全局内存上限约束。

// Records one change: `before` holds the region's old pixels, `after` the
// new ones (both full page buffers). Clears the page's redo stack.
// 记录一次修改：before为该区域旧像素所在的整页缓冲区，after为新像素所在的整页缓冲区。会清空该页的重做栈。
bool handwrite_history_record(int page,
                              const HistoryRegion &region,
                              const uint8_t *before,
                              const uint8_t *after,
                              size_t stride);

// Applies the newest undo (or redo) step of `page` to every given buffer.
// `touched_gray` reports whether gray pixels were involved, which needs a
// grayscale refresh to look right.
// 将该页最新的撤销（或重做）步骤应用到给定的每个缓冲区；touched_gray表示是否涉及灰色像素，需要灰度刷新才能正确显示。
bool handwrite_history_undo(int page, uint8_t *const *buffers, int buffer_count,
                            size_t stride, bool &touched_gray);
bool handwrite_history_redo(int page, uint8_t *const *buffers, int buffer_count,
                            size_t stride, bool &touched_gray);

bool handwrite_history_can_undo(int page);
bool handwrite_history_can_redo(int page);

// Persists the history to the "history" flash partition: steps not yet in
// flash are appended to a log, then a small index of every page's undo and
// redo stacks is written. Call after the notebook pages are saved.
// 将历史保存到history分区：尚未写入的步骤追加到日志，再写入记录各页撤销/重做栈的小索引。
// 应在笔记页面保存之后调用。
esp_err_t handwrite_history_save();

// Restores the saved history. Pass the restored page count; with no pages
// (a fresh notebook) nothing is loaded and the next save starts a new log.
// 恢复已保存的历史。传入恢复出的页数；页数为0（全新笔记）时不加载任何内容，下次保存会重新开始日志。
esp_err_t handwrite_history_load(int page_count);
