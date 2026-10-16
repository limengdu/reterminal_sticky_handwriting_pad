#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Notebook state saved alongside the pages.
// 与页面一起保存的笔记本状态。
struct HandwriteStorageHeader {
    int page_count = 0;
    int current_page = 0;
    uint8_t pen_color = 0;
    uint8_t pen_size = 1;
    uint8_t pen_style = 0;
};

// Locates the "notes" flash partition.
// 查找名为notes的Flash分区。
esp_err_t handwrite_storage_init();

// Reads the saved header; returns false when no valid notebook is stored.
// 读取已保存的头信息；没有有效笔记时返回false。
bool handwrite_storage_load_header(HandwriteStorageHeader &header);

// Pages are stored in the Canvas 2bpp layout so gray ink survives sleep.
// 页面按画布的2位格式原样保存，灰色笔迹在睡眠后也能保留。
esp_err_t handwrite_storage_load_page(int index, uint8_t *page, size_t page_bytes);
esp_err_t handwrite_storage_save_page(int index, const uint8_t *page, size_t page_bytes);

// Writes the header last so a notebook is only valid once its pages are saved.
// 头信息最后写入，确保页面全部保存后笔记才被视为有效。
esp_err_t handwrite_storage_save_header(const HandwriteStorageHeader &header);
