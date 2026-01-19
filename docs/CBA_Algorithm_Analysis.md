# CBA (Cost vs. Benefit Analysis) 算法调研报告

## 一、算法概述

CBA 是 Bourbon 中用于**动态决策是否为某个文件学习模型**的核心算法。在混合工作负载场景下，LSM 会因为 compaction 导致文件频繁创建和删除，不是所有文件都值得学习模型。CBA 通过运行时统计信息预测学习收益，避免浪费计算资源。

**核心思想**：只有当学习模型带来的查找加速收益 > 学习成本时，才为该文件学习模型。

---

## 二、数据结构

### 2.1 核心类：`CBModel_Learn`

```cpp
class CBModel_Learn {
private:
    // 按层级和模型类型分类的查找时间统计
    std::vector<Counter> negative_lookups_time;  // [baseline, model]
    std::vector<Counter> positive_lookups_time;  // [baseline, model]

    // 文件级别的统计
    Counter num_negative_lookups_file;  // 负向查找次数
    Counter num_positive_lookups_file;  // 正向查找次数
    Counter file_sizes;                 // 文件大小总和

    // 学习成本统计（当前版本未使用）
    Counter learn_costs;
    Counter learn_sizes;

    // 常量阈值
    static const int const_size_to_cost = 10;  // 大小-成本转换系数
    static const int lookup_average_limit = 10000;  // 查找样本数下限
};
```

**关键成员变量说明**：
- `negative_lookups_time[2]`：索引0记录baseline（二分查找）的负向查找时间，索引1记录使用模型的负向查找时间
- `positive_lookups_time[2]`：索引0记录baseline的正向查找时间，索引1记录使用模型的正向查找时间
- `const_size_to_cost`：大小到成本的转换系数，作为学习决策的阈值

### 2.2 辅助类：`Counter`

```cpp
class Counter {
    std::vector<uint64_t> counts;  // 每层的累积值（如时间）
    std::vector<uint64_t> nums;    // 每层的计数
    void Increment(int level, uint64_t n);  // level 层增加 n
};
```

**设计特点**：
- 按层级（0-6）分别统计
- `counts` 记录累积值（如查找时间总和）
- `nums` 记录事件发生次数

### 2.3 文件统计：`FileStats`

```cpp
class FileStats {
    uint64_t start;        // 文件创建时间
    uint64_t end;          // 文件删除时间
    int level;             // 所在层级
    uint32_t num_lookup_pos;  // 正向查找次数
    uint32_t num_lookup_neg;  // 负向查找次数
    uint64_t size;         // 文件大小
};
```

**用途**：追踪每个文件从创建到删除的完整生命周期统计

---

## 三、算法流程

### 3.1 数据收集阶段

#### 时机 1：每次查找操作时

**代码位置**：`db/version_set.cc:505`

```cpp
#ifdef RECORD_LEVEL_INFO
adgMod::learn_cb_model->AddLookupData(
    level,                    // 层级
    saver.state == kFound,    // true=正向查找, false=负向查找
    file_learned,             // true=使用了模型, false=使用baseline
    temp.second - temp.first  // 查找耗时
);
#endif
```

**记录内容**：
- **正向查找**（找到键）：`positive_lookups_time[model].Increment(level, time)`
- **负向查找**（未找到）：`negative_lookups_time[model].Increment(level, time)`
- `model=0`: baseline（二分查找）
- `model=1`: 使用学习模型

**实现**：`CBMode_Learn.cpp:9-13`

```cpp
void CBModel_Learn::AddLookupData(int level, bool positive, bool model, uint64_t value) {
    leveldb::MutexLock guard(&lookup_mutex);
    std::vector<Counter>& target = positive ? positive_lookups_time : negative_lookups_time;
    target[model].Increment(level, value);
}
```

#### 时机 2：文件被删除时

**代码位置**：`db/db_impl.cc:298`

```cpp
if (file_stat.end - file_stat.start >= adgMod::learn_trigger_time) {
    adgMod::learn_cb_model->AddFileData(
        file_stat.level,
        file_stat.num_lookup_neg,  // 负向查找次数
        file_stat.num_lookup_pos,  // 正向查找次数
        file_stat.size             // 文件大小
    );
}
```

**记录内容**：
- 文件的生命周期查找统计
- 只有存活时间超过 `learn_trigger_time`（默认 50ms）的文件才会被记录
- 避免学习刚创建就死亡的文件

**实现**：`CBMode_Learn.cpp:15-20`

```cpp
void CBModel_Learn::AddFileData(int level, uint64_t num_negative, uint64_t num_positive, uint64_t size) {
    leveldb::MutexLock guard(&file_mutex);
    num_negative_lookups_file.Increment(level, num_negative);
    num_positive_lookups_file.Increment(level, num_positive);
    file_sizes.Increment(level, size);
}
```

#### 时机 3：文件创建时

**代码位置**：`db/db_impl.cc:564` 和 `db/db_impl.cc:998`

```cpp
// compaction 产生新文件时
adgMod::file_stats_mutex.Lock();
assert(adgMod::file_stats.find(meta.number) == adgMod::file_stats.end());
adgMod::file_stats.insert({meta.number, adgMod::FileStats(level, meta.file_size)});
adgMod::file_stats_mutex.Unlock();
```

---

### 3.2 核心计算：`CalculateCB()`

**代码位置**：`mod/CBMode_Learn.cpp:27-101`

#### 步骤 1: 提取统计数据

```cpp
int num_pos[2] = {0, 0}, num_neg[2] = {0, 0}, num_files = 0;
uint64_t time_pos[2] = {0, 0}, time_neg[2] = {0, 0};
uint64_t num_neg_lookups_file, num_pos_lookups_file, size_sum;

// model=0: baseline, model=1: learned model
for (int i = 0; i < 2; ++i) {
    num_pos[i] = positive_lookups_time[i].nums[level];
    num_neg[i] = negative_lookups_time[i].nums[level];
    time_pos[i] = positive_lookups_time[i].counts[level];
    time_neg[i] = negative_lookups_time[i].counts[level];
}

num_files = num_negative_lookups_file.nums[level];
num_neg_lookups_file = num_negative_lookups_file.counts[level];
num_pos_lookups_file = num_positive_lookups_file.counts[level];
size_sum = file_sizes.counts[level];
```

**数据说明**：
- `num_pos[0]`：baseline 正向查找次数
- `num_pos[1]`：模型正向查找次数
- `time_pos[0]`：baseline 正向查找总时间
- `time_pos[1]`：模型正向查找总时间
- 负向查找同理

#### 步骤 2: 前置检查

```cpp
// 检查 2.1: 样本文件数量不足
if (num_files < file_average_limit[level])  // L0:10, L1-L5:20
    return const_size_to_cost + 1;  // 拒绝学习

// 检查 2.2: 查找次数不足
for (int i = 0; i < 2; ++i) {
    if (num_pos[i] + num_neg[i] < lookup_average_limit)  // 10000
        return 0;  // 返回 0 表示不学习
}
```

**阈值定义** (`mod/CBMode_Learn.h:11`)：
```cpp
static const int file_average_limit[7] = {10, 20, 20, 20, 20, 500, 500};
```
- L0 层至少需要 10 个文件样本
- L1-L5 层至少需要 20 个文件样本
- L6 层至少需要 500 个文件样本（通常不会达到）

#### 步骤 3: 计算平均查找时间

```cpp
double average_pos_lookups = (double) num_pos_lookups_file / num_files;
double average_neg_lookups = (double) num_neg_lookups_file / num_files;
double average_pos_time[2] = {0, 0}, average_neg_time[2] = {0, 0};

for (int i = 0; i < 2; ++i) {
    // 正向查找时间（需要至少 500 次样本）
    if (num_pos[i] < 500) {
        average_pos_lookups = 0;
        average_pos_time[i] = 0;
    } else {
        average_pos_time[i] = (double) time_pos[i] / num_pos[i];
    }

    // 负向查找时间（需要至少 500 次样本）
    if (num_neg[i] < 500) {
        average_neg_lookups = 0;
        average_neg_time[i] = 0;
    } else {
        average_neg_time[i] = (double) time_neg[i] / num_neg[i];
    }
}
```

**设计考虑**：
- 分别计算 baseline 和 model 的平均查找时间
- 样本数 < 500 时忽略该类查找（避免统计噪声）
- 正向和负向查找分开计算（查找模式可能不同）

#### 步骤 4: 计算收益

```cpp
// 正向查找收益 = (baseline时间 - model时间) × 每文件平均正向查找次数
double pos_gain = (average_pos_time[0] - average_pos_time[1]) * average_pos_lookups;

// 负向查找收益 = (baseline时间 - model时间) × 每文件平均负向查找次数
double neg_gain = (average_neg_time[0] - average_neg_time[1]) * average_neg_lookups;

// 总收益
double total_gain = pos_gain + neg_gain;
```

**收益公式解释**：
- `average_pos_time[0] - average_pos_time[1]`：每次正向查找节省的时间
- 乘以 `average_pos_lookups`：每文件正向查找次数
- 结果 = 每文件因使用模型而节省的正向查找总时间
- 负向查找同理

#### 步骤 5: 归一化并返回

```cpp
// 收益 / 总大小 × 文件数 = 单位大小的收益
return (pos_gain + neg_gain) / size_sum * num_files;
```

**返回值含义**：
- 单位：时间/大小（类似"每KB节省的微秒数"）
- 乘以 `num_files`：放大因子，样本越多越可信
- 与 `const_size_to_cost = 10` 比较决定是否学习

**完整公式**：
```
score = [(T_baseline_pos - T_model_pos) × N_pos_per_file +
         (T_baseline_neg - T_model_neg) × N_neg_per_file] ×
        N_files / Total_size

决策: score > 10 ? 学习 : 不学习
```

---

### 3.3 学习决策

**代码位置**：`util/env_posix.cc:815-816`

```cpp
double score = adgMod::learn_cb_model->CalculateCB(level, meta->file_size);

// 如果分数 > 阈值，则加入学习优先队列
if (score > CBModel_Learn::const_size_to_cost) {  // score > 10
    learn_pq.push(std::make_pair(score, front));
}
```

**决策逻辑**：
- `score > 10`：值得学习，按分数降序排列（优先学习收益高的）
- `score ≤ 10`：不值得学习
- 使用优先队列确保高收益文件优先学习

---

## 四、关键参数

| 参数 | 默认值 | 作用 | 定义位置 |
|------|--------|------|----------|
| `const_size_to_cost` | 10 | 学习决策阈值 | `CBMode_Learn.h:36` |
| `lookup_average_limit` | 10000 | 总查找样本下限 | `CBMode_Learn.h:37` |
| `file_average_limit[7]` | [10,20,20,20,20,500,500] | 各层文件数下限 | `CBMode_Learn.h:11` |
| `learn_trigger_time` | 50000000 (50ms) | 文件最短存活时间 | `util.cpp:38` |
| 正向/负向查找样本下限 | 500 | 计算平均时间所需样本 | `CBMode_Learn.cpp:63,70` |
| `policy` | 0 | 测试策略开关 | `util.cpp:39` |

**层级差异说明**：
- **L0 层阈值低（10）**：L0 文件小且多，compaction 频繁，需要更少样本
- **L1-L5 阈值中等（20）**：标准层级
- **L6 阈值极高（500）**：最后一层通常不会有这么多文件，实际起到禁用作用

---

## 五、学习调度流程

### 5.1 整体架构

```
文件创建
    ↓
记录到 file_stats（开始计时）
    ↓
等待 50ms（learn_trigger_time）
    ↓
加入 learning_prepare 队列
    ↓
PrepareLearn 线程定期扫描
    ↓
调用 CalculateCB() 评估
    ↓
score > 10?
    ├─ 是 → 加入 learn_pq 优先队列 → 按分数排序 → 执行学习
    └─ 否 → 放弃学习
```

### 5.2 关键代码

**代码位置**：`util/env_posix.cc:786-841`

```cpp
void PrepareLearn() {
    adgMod::Stats* instance = adgMod::Stats::GetInstance();
    std::priority_queue<std::pair<double, LearnParam>> learn_pq;
    bool wait_for_time = false;
    int64_t time_diff = 1000000;
    prepare_queue_mutex.Lock();

    // 死循环
    while (true) {
        // 1. 等待队列非空
        while (learning_prepare.empty()) {
            preparing_queue_cv.Wait();
        }

        uint32_t dummy;
        uint64_t time_start = (__rdtscp(&dummy) - instance->initial_time) /
                              adgMod::reference_frequency;

        // 2. 处理队列中的文件
        while (!learning_prepare.empty()) {
            auto front = learning_prepare.front();
            int level = front.second.first;

            // 检查文件存活时间是否超过 learn_trigger_time (50ms)
            time_diff = front.first + adgMod::learn_trigger_time - time_start;
            if (time_diff > 0) {
                wait_for_time = true;
                break;  // 时间未到，跳出等待
            }

            learning_prepare.pop();

            // 3. CBA 评估
            double score = adgMod::learn_cb_model->CalculateCB(
                level, front.second.second->file_size);

            // 4. 决策：是否值得学习
            if (score > CBModel_Learn::const_size_to_cost) {
                learn_pq.push(std::make_pair(score, front));
            }
        }

        // 5. 按优先级学习（分数高的先学）
        while (!learn_pq.empty()) {
            auto& top = learn_pq.top();
            int level = top.second.second.first;
            FileMetaData* meta = top.second.second.second;
            adgMod::LearnedIndexData* model =
                adgMod::file_data->GetModel(meta->number);
            prepare_queue_mutex.Unlock();
            adgMod::LearnedIndexData::FileLearn(
                new adgMod::MetaAndSelf{nullptr, 0, meta, model, level});
            prepare_queue_mutex.Lock();
            learn_pq.pop();
        }

        // 6. 如果需要等待，睡眠一段时间
        if (wait_for_time) {
            prepare_queue_mutex.Unlock();
            SleepForMicroseconds((int)(time_diff / 1000));
            prepare_queue_mutex.Lock();
            wait_for_time = false;
        }
    }
}
```

### 5.3 调度线程启动

**代码位置**：`util/env_posix.cc:847-858`

```cpp
void PrepareLearning(uint64_t time_start, int level, FileMetaData* meta) {
    // 只在 Bourbon 模式下启用
    if (adgMod::fresh_write ||
        (adgMod::MOD != 6 && adgMod::MOD != 7 && adgMod::MOD != 9))
        return;

    MutexLock guard(&prepare_queue_mutex);

    // 懒加载：首次调用时启动线程
    if (!preparing_thread_started) {
        preparing_thread_started = true;
        std::thread background_thread(PosixEnv::PrepareLearnEntryPoint, this);
        background_thread.detach();
    }

    // 唤醒线程
    if (learning_prepare.empty()) preparing_queue_cv.Signal();
    learning_prepare.emplace(std::make_pair(
        time_start, std::make_pair(level, meta)));
}
```

---

## 六、算法特点与设计哲学

### 6.1 优点

1. **自适应性强**
   - 根据实际工作负载动态调整
   - 无需人工配置或调参
   - 自动适应不同的访问模式

2. **资源高效**
   - 避免为短命文件浪费学习资源
   - 50ms 等待机制过滤掉临时文件
   - 样本数检查避免统计噪声

3. **层次感知**
   - 针对不同层级使用不同的文件数阈值
   - L0 层阈值更低（10 vs 20），适应其高 churn 特性
   - 最后一层实际禁用（阈值500）

4. **统计驱动**
   - 基于真实性能数据，而非理论估算
   - 分离正向/负向查找，精确计算收益
   - Baseline vs Model 对比，直接测量加速效果

5. **优先级调度**
   - 使用优先队列，高收益文件优先学习
   - 最大化单位时间的性能提升

### 6.2 设计亮点

1. **等待机制**
   - `learn_trigger_time = 50ms` 避免学习即将被删除的文件
   - 在快速 compaction 环境中特别重要
   - 过滤掉生命周期短的临时文件

2. **多重样本过滤**
   - 500 次查找：计算平均时间的可靠性
   - 10000 次总查找：保证统计显著性
   - 文件数阈值（10/20）：避免单文件偏差

3. **分离统计**
   - 正向/负向查找分开：查找模式可能不同
   - Baseline/Model 分开：直接对比性能
   - 按层级统计：不同层特性不同

4. **归一化分数**
   - 除以文件大小：考虑学习成本与文件大小相关
   - 乘以文件数：样本越多，分数越高（可信度）

### 6.3 局限性

1. **冷启动问题**
   - 新系统没有历史统计
   - 所有文件都会被拒绝学习
   - 需要预热期积累样本

2. **时间敏感性**
   - 工作负载模式变化后，旧统计可能误导决策
   - 没有统计数据的时效性衰减机制
   - 可能需要定期重置计数器

3. **层级差异**
   - L0-L5 阈值不同，但 L6（最后一层）阈值极高（500文件）
   - 实际上禁用了 L6 的学习
   - 可能错失大文件的优化机会

4. **未使用的学习成本**
   - 代码中预留了 `learn_costs` 和 `learn_sizes`
   - 第 49-51 行和 100 行的注释显示原本计划使用实际学习成本
   - 当前使用固定阈值 10，未考虑实际学习开销

5. **模型假设**
   - 假设同一层内所有文件特性相似
   - 实际上文件大小、访问模式可能有差异
   - 粒度较粗（按层统计，不是按文件）

6. **内存开销**
   - 需要维护每个文件的统计信息
   - 文件数量大时（L0 可能有数百个）内存占用显著
   - `file_stats` map 的锁竞争可能成为瓶颈

---

## 七、代码执行流程示例

### 场景：新文件创建到学习决策

```cpp
// ========== 阶段 1: 文件创建 ==========
// db/db_impl.cc:564
FileMetaData* meta = new FileMetaData();
meta->number = 12345;
meta->file_size = 1024 * 1024;  // 1MB

// 记录文件统计
adgMod::file_stats.insert({
    12345,
    adgMod::FileStats(level=2, size=1048576)
});
// FileStats 构造函数自动记录创建时间（start）

// ========== 阶段 2: 查找操作 ==========
// db/version_set.cc:505 (每次查找时)
adgMod::learn_cb_model->AddLookupData(
    level=2,
    positive=true,
    file_learned=false,  // 还没有模型，使用 baseline
    value=150  // 耗时 150 微秒
);
// positive_lookups_time[0].Increment(2, 150)

// 多次查找后...
// positive_lookups_time[0].counts[2] = 500000
// positive_lookups_time[0].nums[2] = 5000

// ========== 阶段 3: 文件被调度学习 ==========
// util/env_posix.cc
env->PrepareLearning(current_time, level=2, meta);

// 加入队列
learning_prepare.push({
    current_time,
    {level=2, meta}
});

// ========== 阶段 4: 50ms 后评估 ==========
// util/env_posix.cc:815
double score = learn_cb_model->CalculateCB(level=2, file_size=1048576);

// 假设计算结果：
// average_pos_time[0] = 100  (baseline)
// average_pos_time[1] = 20   (model)
// average_pos_lookups = 1000
// average_neg_time[0] = 80
// average_neg_time[1] = 30
// average_neg_lookups = 500
// num_files = 25
// size_sum = 25 * 1048576

// pos_gain = (100 - 20) * 1000 = 80000
// neg_gain = (80 - 30) * 500 = 25000
// score = (80000 + 25000) / (25*1048576) * 25 = 105 / 1048576 ≈ 100.8

// 100.8 > 10 → 值得学习！

// ========== 阶段 5: 执行学习 ==========
// 加入优先队列
learn_pq.push({score=100.8, {...}});

// 按优先级执行学习
LearnedIndexData::FileLearn(...);
```

---

## 八、相关文件索引

| 文件路径 | 核心功能 | 关键代码行 |
|---------|---------|-----------|
| `mod/CBMode_Learn.h` | CBA 算法类定义 | 全文 |
| `mod/CBMode_Learn.cpp` | CBA 核心实现 | 27-101 (CalculateCB) |
| `mod/Counter.h` | 统计计数器定义 | 全文 |
| `mod/Counter.cpp` | 计数器实现 | 全文 |
| `mod/util.cpp` | 全局参数定义 | 29, 38-39 |
| `mod/util.h` | 全局变量声明 | 59 |
| `mod/learned_index.h` | LearnedIndexData 类 | 64-148 |
| `db/version_set.cc` | 查找数据收集 | 505 |
| `db/db_impl.cc` | 文件删除统计 | 298-302 |
| `db/db_impl.cc` | 文件创建统计 | 564-568, 996-1000 |
| `db/db_impl.cc` | CBA 初始化 | 1713 |
| `db/table_cache.cc` | 查找计时 | 175-420 (多处) |
| `util/env_posix.cc` | 学习调度 | 786-858 |
| `util/env_posix.cc` | CBA 决策 | 815-816 |

---

## 九、总结

CBA 算法通过**在线监控 + 离线评估 + 优先级调度**的三阶段设计，实现了在动态 LSM 环境下的智能学习决策。

### 核心公式

```
score = [(T_baseline_pos - T_model_pos) × N_pos_per_file +
         (T_baseline_neg - T_model_neg) × N_neg_per_file] ×
        N_files / Total_size

决策规则: score > 10 → 学习
         score ≤ 10 → 不学习
```

### 关键设计决策

1. **50ms 等待期**：过滤短命文件
2. **多重样本阈值**：保证统计可靠性
3. **按层统计**：适应层级差异
4. **优先级队列**：最大化收益
5. **分离正向/负向**：精确计算收益

### 适用场景

- ✅ 混合读写工作负载
- ✅ 高 compaction 频率环境
- ✅ 文件 churn 较高的场景
- ❌ 只读工作负载（无需 CBA）
- ❌ 静态数据集（直接离线学习）

这种设计使得 Bourbon 能够在混合工作负载下自适应地选择学习对象，在性能提升和计算成本之间取得平衡。
