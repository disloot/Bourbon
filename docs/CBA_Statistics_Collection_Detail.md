# CBA 算法：正向/负向查找统计机制详解

## 一、统计概述

CBA 算法需要两个维度的统计数据：
1. **查找次数**：每个文件的正向查找次数和负向查找次数
2. **查找时间**：baseline 和 model 的正向/负向查找时间

这些数据通过**两层统计机制**收集：
- **文件级别统计**（`file_stats`）：追踪每个文件的查找次数
- **层级级别统计**（`CBModel_Learn`）：聚合每个层的查找时间

---

## 二、时间测量机制

### 2.1 高精度时间戳：RDTSC 指令

Bourbon 使用 x86 的 `__rdtscp()` 和 `__rdtsc()` 指令进行纳秒级精度的时间测量：

```cpp
// mod/stats.cpp:19 - 系统初始化时记录基准时间
Stats::Stats() : timers(20, Timer{}), initial_time(__rdtsc()) {
    // ...
}

// mod/timer.cpp:18 - 开始计时时记录当前时间戳
void Timer::Start() {
    unsigned int dummy = 0;
    time_started = __rdtscp(&dummy);  // 读取 CPU 时间戳计数器
    started = true;
}

// mod/timer.cpp:25 - 暂停计时器并计算耗时
std::pair<uint64_t, uint64_t> Timer::Pause(bool record) {
    unsigned int dummy = 0;
    uint64_t time_elapse = __rdtscp(&dummy) - time_started;
    time_accumulated += time_elapse / reference_frequency;  // 转换为微秒
    // ...
}
```

**为什么使用 RDTSC？**
- **精度高**：CPU 周期级别，约 0.3ns（假设 3GHz CPU）
- **开销小**：单条指令，比系统调用快 100 倍以上
- **连续性好**：单调递增，不受时钟调整影响

**频率转换**：
```cpp
// mod/util.cpp:45
float reference_frequency = 2.6;  // CPU 频率（GHz）

// 时间（微秒）= CPU 周期数 / 频率
uint64_t time_us = rdtsc_cycles / reference_frequency;
```

---

## 三、查找次数统计（文件级别）

### 3.1 统计代码位置

**代码位置**：`db/version_set.cc:514-522, 529-537`

```cpp
switch (saver.state) {
    case kNotFound: {
        // 负向查找：键不在该文件中
#ifdef RECORD_LEVEL_INFO
        adgMod::levelled_counters[4].Increment(level, temp.second - temp.first);
        if (!adgMod::fresh_write) {
            adgMod::file_stats_mutex.Lock();
            auto iter = adgMod::file_stats.find(f->number);
            if (iter != adgMod::file_stats.end()) {
                iter->second.num_lookup_neg += 1;  // 负向查找次数 +1
            }
            adgMod::file_stats_mutex.Unlock();
        }
#endif
        break;  // 继续在其他文件中查找
    }
    case kFound: {
        // 正向查找：找到了键
#ifdef RECORD_LEVEL_INFO
        adgMod::levelled_counters[3].Increment(level, temp.second - temp.first);
        if (!adgMod::fresh_write) {
            adgMod::file_stats_mutex.Lock();
            auto iter = adgMod::file_stats.find(f->number);
            if (iter != adgMod::file_stats.end()) {
                iter->second.num_lookup_pos += 1;  // 正向查找次数 +1
            }
            adgMod::file_stats_mutex.Unlock();
        }
#endif
        return s;  // 查找成功，返回
    }
}
```

### 3.2 统计流程

```
用户请求 Get(key)
    ↓
遍历 LSM 层级 (Level 0 → 6)
    ↓
遍历每层的文件
    ↓
┌─────────────────────────────────────┐
│ 对每个文件执行查找                   │
│                                     │
│ 1. StartTimer(6) → 记录开始时间      │
│ 2. table_cache->Get(...)            │
│    - 读取文件                       │
│    - 在文件中查找 key               │
│ 3. PauseTimer(6, true) → 计算耗时    │
│    返回: {start_time, end_time}     │
│    耗时 = end_time - start_time     │
└─────────────────────────────────────┘
    ↓
判断查找结果：
    ├─ kNotFound → 该文件中没有此 key
    │   ├─ num_lookup_neg += 1
    │   └─ 继续查找下一个文件
    │
    └─ kFound → 找到了 key！
        ├─ num_lookup_pos += 1
        └─ 返回结果
```

### 3.3 关键细节

1. **LSM 的查找特性**：
   - 一个 key 可能存在于多个文件中（因为有旧版本）
   - 从 Level 0 开始逐层查找
   - 在每层内按文件顺序查找
   - 找到最新版本后立即返回

2. **正向/负向查找的定义**：
   - **正向查找**（`kFound`）：在该文件中找到了目标 key
   - **负向查找**（`kNotFound`）：该文件中没有目标 key（需要继续查找）

3. **示例场景**：

```
假设 key="user12345" 分布在以下文件中：
┌─────────┬─────────────────┬────────────┐
│  层级   │   文件          │  查找结果   │
├─────────┼─────────────────┼────────────┤
│  L0     │ file_100.sst    │ kNotFound  │ ← 负向查找
│  L0     │ file_101.sst    │ kNotFound  │ ← 负向查找
│  L1     │ file_200.sst    │ kFound!    │ ← 正向查找 ✓
└─────────┴─────────────────┴────────────┘

统计结果：
- file_100.num_lookup_neg += 1
- file_101.num_lookup_neg += 1
- file_200.num_lookup_pos += 1
```

---

## 四、查找时间统计（层级级别）

### 4.1 统计代码位置

**代码位置**：`db/version_set.cc:505`

```cpp
// 在每次文件查找后立即执行
#ifdef RECORD_LEVEL_INFO
adgMod::learn_cb_model->AddLookupData(
    level,                    // 层级 (0-6)
    saver.state == kFound,    // true=正向查找, false=负向查找
    file_learned,             // true=使用了模型, false=使用baseline
    temp.second - temp.first  // 查找耗时（微秒）
);
#endif
```

### 4.2 数据存储结构

```cpp
// mod/CBMode_Learn.h:18-21
class CBModel_Learn {
private:
    // [baseline, model] × [level 0-6]
    std::vector<Counter> negative_lookups_time;  // 索引0: baseline, 索引1: model
    std::vector<Counter> positive_lookups_time;  // 索引0: baseline, 索引1: model
};

// Counter 类存储：
// counts[level]  = 该层累积的总时间
// nums[level]    = 该层的查找次数
```

### 4.3 数据分类存储

每次查找后，数据被分类存入 4 个 Counter 中：

```
                    ┌─ positive_lookups_time[0][level]  (baseline 正向)
                    │
                    ├─ positive_lookups_time[1][level]  (model 正向)
                    │
saver.state + file_learned
                    ├─ negative_lookups_time[0][level]  (baseline 负向)
                    │
                    └─ negative_lookups_time[1][level]  (model 负向)
```

**示例**：

```cpp
// 场景：在 L2 层使用 model 进行正向查找，耗时 150 微秒
AddLookupData(
    level = 2,
    positive = true,      // 正向查找
    model = true,         // 使用了 model
    value = 150           // 耗时 150 微秒
);

// 内部执行：
// positive_lookups_time[1].Increment(2, 150);
//   → counts[2] += 150   (L2 层的正向查找时间累积)
//   → nums[2] += 1      (L2 层的正向查找次数累积)
```

### 4.4 时间测量代码详解

**Timer 6 的使用**（`db/version_set.cc:483-496`）：

```cpp
adgMod::LearnedIndexData *model = nullptr;
bool file_learned = false;

// === 开始计时 ===
instance->StartTimer(6);  // 记录开始时间戳
// __rdtscp(&dummy) - initial_time

if (adgMod::MOD == 0 || adgMod::MOD == 8) {
    // baseline 路径：不使用学习模型
    s = vset_->table_cache_->Get(...);
} else {
    // 学习模型路径
    s = vset_->table_cache_->Get(..., &model, &file_learned);
    // file_learned 会被设置为 true/false
}

// === 暂停计时 ===
auto temp = instance->PauseTimer(6, true);
// 返回 {start_absolute, end_absolute}
// 耗时 = end_absolute - start_absolute

// === 记录统计数据 ===
AddLookupData(level, saver.state == kFound, file_learned, temp.second - temp.first);
```

**Timer 内部实现**（`mod/timer.cpp:15-38`）：

```cpp
void Timer::Start() {
    unsigned int dummy = 0;
    time_started = __rdtscp(&dummy);  // 读取 CPU 时间戳计数器
    // 例如: time_started = 1234567890000
    started = true;
}

std::pair<uint64_t, uint64_t> Timer::Pause(bool record) {
    unsigned int dummy = 0;
    uint64_t time_elapse = __rdtscp(&dummy) - time_started;
    // 例如: time_elapse = 390 (CPU 周期)

    // 转换为微秒
    time_accumulated += time_elapse / reference_frequency;
    // 假设 reference_frequency = 2.6 GHz
    // time_accumulated += 390 / 2.6 ≈ 150 微秒

    if (record) {
        Stats* instance = Stats::GetInstance();
        uint64_t start_absolute = time_started - instance->initial_time;
        uint64_t end_absolute = start_absolute + time_elapse;
        started = false;
        return {start_absolute / reference_frequency,
                end_absolute / reference_frequency};
        // 返回: {47559884230, 47559884380}
        // 耗时: 47559884380 - 47559884230 = 150 微秒
    }
}
```

---

## 五、文件生命周期追踪

### 5.1 文件创建时

**代码位置**：`db/db_impl.cc:564-568`

```cpp
// Compaction 产生新文件
adgMod::file_stats_mutex.Lock();
assert(adgMod::file_stats.find(meta.number) == adgMod::file_stats.end());
adgMod::file_stats.insert({
    meta.number,
    adgMod::FileStats(level, meta.file_size)
});
adgMod::file_stats_mutex.Unlock();
```

**FileStats 构造函数**（`mod/util.h:123-127`）：

```cpp
explicit FileStats(int level_, uint64_t size_)
    : start(0), end(0), level(level_),
      num_lookup_pos(0), num_lookup_neg(0), size(size_) {

    adgMod::Stats* instance = adgMod::GetInstance();
    uint32_t dummy;
    // 记录文件创建时间（相对系统启动时间）
    start = (__rdtscp(&dummy) - instance->initial_time) / adgMod::reference_frequency;
}
```

### 5.2 文件删除时

**代码位置**：`db/db_impl.cc:298-302`

```cpp
adgMod::FileStats& file_stat = iter->second;
file_stat.Finish();  // 记录删除时间

// 只有存活时间超过 50ms 的文件才被统计
if (file_stat.end - file_stat.start >= adgMod::learn_trigger_time) {
    adgMod::learn_cb_model->AddFileData(
        file_stat.level,
        file_stat.num_lookup_neg,  // 累积的负向查找次数
        file_stat.num_lookup_pos,  // 累积的正向查找次数
        file_stat.size
    );
}
```

**Finish() 方法**（`mod/util.h:129-133`）：

```cpp
void Finish() {
    adgMod::Stats* instance = adgMod::GetInstance();
    uint32_t dummy;
    // 记录文件删除时间（相对系统启动时间）
    end = (__rdtscp(&dummy) - instance->initial_time) / adgMod::reference_frequency;
}
```

---

## 六、完整统计流程示例

### 场景：一次 Get 操作的完整统计

```cpp
// === 用户代码 ===
db->Get(read_options, "user12345", &value);

// === 内部执行流程 ===

// 1. 进入 Version::Get()
// db/version_set.cc:483
instance->StartTimer(6);  // 记录开始时间
// 假设: time_started = 1000000 (微秒)

// 2. 调用 table_cache->Get()
// db/table_cache.cc:175-420
// - 打开文件
// - 读取 block
// - 在 block 中查找 key
// - 使用 model 或 baseline

// 3. 返回查找结果
// saver.state = kFound  (找到了 key)
// file_learned = true   (使用了模型)

// 4. 暂停计时器
auto temp = instance->PauseTimer(6, true);
// 假设返回: {1000000, 1000150}
// 耗时 = 1000150 - 1000000 = 150 微秒

// 5. 记录统计数据
// db/version_set.cc:505
#ifdef RECORD_LEVEL_INFO
AddLookupData(
    level = 2,
    positive = true,
    model = true,
    value = 150
);
// 执行: positive_lookups_time[1].Increment(2, 150)
// 结果:
//   positive_lookups_time[1].counts[2] += 150
//   positive_lookups_time[1].nums[2] += 1
#endif

// db/version_set.cc:534
#ifdef RECORD_LEVEL_INFO
adgMod::file_stats_mutex.Lock();
auto iter = adgMod::file_stats.find(f->number);
iter->second.num_lookup_pos += 1;  // 文件级统计
adgMod::file_stats_mutex.Unlock();
#endif

// === 统计结果 ===

// 文件级统计（file_stats）:
file_200.num_lookup_pos = 1234  // 该文件被正向查找了 1234 次
file_200.num_lookup_neg = 567   // 该文件被负向查找了 567 次

// 层级级统计（learn_cb_model）:
positive_lookups_time[0].counts[2] = 150000  // baseline 正向查找总时间
positive_lookups_time[0].nums[2] = 1000      // baseline 正向查找次数
positive_lookups_time[1].counts[2] = 45000   // model 正向查找总时间
positive_lookups_time[1].nums[2] = 300       // model 正向查找次数

// 计算平均值：
// average_pos_time[0] = 150000 / 1000 = 150 微秒/次
// average_pos_time[1] = 45000 / 300 = 150 微秒/次
```

---

## 七、统计数据的使用

### 7.1 文件删除时的聚合

当文件被删除时（存活时间 ≥ 50ms），其累积的统计被聚合到层级级别：

```cpp
// db/db_impl.cc:299
learn_cb_model->AddFileData(
    level,
    file_stat.num_lookup_neg,  // 该文件累积的负向查找次数
    file_stat.num_lookup_pos,  // 该文件累积的正向查找次数
    file_stat.size
);

// CBMode_Learn.cpp:15
void CBModel_Learn::AddFileData(int level, uint64_t num_negative,
                                uint64_t num_positive, uint64_t size) {
    leveldb::MutexLock guard(&file_mutex);
    num_negative_lookups_file.Increment(level, num_negative);
    // num_negative_lookups_file.counts[level] += num_negative
    // num_negative_lookups_file.nums[level] += 1

    num_positive_lookups_file.Increment(level, num_positive);
    file_sizes.Increment(level, size);
}
```

### 7.2 CBA 计算时的使用

```cpp
// CBMode_Learn.cpp:44-47
// 获取该层的平均查找次数
double average_pos_lookups = (double) num_pos_lookups_file / num_files;
double average_neg_lookups = (double) num_neg_lookups_file / num_files;

// 计算收益
double pos_gain = (average_pos_time[0] - average_pos_time[1]) * average_pos_lookups;
double neg_gain = (average_neg_time[0] - average_neg_time[1]) * average_neg_lookups;
```

---

## 八、统计机制的特点

### 8.1 优点

1. **实时统计**：每次查找后立即更新，无延迟
2. **高精度计时**：RDTSC 提供纳秒级精度
3. **细粒度分类**：baseline vs model，正向 vs 负向，分别统计
4. **层级感知**：按层级聚合，适应 LSM 结构

### 8.2 开销

1. **锁竞争**：
   - `file_stats_mutex`：每次查找都要获取
   - `lookup_mutex`：`AddLookupData` 时的锁
   - 可能成为高并发场景的瓶颈

2. **内存开销**：
   - 每个文件一个 `FileStats` 结构
   - L0 可能有数百个文件
   - 估计开销：~100 bytes/file

3. **CPU 开销**：
   - RDTSC 指令本身：~10-20 CPU 周期
   - 相比查找操作（数千周期），开销可忽略

### 8.3 准确性考虑

1. **时间测量**：
   - RDTSC 可能受 CPU 频率动态调整影响
   - 使用 `__rdtscp` 而非 `__rdtsc` 避免乱序执行
   - `reference_frequency` 需要校准

2. **采样偏差**：
   - 只有存活时间 ≥ 50ms 的文件被统计
   - 可能低估短期文件的查找特性
   - 这是有意设计，避免为短命文件学习

3. **冷启动**：
   - 新文件没有 model，所有查找都是 baseline
   - 随着 model 被学习，后续查找会使用 model
   - 早期数据可能不代表稳定状态

---

## 九、总结

CBA 的统计机制通过**三层结构**收集数据：

```
┌─────────────────────────────────────────┐
│  Timer 6 (高精度计时)                    │
│  __rdtscp() → CPU 周期级时间戳           │
└─────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────┐
│  文件级统计 (file_stats)                │
│  num_lookup_pos/neg: 查找次数累加       │
│  start/end: 文件生命周期                │
└─────────────────────────────────────────┘
              ↓ (文件删除时聚合)
┌─────────────────────────────────────────┐
│  层级级统计 (CBModel_Learn)             │
│  positive/negative_lookups_time[2][7]  │
│  [baseline/model] × [level 0-6]        │
└─────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────┐
│  CBA 计算 (CalculateCB)                 │
│  评估学习收益，决定是否学习模型         │
└─────────────────────────────────────────┘
```

**核心设计**：
- **不估算**，完全基于实测数据
- **细粒度**，区分 baseline/model、正向/负向
- **自适应**，随工作负载变化动态调整
- **低开销**，RDTSC 计时 + 简单累加

这使得 CBA 能够准确捕捉 LSM 的动态特性，在混合工作负载下做出智能的学习决策。
