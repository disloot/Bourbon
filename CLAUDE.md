# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Bourbon is a learned index implementation for Log-Structured Merge Trees (LSMs), built as a modification of Google LevelDB 1.22.0. It implements the paper "A Learned Index for Log-Structured Merge Trees" (OSDI '20) and integrates machine learning-based indexing with WiscKey's value separation technique.

**Core Innovation**: Uses Piecewise Linear Regression (PLR) models to predict key positions, reducing LSM tree traversal cost and disk seeks.

**Key Feature**: Cost-Benefit Analysis (CBA) algorithm for intelligent model learning decisions in mixed workloads. The system automatically decides which files are worth learning models for based on runtime statistics.

## Build System

This project uses CMake (minimum version 3.7).

### Standard Build (Cold Reads / Read-Only Workloads)

```bash
mkdir build && cd build
cmake ../bourbon -DCMAKE_BUILD_TYPE=RELEASE -DNDEBUG_SWITCH=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make -j
```

### Build with Mixed Workloads (Reads + Writes)

```bash
mkdir build && cd build
cmake ../bourbon -DCMAKE_BUILD_TYPE=RELEASE \
    -DNDEBUG_SWITCH=ON -DLEVEL_SWITCH=ON -DINTERNAL_TIMER_SWITCH=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make -j
```

### Important CMake Options

- `LEVELDB_BUILD_TESTS=ON` (default) - Build unit tests
- `LEVELDB_BUILD_BENCHMARKS=ON` (default) - Build benchmark tools
- `NDEBUG_SWITCH=ON` - Disable assertions for release builds (recommended)
- `LEVEL_SWITCH=ON` - Record file info in each level (for mixed workloads)
- `INTERNAL_TIMER_SWITCH=ON` - Enable internal timing (for mixed workloads)
- `PROFILER_SWITCH=ON` - Enable profiler

## Running Tests

Unit tests are built when `LEVELDB_BUILD_TESTS=ON`. After building:

```bash
# Run all tests
cd build
ctest

# Run specific test
./<test_name>
```

Key test executables:
- `db_test` - Core LevelDB functionality tests
- `read` - Basic read performance test
- `read_cold` - Cold read benchmark (primary evaluation tool)

## Benchmarking and Evaluation

### Primary Benchmark Tool: `read_cold`

Located in `mod/read_cold.cc`, this is the main application for performance evaluation.

#### Key Command-Line Options

- `-m <mode>`: Running mode (7=Bourbon, 8=WiscKey, 0=LevelDB)
- `-u`: Unlimited file descriptors (recommended, sets limit to 65536)
- `-w`: Perform database load (write mode)
- `-l <type>`: Load type (0=ordered, 3=random)
- `-i <n>`: Number of iterations (typically 5)
- `-n <n>`: Number of requests in thousands
- `-f <path>`: Path to dataset file (one key per line)
- `-d <path>`: Path to database directory
- `-k <bytes>`: Key size in bytes
- `-v <bytes>`: Value size in bytes
- `-c`: Cold read (clears OS cache before workload)
- `--distribution <path>`: Request distribution file
- `--YCSB <path>`: YCSB trace file
- `--change_level_load`: Disable using existing level models

#### Example Usage

```bash
# Load database
./read_cold -m 7 -u -w -l 0 -n 1000 -f /path/to/dataset -d /path/to/db

# Run read workload
./read_cold -m 7 -u -i 5 -n 1000 -f /path/to/dataset -d /path/to/db --distribution /path/to/distribution
```

### Performance Timers (Internal)

Key timers reported in output:
- Timer 0: File lookup time within one level
- Timer 1: File reading time
- Timer 2: File model inference time
- Timer 3/5: Key search time in file
- Timer 13: **Total time** (primary metric)
- Timer 4: Total time for all get requests
- Timer 10: Total time for all put requests

## Architecture

### Core Components

**Learned Index System** (`mod/learned_index.h/cpp`)
- `LearnedIndexData`: Base class for learned models (file-level or level-level)
- `FileLearnedIndexData`: Manages per-file models with thread safety
- `LevelLearnedIndexData`: Manages per-level models
- `AccumulatedNumEntriesArray`: Maps level model predictions to specific files

**PLR Algorithm** (`mod/plr.h/cpp`)
- `GreedyPLR`: Greedy piecewise linear regression algorithm
- `Segment`: Single linear segment (slope + intercept)
- Modified from https://github.com/RyanMarcus/plr

**Cost-Benefit Analysis (CBA)** (`mod/CBMode_Learn.h/cpp`)
- `CBModel_Learn`: Decision engine for whether to learn models for files
- Tracks positive/negative lookup times for baseline vs. model
- Calculates cost-benefit scores to prioritize learning
- Only learns when benefits > costs (score > 10)

**Statistics & Timing** (`mod/stats.h/cpp`, `mod/timer.h/cpp`)
- High-precision timing using RDTSC instructions
- Per-level and per-model performance tracking
- Timer IDs: 0=file lookup, 1=file read, 2=model inference, 13=total

**Two-Level Indexing**:
1. **Level models** → predict which file contains the key
2. **File models** → predict position within that file

### Integration Points

- Hooks into LevelDB's version management (`db/version_set.cc`)
- Integrates with table cache (`db/table_cache.cc`)
- Background learning scheduled during idle periods via `PrepareLearn` thread (`util/env_posix.cc`)
- Models persisted to disk and loaded on startup
- File lifecycle tracking in `db/db_impl.cc`

### Operation Modes

Controlled by `MOD` variable in `mod/util.h`:
- `MOD=0`: Original LevelDB
- `MOD=6`: Learned Index only (no WiscKey)
- `MOD=7`: Bourbon (Learned Index + WiscKey) - **primary mode**
- `MOD=8`: WiscKey only (baseline)

### CBA Learning Decision Flow

**Mixed workload mode only** (`LEVEL_SWITCH=ON`):
1. File created → tracked in `file_stats` with creation timestamp
2. After 50ms (`learn_trigger_time`), file becomes eligible for learning
3. `CalculateCB()` evaluates benefit based on:
   - Average lookup times (baseline vs. model)
   - Positive vs. negative lookups
   - Per-file lookup frequency
   - Score formula: `[(T_baseline - T_model) × lookups] × num_files / total_size`
4. If score > 10 → add to learning priority queue
5. Background thread learns models in priority order

**Important**: CBA requires sufficient samples before making decisions:
- Minimum 10-20 files per level (varies by level)
- Minimum 10,000 total lookups
- Minimum 500 lookups per type (positive/negative, baseline/model)

### Error Handling

- Models predict intervals with error bounds
- `file_model_error`: Default 8 entries (configurable in `mod/util.h`)
- `level_model_error`: Default 1 entry
- If prediction interval is invalid, fallback to binary search
- Slope NaN protection (fixed in commit 0cd4715)

### Threading and Concurrency

- `FileLearnedIndexData` uses mutex protection
- Models use atomic flags for learning state (`learned`, `learning`, `aborted`)
- Background learning via `PrepareLearn` thread (lazy initialization)
- File statistics protected by `file_stats_mutex`
- CBA statistics protected by separate `lookup_mutex` and `file_mutex`

## Common Development Tasks

### Adding a New Test

1. Create test file in appropriate directory (`db/`, `table/`, `util/`, or `mod/`)
2. Add to CMakeLists.txt using `leveldb_test()` or `leveldb_benchmark()`
3. Rebuild with `make -j`

### Modifying Model Parameters

Edit `mod/util.h`:
- `file_model_error`: Error bound for file models (default: 8)
- `level_model_error`: Error bound for level models (default: 1)
- `key_size`, `value_size`: Default key/value sizes
- `learn_trigger_time`: Minimum file lifetime before CBA evaluation (default: 50ms)
- `reference_frequency`: CPU frequency for RDTSC timing (default: 2.6 GHz)

**For CBA tuning** (mixed workloads):
- `CBModel_Learn::const_size_to_cost`: Learning threshold (default: 10)
- `CBModel_Learn::file_average_limit[7]`: Min files per level [10,20,20,20,20,500,500]
- `CBModel_Learn::lookup_average_limit`: Min total lookups (default: 10000)

### Experiment Scripts

Located in `scripts/`:
- `run_all_expr.sh`: Run all experiments at once
- `run_dataset.sh`: Dataset variation experiments
- `run_load_order.sh`: Load order experiments
- `run_request_distribution.sh`: Request distribution experiments
- `run_ycsb.sh`: YCSB workload experiments
- `run_sosd.sh`: SOSD workload experiments
- `load_db.sh`: Database loading script
- `collect_results.py`: Parse experiment results

### Debugging CBA Decisions

To understand why CBA accepts/rejects files for learning:
1. Enable `LEVEL_SWITCH=ON` and `INTERNAL_TIMER_SWITCH=ON` in build
2. Check `db/version_set.cc:505` for lookup data collection
3. Check `db/db_impl.cc:298` for file deletion statistics
4. Review `CBModel_Learn::CalculateCB()` output in debugger
5. See `docs/CBA_Algorithm_Analysis.md` for detailed algorithm documentation

### Working with Timers

The system uses high-precision timers (RDTSC) throughout:
- Timer 0: File lookup within level
- Timer 1: File reading time
- Timer 2: Model inference time
- Timer 3/5: Key search in file
- Timer 13: **Total time** (primary performance metric)
- Timer 4: Total get request time
- Timer 7: Compaction time
- Timer 10: Total put request time
- Timer 11: Model learning time

Timer implementation in `mod/timer.cpp`, usage tracked in `mod/stats.cpp`

## Known Issues and Considerations

1. **PLR Slope Bug**: Fixed in commit 0cd4715 - prevents NaN values in slope parameters during learning
2. **File Descriptor Limits**: Use `-u` flag for production workloads to avoid 1024 fd limit
3. **Model Learning**: Models are learned during database idle time; first run may be slower
4. **Memory Usage**: Learned models require additional memory for storing linear segments
5. **Thread Safety**: File-level models are thread-safe; ensure proper locking when modifying
6. **CBA Cold Start**: CBA requires warm-up period to collect statistics before making decisions
7. **Platform Specifics**: RDTSC timing assumes x86/ARM64; other platforms use clock_gettime fallback
8. **Level 6 Learning**: Effectively disabled (500 file threshold) in current CBA configuration

## References

- Paper: https://www.usenix.org/conference/osdi20/presentation/dai
- Author: Yifan DAI (https://pages.cs.wisc.edu/~yifann/)
- Based on: Google LevelDB 1.22.0 + WiscKey
- PLR Algorithm: https://github.com/RyanMarcus/plr

## Additional Documentation

- `docs/CBA_Algorithm_Analysis.md`: In-depth analysis of Cost-Benefit Analysis algorithm (650 lines)
- `docs/CBA_Statistics_Collection_Detail.md`: Detailed explanation of statistics collection mechanisms
- `ARTIFACT_README`: Artifact evaluation instructions for paper reproduction
- `CS736_README`: Course project documentation
