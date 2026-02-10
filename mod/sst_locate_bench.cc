#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <sstream>
#include <chrono>
#include <limits>
#include <vector>

#include "cxxopts.hpp"
#include "port/port.h"
#include "db/dbformat.h"
#include "db/version_set.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "mod/stats.h"
#include "mod/util.h"

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

struct TargetFile {
  leveldb::FileMetaData* meta = nullptr;
  int level = -1;
};

enum class BenchMode {
  kBaseline,
  kLearnedSingle,
  kLearnedMulti,
};

bool FileExists(const std::string& path) {
  std::ifstream input(path);
  return input.good();
}

BenchMode ParseMode(const std::string& mode_str) {
  if (mode_str == "baseline") return BenchMode::kBaseline;
  if (mode_str == "learned_single") return BenchMode::kLearnedSingle;
  if (mode_str == "learned_multi") return BenchMode::kLearnedMulti;
  throw std::invalid_argument("invalid mode: " + mode_str);
}

const char* ModeName(BenchMode mode) {
  switch (mode) {
    case BenchMode::kBaseline:
      return "baseline";
    case BenchMode::kLearnedSingle:
      return "learned_single";
    case BenchMode::kLearnedMulti:
      return "learned_multi";
    default:
      return "unknown";
  }
}

bool DropCaches() {
  const int rc =
      std::system("sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null");
  return rc == 0;
}

bool SelectTargetFile(leveldb::Version* current, bool has_file_number,
                      uint64_t requested_file_number, TargetFile* target) {
  if (has_file_number) {
    for (int level = 0; level < leveldb::config::kNumLevels; ++level) {
      std::vector<leveldb::FileMetaData*> files;
      current->GetOverlappingInputs(level, nullptr, nullptr, &files);
      for (auto* file : files) {
        if (file->number == requested_file_number) {
          target->meta = file;
          target->level = level;
          return true;
        }
      }
    }
    return false;
  }

  for (int level = leveldb::config::kNumLevels - 1; level >= 0; --level) {
    std::vector<leveldb::FileMetaData*> files;
    current->GetOverlappingInputs(level, nullptr, nullptr, &files);
    if (files.empty()) continue;

    auto* selected = *std::max_element(
        files.begin(), files.end(),
        [](leveldb::FileMetaData* lhs, leveldb::FileMetaData* rhs) {
          return lhs->file_size < rhs->file_size;
        });
    target->meta = selected;
    target->level = level;
    return true;
  }
  return false;
}

struct QueryRunResult {
  uint64_t found = 0;
  uint64_t wall_nanos = 0;
};

class PerfCounter {
 public:
  PerfCounter(bool enabled, uint32_t type, uint64_t config,
              const std::string& name)
      : enabled_(enabled), type_(type), config_(config), name_(name) {}

  ~PerfCounter() {
#if defined(__linux__)
    if (fd_ >= 0) {
      close(fd_);
    }
#endif
  }

  bool Start() {
    if (!enabled_) {
      return false;
    }
#if defined(__linux__)
    if (!Open()) {
      return false;
    }
    if (ioctl(fd_, PERF_EVENT_IOC_RESET, 0) == -1) {
      valid_ = false;
      error_ = std::strerror(errno);
      return false;
    }
    if (ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0) == -1) {
      valid_ = false;
      error_ = std::strerror(errno);
      return false;
    }
    running_ = true;
    return true;
#else
    valid_ = false;
    error_ = "perf_event_open unsupported on this platform";
    return false;
#endif
  }

  uint64_t Stop() {
    if (!running_) {
      return 0;
    }
#if defined(__linux__)
    if (ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0) == -1) {
      valid_ = false;
      error_ = std::strerror(errno);
      running_ = false;
      return 0;
    }
    uint64_t cycles = 0;
    const ssize_t bytes = read(fd_, &cycles, sizeof(cycles));
    if (bytes != static_cast<ssize_t>(sizeof(cycles))) {
      valid_ = false;
      error_ = std::strerror(errno);
      running_ = false;
      return 0;
    }
    running_ = false;
    return cycles;
#else
    running_ = false;
    return 0;
#endif
  }

  bool valid() const { return valid_; }

  const std::string& error() const { return error_; }
  const std::string& name() const { return name_; }

 private:
#if defined(__linux__)
  bool Open() {
    if (fd_ >= 0) {
      return true;
    }
    struct perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.type = type_;
    attr.size = sizeof(attr);
    attr.config = config_;
    attr.disabled = 1;
    attr.exclude_hv = 1;
    attr.exclude_idle = 1;
    fd_ = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
    if (fd_ < 0) {
      valid_ = false;
      error_ = std::strerror(errno);
      return false;
    }
    valid_ = true;
    return true;
  }
#endif

  bool enabled_ = true;
  bool running_ = false;
  bool valid_ = false;
  std::string error_;
  uint32_t type_ = 0;
  uint64_t config_ = 0;
  std::string name_;
#if defined(__linux__)
  int fd_ = -1;
#endif
};

QueryRunResult RunQueries(leveldb::Version* current, leveldb::FileMetaData* file_meta,
                          int level, const leveldb::ReadOptions& read_options,
                          const std::vector<std::string>& user_keys,
                          const std::vector<size_t>& query_order,
                          uint64_t snapshot, int num_queries, int start_offset) {
  if (num_queries <= 0 || user_keys.empty() || query_order.empty()) {
    return {};
  }

  QueryRunResult result;
  for (int i = 0; i < num_queries; ++i) {
    const size_t query_idx =
        query_order[(static_cast<size_t>(start_offset + i)) % query_order.size()];
    const std::string& user_key = user_keys[query_idx];
    leveldb::LookupKey lkey(user_key, snapshot);
    std::string value;
    bool file_learned = false;
    const auto wall_start = std::chrono::steady_clock::now();
    leveldb::Status s = current->GetFromFile(read_options, file_meta, level, lkey,
                                             &value, &file_learned);
    const auto wall_end = std::chrono::steady_clock::now();
    result.wall_nanos += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start)
            .count());
    if (s.ok()) {
      ++result.found;
    }
  }
  return result;
}

void AppendResult(const std::string& csv_path, BenchMode mode,
                  const TargetFile& target, uint64_t block_entries,
                  uint32_t requested_error, double model_error,
                  uint64_t model_segments,
                  double span_blocks_mean, const std::string& span_hist,
                  double k_expected, double lookup_data_blocks_read_total,
                  double read_bytes_total, double read_bytes_logical_total,
                  double cpu_cycles_total, double cpu_cycles_valid,
                  double cpu_task_clock_ns_total, double cpu_task_clock_valid,
                  double io_us_total,
                  double cpu_us_total, double total_us_total,
                  double comparisons_us_total, double strict_compare_calls_total,
                  double strict_compare_us_total,
                  double getposition_calls_total,
                  double getposition_compare_calls_total,
                  double getposition_us_total,
                  double getposition_compare_us_total,
                  int num_queries, uint64_t found) {
  const bool has_header = FileExists(csv_path);
  std::ofstream out(csv_path, std::ios::app);
  if (!out.is_open()) {
    throw std::runtime_error("cannot open csv path: " + csv_path);
  }
  if (!has_header) {
    out << "mode,file_number,level,B,error,model_error,model_segments,index_span_mean,"
           "index_span_hist,K_expected,lookup_data_blocks_read_total,"
           "read_bytes_total,read_bytes_logical_total,cpu_cycles_total,cpu_cycles_valid,"
           "cpu_task_clock_ns_total,cpu_task_clock_valid,"
           "io_us_total,cpu_us_total,total_us_total,"
           "comparisons_us_total,strict_compare_calls_total,"
           "strict_compare_us_total,getposition_calls_total,"
           "getposition_compare_calls_total,getposition_us_total,"
           "getposition_compare_us_total,"
           "queries,found\n";
  }
  out << ModeName(mode) << "," << target.meta->number << "," << target.level
      << "," << block_entries << "," << requested_error << ","
      << std::fixed << std::setprecision(6) << model_error << ","
      << model_segments << ","
      << span_blocks_mean << ",\"" << span_hist << "\","
      << k_expected << "," << lookup_data_blocks_read_total << ","
      << read_bytes_total << "," << read_bytes_logical_total << ","
      << cpu_cycles_total << "," << cpu_cycles_valid << ","
      << cpu_task_clock_ns_total << "," << cpu_task_clock_valid << ","
      << io_us_total << "," << cpu_us_total << ","
      << total_us_total << "," << comparisons_us_total << ","
      << strict_compare_calls_total << "," << strict_compare_us_total << ","
      << getposition_calls_total << "," << getposition_compare_calls_total
      << "," << getposition_us_total << ","
      << getposition_compare_us_total << ","
      << num_queries << "," << found << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string db_path;
  std::string mode_str;
  std::string csv_path;
  bool has_file_number = false;
  uint64_t file_number = 0;
  int num_queries = 100000;
  int warmup_queries = 10000;
  bool drop_caches = false;
  bool fill_cache = false;
  int block_restart_interval = 1;
  std::string compression = "none";
  uint32_t file_model_error = 8;
  std::string query_order = "sequential";
  uint64_t query_seed = 20260211;
  std::string fixed_query_key;
  size_t fixed_query_index = 0;
  bool disable_filter_for_bench = true;
  bool disable_block_cache_for_bench = true;
  bool use_direct_io = true;
  bool collect_cpu_cycles = true;

  cxxopts::Options options("sst_locate_bench",
                           "SST-only lookup micro benchmark");
  options.add_options()("db_path", "DB path",
                        cxxopts::value<std::string>(db_path))(
      "file_number", "Target SST file number",
      cxxopts::value<uint64_t>(file_number))(
      "mode", "baseline|learned_single|learned_multi",
      cxxopts::value<std::string>(mode_str)->default_value("baseline"))(
      "num_queries", "Measured query count",
      cxxopts::value<int>(num_queries)->default_value("100000"))(
      "warmup_queries", "Warmup query count",
      cxxopts::value<int>(warmup_queries)->default_value("10000"))(
      "drop_caches", "Drop Linux page cache before benchmark",
      cxxopts::value<bool>(drop_caches)->default_value("false"))(
      "fill_cache", "ReadOptions.fill_cache",
      cxxopts::value<bool>(fill_cache)->default_value("false"))(
      "block_restart_interval", "Must be 1 for learned path assertions",
      cxxopts::value<int>(block_restart_interval)->default_value("1"))(
      "compression", "Must be none for learned raw-entry reads",
      cxxopts::value<std::string>(compression)->default_value("none"))(
      "file_model_error", "File model error bound",
      cxxopts::value<uint32_t>(file_model_error)->default_value("8"))(
      "query_order", "Query order: sequential|random|fixed",
      cxxopts::value<std::string>(query_order)->default_value("sequential"))(
      "query_seed", "Random query seed (used when query_order=random)",
      cxxopts::value<uint64_t>(query_seed)->default_value("20260211"))(
      "fixed_query_key", "Use this exact user key when query_order=fixed",
      cxxopts::value<std::string>(fixed_query_key)->default_value(""))(
      "fixed_query_index", "Fallback key index when query_order=fixed and fixed_query_key is empty",
      cxxopts::value<size_t>(fixed_query_index)->default_value("0"))(
      "disable_filter_for_bench",
      "Disable Bloom filter checks for both baseline and learned paths",
      cxxopts::value<bool>(disable_filter_for_bench)->default_value("true"))(
      "disable_block_cache_for_bench",
      "Disable DB block cache by setting cache capacity to zero",
      cxxopts::value<bool>(disable_block_cache_for_bench)->default_value("true"))(
      "use_direct_io",
      "Use O_DIRECT for random SST reads (Linux)",
      cxxopts::value<bool>(use_direct_io)->default_value("true"))(
      "collect_cpu_cycles",
      "Collect end-to-end CPU cycles via perf_event_open",
      cxxopts::value<bool>(collect_cpu_cycles)->default_value("true"))(
      "csv_path", "Output csv path",
      cxxopts::value<std::string>(csv_path)->default_value(
          "sst_locate_results.csv"))("h,help", "Print help");

  auto parsed = options.parse(argc, argv);
  if (parsed.count("help") > 0 || db_path.empty()) {
    std::cout << options.help() << std::endl;
    return 0;
  }
  has_file_number = parsed.count("file_number") > 0;

  if (block_restart_interval != 1) {
    std::cerr << "block_restart_interval must be 1." << std::endl;
    return 1;
  }
  if (compression != "none") {
    std::cerr << "compression must be none." << std::endl;
    return 1;
  }
  if (query_order != "sequential" && query_order != "random" &&
      query_order != "fixed") {
    std::cerr << "query_order must be sequential, random, or fixed."
              << std::endl;
    return 1;
  }

  BenchMode mode = BenchMode::kBaseline;
  try {
    mode = ParseMode(mode_str);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  adgMod::MOD = mode == BenchMode::kBaseline ? 0 : 7;
  adgMod::learned_verify_multi = (mode == BenchMode::kLearnedMulti);
  adgMod::bench_use_direct_io = use_direct_io;
  adgMod::bench_disable_block_cache = disable_block_cache_for_bench;
  adgMod::load_level_model = false;
  adgMod::load_file_model = false;
  adgMod::enable_streaming_plr = false;
  adgMod::file_model_error = file_model_error;

  leveldb::Options db_options;
  db_options.create_if_missing = false;
  db_options.compression = leveldb::kNoCompression;
  db_options.block_restart_interval = 1;
  if (disable_filter_for_bench) {
    db_options.filter_policy = nullptr;
  }
  leveldb::Cache* bench_block_cache = nullptr;
  if (disable_block_cache_for_bench) {
    bench_block_cache = leveldb::NewLRUCache(0);
    db_options.block_cache = bench_block_cache;
  }

  leveldb::DB* db = nullptr;
  leveldb::Status s = leveldb::DB::Open(db_options, db_path, &db);
  if (!s.ok()) {
    std::cerr << "Open DB failed: " << s.ToString() << std::endl;
    delete bench_block_cache;
    return 1;
  }

  leveldb::DBImpl* impl = adgMod::db;
  if (impl == nullptr) {
    std::cerr << "DBImpl not initialized." << std::endl;
    delete db;
    delete bench_block_cache;
    return 1;
  }

  leveldb::Version* current = impl->versions_->current();
  current->Ref();

  TargetFile target;
  if (!SelectTargetFile(current, has_file_number, file_number, &target)) {
    std::cerr << "Cannot locate target SST file." << std::endl;
    current->Unref();
    delete db;
    delete bench_block_cache;
    return 1;
  }

  leveldb::ReadOptions read_options;
  read_options.fill_cache = fill_cache;

  adgMod::LearnedIndexData* model = adgMod::file_data->GetModel(target.meta->number);
  if (!current->FillData(read_options, target.meta, model) ||
      model->string_keys.empty()) {
    std::cerr << "Failed to fill file keys for target file." << std::endl;
    current->Unref();
    delete db;
    delete bench_block_cache;
    return 1;
  }
  if (mode != BenchMode::kBaseline) {
    if (!model->Learned() && !model->Learn()) {
      std::cerr << "Failed to train file model." << std::endl;
      current->Unref();
      delete db;
      delete bench_block_cache;
      return 1;
    }
  }

  if (adgMod::block_num_entries == 0) {
    std::cerr << "Invalid block_num_entries=0." << std::endl;
    current->Unref();
    delete db;
    delete bench_block_cache;
    return 1;
  }

  std::vector<size_t> query_indices(model->string_keys.size());
  std::iota(query_indices.begin(), query_indices.end(), 0);
  if (query_order == "random") {
    std::mt19937_64 rng(query_seed);
    std::shuffle(query_indices.begin(), query_indices.end(), rng);
  } else if (query_order == "fixed") {
    size_t chosen_index = fixed_query_index % model->string_keys.size();
    if (!fixed_query_key.empty()) {
      bool found_key = false;
      for (size_t i = 0; i < model->string_keys.size(); ++i) {
        if (model->string_keys[i] == fixed_query_key) {
          chosen_index = i;
          found_key = true;
          break;
        }
      }
      if (!found_key) {
        std::cerr << "fixed_query_key not found in selected SST file."
                  << std::endl;
        current->Unref();
        delete db;
        delete bench_block_cache;
        return 1;
      }
    }
    query_indices.assign(1, chosen_index);
  }

  const uint64_t snapshot = impl->versions_->LastSequence();
  RunQueries(current, target.meta, target.level, read_options, model->string_keys,
             query_indices, snapshot, warmup_queries, 0);
  if (drop_caches && !DropCaches()) {
    std::cerr << "drop_caches failed; continuing with current cache state."
              << std::endl;
  }

  adgMod::Stats* stats = adgMod::Stats::GetInstance();
  stats->ResetAll();
  adgMod::ResetLookupMetrics();

#if defined(__linux__)
  const uint32_t cycles_type = PERF_TYPE_HARDWARE;
  const uint64_t cycles_config = PERF_COUNT_HW_CPU_CYCLES;
  const uint32_t task_clock_type = PERF_TYPE_SOFTWARE;
  const uint64_t task_clock_config = PERF_COUNT_SW_TASK_CLOCK;
#else
  const uint32_t cycles_type = 0;
  const uint64_t cycles_config = 0;
  const uint32_t task_clock_type = 0;
  const uint64_t task_clock_config = 0;
#endif
  PerfCounter cycles_counter(collect_cpu_cycles, cycles_type, cycles_config,
                             "cpu_cycles");
  PerfCounter task_clock_counter(collect_cpu_cycles, task_clock_type,
                                 task_clock_config, "cpu_task_clock_ns");
  if (collect_cpu_cycles && !cycles_counter.Start()) {
    std::cerr << cycles_counter.name() << " disabled: " << cycles_counter.error()
              << std::endl;
  }
  if (collect_cpu_cycles && !task_clock_counter.Start()) {
    std::cerr << task_clock_counter.name()
              << " disabled: " << task_clock_counter.error() << std::endl;
  }
  const QueryRunResult query_result = RunQueries(
      current, target.meta, target.level, read_options, model->string_keys,
      query_indices, snapshot, num_queries, warmup_queries);
  const uint64_t cpu_cycles_raw = cycles_counter.Stop();
  const uint64_t cpu_task_clock_ns_raw = task_clock_counter.Stop();
  const uint64_t found = query_result.found;

  const double total_us_total =
      static_cast<double>(query_result.wall_nanos) / 1000.0;
  const bool cpu_cycles_ok = cycles_counter.valid();
  const bool cpu_task_clock_ok = task_clock_counter.valid();
  const double cpu_cycles_total = cpu_cycles_ok
      ? static_cast<double>(cpu_cycles_raw)
      : std::numeric_limits<double>::quiet_NaN();
  const double cpu_cycles_valid = cpu_cycles_ok ? 1.0 : 0.0;
  const double cpu_task_clock_ns_total = cpu_task_clock_ok
      ? static_cast<double>(cpu_task_clock_ns_raw)
      : std::numeric_limits<double>::quiet_NaN();
  const double cpu_task_clock_valid = cpu_task_clock_ok ? 1.0 : 0.0;
  const double io_us_total =
      static_cast<double>(
          adgMod::lookup_read_nanos.load(std::memory_order_relaxed)) /
      1000.0;
  const double cpu_us_total = std::max(0.0, total_us_total - io_us_total);
  const double total_us_components_total = io_us_total + cpu_us_total;
  const double k_expected =
      std::ceil((2.0 * static_cast<double>(file_model_error) + 1.0) /
                static_cast<double>(adgMod::block_num_entries));
  const double lookup_data_blocks_read_total = static_cast<double>(
      adgMod::lookup_data_blocks_read.load(std::memory_order_relaxed));
  const double read_bytes_total = static_cast<double>(
      adgMod::lookup_read_bytes.load(std::memory_order_relaxed));
  const double read_bytes_logical_total = static_cast<double>(
      adgMod::lookup_read_logical_bytes.load(std::memory_order_relaxed));
  const double strict_compare_calls_total = static_cast<double>(
      adgMod::lookup_compare_calls.load(std::memory_order_relaxed));
  const double strict_compare_us_total =
      static_cast<double>(
          adgMod::lookup_compare_nanos.load(std::memory_order_relaxed)) /
      1000.0;
  const double comparisons_us_total = strict_compare_us_total;
  const double getposition_calls_total = static_cast<double>(
      adgMod::lookup_getposition_calls.load(std::memory_order_relaxed));
  const double getposition_compare_calls_total = static_cast<double>(
      adgMod::lookup_getposition_compare_calls.load(std::memory_order_relaxed));
  const double getposition_us_total =
      static_cast<double>(
          adgMod::lookup_getposition_nanos.load(std::memory_order_relaxed)) /
      1000.0;
  const double getposition_compare_us_total =
      static_cast<double>(
          adgMod::lookup_getposition_compare_nanos.load(
              std::memory_order_relaxed)) /
      1000.0;
  const uint64_t span_count = adgMod::lookup_interval_span_count.load(
      std::memory_order_relaxed);
  const double span_blocks_mean =
      span_count > 0
          ? static_cast<double>(
                adgMod::lookup_interval_span_sum.load(std::memory_order_relaxed)) /
                static_cast<double>(span_count)
          : 0.0;

  std::ostringstream span_hist_stream;
  for (size_t i = 0; i < adgMod::lookup_interval_span_hist.size(); ++i) {
    const uint64_t count =
        adgMod::lookup_interval_span_hist[i].load(std::memory_order_relaxed);
    if (count == 0) continue;
    if (span_hist_stream.tellp() > 0) {
      span_hist_stream << "|";
    }
    if (i + 1 == adgMod::lookup_interval_span_hist.size()) {
      span_hist_stream << ">=10:" << count;
    } else {
      span_hist_stream << (i + 1) << ":" << count;
    }
  }
  const std::string span_hist = span_hist_stream.str();
  const double model_error = mode == BenchMode::kBaseline ? 0.0 : model->GetError();
  const uint64_t model_segments =
      (mode != BenchMode::kBaseline && model->Learned() &&
       model->string_segments.size() > 0)
          ? static_cast<uint64_t>(model->string_segments.size() - 1)
          : 0;
  try {
    AppendResult(csv_path, mode, target, adgMod::block_num_entries,
                 file_model_error, model_error, model_segments, span_blocks_mean,
                 span_hist,
                 k_expected, lookup_data_blocks_read_total, read_bytes_total,
                 read_bytes_logical_total, cpu_cycles_total, cpu_cycles_valid,
                 cpu_task_clock_ns_total, cpu_task_clock_valid,
                 io_us_total, cpu_us_total, total_us_total, comparisons_us_total,
                 strict_compare_calls_total, strict_compare_us_total,
                 getposition_calls_total, getposition_compare_calls_total,
                 getposition_us_total, getposition_compare_us_total,
                 num_queries, found);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    current->Unref();
    delete db;
    delete bench_block_cache;
    return 1;
  }

  std::cout << "mode=" << ModeName(mode)
            << " file_number=" << target.meta->number
            << " level=" << target.level << " B=" << adgMod::block_num_entries
            << " error=" << file_model_error
            << " model_error=" << model_error
            << " model_segments=" << model_segments
            << " span_mean=" << span_blocks_mean
            << " span_hist=" << (span_hist.empty() ? "none" : span_hist)
            << " K_expected=" << std::fixed << std::setprecision(2)
            << k_expected
            << " lookup_data_blocks_read_total=" << lookup_data_blocks_read_total
            << " read_bytes_total=" << read_bytes_total
            << " read_bytes_logical_total=" << read_bytes_logical_total
            << " cpu_cycles_total=" << cpu_cycles_total
            << " cpu_cycles_valid=" << cpu_cycles_valid
            << " cpu_task_clock_ns_total=" << cpu_task_clock_ns_total
            << " cpu_task_clock_valid=" << cpu_task_clock_valid
            << " io_us_total=" << io_us_total
            << " cpu_us_total=" << cpu_us_total
            << " total_us_total=" << total_us_total
            << " total_us_components_total=" << total_us_components_total
            << " strict_compare_calls_total=" << strict_compare_calls_total
            << " strict_compare_us_total=" << strict_compare_us_total
            << " getposition_calls_total=" << getposition_calls_total
            << " getposition_compare_calls_total="
            << getposition_compare_calls_total
            << " getposition_us_total=" << getposition_us_total
            << " getposition_compare_us_total="
            << getposition_compare_us_total << std::endl;

  current->Unref();
  delete db;
  delete bench_block_cache;
  return 0;
}
