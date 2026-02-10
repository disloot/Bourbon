//
// Created by daiyi on 2020/02/02.
//

#include "learned_index.h"

#include "db/version_set.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <utility>

#include "util/mutexlock.h"

#include "util.h"

namespace adgMod {

std::pair<uint64_t, uint64_t> LearnedIndexData::GetPosition(
    const Slice& target_x) const {
  const uint64_t getposition_start_ns = LookupNowNanos();
  uint64_t getposition_compare_nanos = 0;
  uint64_t segment_compare_calls = 0;
  lookup_getposition_calls.fetch_add(1, std::memory_order_relaxed);
  assert(string_segments.size() > 1);
  ++served;

  // check if the key is within the model bounds
  uint64_t target_int = SliceToInteger(target_x);
  if (target_int > max_key) {
    lookup_getposition_nanos.fetch_add(LookupNowNanos() - getposition_start_ns,
                                       std::memory_order_relaxed);
    return std::make_pair(size, size);
  }
  if (target_int < min_key) {
    lookup_getposition_nanos.fetch_add(LookupNowNanos() - getposition_start_ns,
                                       std::memory_order_relaxed);
    return std::make_pair(size, size);
  }

  // binary search between segments
  uint32_t left = 0, right = (uint32_t)string_segments.size() - 1;
  while (left != right - 1) {
    ++segment_compare_calls;
    uint32_t mid = (right + left) / 2;
    const uint64_t compare_start_ns = LookupNowNanos();
    if (target_int < string_segments[mid].x)
      right = mid;
    else
      left = mid;
    getposition_compare_nanos += LookupNowNanos() - compare_start_ns;
  }

  // calculate the interval according to the selected segment
  double result =
      target_int * string_segments[left].k + string_segments[left].b;
  result = is_level ? result / 2 : result;
  uint64_t lower =
      result - error > 0 ? (uint64_t)std::floor(result - error) : 0;
  uint64_t upper = (uint64_t)std::ceil(result + error);
  if (lower >= size) return std::make_pair(size, size);
  upper = upper < size ? upper : size - 1;
  lookup_getposition_compare_calls.fetch_add(segment_compare_calls,
                                             std::memory_order_relaxed);
  lookup_getposition_compare_nanos.fetch_add(getposition_compare_nanos,
                                             std::memory_order_relaxed);
  lookup_getposition_nanos.fetch_add(LookupNowNanos() - getposition_start_ns,
                                     std::memory_order_relaxed);
  //                printf("%s %s %s\n", string_keys[lower].c_str(),
  //                string(target_x.data(), target_x.size()).c_str(),
  //                string_keys[upper].c_str()); assert(target_x >=
  //                string_keys[lower] && target_x <= string_keys[upper]);
  return std::make_pair(lower, upper);
}

uint64_t LearnedIndexData::MaxPosition() const { return size - 1; }

double LearnedIndexData::GetError() const { return error; }

// Actual function doing learning
bool LearnedIndexData::Learn() {
  // FILL IN GAMMA (error)
  PLR plr = PLR(error);

  // check if data if filled
  if (string_keys.empty()) assert(false);

  // fill in some bounds for the model
  uint64_t temp = atoll(string_keys.back().c_str());
  min_key = atoll(string_keys.front().c_str());
  max_key = atoll(string_keys.back().c_str());
  size = string_keys.size();

  // actual training
  std::vector<Segment> segs = plr.train(string_keys, !is_level);
  if (segs.empty()) return false;
  // fill in a dummy last segment (used in segment binary search)
  segs.push_back((Segment){temp, 0, 0});
  string_segments = std::move(segs);

  for (auto& str : string_segments) {
    // printf("%s %f\n", str.first.c_str(), str.second);
  }

  learned.store(true);
  streaming_learned = false;  // 批式学习
  // string_keys.clear();
  return true;
}

// NEW: 从预构建的 segments 学习（绕过 string_keys 收集）
bool LearnedIndexData::LearnFromSegments(const std::vector<Segment>& segments,
                                         uint64_t min_key, uint64_t max_key,
                                         uint64_t num_keys) {
    if (segments.empty()) return false;

    this->min_key = min_key;
    this->max_key = max_key;
    this->size = num_keys;

    // 添加 dummy last segment（用于 segment binary search）
    string_segments = segments;
    string_segments.push_back(Segment{max_key, 0, 0});

    learned.store(true);
    streaming_learned = true;  // NEW: 标记为流式学习
    return true;
}

// NEW: 直接设置 segments（用于流式 PLR）
void LearnedIndexData::SetSegments(const std::vector<Segment>& segments,
                                   uint64_t min_key, uint64_t max_key,
                                   uint64_t num_keys) {
    LearnFromSegments(segments, min_key, max_key, num_keys);
}

// static learning function to be used with LevelDB background scheduling
// level learning
void LearnedIndexData::LevelLearn(void* arg, bool nolock) {
  Stats* instance = Stats::GetInstance();
  bool success = false;
  bool entered = false;
  instance->StartTimer(8);

  VersionAndSelf* vas = reinterpret_cast<VersionAndSelf*>(arg);
  LearnedIndexData* self = vas->self;
  self->is_level = true;
  self->level = vas->level;
  Version* c;
  if (!nolock) {
    c = db->GetCurrentVersion();
  }
  if (db->version_count == vas->v_count) {
    entered = true;
    if (vas->version->FillLevel(adgMod::read_options, vas->level)) {
      self->filled = true;
      if (db->version_count == vas->v_count) {
        if (env->compaction_awaiting.load() == 0 && self->Learn()) {
          success = true;
        } else {
          self->learning.store(false);
        }
      }
    }
  }
  if (!nolock) {
    adgMod::db->ReturnCurrentVersion(c);
  }

  auto time = instance->PauseTimer(8, true);

  if (entered) {
    self->cost = time.second - time.first;
    learn_counter_mutex.Lock();
    events[1].push_back(new LearnEvent(time, 0, self->level, success));
    levelled_counters[6].Increment(vas->level, time.second - time.first);
    learn_counter_mutex.Unlock();
  }

  delete vas;
}

// static learning function to be used with LevelDB background scheduling
// file learning
uint64_t LearnedIndexData::FileLearn(void* arg) {
  Stats* instance = Stats::GetInstance();
  bool entered = false;
  instance->StartTimer(11);

  MetaAndSelf* mas = reinterpret_cast<MetaAndSelf*>(arg);
  LearnedIndexData* self = mas->self;
  self->level = mas->level;

  // NEW: 如果已经通过流式学习完成，跳过批式学习（防御性编程）
  if (self->learned.load() && self->IsStreamingLearned()) {
    // File already learned via streaming PLR, skip batch learning
    delete mas->meta;
    delete mas;
    instance->PauseTimer(11, false);
    return 0;
  }

  Version* c = db->GetCurrentVersion();
  if (self->FillData(c, mas->meta)) {
    self->Learn();
    entered = true;
  } else {
    self->learning.store(false);
  }
  adgMod::db->ReturnCurrentVersion(c);

  auto time = instance->PauseTimer(11, true);

  if (entered) {
    // count how many file learning are done.
    self->cost = time.second - time.first;
    learn_counter_mutex.Lock();
    events[1].push_back(new LearnEvent(time, 1, self->level, true));
    levelled_counters[11].Increment(mas->level, time.second - time.first);
    learn_counter_mutex.Unlock();
  }

  //        if (fresh_write) {
  //            self->WriteModel(adgMod::db->versions_->dbname_ + "/" +
  //            to_string(mas->meta->number) + ".fmodel");
  //            self->string_keys.clear();
  //            self->num_entries_accumulated.array.clear();
  //        }

  if (!fresh_write) delete mas->meta;
  delete mas;
  return entered ? time.second - time.first : 0;
}

// general model checker
bool LearnedIndexData::Learned() {
  if (learned_not_atomic)
    return true;
  else if (learned.load()) {
    learned_not_atomic = true;
    return true;
  } else
    return false;
}

// level model checker, used to be also learning trigger
bool LearnedIndexData::Learned(Version* version, int v_count, int level) {
  if (learned_not_atomic)
    return true;
  else if (learned.load()) {
    learned_not_atomic = true;
    return true;
  }
  return false;
  //        } else {
  //            if (level_learning_enabled && ++current_seek >= allowed_seek &&
  //            !learning.exchange(true)) {
  //                env->ScheduleLearning(&LearnedIndexData::Learn, new
  //                VersionAndSelf{version, v_count, this, level}, 0);
  //            }
  //            return false;
  //        }
}

// file model checker, used to be also learning trigger
bool LearnedIndexData::Learned(Version* version, int v_count,
                               FileMetaData* meta, int level) {
  if (learned_not_atomic)
    return true;
  else if (learned.load()) {
    learned_not_atomic = true;
    return true;
  } else
    return false;
  //        } else {
  //            if (file_learning_enabled && (true || level != 0 && level != 1)
  //            && ++current_seek >= allowed_seek && !learning.exchange(true)) {
  //                env->ScheduleLearning(&LearnedIndexData::FileLearn, new
  //                MetaAndSelf{version, v_count, meta, this, level}, 0);
  //            }
  //            return false;
  //        }
}

bool LearnedIndexData::FillData(Version* version, FileMetaData* meta) {
  // if (filled) return true;

  if (version->FillData(adgMod::read_options, meta, this)) {
    // filled = true;
    return true;
  }
  return false;
}

void LearnedIndexData::WriteModel(const string& filename) {
  if (!learned.load()) return;

  std::ofstream output_file(filename);
  output_file.precision(15);
  output_file << adgMod::block_num_entries << " " << adgMod::block_size << " "
              << adgMod::entry_size << "\n";
  for (Segment& item : string_segments) {
    output_file << item.x << " " << item.k << " " << item.b << "\n";
  }
  output_file << "StartAcc"
              << " " << min_key << " " << max_key << " " << size << " " << level
              << " " << cost << "\n";
  for (auto& pair : num_entries_accumulated.array) {
    output_file << pair.first << " " << pair.second << "\n";
  }
}

void LearnedIndexData::ReadModel(const string& filename) {
  std::ifstream input_file(filename);

  if (!input_file.good()) return;
  input_file >> adgMod::block_num_entries >> adgMod::block_size >>
      adgMod::entry_size;
  while (true) {
    string x;
    double k, b;
    input_file >> x;
    if (x == "StartAcc") break;
    input_file >> k >> b;
    string_segments.emplace_back(atoll(x.c_str()), k, b);
  }
  input_file >> min_key >> max_key >> size >> level >> cost;
  while (true) {
    uint64_t first;
    string second;
    if (!(input_file >> first >> second)) break;
    num_entries_accumulated.Add(first, std::move(second));
  }

  learned.store(true);
}

void LearnedIndexData::ReportStats() {
  //        double neg_gain, pos_gain;
  //        if (num_neg_model == 0 || num_neg_baseline == 0) {
  //            neg_gain = 0;
  //        } else {
  //            neg_gain = ((double) time_neg_baseline / num_neg_baseline -
  //            (double) time_neg_model / num_neg_model) * num_neg_model;
  //        }
  //        if (num_pos_model == 0 || num_pos_baseline == 0) {
  //            pos_gain = 0;
  //        } else {
  //            pos_gain = ((double) time_pos_baseline / num_pos_baseline -
  //            (double) time_pos_model / num_pos_model) * num_pos_model;
  //        }

  printf("%d %d %lu %lu %lu\n", level, served, string_segments.size(), cost,
         size);  //, file_size);
  //        printf("\tPredicted: %lu %lu %lu %lu %d %d %d %d %d %lf\n",
  //        time_neg_baseline_p, time_neg_model_p, time_pos_baseline_p,
  //        time_pos_model_p,
  //                num_neg_baseline_p, num_neg_model_p, num_pos_baseline_p,
  //                num_pos_model_p, num_files_p, gain_p);
  //        printf("\tActual: %lu %lu %lu %lu %d %d %d %d %f\n",
  //        time_neg_baseline, time_neg_model, time_pos_baseline,
  //        time_pos_model,
  //               num_neg_baseline, num_neg_model, num_pos_baseline,
  //               num_pos_model, pos_gain + neg_gain);
}

void LearnedIndexData::FillCBAStat(bool positive, bool model, uint64_t time) {
  //        int& num_to_update = positive ? (model ? num_pos_model :
  //        num_pos_baseline) : (model ? num_neg_model : num_neg_baseline);
  //        uint64_t& time_to_update =  positive ? (model ? time_pos_model :
  //        time_pos_baseline) : (model ? time_neg_model : time_neg_baseline);
  //        time_to_update += time;
  //        num_to_update += 1;
}

LearnedIndexData* FileLearnedIndexData::GetModel(int number) {
  leveldb::MutexLock l(&mutex);
  if (file_learned_index_data.size() <= number)
    file_learned_index_data.resize(number + 1, nullptr);
  if (file_learned_index_data[number] == nullptr)
    file_learned_index_data[number] = new LearnedIndexData(file_allowed_seek, false);
  return file_learned_index_data[number];
}

bool FileLearnedIndexData::FillData(Version* version, FileMetaData* meta) {
  LearnedIndexData* model = GetModel(meta->number);
  return model->FillData(version, meta);
}

std::vector<std::string>& FileLearnedIndexData::GetData(FileMetaData* meta) {
  auto* model = GetModel(meta->number);
  return model->string_keys;
}

bool FileLearnedIndexData::Learned(Version* version, FileMetaData* meta,
                                   int level) {
  LearnedIndexData* model = GetModel(meta->number);
  return model->Learned(version, db->version_count, meta, level);
}

AccumulatedNumEntriesArray* FileLearnedIndexData::GetAccumulatedArray(
    int file_num) {
  auto* model = GetModel(file_num);
  return &model->num_entries_accumulated;
}

std::pair<uint64_t, uint64_t> FileLearnedIndexData::GetPosition(
    const Slice& key, int file_num) {
  return file_learned_index_data[file_num]->GetPosition(key);
}

FileLearnedIndexData::~FileLearnedIndexData() {
  leveldb::MutexLock l(&mutex);
  for (auto pointer : file_learned_index_data) {
    delete pointer;
  }
}

void FileLearnedIndexData::Report() {
  leveldb::MutexLock l(&mutex);

  std::set<uint64_t> live_files;
  adgMod::db->versions_->AddLiveFiles(&live_files);

  for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
    auto pointer = file_learned_index_data[i];
    if (pointer != nullptr && pointer->cost != 0) {
      printf("FileModel %lu %d ", i, i > watermark);
      pointer->ReportStats();
    }
  }
}

void AccumulatedNumEntriesArray::Add(uint64_t num_entries, string&& key) {
  array.emplace_back(num_entries, key);
}

bool AccumulatedNumEntriesArray::Search(const Slice& key, uint64_t lower,
                                        uint64_t upper, size_t* index,
                                        uint64_t* relative_lower,
                                        uint64_t* relative_upper) {
  if (adgMod::MOD == 4) {
    uint64_t lower_pos = lower / array[0].first;
    uint64_t upper_pos = upper / array[0].first;
    if (lower_pos != upper_pos) {
      while (true) {
        if (lower_pos >= array.size()) return false;
        if (key <= array[lower_pos].second) break;
        lower = array[lower_pos].first;
        ++lower_pos;
      }
      upper = std::min(upper, array[lower_pos].first - 1);
      *index = lower_pos;
      *relative_lower =
          lower_pos > 0 ? lower - array[lower_pos - 1].first : lower;
      *relative_upper =
          lower_pos > 0 ? upper - array[lower_pos - 1].first : upper;
      return true;
    }
    *index = lower_pos;
    *relative_lower = lower % array[0].first;
    *relative_upper = upper % array[0].first;
    return true;

  } else {
    size_t left = 0, right = array.size() - 1;
    while (left < right) {
      size_t mid = (left + right) / 2;
      if (lower < array[mid].first)
        right = mid;
      else
        left = mid + 1;
    }

    if (upper >= array[left].first) {
      while (true) {
        if (left >= array.size()) return false;
        if (key <= array[left].second) break;
        lower = array[left].first;
        ++left;
      }
      upper = std::min(upper, array[left].first - 1);
    }

    *index = left;
    *relative_lower = left > 0 ? lower - array[left - 1].first : lower;
    *relative_upper = left > 0 ? upper - array[left - 1].first : upper;
    return true;
  }
}

bool AccumulatedNumEntriesArray::SearchNoError(uint64_t position, size_t* index,
                                               uint64_t* relative_position) {
  *index = position / array[0].first;
  *relative_position = position % array[0].first;
  return *index < array.size();

  //        size_t left = 0, right = array.size() - 1;
  //        while (left < right) {
  //            size_t mid = (left + right) / 2;
  //            if (position < array[mid].first) right = mid;
  //            else left = mid + 1;
  //        }
  //        *index = left;
  //        *relative_position = left > 0 ? position - array[left - 1].first :
  //        position; return left < array.size();
}

uint64_t AccumulatedNumEntriesArray::NumEntries() const {
  return array.empty() ? 0 : array.back().first;
}

void LearnedIndexData::ComputeLearnabilityStats(LevelLearnabilityStats& stats) {
  stats.level = level;
  stats.num_segments = string_segments.size() > 0 ? string_segments.size() - 1 : 0;
  stats.num_keys = size;
  stats.min_key = min_key;
  stats.max_key = max_key;
  stats.key_range = max_key - min_key;
  stats.key_density = stats.key_range > 0 ? (double)stats.num_keys / stats.key_range : 0;

  // Calculate MAE and Max Error
  double total_error = 0;
  double max_err = 0;
  int sampled_keys = 0;
  int sample_step = std::max(1, stats.num_keys / 1000); // Sample at most 1000 keys for efficiency

  for (size_t i = 0; i < string_keys.size(); i += sample_step) {
    uint64_t actual_pos = i;
    std::pair<uint64_t, uint64_t> predicted = GetPosition(Slice(string_keys[i]));
    uint64_t predicted_pos = (predicted.first + predicted.second) / 2;
    double error = std::abs((int64_t)predicted_pos - (int64_t)actual_pos);
    total_error += error;
    if (error > max_err) max_err = error;
    sampled_keys++;
  }

  stats.mae = sampled_keys > 0 ? total_error / sampled_keys : 0;
  stats.max_error = max_err;

  // Calculate slope variance as a proxy for linearity
  double avg_slope = 0;
  int valid_segments = 0;
  for (size_t i = 0; i < string_segments.size() - 1; i++) {
    avg_slope += string_segments[i].k;
    valid_segments++;
  }
  avg_slope = valid_segments > 0 ? avg_slope / valid_segments : 0;

  double variance = 0;
  for (size_t i = 0; i < string_segments.size() - 1; i++) {
    double diff = string_segments[i].k - avg_slope;
    variance += diff * diff;
  }
  stats.avg_slope_variance = valid_segments > 0 ? variance / valid_segments : 0;

  // Linearity score: inverse of normalized variance (0-1, higher is better)
  // Low variance = high linearity
  double normalized_variance = stats.avg_slope_variance / (avg_slope * avg_slope + 1e-10);
  stats.linearity_score = 1.0 / (1.0 + normalized_variance);
}

void LearnedIndexData::ExportStatsToFile(const std::string& filename) {
  std::ofstream out(filename);
  if (!out.is_open()) {
    std::cerr << "Failed to open file for writing: " << filename << std::endl;
    return;
  }

  LevelLearnabilityStats stats;
  ComputeLearnabilityStats(stats);

  out << "# Level Learnability Statistics\n";
  out << "Level," << stats.level << "\n";
  out << "NumSegments," << stats.num_segments << "\n";
  out << "NumKeys," << stats.num_keys << "\n";
  out << "MinKey," << stats.min_key << "\n";
  out << "MaxKey," << stats.max_key << "\n";
  out << "KeyRange," << stats.key_range << "\n";
  out << "KeyDensity," << stats.key_density << "\n";
  out << "MAE," << stats.mae << "\n";
  out << "MaxError," << stats.max_error << "\n";
  out << "AvgSlopeVariance," << stats.avg_slope_variance << "\n";
  out << "LinearityScore," << stats.linearity_score << "\n";

  // Export segment details
  out << "\n# Segment Details\n";
  out << "SegmentIndex,X,K,B\n";
  for (size_t i = 0; i < string_segments.size(); i++) {
    out << i << "," << string_segments[i].x << ","
        << string_segments[i].k << "," << string_segments[i].b << "\n";
  }

  out.close();
}

}  // namespace adgMod
