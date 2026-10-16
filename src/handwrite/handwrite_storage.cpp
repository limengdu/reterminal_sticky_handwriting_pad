#include "handwrite_storage.h"

#include "app_log.h"
#include "esp_check.h"
#include "esp_partition.h"

#include <cstring>

namespace {

constexpr char kTag[] = "handwrite_store";
constexpr char kPartitionLabel[] = "notes";
constexpr uint32_t kMagic = 0x4E574831U;  // "1HWN"
constexpr uint32_t kVersion = 2U;
constexpr int kMaxPages = 20;
constexpr size_t kSectorSize = 4096U;
// 800x480 at 2bpp is 96,000 bytes, rounded up to whole erase sectors.
// 800x480的2位页面为96000字节，向上取整到完整擦除扇区。
constexpr size_t kPageBytes = 800U * 480U / 4U;
constexpr size_t kSlotSize =
    (kPageBytes + kSectorSize - 1U) / kSectorSize * kSectorSize;
constexpr size_t kHeaderOffset = kSlotSize * kMaxPages;
// Flash is accessed through a small internal buffer because the cache (and
// with it PSRAM) is disabled while the flash chip is being written.
// 写Flash期间缓存和PSRAM不可用，因此通过一块小的内部RAM缓冲区中转。
constexpr size_t kChunkBytes = 1024U;

struct StoredHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t page_count;
    uint32_t current_page;
    uint32_t pen;  // color | size << 8 | style << 16
    uint32_t checksum;
};

const esp_partition_t *s_partition = nullptr;

uint32_t header_checksum(const StoredHeader &header)
{
    return header.magic ^ header.version ^ (header.page_count * 0x9E3779B1U) ^
           (header.current_page * 0x85EBCA77U) ^ (header.pen * 0xC2B2AE3DU);
}

// 96,000 is not a multiple of the chunk size, so the final chunk is shorter.
// 96000不是分块大小的整数倍，最后一块需要截短。
size_t chunk_length(size_t done)
{
    const size_t remaining = kPageBytes - done;
    return remaining < kChunkBytes ? remaining : kChunkBytes;
}

bool valid_page_args(int index, size_t page_bytes)
{
    return s_partition != nullptr && index >= 0 && index < kMaxPages &&
           page_bytes == kPageBytes;
}

}  // namespace

esp_err_t handwrite_storage_init()
{
    app_log_register_tag(kTag);
    s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    if (s_partition == nullptr) {
        STICKY_LOGE(kTag, "storage=init partition=%s result=not_found",
                    kPartitionLabel);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_partition->size < kHeaderOffset + kSectorSize) {
        STICKY_LOGE(kTag, "storage=init size=%u result=too_small",
                    static_cast<unsigned>(s_partition->size));
        s_partition = nullptr;
        return ESP_ERR_INVALID_SIZE;
    }
    STICKY_LOGI(kTag, "storage=ready partition=%s size=%u slot_bytes=%u",
                kPartitionLabel,
                static_cast<unsigned>(s_partition->size),
                static_cast<unsigned>(kSlotSize));
    return ESP_OK;
}

bool handwrite_storage_load_header(HandwriteStorageHeader &header)
{
    if (s_partition == nullptr) {
        return false;
    }
    StoredHeader stored = {};
    if (esp_partition_read(s_partition, kHeaderOffset, &stored, sizeof(stored)) !=
        ESP_OK) {
        return false;
    }
    if (stored.magic != kMagic || stored.version != kVersion ||
        stored.checksum != header_checksum(stored) || stored.page_count == 0U ||
        stored.page_count > static_cast<uint32_t>(kMaxPages) ||
        stored.current_page >= stored.page_count) {
        STICKY_LOGI(kTag, "storage=load_header result=empty");
        return false;
    }
    header.page_count = static_cast<int>(stored.page_count);
    header.current_page = static_cast<int>(stored.current_page);
    header.pen_color = static_cast<uint8_t>(stored.pen & 0xFFU);
    header.pen_size = static_cast<uint8_t>((stored.pen >> 8) & 0xFFU);
    header.pen_style = static_cast<uint8_t>((stored.pen >> 16) & 0xFFU);
    return true;
}

esp_err_t handwrite_storage_load_page(int index, uint8_t *page, size_t page_bytes)
{
    ESP_RETURN_ON_FALSE(valid_page_args(index, page_bytes) && page != nullptr,
                        ESP_ERR_INVALID_ARG, kTag, "invalid page");
    uint8_t chunk[kChunkBytes];
    const size_t slot_offset = static_cast<size_t>(index) * kSlotSize;
    for (size_t done = 0; done < kPageBytes; done += kChunkBytes) {
        const size_t count = chunk_length(done);
        ESP_RETURN_ON_ERROR(
            esp_partition_read(s_partition, slot_offset + done, chunk, count),
            kTag, "read page");
        memcpy(page + done, chunk, count);
    }
    return ESP_OK;
}

esp_err_t handwrite_storage_save_page(int index, const uint8_t *page, size_t page_bytes)
{
    ESP_RETURN_ON_FALSE(valid_page_args(index, page_bytes) && page != nullptr,
                        ESP_ERR_INVALID_ARG, kTag, "invalid page");
    const size_t slot_offset = static_cast<size_t>(index) * kSlotSize;
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_partition, slot_offset, kSlotSize),
                        kTag, "erase page");
    uint8_t chunk[kChunkBytes];
    for (size_t done = 0; done < kPageBytes; done += kChunkBytes) {
        const size_t count = chunk_length(done);
        memcpy(chunk, page + done, count);
        ESP_RETURN_ON_ERROR(
            esp_partition_write(s_partition, slot_offset + done, chunk, count),
            kTag, "write page");
    }
    return ESP_OK;
}

esp_err_t handwrite_storage_save_header(const HandwriteStorageHeader &header)
{
    ESP_RETURN_ON_FALSE(s_partition != nullptr, ESP_ERR_INVALID_STATE, kTag,
                        "storage not ready");
    StoredHeader stored = {};
    stored.magic = kMagic;
    stored.version = kVersion;
    stored.page_count = static_cast<uint32_t>(header.page_count);
    stored.current_page = static_cast<uint32_t>(header.current_page);
    stored.pen = header.pen_color | (static_cast<uint32_t>(header.pen_size) << 8) |
                 (static_cast<uint32_t>(header.pen_style) << 16);
    stored.checksum = header_checksum(stored);
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_partition, kHeaderOffset, kSectorSize),
                        kTag, "erase header");
    return esp_partition_write(s_partition, kHeaderOffset, &stored, sizeof(stored));
}
