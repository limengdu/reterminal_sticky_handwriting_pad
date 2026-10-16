#include "handwrite_history.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>

#include "app_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

namespace {

constexpr char kTag[] = "handwrite_hist";
constexpr int kMaxPages = 20;
constexpr size_t kMaxStepsPerPage = 50U;
constexpr size_t kMemoryBudgetBytes = 3U * 1024U * 1024U;
constexpr int kPageByteWidth = 800 / 4;
constexpr int kPageRows = 480;

// Flash layout of the "history" partition: two ping-pong index slots, then
// an append-only log of step records. A step is written once; undo/redo
// only moves it between stacks, which just changes the index.
// history分区布局：两个交替使用的索引槽，之后是只追加的步骤日志。每个步骤只写一次；
// 撤销/重做只是在栈之间移动步骤，只需更新索引。
constexpr char kPartitionLabel[] = "history";
constexpr uint32_t kIndexMagic = 0x58444948U;  // "HIDX"
constexpr uint32_t kStepMagic = 0x50455453U;   // "STEP"
constexpr uint32_t kFormatVersion = 1U;
constexpr size_t kSectorSize = 4096U;
constexpr size_t kIndexSlotSize = 32U * 1024U;
constexpr size_t kLogStart = 2U * kIndexSlotSize;
constexpr uint32_t kNotPersisted = UINT32_MAX;
// Flash writes disable the cache (and PSRAM), so data passes through an
// internal-RAM bounce buffer.
// 写Flash时缓存和PSRAM不可用，因此数据经由内部RAM中转缓冲区传输。
constexpr size_t kBounceBytes = 1024U;

struct IndexHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t generation;
    uint32_t log_end;
    uint32_t entry_count;
    uint32_t entries_checksum;
    uint32_t header_checksum;
};

struct IndexEntry {
    uint8_t page;
    uint8_t stack;  // 0 = undo, 1 = redo
    uint16_t reserved;
    uint32_t offset;
};

struct StepRecord {
    uint32_t magic;
    // Bumped whenever the log is rewritten from the start; records from an
    // older generation are stale even if their bytes still look valid.
    // 每次从头重写日志时递增；旧代次的记录即使字节看似有效也已过期。
    uint32_t generation;
    uint16_t byte_x;
    uint16_t row;
    uint16_t byte_width;
    uint16_t rows;
    uint8_t gray;
    uint8_t padding[3];
    uint32_t data_checksum;
};

constexpr size_t kMaxIndexEntries =
    (kIndexSlotSize - sizeof(IndexHeader)) / sizeof(IndexEntry);

struct Step {
    HistoryRegion region;
    // One PSRAM block: region bytes before the change, then after it.
    // 一整块PSRAM：先存修改前的区域字节，再存修改后的区域字节。
    uint8_t *data = nullptr;
    size_t region_bytes = 0;
    bool gray = false;
    // Offset of this step's record in the flash log, if already written.
    // 该步骤记录在Flash日志中的偏移；尚未写入时为kNotPersisted。
    uint32_t flash_offset = kNotPersisted;
};

struct PageHistory {
    std::deque<Step> undo;
    std::deque<Step> redo;
};

PageHistory s_pages[kMaxPages];
size_t s_used_bytes = 0;

const esp_partition_t *s_partition = nullptr;
uint8_t s_bounce[kBounceBytes];
// False until a log is loaded or rewritten; the next save then starts over.
// 加载或重写日志之前为false，此时下一次保存会从头开始写日志。
bool s_log_known = false;
uint32_t s_generation = 0;
uint32_t s_sequence = 0;
int s_index_slot = -1;
size_t s_log_end = 0;
size_t s_erased_until = 0;

bool valid_page(int page)
{
    return page >= 0 && page < kMaxPages;
}

void release(Step &step)
{
    heap_caps_free(step.data);
    s_used_bytes -= step.region_bytes * 2U;
    step.data = nullptr;
}

void clear_stack(std::deque<Step> &stack)
{
    for (Step &step : stack) {
        release(step);
    }
    stack.clear();
}

bool region_has_gray(const uint8_t *data, size_t bytes)
{
    // A packed pixel is gray (01 or 10) exactly when its two bits differ.
    // 打包像素的两位不同（01或10）时即为灰色。
    for (size_t i = 0; i < bytes; ++i) {
        if (((data[i] >> 1) ^ data[i]) & 0x55U) {
            return true;
        }
    }
    return false;
}

void copy_region_out(const HistoryRegion &region, const uint8_t *page,
                     size_t stride, uint8_t *out)
{
    for (int row = 0; row < region.rows; ++row) {
        std::memcpy(out + static_cast<size_t>(row) * region.byte_width,
                    page + static_cast<size_t>(region.row + row) * stride + region.byte_x,
                    region.byte_width);
    }
}

void copy_region_in(const HistoryRegion &region, const uint8_t *in,
                    size_t stride, uint8_t *page)
{
    for (int row = 0; row < region.rows; ++row) {
        std::memcpy(page + static_cast<size_t>(region.row + row) * stride + region.byte_x,
                    in + static_cast<size_t>(row) * region.byte_width,
                    region.byte_width);
    }
}

// Frees old steps until `needed` more bytes fit: first the oldest steps of
// the page being edited, then whole histories of other pages.
// 释放旧步骤直到能再容纳needed字节：先释放当前页最早的步骤，再整页清除其他页的历史。
void make_room(int page, size_t needed)
{
    PageHistory &current = s_pages[page];
    while (s_used_bytes + needed > kMemoryBudgetBytes && !current.undo.empty()) {
        release(current.undo.front());
        current.undo.pop_front();
    }
    for (int other = 0; other < kMaxPages && s_used_bytes + needed > kMemoryBudgetBytes;
         ++other) {
        if (other != page) {
            clear_stack(s_pages[other].redo);
            clear_stack(s_pages[other].undo);
        }
    }
}

// Moves the newest step from `from` to `to`, writing its before (undo) or
// after (redo) pixels into every buffer.
// 将from栈中最新的步骤移到to栈，并把其修改前（撤销）或修改后（重做）的像素写入每个缓冲区。
bool apply(std::deque<Step> &from, std::deque<Step> &to, bool use_before,
           uint8_t *const *buffers, int buffer_count, size_t stride, bool &touched_gray)
{
    if (from.empty()) {
        return false;
    }
    Step step = from.back();
    from.pop_back();
    const uint8_t *pixels = step.data + (use_before ? 0U : step.region_bytes);
    for (int i = 0; i < buffer_count; ++i) {
        copy_region_in(step.region, pixels, stride, buffers[i]);
    }
    touched_gray = step.gray;
    to.push_back(step);
    return true;
}

}  // namespace

bool handwrite_history_record(int page,
                              const HistoryRegion &region,
                              const uint8_t *before,
                              const uint8_t *after,
                              size_t stride)
{
    if (!valid_page(page) || region.byte_width <= 0 || region.rows <= 0) {
        return false;
    }
    PageHistory &history = s_pages[page];
    clear_stack(history.redo);

    Step step;
    step.region = region;
    step.region_bytes = static_cast<size_t>(region.byte_width) * region.rows;
    make_room(page, step.region_bytes * 2U);
    step.data = static_cast<uint8_t *>(heap_caps_malloc(
        step.region_bytes * 2U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (step.data == nullptr) {
        STICKY_LOGW(kTag, "history=record bytes=%u result=no_mem",
                    static_cast<unsigned>(step.region_bytes * 2U));
        return false;
    }
    s_used_bytes += step.region_bytes * 2U;
    copy_region_out(region, before, stride, step.data);
    copy_region_out(region, after, stride, step.data + step.region_bytes);
    step.gray = region_has_gray(step.data, step.region_bytes * 2U);

    if (history.undo.size() >= kMaxStepsPerPage) {
        release(history.undo.front());
        history.undo.pop_front();
    }
    history.undo.push_back(step);
    return true;
}

bool handwrite_history_undo(int page, uint8_t *const *buffers, int buffer_count,
                            size_t stride, bool &touched_gray)
{
    return valid_page(page) &&
           apply(s_pages[page].undo, s_pages[page].redo, true, buffers,
                 buffer_count, stride, touched_gray);
}

bool handwrite_history_redo(int page, uint8_t *const *buffers, int buffer_count,
                            size_t stride, bool &touched_gray)
{
    return valid_page(page) &&
           apply(s_pages[page].redo, s_pages[page].undo, false, buffers,
                 buffer_count, stride, touched_gray);
}

bool handwrite_history_can_undo(int page)
{
    return valid_page(page) && !s_pages[page].undo.empty();
}

bool handwrite_history_can_redo(int page)
{
    return valid_page(page) && !s_pages[page].redo.empty();
}

namespace {

uint32_t checksum(const void *data, size_t length, uint32_t hash = 2166136261U)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

size_t round_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

size_t log_capacity()
{
    return s_partition->size - kLogStart;
}

size_t record_size(const Step &step)
{
    return round_up(sizeof(StepRecord) + step.region_bytes * 2U, 4U);
}

esp_err_t find_partition()
{
    if (s_partition == nullptr) {
        s_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    }
    return s_partition != nullptr ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t write_bytes(size_t offset, const uint8_t *source, size_t length)
{
    for (size_t done = 0; done < length; done += kBounceBytes) {
        const size_t count = std::min(kBounceBytes, length - done);
        std::memcpy(s_bounce, source + done, count);
        ESP_RETURN_ON_ERROR(esp_partition_write(s_partition, offset + done, s_bounce, count),
                            kTag, "write");
    }
    return ESP_OK;
}

esp_err_t read_bytes(size_t offset, uint8_t *destination, size_t length)
{
    for (size_t done = 0; done < length; done += kBounceBytes) {
        const size_t count = std::min(kBounceBytes, length - done);
        ESP_RETURN_ON_ERROR(esp_partition_read(s_partition, offset + done, s_bounce, count),
                            kTag, "read");
        std::memcpy(destination + done, s_bounce, count);
    }
    return ESP_OK;
}

// Erases ahead of the write position in 64 KB steps, which the flash
// driver turns into fast block erases.
// 以64KB为单位提前擦除写入位置之后的区域，Flash驱动会将其转为较快的块擦除。
esp_err_t ensure_erased(size_t log_end)
{
    if (log_end <= s_erased_until) {
        return ESP_OK;
    }
    const size_t until = std::min(round_up(log_end, 64U * 1024U), log_capacity());
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_partition, kLogStart + s_erased_until,
                                                  until - s_erased_until),
                        kTag, "erase log");
    s_erased_until = until;
    return ESP_OK;
}

esp_err_t append_step(Step &step)
{
    const size_t size = record_size(step);
    ESP_RETURN_ON_FALSE(s_log_end + size <= log_capacity(), ESP_ERR_NO_MEM, kTag, "log full");
    ESP_RETURN_ON_ERROR(ensure_erased(s_log_end + size), kTag, "erase");

    StepRecord record = {};
    record.magic = kStepMagic;
    record.generation = s_generation;
    record.byte_x = static_cast<uint16_t>(step.region.byte_x);
    record.row = static_cast<uint16_t>(step.region.row);
    record.byte_width = static_cast<uint16_t>(step.region.byte_width);
    record.rows = static_cast<uint16_t>(step.region.rows);
    record.gray = step.gray ? 1U : 0U;
    record.data_checksum = checksum(step.data, step.region_bytes * 2U);
    ESP_RETURN_ON_ERROR(write_bytes(kLogStart + s_log_end,
                                    reinterpret_cast<const uint8_t *>(&record),
                                    sizeof(record)),
                        kTag, "write record");
    ESP_RETURN_ON_ERROR(write_bytes(kLogStart + s_log_end + sizeof(record), step.data,
                                    step.region_bytes * 2U),
                        kTag, "write step data");
    step.flash_offset = static_cast<uint32_t>(s_log_end);
    s_log_end += size;
    return ESP_OK;
}

template <typename Visitor>
void for_each_step(Visitor visit)
{
    for (int page = 0; page < kMaxPages; ++page) {
        for (Step &step : s_pages[page].undo) {
            visit(page, 0, step);
        }
        for (Step &step : s_pages[page].redo) {
            visit(page, 1, step);
        }
    }
}

esp_err_t write_index()
{
    const int slot = s_index_slot == 0 ? 1 : 0;
    const size_t base = static_cast<size_t>(slot) * kIndexSlotSize;
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_partition, base, kIndexSlotSize),
                        kTag, "erase index");

    // Entries are streamed through the bounce buffer; the header goes last
    // so a half-written slot never looks valid.
    // 索引项经中转缓冲区分批写入；头信息最后写入，写到一半的槽永远不会被当作有效。
    constexpr size_t kEntriesPerChunk = kBounceBytes / sizeof(IndexEntry);
    IndexEntry chunk[kEntriesPerChunk];
    size_t pending = 0;
    uint32_t count = 0;
    uint32_t entries_checksum = 2166136261U;
    esp_err_t result = ESP_OK;
    auto flush = [&]() {
        if (pending == 0 || result != ESP_OK) {
            return;
        }
        const size_t offset = base + sizeof(IndexHeader) +
                              (count - pending) * sizeof(IndexEntry);
        result = write_bytes(offset, reinterpret_cast<const uint8_t *>(chunk),
                             pending * sizeof(IndexEntry));
        pending = 0;
    };
    for_each_step([&](int page, int stack, Step &step) {
        if (count >= kMaxIndexEntries || step.flash_offset == kNotPersisted) {
            return;
        }
        IndexEntry &entry = chunk[pending++];
        entry = {};
        entry.page = static_cast<uint8_t>(page);
        entry.stack = static_cast<uint8_t>(stack);
        entry.offset = step.flash_offset;
        entries_checksum = checksum(&entry, sizeof(entry), entries_checksum);
        ++count;
        if (pending == kEntriesPerChunk) {
            flush();
        }
    });
    flush();
    ESP_RETURN_ON_ERROR(result, kTag, "write index entries");

    IndexHeader header = {};
    header.magic = kIndexMagic;
    header.version = kFormatVersion;
    header.sequence = s_sequence + 1U;
    header.generation = s_generation;
    header.log_end = static_cast<uint32_t>(s_log_end);
    header.entry_count = count;
    header.entries_checksum = entries_checksum;
    header.header_checksum = checksum(&header, offsetof(IndexHeader, header_checksum));
    ESP_RETURN_ON_ERROR(write_bytes(base, reinterpret_cast<const uint8_t *>(&header),
                                    sizeof(header)),
                        kTag, "write index header");
    s_sequence = header.sequence;
    s_index_slot = slot;
    return ESP_OK;
}

bool read_index_header(int slot, IndexHeader &header)
{
    const size_t base = static_cast<size_t>(slot) * kIndexSlotSize;
    if (read_bytes(base, reinterpret_cast<uint8_t *>(&header), sizeof(header)) != ESP_OK ||
        header.magic != kIndexMagic || header.version != kFormatVersion ||
        header.header_checksum != checksum(&header, offsetof(IndexHeader, header_checksum)) ||
        header.entry_count > kMaxIndexEntries) {
        return false;
    }
    uint32_t entries_checksum = 2166136261U;
    IndexEntry entry;
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        if (read_bytes(base + sizeof(IndexHeader) + i * sizeof(IndexEntry),
                       reinterpret_cast<uint8_t *>(&entry), sizeof(entry)) != ESP_OK) {
            return false;
        }
        entries_checksum = checksum(&entry, sizeof(entry), entries_checksum);
    }
    return entries_checksum == header.entries_checksum;
}

void clear_all()
{
    for (PageHistory &history : s_pages) {
        clear_stack(history.undo);
        clear_stack(history.redo);
    }
}

esp_err_t load_step(const IndexEntry &entry, Step &step)
{
    StepRecord record = {};
    ESP_RETURN_ON_FALSE(entry.offset + sizeof(record) <= log_capacity(), ESP_ERR_INVALID_SIZE,
                        kTag, "offset");
    ESP_RETURN_ON_ERROR(read_bytes(kLogStart + entry.offset,
                                   reinterpret_cast<uint8_t *>(&record), sizeof(record)),
                        kTag, "read record");
    ESP_RETURN_ON_FALSE(record.magic == kStepMagic && record.generation == s_generation &&
                            record.byte_width > 0 && record.rows > 0 &&
                            record.byte_x + record.byte_width <= kPageByteWidth &&
                            record.row + record.rows <= kPageRows,
                        ESP_ERR_INVALID_STATE, kTag, "stale record");
    step.region.byte_x = record.byte_x;
    step.region.row = record.row;
    step.region.byte_width = record.byte_width;
    step.region.rows = record.rows;
    step.region_bytes = static_cast<size_t>(record.byte_width) * record.rows;
    step.gray = record.gray != 0U;
    step.flash_offset = entry.offset;
    step.data = static_cast<uint8_t *>(heap_caps_malloc(
        step.region_bytes * 2U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(step.data != nullptr, ESP_ERR_NO_MEM, kTag, "alloc");
    s_used_bytes += step.region_bytes * 2U;
    esp_err_t result = read_bytes(kLogStart + entry.offset + sizeof(record), step.data,
                                  step.region_bytes * 2U);
    if (result == ESP_OK &&
        checksum(step.data, step.region_bytes * 2U) != record.data_checksum) {
        result = ESP_ERR_INVALID_CRC;
    }
    if (result != ESP_OK) {
        release(step);
    }
    return result;
}

}  // namespace

esp_err_t handwrite_history_save()
{
    ESP_RETURN_ON_ERROR(find_partition(), kTag, "partition");

    size_t needed = 0;
    for_each_step([&](int, int, Step &step) {
        if (step.flash_offset == kNotPersisted) {
            needed += record_size(step);
        }
    });
    // Rewrites the log from the start when it is unknown or would overflow;
    // live steps are all still in RAM, so nothing is lost.
    // 日志状态未知或即将写满时从头重写；所有有效步骤都还在内存中，不会丢失。
    const bool rewrite = !s_log_known || s_log_end + needed > log_capacity();
    if (rewrite) {
        ++s_generation;
        s_log_end = 0;
        s_erased_until = 0;
        for_each_step([](int, int, Step &step) { step.flash_offset = kNotPersisted; });
        s_log_known = true;
    }

    int appended = 0;
    esp_err_t result = ESP_OK;
    for_each_step([&](int, int, Step &step) {
        if (result == ESP_OK && step.flash_offset == kNotPersisted) {
            result = append_step(step);
            ++appended;
        }
    });
    if (result == ESP_OK) {
        result = write_index();
    }
    if (result != ESP_OK) {
        // The log position is uncertain now; start over on the next save.
        // 此时日志位置不确定，下次保存时从头开始。
        s_log_known = false;
    }
    STICKY_LOGI(kTag, "history=save appended=%d rewrite=%d log_kb=%u result=%s",
                appended, rewrite ? 1 : 0, static_cast<unsigned>(s_log_end / 1024U),
                esp_err_to_name(result));
    return result;
}

esp_err_t handwrite_history_load(int page_count)
{
    app_log_register_tag(kTag);
    ESP_RETURN_ON_ERROR(find_partition(), kTag, "partition");
    ESP_RETURN_ON_FALSE(s_partition->size > kLogStart, ESP_ERR_INVALID_SIZE, kTag, "size");

    IndexHeader headers[2];
    const bool valid[2] = {read_index_header(0, headers[0]),
                           read_index_header(1, headers[1])};
    if (!valid[0] && !valid[1]) {
        STICKY_LOGI(kTag, "history=load result=empty");
        return ESP_OK;
    }
    const int slot = !valid[1] || (valid[0] && headers[0].sequence > headers[1].sequence)
                         ? 0
                         : 1;
    const IndexHeader &header = headers[slot];
    s_index_slot = slot;
    s_sequence = header.sequence;
    s_generation = header.generation;
    if (page_count <= 0) {
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    const size_t base = static_cast<size_t>(slot) * kIndexSlotSize;
    for (uint32_t i = 0; i < header.entry_count && result == ESP_OK; ++i) {
        IndexEntry entry;
        result = read_bytes(base + sizeof(IndexHeader) + i * sizeof(IndexEntry),
                            reinterpret_cast<uint8_t *>(&entry), sizeof(entry));
        if (result != ESP_OK || entry.page >= page_count || entry.stack > 1U) {
            continue;
        }
        Step step;
        result = load_step(entry, step);
        if (result == ESP_OK) {
            PageHistory &history = s_pages[entry.page];
            (entry.stack == 0U ? history.undo : history.redo).push_back(step);
        }
    }
    if (result != ESP_OK) {
        // A damaged log only costs the undo history, never the notes.
        // 日志损坏只会丢失撤销历史，不会影响笔记内容。
        clear_all();
        STICKY_LOGW(kTag, "history=load result=%s action=discarded", esp_err_to_name(result));
        return result;
    }
    // Resume appending at a fresh sector: bytes after log_end may hold an
    // unindexed record from an interrupted save.
    // 从新的扇区继续追加：log_end之后可能残留上次中断保存时未进入索引的记录。
    s_log_end = round_up(header.log_end, kSectorSize);
    s_erased_until = s_log_end;
    s_log_known = s_log_end <= log_capacity();
    STICKY_LOGI(kTag, "history=load steps=%u log_kb=%u result=ok",
                static_cast<unsigned>(header.entry_count),
                static_cast<unsigned>(s_log_end / 1024U));
    return ESP_OK;
}
