# read_cold 使用说明

## 简介

`read_cold` 是 Bourbon 项目的 benchmark 工具，类似于 RocksDB 的 `db_bench`。它支持：

- **多种工作负载**：纯读、纯写、混合读写、YCSB 工作负载
- **灵活的数据加载**：顺序、逆序、随机、分块随机
- **完整的 LevelDB Options 配置**：支持 15+ 个性能关键参数
- **Learned Index 实验**：MOD=7 (Bourbon) 支持 PLR 模型训练和统计

## 快速开始

### 基本用法

```bash
cd third_party/Bourbon/build

# 加载数据库（fresh write）
./read_cold -m 7 -u -w -l 0 -n 1000 -f keys.txt -d /tmp/db

# 读取测试
./read_cold -m 7 -u -n 10000 -d /tmp/db

# 混合工作负载（5% 写入）
./read_cold -m 7 -u -mix 50 -n 100000 -d /tmp/db
```

## 命令行参数完整列表

### 基本参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-n, --get_number` | 操作数量（乘以 1024） | 1000 |
| `-i, --iteration` | 迭代次数 | 1 |
| `-m, --modification` | 运行模式：0=LevelDB, 6=Learned Index, 7=Bourbon, 8=WiscKey | 0 |
| `-d, --directory` | 数据库路径 | /mnt/ssd/testdb |
| `-h, --help` | 显示帮助信息 | - |
| `-f, --input_file` | 输入文件路径 | "" |
| `-l, --load_type` | 加载类型：0=顺序, 1=逆序, 3=随机 | 0 |

### 数据大小参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-k, --key_size` | Key 大小（字节） | 16 |
| `-v, --value_size` | Value 大小（字节） | 8 |

### LevelDB Options 配置 ⭐ 新增

#### 性能关键参数

| 参数 | 说明 | 默认值 | 示例 |
|------|------|--------|------|
| `-W, --write_buffer_size` | MemTable 大小 | 4MB | `-W 16M`, `-W 32M` |
| `-C, --cache_size` | 块缓存大小 | 8MB | `-C 64M`, `-C 256M` |
| `-F, --max_open_files` | 最大打开文件数 | 65536 | `-F 100000` |
| `-B, --block_size` | SSTable 块大小 | 4KB | `-B 8K`, `-B 16K` |
| `--max_file_size` | 单个 SSTable 文件大小限制 | 2MB | `--max_file_size 16M` |

**大小格式支持**：支持 `K`/`M`/`G` 后缀（不区分大小写）
- `4K` = 4096 bytes
- `16M` = 16 * 1024 * 1024 bytes
- `1G` = 1024 * 1024 * 1024 bytes
- 直接输入数字表示字节数（如 `4194304`）

**默认值说明**：设置为 `0` 或不指定则使用 LevelDB 默认值

#### 过滤器和压缩

| 参数 | 说明 | 默认值 | 示例 |
|------|------|--------|------|
| `--bloom_bits` | Bloom filter 位数（-1=默认，0=禁用） | -1 | `--bloom_bits 16` |
| `--compression` | 压缩类型：none, snappy | none | `--compression snappy` |

**Bloom filter 调优建议**：
- `--bloom_bits 6`：弱过滤器，存储开销小
- `--bloom_bits 10`：默认值，平衡性能和存储
- `--bloom_bits 16`：强过滤器，读密集型工作负载

#### 读写选项

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--verify_checksums` | 读取时验证校验和 | false |
| `--fill_cache` | 缓存读取数据 | true |
| `--write_sync` | 写入时同步到磁盘 | false |

#### 高级选项

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--paranoid_checks` | 激进的数据校验 | false |
| `--block_restart_interval` | 块内键编码重启间隔 | 16 |
| `--reuse_logs` | 重用日志文件以加快 DB 打开 | false |
| `--error_if_exists` | 数据库已存在时报错 | false |
| `--create_if_missing` | 数据库不存在时创建 | true |

### Learned Index 配置

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--file_model_error` | 文件模型误差（条目数） | 8 |
| `--level_model_error` | 级别模型误差（条目数） | 1 |
| `--string_mode` | 使用字符串模式训练模型 | false |
| `--filter` | 使用过滤器 | false |
| `--change_level_load` | 加载级别模型 | false |
| `--change_file_load` | 启用级别学习 | false |
| `--policy` | 学习策略 | 0 |

### 工作负载参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--mix` | 混合工作负载中写入操作的比例（每 1000 次操作） | 0 |
| `--distribution` | 操作分布文件路径 | "" |
| `--YCSB` | YCSB 工作负载文件路径 | "" |
| `--insert` | 插入新值的上限 | 0 |

### 其他参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-w, --write` | 执行数据库写入 | false |
| `-u, --unlimit_fd` | 取消文件描述符限制 | false |
| `-c, --uncache` | 清空缓存 | false |
| `-p, --pause` | 操作之间暂停 | false |
| `--single_timing` | 打印每次读取的详细时间 | false |
| `--file_info` | 打印文件结构信息 | false |
| `--output` | 输出 key 列表文件 | key_list.txt |

## 使用场景与配置建议

### 场景 1: 写入密集型工作负载

**目标**：最大化写吞吐量，减少 compaction 开销

```bash
./read_cold -m 7 -u -w \
    -W 64M \              # 大 MemTable 减少 flush 频率
    -C 0 \                # 禁用缓存（写入不需要）
    --no-fill_cache \
    -l 3 \                # 随机写入触发更多 compaction
    -n 10000000 \
    -f keys.txt \
    -d /tmp/write_heavy_db
```

**参数解释**：
- `-W 64M`：增大 MemTable 到 64MB，减少 flush 到磁盘的次数
- `-C 0` / `--no-fill_cache`：写入操作不需要缓存
- `-l 3`：随机写入可以更好地测试 compaction 性能

### 场景 2: 读取密集型工作负载

**目标**：最大化读性能，减少磁盘 I/O

```bash
./read_cold -m 7 -u \
    -C 256M \             # 超大缓存
    --bloom_bits 16 \     # 强 Bloom filter
    --compression snappy \ # 压缩减少 I/O
    --fill_cache \
    -n 10000000 \
    -f keys.txt \
    -d /tmp/read_heavy_db
```

**参数解释**：
- `-C 256M`：大缓存提高数据命中率
- `--bloom_bits 16`：强过滤器减少不必要的磁盘读取
- `--compression snappy`：压缩牺牲 CPU 换取更少 I/O

### 场景 3: 内存受限环境

**目标**：在有限内存下运行

```bash
./read_cold -m 7 -u -w \
    -W 1M \               # 小 MemTable
    -C 4M \               # 小缓存
    -B 2K \               # 小块大小
    --bloom_bits 6 \      # 弱过滤器
    -l 3 \
    -n 1000000 \
    -f keys.txt \
    -d /tmp/memory_constrained_db
```

**参数解释**：
- `-W 1M`：小 MemTable 控制内存使用
- `-C 4M`：小缓存减少内存占用
- `-B 2K`：小块大小减少单次 I/O 的内存需求
- `--bloom_bits 6`：弱过滤器减少存储开销

### 场景 4: 可重现的研究实验

**目标**：确保实验可重现

```bash
# 定义配置变量
export READ_COLD_REPRO="-m 7 -u -W 4M -C 8M -B 4K --bloom_bits 10 --compression none"

# 使用相同配置运行多次实验
./read_cold $READ_COLD_REPRO -w -l 0 -n 1000000 -f keys1.txt -d /tmp/exp1
./read_cold $READ_COLD_REPRO -l 3 -n 1000000 -f keys2.txt -d /tmp/exp2
./read_cold $READ_COLD_REPRO -mix 50 -n 5000000 -f keys3.txt -d /tmp/exp3
```

### 场景 5: YCSB 工作负载测试

```bash
# 生成 YCSB Workload A trace（50% 读，50% 写）
cd python/experiments/level_learnability
python generate_ycsb_trace.py --workload A --num-keys 10000000 \
    --num-ops 100000000 --distribution zipfian \
    --output ycsb_workload_a.dat

# 运行 YCSB 测试
cd ../../third_party/Bourbon/build
./read_cold -m 7 -u \
    -C 64M \
    --bloom_bits 12 \
    --YCSB ../../../python/experiments/level_learnability/ycsb_workload_a.dat \
    -d /tmp/ycsb_db
```

## 性能调优指南

### write_buffer_size（MemTable 大小）

**影响**：控制内存中未排序数据量，影响写吞吐和恢复时间

**调优建议**：
- **默认值**：4MB
- **写入密集**：增加到 32MB-64MB
- **内存受限**：减少到 1MB-2MB
- **权衡**：更大的值 → 更高的写吞吐，但更长的恢复时间和更高的内存使用

### cache_size（块缓存大小）

**影响**：控制读缓存容量，直接影响读性能

**调优建议**：
- **默认值**：8MB（LevelDB 内部缓存）
- **读密集**：增加到 64MB-256MB
- **写密集**：可以设为 0
- **内存受限**：减少到 4MB 或更小

### max_open_files

**影响**：控制文件描述符数量，影响缓存策略

**调优建议**：
- **默认值**：65536
- **大型数据库**：保持默认或增加到 100000+
- **系统限制低**：使用 `-u` 标志解除限制

### block_size

**影响**：SSTable 块大小，影响压缩效率和 I/O 粒度

**调优建议**：
- **默认值**：4KB
- **读密集**：增加到 8KB-16KB（减少 I/O 次数）
- **写密集**：保持 4KB
- **SSD**：可以增加到 8KB-32KB
- **HDD**：保持 4KB 或增加到 8KB

### bloom_bits

**影响**：Bloom filter 位数，影响读放大和存储开销

**调优建议**：
- **默认值**：10（约 1% 假阳性率）
- **读密集**：增加到 12-16（减少假阳性）
- **存储受限**：减少到 6-8（接受更高假阳性）
- **禁用**：设置为 0

### compression

**影响**：压缩类型，影响 CPU 使用和存储空间

**调优建议**：
- **默认值**：none（不压缩）
- **节省空间**：使用 `--compression snappy`
- **CPU 受限**：保持 none
- **SSD**：通常不需要压缩
- **HDD**：建议使用 snappy

## 常见问题

### Q1: 如何查看当前配置？

使用 `--help` 查看所有参数及其默认值：

```bash
./read_cold --help
```

### Q2: 为什么设置 `-C 16M` 报错？

**问题**：旧版本不支持 K/M/G 后缀

**解决**：确保使用最新版本，支持以下格式：
- `-C 16M` ✅
- `-C 16777216` ✅（直接输入字节数）

### Q3: 如何设置缓存为默认值？

**方法 1**：不指定 `-C` 参数
```bash
./read_cold -m 7 -u -w -l 0 -n 1000 -f keys.txt -d /tmp/db
```

**方法 2**：显式设置为 0
```bash
./read_cold -m 7 -u -w -C 0 -l 0 -n 1000 -f keys.txt -d /tmp/db
```

### Q4: Bloom filter 对性能的影响有多大？

**测试**：

```bash
# 禁用 Bloom filter
./read_cold -m 7 -u --bloom_bits 0 -n 1000000 -f keys.txt -d /tmp/no_bloom

# 默认 Bloom filter
./read_cold -m 7 -u --bloom_bits 10 -n 1000000 -f keys.txt -d /tmp/default_bloom

# 强 Bloom filter
./read_cold -m 7 -u --bloom_bits 16 -n 1000000 -f keys.txt -d /tmp/strong_bloom
```

**预期结果**：
- `--bloom_bits 0`：读放大最高，性能最差
- `--bloom_bits 10`：平衡，默认值
- `--bloom_bits 16`：读放大最低，读性能最好

### Q5: 如何验证配置是否生效？

**方法 1**：使用 `--file_info` 查看文件结构
```bash
./read_cold -m 7 -u -w -B 8K --file_info -l 0 -n 10000 -f keys.txt -d /tmp/test
```

**方法 2**：检查数据库目录大小
```bash
# 无压缩
./read_cold -m 7 -u -w --compression none -l 0 -n 100000 -f keys.txt -d /tmp/no_comp
du -sh /tmp/no_comp

# 启用压缩
./read_cold -m 7 -u -w --compression snappy -l 0 -n 100000 -f keys.txt -d /tmp/with_comp
du -sh /tmp/with_comp
```

## 输出说明

### 关键输出信息

```
Put Complete                          # 数据加载完成
Starting up                           # 开始读取测试
Progress:10%                          # 进度（每 10% 输出）
LevelSize 2 4 8 16 32 64             # 各层级文件数量
Shutting down                         # 测试结束
```

### 计时器说明

| 计时器 ID | 说明 |
|----------|------|
| 4 | 读取操作时间 |
| 9 | 数据加载时间 |
| 10 | 写入操作时间 |
| 13 | 总运行时间 |

### 统计信息输出

```
Timer 4 MEAN: 1234, STDDEV: 56.78      # 读取平均延迟和标准差
Timer 10 MEAN: 2345, STDDEV: 67.89     # 写入平均延迟和标准差
Timer 13 MEAN: 3456789, STDDEV: 123.45 # 总运行时间
```

## 相关文档

- [Bourbon 架构说明](../../CLAUDE.md)
- [LevelDB Options 源码](../include/leveldb/options.h)
- [YCSB 使用指南](../../python/experiments/level_learnability/docs/YCSB_USAGE.md)
- [流式 PLR 实现](../../doc/plan/streaming_plr_implementation.md)

## 更新日志

**2026-01-22**
- ✨ 新增 15 个 LevelDB Options 配置参数
- ✨ 支持人类可读的大小格式（K/M/G 后缀）
- ✨ 新增压缩类型配置
- ✨ 新增 Bloom filter 强度配置
- ✨ 改进帮助信息展示
