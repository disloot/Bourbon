#ifndef LEVELDB_STREAMING_PLR_H
#define LEVELDB_STREAMING_PLR_H

#include "learned_index.h"  // learned_index.h 已经包含了 plr.h
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace adgMod {

// 流式 PLR 构建器，封装 GreedyPLR 用于 compaction 场景
class StreamingPLRBuilder {
private:
    GreedyPLR plr_;
    double gamma_;
    std::vector<Segment> segments_;
    uint64_t min_key_;
    uint64_t max_key_;
    int num_keys_;
    bool finished_;

public:
    explicit StreamingPLRBuilder(double gamma)
        : plr_(gamma), gamma_(gamma),
          min_key_(UINT64_MAX), max_key_(0), num_keys_(0), finished_(false) {}

    // 流式处理单个 key-value pair
    // 返回: true 如果创建了新 segment，false 否则
    bool ProcessKey(uint64_t key, int64_t position) {
        if (finished_) return false;
        // 更新 key 范围
        if (key < min_key_) min_key_ = key;
        if (key > max_key_) max_key_ = key;
        num_keys_++;

        // 转换为 point
        point pt{(double)key, (double)position};

        // 通过 GreedyPLR 处理
        Segment seg = plr_.process(pt, true);  // true = file model

        // 检查是否创建了新 segment
        if (seg.x != 0 || seg.k != 0 || seg.b != 0) {
            segments_.push_back(seg);
            return true;
        }
        return false;
    }

    // 完成学习，获取所有 segments
    const std::vector<Segment>& Finish() {
        if (finished_) return segments_;
        // 获取最后一个 segment
        Segment last = plr_.finish();
        if (last.x != 0 || last.k != 0 || last.b != 0) {
            segments_.push_back(last);
        }
        finished_ = true;
        return segments_;
    }

    // 导出 segments 到 LearnedIndexData
    void ExportToLearnedData(LearnedIndexData* data) {
        const auto& segs = Finish();
        if (!segs.empty()) {
            data->SetSegments(segs, min_key_, max_key_, num_keys_);
        }
    }

    // 获取统计信息
    int GetSegmentCount() const { return segments_.size(); }
    int GetKeyCount() const { return num_keys_; }
    uint64_t GetMinKey() const { return min_key_; }
    uint64_t GetMaxKey() const { return max_key_; }
};

} // namespace adgMod

#endif
