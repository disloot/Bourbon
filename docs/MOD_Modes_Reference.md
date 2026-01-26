# MOD Modes Reference

本文档详细说明了 Bourbon 系统中所有可用的 MOD（操作模式）配置及其功能差异。

## 快速参考表

| MOD | 名称 | Learned Index | WiscKey | Level Model | 流式 PLR | 推荐用途 |
|-----|------|--------------|---------|-------------|---------|---------|
| **0** | Original LevelDB | ❌ | ❌ | ❌ | ❌ | Baseline 对照 |
| **4** | 早期测试模式 | ⚠️ 部分 | ❌ | ❌ | ❌ | 早期测试 |
| **5** | 默认模式 | ❌ | ❌ | ❌ | ❌ | 默认初始化 |
| **6** | Learned Index Only | ✅ | ❌ | ✅ | ✅ | 测试 Learned Index |
| **7** | **Bourbon** | ✅ | ✅ | ✅ | ✅ | **主要实验模式** |
| **8** | WiscKey Only | ❌ | ✅ | ❌ | ❌ | 测试 WiscKey |
| **9** | Enhanced Learned Index | ✅ | ❌ | ✅ (增强) | ❌ | 实时 Level Learning |

## 详细说明

### MOD = 0: Original LevelDB

**Baseline 对照组** - 完全不使用任何优化技术。

**特点**:
- ❌ 不使用 Learned Index
- ❌ 不使用 WiscKey key-value 分离
- ❌ 不使用 Level Model
- ❌ 不使用流式 PLR

**实现**: 使用原始 LevelDB 的二分查找和 SSTable 结构。

**代码位置**:
- [db/version_set.cc:484](../db/version_set.cc#L484) - Baseline 查询路径
- [docs/CBA_Statistics_Collection_Detail.md:241](CBA_Statistics_Collection_Detail.md#L241)

**使用场景**: 性能对比的 baseline，用于评估 Learned Index 和 WiscKey 的改进效果。

---

### MOD = 4: 早期测试模式

**早期开发测试模式** - 用于特定搜索逻辑的测试。

**特点**:
- ⚠️ 部分支持 Learned Index（仅一处特殊逻辑）
- ❌ 不使用 WiscKey
- ❌ 不使用 Level Model
- ❌ 不使用流式 PLR

**代码位置**:
- [mod/learned_index.cpp:423-440](../mod/learned_index.cpp#L423) - `AccumulatedNumEntriesArray::Search()` 特殊搜索逻辑

**使用场景**: 早期开发测试，现已废弃。

---

### MOD = 5: 默认模式

**默认初始化值** - 用于系统初始化。

**特点**:
- ❌ 不启用任何优化功能
- 作为 `MOD` 变量的默认初始值

**代码位置**:
- [mod/util.cpp:13](../mod/util.cpp#L13) - `int MOD = 5;`

**使用场景**: 系统默认状态，需要通过命令行参数 `-m` 或 `--mod` 修改为实际使用的模式。

---

### MOD = 6: Learned Index Only

**仅 Learned Index** - 测试 Learned Index 性能（无 WiscKey 干扰）。

**特点**:
- ✅ **File Model**: 预测 key 在文件中的位置
- ✅ **Level Model**: 预测 key 所在的文件
- ✅ **离线批式学习**: `PrepareLearning()`
- ✅ **流式 PLR**: `StreamingPLRBuilder`（compaction 时实时训练）
- ✅ **自动回退**: 流式失败时回退到批式学习
- ❌ 不使用 WiscKey key-value 分离

**学习流程**:

1. **Compaction 阶段** (流式 PLR):
   ```cpp
   // db/db_impl.cc:974
   if (enable_streaming_plr && MOD == 6) {
       file_plr_builder = new StreamingPLRBuilder(...);
       // 每个 key: file_plr_builder->ProcessKey(key, position)
   }
   ```

2. **流式失败时回退**:
   ```cpp
   // db/db_impl.cc:1074-1083
   catch (const std::exception& e) {
       Log("Streaming PLR failed, falling back to batch learning.");
       env_->PrepareLearning(...);  // 批式学习
   }
   ```

3. **离线批式学习**:
   ```cpp
   // mod/read_cold.cc:412
   if (MOD == 6 && !enable_streaming_plr) {
       // 离线 level learning
       for (int i = 1; i < kNumLevels; ++i) {
           LearnedIndexData::LevelLearn(...);
       }
   }
   ```

**代码位置**:
- [db/db_impl.cc:974](../db/db_impl.cc#L974) - 流式 PLR 创建
- [db/db_impl.cc:1101](../db/db_impl.cc#L1101) - 批式学习路径
- [db/table_cache.cc:160](../db/table_cache.cc#L160) - Learned Index 查询
- [mod/read_cold.cc:412](../mod/read_cold.cc#L412) - 离线学习

**使用场景**: 隔离测试 Learned Index 的性能提升，排除 WiscKey 的影响。

---

### MOD = 7: Bourbon (推荐)

**主要实验模式** - 完整的 Learned Index + WiscKey 集成。

**特点**:
- ✅ **File Model**: 文件内位置预测
- ✅ **Level Model**: 跨文件预测
- ✅ **WiscKey**: Key-Value 分离（keys in SST, values in vLog）
- ✅ **离线批式学习**: 完整的 `PrepareLearning()` 流程
- ✅ **流式 PLR**: Compaction 时实时训练 PLR 模型
- ✅ **自动回退**: 流式失败时回退到批式学习
- ✅ **Cost-Benefit Analysis (CBA)**: 智能学习决策

**学习流程**:

与 MOD 6 相同，但额外支持 WiscKey key-value 分离：

1. **流式 PLR** ([db/db_impl.cc:974](../db/db_impl.cc#L974)):
   ```cpp
   if (enable_streaming_plr && (MOD == 7 || MOD == 6)) {
       file_plr_builder = new StreamingPLRBuilder(...);
   }
   ```

2. **WiscKey 集成**:
   - Keys 存储在 SSTable（用于 Learned Index 训练）
   - Values 存储在 vLog（减少写放大）
   - 通过 `WiscKey` 命名空间处理 value 指针

3. **查询路径** ([db/version_set.cc:397-484](../db/version_set.cc#L397)):
   ```cpp
   if (MOD == 0 || MOD == 8) {
       // Baseline: binary search
   } else {
       // Bourbon: use learned models
       if (level_learned) {
           // Level model predicts file
       }
       if (file_learned) {
           // File model predicts position
       }
   }
   ```

**代码位置**:
- [mod/util.h:58](../mod/util.h#L58) - 模式定义注释
- [db/db_impl.cc:974](../db/db_impl.cc#L974) - 流式 PLR
- [db/table_cache.cc:160](../db/table_cache.cc#L160) - Learned Index 查询
- [db/version_set.cc:484](../db/version_set.cc#L484) - Baseline 路径判断

**使用场景**:
- ✅ **主要实验模式**: 完整评估 Bourbon 性能
- ✅ **生产环境**: Learned Index + WiscKey 的最佳组合
- ✅ **性能对比**: 与 MOD 0/6/8 对比评估各技术贡献

---

### MOD = 8: WiscKey Only

**仅 WiscKey** - 测试 WiscKey key-value 分离性能（无 Learned Index）。

**特点**:
- ❌ 不使用 Learned Index（无 File/Level Model）
- ✅ **WiscKey**: Key-Value 分离
  - Keys: 存储在 SSTable
  - Values: 存储在 vLog（value log）
- ✅ **减少写放大**: Values 不参与 compaction
- ❌ 不使用流式 PLR
- ❌ 不使用离线批式学习

**实现**:

查询使用原始 LevelDB 二分查找：

```cpp
// db/version_set.cc:484
if (MOD == 0 || MOD == 8) {
    // Baseline path: binary search in SSTable
    s = vset_->table_cache_->Get(options, f->number, ...);
}
```

但 value 存储在 vLog（通过 WiscKey 指针访问）。

**代码位置**:
- [db/version_set.cc:484](../db/version_set.cc#L484) - Baseline 查询路径
- [mod/util.h:59](../mod/util.h#L59) - 模式定义

**使用场景**:
- 测试 WiscKey 单独的性能提升（写放大减少）
- 与 MOD 0 对比评估 WiscKey 贡献
- 与 MOD 7 对比评估 Learned Index 额外贡献

---

### MOD = 9: Enhanced Learned Index

**增强型 Learned Index** - 实时 Level Learning。

**特点**:
- ✅ **File Model**: 文件内位置预测
- ✅ **Level Model**: 跨文件预测（增强版）
- ✅ **实时 Level Learning**: Compaction 完成后立即重新训练 level model
- ✅ **离线批式学习**: `PrepareLearning()`
- ❌ **不使用流式 PLR**: 仅支持批式学习
- ❌ 不使用 WiscKey

**与 MOD 6/7 的区别**:

| 功能 | MOD 6/7 | MOD 9 |
|-----|---------|-------|
| File Model | ✅ | ✅ |
| Level Model | ✅ (离线训练) | ✅ (实时更新) |
| 流式 PLR | ✅ | ❌ |
| WiscKey | 6❌ / 7✅ | ❌ |
| 实时 Level Learning | ❌ | ✅ |

**额外功能**:

1. **Compaction 后实时更新 Level Model** ([db/db_impl.cc:859-866](../db/db_impl.cc#L859)):
   ```cpp
   if (MOD == 9 && !fresh_write && !enable_streaming_plr) {
       // 立即重新训练受影响的 level model
       int level = c->level();
       LearnedIndexData::LevelLearn(..., level, true);
       LearnedIndexData::LevelLearn(..., level+1, true);
   }
   ```

2. **查询时优先使用 Level Model** ([db/version_set.cc:397-404](../db/version_set.cc#L397)):
   ```cpp
   if (MOD == 9) {
       if (learned_this_level->Learned(...)) {
           // Use level model to get target file
           bounds = learned_this_level->GetPosition(user_key);
       }
   }
   ```

**批式学习路径** ([db/db_impl.cc:1101](../db/db_impl.cc#L1101)):
```cpp
} else if (!enable_streaming_plr && !fresh_write &&
           (MOD == 7 || MOD == 6 || MOD == 9)) {
    // 批式学习
    env_->PrepareLearning(...);
}
```

**代码位置**:
- [db/db_impl.cc:859](../db/db_impl.cc#L859) - 实时 Level Learning
- [db/version_set.cc:397](../db/version_set.cc#L397) - Level Model 查询
- [db/db_impl.cc:1101](../db/db_impl.cc#L1101) - 批式学习

**使用场景**:
- 测试实时 Level Learning 的效果
- 评估 compaction 后立即更新模型 vs 离线更新的差异

---

## 流式 PLR 支持矩阵

流式 PLR（Streaming PLR）在 compaction 过程中实时训练 Learned Index，无需存储所有 keys。

### 支持情况

| MOD | 流式 PLR | 批式学习 | 自动回退 | 说明 |
|-----|---------|---------|---------|------|
| 0 | ❌ | ❌ | - | Baseline |
| 4 | ❌ | ❌ | - | 早期测试 |
| 5 | ❌ | ❌ | - | 默认值 |
| 6 | ✅ | ✅ | ✅ | Learned Index Only |
| **7** | ✅ | ✅ | ✅ | **Bourbon (推荐)** |
| 8 | ❌ | ❌ | - | WiscKey Only |
| 9 | ❌ | ✅ | - | Enhanced Level Learning |

### 关键代码

**1. 流式 PLR 创建** ([db/db_impl.cc:974-981](../db/db_impl.cc#L974)):
```cpp
if (adgMod::enable_streaming_plr && (adgMod::MOD == 7 || adgMod::MOD == 6)) {
    compact->file_plr_builder = new StreamingPLRBuilder(
        adgMod::file_model_error,
        &compact->current_file_position
    );
}
```

**2. 流式处理循环** ([db/db_impl.cc:1184-1202](../db/db_impl.cc#L1184)):
```cpp
for (input->Valid(); ...) {
    builder->Add(key, value);  // 添加到 SSTable

    // 流式 PLR: 实时训练
    if (compact->file_plr_builder) {
        compact->file_plr_builder->ProcessKey(key, position);
    }
}
```

**3. 导出模型** ([db/db_impl.cc:1021-1067](../db/db_impl.cc#L1021)):
```cpp
Status DBImpl::FinishCompactionOutputFile(...) {
    if (compact->file_plr_builder) {
        // 导出 segments 到 LearnedIndexData
        auto segments = compact->file_plr_builder->GetSegments();
        meta->learned_index_data = new LearnedIndexData(segments);
    }
}
```

**4. 流式失败时回退** ([db/db_impl.cc:1074-1083](../db/db_impl.cc#L1074)):
```cpp
try {
    compact->file_plr_builder->ProcessKey(key, position);
} catch (const std::exception& e) {
    if (!adgMod::enable_streaming_plr) {  // ❌ 这里的逻辑需要修复
        Log("Streaming PLR failed, falling back to batch learning.");
        env_->PrepareLearning(...);  // 回退到批式学习
    } else {
        delete meta;
    }
}
```

**⚠️ 注意**: 流式失败时的回退逻辑有 bug，应该改为：
```cpp
if (adgMod::enable_streaming_plr) {  // 修复: 应该是 true
    // 回退到批式学习
    env_->PrepareLearning(...);
}
```

---

## 使用方法

### 命令行参数

在 `read_cold` 工具中通过 `-m` 或 `--mod` 参数指定：

```bash
cd third_party/Bourbon/build

# MOD = 7 (Bourbon - 推荐)
./read_cold -m 7 -u -w -l 3 -n 1000 -f dataset -d /tmp/db

# MOD = 0 (LevelDB Baseline)
./read_cold -m 0 -u -w -l 3 -n 1000 -f dataset -d /tmp/db

# MOD = 6 (Learned Index Only)
./read_cold -m 6 -u -w -l 3 -n 1000 -f dataset -d /tmp/db

# MOD = 8 (WiscKey Only)
./read_cold -m 8 -u -w -l 3 -n 1000 -f dataset -d /tmp/db
```

### 代码配置

也可以在代码中直接修改默认值：

**mod/util.cpp**:
```cpp
namespace adgMod {
    int MOD = 7;  // 修改默认值
}
```

### 流式 PLR 开关

独立于 MOD 的流式 PLR 开关：

**mod/util.cpp**:
```cpp
bool enable_streaming_plr = true;   // 启用流式 PLR（默认）
bool enable_streaming_plr = false;  // 禁用，使用批式学习
```

---

## 性能对比建议

### 实验设计

为了科学评估各技术的贡献，建议进行以下对比实验：

| 实验 | MOD | 目的 | 预期结果 |
|-----|-----|------|---------|
| Baseline | 0 | LevelDB 原始性能 | 基线 |
| WiscKey Only | 8 | 评估 WiscKey 贡献 | 写性能提升 |
| Learned Index Only | 6 | 评估 Learned Index 贡献 | 读性能提升 |
| Bourbon (流式) | 7 + streaming=true | 完整系统（流式 PLR） | 最佳性能 |
| Bourbon (批式) | 7 + streaming=false | 完整系统（批式 PLR） | 内存 vs 性能权衡 |
| Enhanced Learning | 9 | 评估实时 Level Learning | Level 更新频率影响 |

### 性能指标

- **读性能**: Learned Index 的主要优化目标
- **写性能**: WiscKey 的主要优化目标（减少写放大）
- **内存开销**:
  - 流式 PLR: ~3000x 内存节省（仅存储 segments）
  - 批式 PLR: 需要存储所有 keys
- **学习时间**:
  - 流式 PLR: Compaction 时实时学习（几乎无额外时间）
  - 批式 PLR: 离线学习（需要额外时间）

---

## 相关文档

- [CLAUDE.md](../CLAUDE.md) - Bourbon 架构和开发指南
- [CBA_Algorithm_Analysis.md](CBA_Algorithm_Analysis.md) - Cost-Benefit Analysis 算法详解
- [CBA_Statistics_Collection_Detail.md](CBA_Statistics_Collection_Detail.md) - 统计收集细节
- [mod/util.h](../mod/util.h#L56) - MOD 模式定义源码
- [mod/util.cpp](../mod/util.cpp#L13) - MOD 默认值

---

## 版本历史

- **2026-01-27**: 创建文档，整理所有 MOD 模式
- **Commit f2566a8**: 添加流式 PLR 功能（MOD 6/7）
- **Commit aa07b29**: 添加 MOD 9 实时 Level Learning

---

## 贡献

如有疑问或建议，请更新本文档或联系维护者。
