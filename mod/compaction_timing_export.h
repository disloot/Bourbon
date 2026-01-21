#ifndef LEVELDB_COMPACTION_TIMING_EXPORT_H
#define LEVELDB_COMPACTION_TIMING_EXPORT_H

#include <string>
#include <vector>
#include <cstdint>

namespace adgMod {

// Compaction 阶段计时数据结构
struct CompactionStageTiming {
    int stage_id;        // Timer ID (20-29)
    int level;           // Compaction level
    uint64_t duration_ns; // 持续时间（纳秒）
    int num_keys;        // 处理的 key 数量（仅部分阶段）
    int num_segments;    // PLR segment 数量（仅流式 PLR 阶段）

    CompactionStageTiming(int stage_id_, int level_, uint64_t duration_ns_,
                         int num_keys_ = 0, int num_segments_ = 0)
        : stage_id(stage_id_), level(level_), duration_ns(duration_ns_),
          num_keys(num_keys_), num_segments(num_segments_) {}
};

// 导出 compaction 计时数据到 CSV
// 输出格式：timestamp,level,file_number,num_keys,streaming_success,input_iteration_ns,output_builder_ns,streaming_plr_ns,...
void ExportCompactionTiming(const std::string& output_path);

// 获取计时数据摘要（用于 Python 脚本）
std::vector<CompactionStageTiming> GetCompactionTimingSummary();

// 获取特定 Timer 的累计时间（纳秒）
uint64_t GetTimerTime(uint32_t timer_id);

} // namespace adgMod

#endif // LEVELDB_COMPACTION_TIMING_EXPORT_H
