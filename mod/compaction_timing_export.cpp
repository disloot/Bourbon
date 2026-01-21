#include "compaction_timing_export.h"
#include "stats.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>

namespace adgMod {

// 累加的计时数据（跨多次 iteration）
static std::map<int, uint64_t> accumulated_timings;

void ExportCompactionTiming(const std::string& output_path) {
    Stats* instance = Stats::GetInstance();

    std::ofstream out(output_path);
    if (!out.is_open()) {
        std::cerr << "Failed to open " << output_path << " for writing" << std::endl;
        return;
    }

    // CSV header
    out << "timer_id,timer_name,duration_ns,duration_us,duration_ms\n";

    // Timer IDs 20-39
    const char* timer_names[] = {
        "BuilderFinish",      // 20
        "OutputBuilder",      // 21
        "StreamingPLR",       // 22
        "FileSyncClose",      // 26
        "StreamingPLRExport", // 27
        "BatchLearningFallback", // 28
        "TotalCompaction",    // 29
        "InputIteratorNext",  // 30
        "ReadBlockIO",        // 31
        "BlockDecompress",    // 32
        "DataBlockWrite",     // 33
        "FilterBlockWrite",   // 34
        "MetaIndexWrite",     // 35
        "IndexBlockWrite",    // 36
        "FooterWrite",        // 37
        "WriteBlockIO"        // 38
    };

    const int timer_ids[] = {20, 21, 22, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38};

    for (size_t i = 0; i < sizeof(timer_ids) / sizeof(timer_ids[0]); i++) {
        int timer_id = timer_ids[i];
        uint64_t current_duration = instance->ReportTime(timer_id);

        // 累加时间
        accumulated_timings[timer_id] += current_duration;
        uint64_t total_duration = accumulated_timings[timer_id];

        out << timer_id << ","
            << timer_names[i] << ","
            << total_duration << ","
            << (total_duration / 1000) << ","
            << (total_duration / 1000000) << "\n";
    }

    out.close();
    std::cout << "Compaction timing data exported to " << output_path << std::endl;
}

std::vector<CompactionStageTiming> GetCompactionTimingSummary() {
    Stats* instance = Stats::GetInstance();
    std::vector<CompactionStageTiming> timings;

    // 收集 Timer IDs 20-39 的数据
    const int timer_ids[] = {20, 21, 22, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38};

    for (int timer_id : timer_ids) {
        uint64_t duration_ns = accumulated_timings[timer_id];
        // level 设为 -1 表示全局汇总
        timings.emplace_back(timer_id, -1, duration_ns, 0, 0);
    }

    return timings;
}

uint64_t GetTimerTime(uint32_t timer_id) {
    return accumulated_timings[timer_id];
}

} // namespace adgMod
