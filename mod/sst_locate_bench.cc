#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cxxopts.hpp"
#include "port/port.h"
#include "db/dbformat.h"
#include "db/version_set.h"
#include "leveldb/db.h"
#include "mod/stats.h"
#include "mod/util.h"

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

uint64_t RunQueries(leveldb::Version* current, leveldb::FileMetaData* file_meta,
                    int level, const leveldb::ReadOptions& read_options,
                    const std::vector<std::string>& user_keys,
                    uint64_t snapshot, int num_queries, int start_offset) {
  if (num_queries <= 0 || user_keys.empty()) {
    return 0;
  }

  uint64_t found = 0;
  for (int i = 0; i < num_queries; ++i) {
    const std::string& user_key =
        user_keys[(static_cast<size_t>(start_offset + i)) % user_keys.size()];
    leveldb::LookupKey lkey(user_key, snapshot);
    std::string value;
    bool file_learned = false;
    leveldb::Status s = current->GetFromFile(read_options, file_meta, level, lkey,
                                             &value, &file_learned);
    if (s.ok()) {
      ++found;
    }
  }
  return found;
}

void AppendResult(const std::string& csv_path, BenchMode mode,
                  const TargetFile& target, uint64_t block_entries,
                  uint32_t model_error, double k_expected, double k_measured,
                  double io_us, double cpu_us, double total_us,
                  double comparisons_us, int num_queries, uint64_t found) {
  const bool has_header = FileExists(csv_path);
  std::ofstream out(csv_path, std::ios::app);
  if (!out.is_open()) {
    throw std::runtime_error("cannot open csv path: " + csv_path);
  }
  if (!has_header) {
    out << "mode,file_number,level,B,error,K_expected,K_measured,io_us,cpu_us,"
           "total_us,comparisons_us,queries,found\n";
  }
  out << ModeName(mode) << "," << target.meta->number << "," << target.level
      << "," << block_entries << "," << model_error << ","
      << std::fixed << std::setprecision(6) << k_expected << "," << k_measured
      << "," << io_us << "," << cpu_us << "," << total_us << ","
      << comparisons_us << "," << num_queries << "," << found << "\n";
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

  BenchMode mode = BenchMode::kBaseline;
  try {
    mode = ParseMode(mode_str);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  adgMod::MOD = mode == BenchMode::kBaseline ? 0 : 7;
  adgMod::learned_verify_multi = (mode == BenchMode::kLearnedMulti);
  adgMod::load_level_model = false;
  adgMod::load_file_model = false;
  adgMod::enable_streaming_plr = false;
  adgMod::file_model_error = file_model_error;

  leveldb::Options db_options;
  db_options.create_if_missing = false;
  db_options.compression = leveldb::kNoCompression;
  db_options.block_restart_interval = 1;

  leveldb::DB* db = nullptr;
  leveldb::Status s = leveldb::DB::Open(db_options, db_path, &db);
  if (!s.ok()) {
    std::cerr << "Open DB failed: " << s.ToString() << std::endl;
    return 1;
  }

  leveldb::DBImpl* impl = adgMod::db;
  if (impl == nullptr) {
    std::cerr << "DBImpl not initialized." << std::endl;
    delete db;
    return 1;
  }

  leveldb::Version* current = impl->versions_->current();
  current->Ref();

  TargetFile target;
  if (!SelectTargetFile(current, has_file_number, file_number, &target)) {
    std::cerr << "Cannot locate target SST file." << std::endl;
    current->Unref();
    delete db;
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
    return 1;
  }
  if (mode != BenchMode::kBaseline) {
    if (!model->Learned() && !model->Learn()) {
      std::cerr << "Failed to train file model." << std::endl;
      current->Unref();
      delete db;
      return 1;
    }
  }

  if (adgMod::block_num_entries == 0) {
    std::cerr << "Invalid block_num_entries=0." << std::endl;
    current->Unref();
    delete db;
    return 1;
  }

  if (drop_caches && !DropCaches()) {
    std::cerr << "drop_caches failed; continuing with current cache state."
              << std::endl;
  }

  const uint64_t snapshot = impl->versions_->LastSequence();
  RunQueries(current, target.meta, target.level, read_options, model->string_keys,
             snapshot, warmup_queries, 0);

  adgMod::Stats* stats = adgMod::Stats::GetInstance();
  stats->ResetAll();
  adgMod::lookup_data_blocks_read.store(0, std::memory_order_relaxed);
  adgMod::lookup_read_io_ops.store(0, std::memory_order_relaxed);

  const uint64_t found = RunQueries(
      current, target.meta, target.level, read_options, model->string_keys,
      snapshot, num_queries, warmup_queries);

  const double io_us = static_cast<double>(stats->ReportTime(17));
  const double cpu_us = static_cast<double>(stats->ReportTime(2) +
                                            stats->ReportTime(3) +
                                            stats->ReportTime(15));
  const double total_us = io_us + cpu_us;
  const double comparisons_us = static_cast<double>(stats->ReportTime(3));
  const double k_expected =
      std::ceil((2.0 * static_cast<double>(file_model_error) + 1.0) /
                static_cast<double>(adgMod::block_num_entries));
  const double k_measured =
      num_queries > 0
          ? static_cast<double>(adgMod::lookup_data_blocks_read.load(
                std::memory_order_relaxed)) /
                static_cast<double>(num_queries)
          : 0.0;

  try {
    AppendResult(csv_path, mode, target, adgMod::block_num_entries,
                 file_model_error, k_expected, k_measured, io_us, cpu_us,
                 total_us, comparisons_us, num_queries, found);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    current->Unref();
    delete db;
    return 1;
  }

  std::cout << "mode=" << ModeName(mode)
            << " file_number=" << target.meta->number
            << " level=" << target.level << " B=" << adgMod::block_num_entries
            << " error=" << file_model_error
            << " K_expected=" << std::fixed << std::setprecision(2)
            << k_expected << " K_measured=" << k_measured
            << " io_us=" << io_us << " cpu_us=" << cpu_us
            << " total_us=" << total_us << std::endl;

  current->Unref();
  delete db;
  return 0;
}
