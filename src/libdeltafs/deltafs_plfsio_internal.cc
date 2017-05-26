/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_internal.h"

#include "pdlfs-common/crc32c.h"
#include "pdlfs-common/hash.h"
#include "pdlfs-common/logging.h"
#include "pdlfs-common/mutexlock.h"
#include "pdlfs-common/strutil.h"

#include <assert.h>
#include <math.h>
#include <algorithm>

namespace pdlfs {
extern const char* GetLengthPrefixedSlice(const char* p, const char* limit,
                                          Slice* result);
namespace plfsio {

static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

static bool BloomKeyMayMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 2) return true;  // Consider it a match

  const char* array = input.data();
  const size_t bits = (len - 1) * 8;

  // Use the encoded k so that we can read filters generated by
  // bloom filters created using different parameters.
  const size_t k = array[len - 1];
  if (k > 30) {
    // Reserved for potentially new encodings for short bloom filters.
    // Consider it a match.
    return true;
  }

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  for (size_t j = 0; j < k; j++) {
    const uint32_t bitpos = h % bits;
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) {
      return false;
    }
    h += delta;
  }

  return true;
}

class BloomBlock {
 public:
  BloomBlock(size_t bits_per_key, size_t size /* bytes */) {
    finished_ = false;
    space_.reserve(size + 1 + kBlockTrailerSize);
    space_.resize(size, 0);
    // Round down to reduce probing cost a little bit
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
    // Remember # of probes in filter
    space_.push_back(static_cast<char>(k_));
    bits_ = 8 * size;
  }

  ~BloomBlock() {}

  void AddKey(const Slice& key) {
    assert(!finished_);  // Finish() has not been called
    // Use double-hashing to generate a sequence of hash values.
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k_; j++) {
      const uint32_t bitpos = h % bits_;
      space_[bitpos / 8] |= (1 << (bitpos % 8));
      h += delta;
    }
  }

  Slice Finish() {
    assert(!finished_);
    finished_ = true;
    return space_;
  }

  Slice Finalize() {
    assert(finished_);
    Slice contents = space_;  // Contents without the trailer
    char trailer[kBlockTrailerSize];
    trailer[0] = kNoCompression;
    uint32_t crc = crc32c::Value(contents.data(), contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    space_.append(trailer, sizeof(trailer));
    return space_;
  }

 private:
  // No copying allowed
  void operator=(const BloomBlock&);
  BloomBlock(const BloomBlock&);

  bool finished_;
  std::string space_;
  size_t bits_;
  size_t k_;
};

class WriteBuffer::Iter : public Iterator {
 public:
  explicit Iter(const WriteBuffer* write_buffer)
      : cursor_(-1),
        offsets_(&write_buffer->offsets_[0]),
        num_entries_(write_buffer->num_entries_),
        buffer_(write_buffer->buffer_) {}

  virtual ~Iter() {}
  virtual void Next() { cursor_++; }
  virtual void Prev() { cursor_--; }
  virtual Status status() const { return Status::OK(); }
  virtual bool Valid() const { return cursor_ >= 0 && cursor_ < num_entries_; }
  virtual void SeekToFirst() { cursor_ = 0; }
  virtual void SeekToLast() { cursor_ = num_entries_ - 1; }
  virtual void Seek(const Slice& target) {
    // Not supported
  }

  virtual Slice key() const {
    assert(Valid());
    Slice result;
    const char* p = &buffer_[offsets_[cursor_]];
    Slice input = buffer_;
    assert(p - buffer_.data() >= 0);
    input.remove_prefix(p - buffer_.data());
    if (GetLengthPrefixedSlice(&input, &result)) {
      return result;
    } else {
      assert(false);
      result = Slice();
      return result;
    }
  }

  virtual Slice value() const {
    assert(Valid());
    Slice result;
    const char* p = &buffer_[offsets_[cursor_]];
    Slice input = buffer_;
    assert(p - buffer_.data() >= 0);
    input.remove_prefix(p - buffer_.data());
    if (GetLengthPrefixedSlice(&input, &result) &&
        GetLengthPrefixedSlice(&input, &result)) {
      return result;
    } else {
      assert(false);
      result = Slice();
      return result;
    }
  }

 private:
  int cursor_;
  const uint32_t* offsets_;
  int num_entries_;
  Slice buffer_;
};

Iterator* WriteBuffer::NewIterator() const {
  assert(finished_);
  return new Iter(this);
}

namespace {
struct STLLessThan {
  Slice buffer_;

  STLLessThan(const std::string& buffer) : buffer_(buffer) {}
  bool operator()(uint32_t a, uint32_t b) {
    Slice key_a = GetKey(a);
    Slice key_b = GetKey(b);
    assert(!key_a.empty() && !key_b.empty());
    return key_a < key_b;
  }

  Slice GetKey(uint32_t offset) {
    Slice result;
    bool ok = GetLengthPrefixedSlice(buffer_.data() + offset,
                                     buffer_.data() + buffer_.size(),  // Limit
                                     &result);
    if (ok) {
      return result;
    } else {
      assert(false);
      return result;
    }
  }
};
}  // namespace

void WriteBuffer::Finish() {
  // Sort entries
  assert(!finished_);
  std::vector<uint32_t>::iterator begin = offsets_.begin();
  std::vector<uint32_t>::iterator end = offsets_.end();
  std::sort(begin, end, STLLessThan(buffer_));
  finished_ = true;
}

void WriteBuffer::Reset() {
  num_entries_ = 0;
  finished_ = false;
  offsets_.clear();
  buffer_.clear();
}

void WriteBuffer::Reserve(uint32_t num_entries, size_t buffer_size) {
  buffer_.reserve(buffer_size);
  offsets_.reserve(num_entries);
}

void WriteBuffer::Add(const Slice& key, const Slice& value) {
  assert(!finished_);       // Finish() has not been called
  assert(key.size() != 0);  // Key cannot be empty
  const size_t offset = buffer_.size();
  PutLengthPrefixedSlice(&buffer_, key);
  PutLengthPrefixedSlice(&buffer_, value);
  offsets_.push_back(static_cast<uint32_t>(offset));
  num_entries_++;
}

TableLogger::TableLogger(const DirOptions& options, LogSink* data,
                         LogSink* index)
    : options_(options),
      num_uncommitted_index_(0),
      num_uncommitted_data_(0),
      data_block_(16),
      index_block_(1),
      meta_block_(1),
      pending_index_entry_(false),
      pending_meta_entry_(false),
      num_tables_(0),
      num_epochs_(0),
      data_sink_(data),
      meta_sink_(index),
      finished_(false) {
  // Sanity checks
  assert(meta_sink_ != NULL && data_sink_ != NULL);

  meta_sink_->Ref();
  data_sink_->Ref();

  // Allocate memory
  const size_t estimated_index_size_per_table = 4 << 10;
  index_block_.Reserve(estimated_index_size_per_table);
  const size_t estimated_meta_size = 16 << 10;
  meta_block_.Reserve(estimated_meta_size);

  uncommitted_indexes_.reserve(1 << 10);
  data_block_.buffer_store()->reserve(options_.block_buffer);
  data_block_.buffer_store()->clear();
  data_block_.SwitchBuffer(NULL);
  data_block_.Reset();
}

TableLogger::~TableLogger() {
  meta_sink_->Unref();
  data_sink_->Unref();
}

void TableLogger::EndEpoch() {
  assert(!finished_);  // Finish() has not been called
  EndTable(static_cast<BloomBlock*>(NULL));
  if (!ok()) {
    return;  // Abort
  } else if (num_tables_ == 0) {
    return;  // Empty epoch
  } else if (num_epochs_ >= kMaxEpoches) {
    status_ = Status::AssertionFailed("Too many epochs");
  } else {
    num_tables_ = 0;
    num_epochs_++;
  }
}

template <typename T>
void TableLogger::EndTable(T* filter_block) {
  assert(!finished_);  // Finish() has not been called

  Flush();
  if (!ok()) {
    return;
  } else if (pending_index_entry_) {
    BytewiseComparator()->FindShortSuccessor(&last_key_);
    PutLengthPrefixedSlice(&uncommitted_indexes_, last_key_);
    pending_index_handle_.EncodeTo(&uncommitted_indexes_);
    pending_index_entry_ = false;
    num_uncommitted_index_++;
  }

  Commit();
  if (!ok()) {
    return;
  } else if (index_block_.empty()) {
    return;  // Empty table
  }

  Slice contents = index_block_.Finish();
  const size_t size = contents.size();
  Slice final_contents =
      index_block_.Finalize();  // No zero padding necessary for index blocks
  const uint64_t offset = meta_sink_->Ltell();
  status_ = meta_sink_->Lwrite(final_contents);
  if (!ok()) return;  // Abort

  size_t filter_size = 0;
  const uint64_t filter_offset = meta_sink_->Ltell();
  Slice final_filter_contents;

  if (filter_block != NULL) {
    Slice filer_contents = filter_block->Finish();
    filter_size = filer_contents.size();
    final_filter_contents = filter_block->Finalize();
    status_ = meta_sink_->Lwrite(final_filter_contents);
  } else {
    // No filter configured
  }

  if (ok()) {
    index_block_.Reset();
    pending_meta_handle_.set_filter_offset(filter_offset);
    pending_meta_handle_.set_filter_size(filter_size);
    pending_meta_handle_.set_offset(offset);
    pending_meta_handle_.set_size(size);
    assert(!pending_meta_entry_);
    pending_meta_entry_ = true;
  } else {
    return;  // Abort
  }

  if (num_tables_ >= kMaxTablesPerEpoch) {
    status_ = Status::AssertionFailed("Too many tables");
  } else if (pending_meta_entry_) {
    pending_meta_handle_.set_smallest_key(smallest_key_);
    BytewiseComparator()->FindShortSuccessor(&largest_key_);
    pending_meta_handle_.set_largest_key(largest_key_);
    std::string handle_encoding;
    pending_meta_handle_.EncodeTo(&handle_encoding);
    meta_block_.Add(EpochKey(num_epochs_, num_tables_), handle_encoding);
    pending_meta_entry_ = false;
  }

  if (ok()) {
    smallest_key_.clear();
    largest_key_.clear();
    last_key_.clear();
    num_tables_++;
  }
}

void TableLogger::Commit() {
  assert(!finished_);  // Finish() has not been called
  if (data_block_.buffer_store()->empty()) return;  // Empty commit
  if (!ok()) return;                                // Abort

  assert(num_uncommitted_data_ == num_uncommitted_index_);
  const size_t offset = data_sink_->Ltell();
  status_ = data_sink_->Lwrite(*data_block_.buffer_store());
  if (!ok()) return;  // Abort

  Slice key;
  int num_index_committed = 0;
  Slice input = uncommitted_indexes_;
  std::string handle_encoding;
  BlockHandle handle;
  while (!input.empty()) {
    if (GetLengthPrefixedSlice(&input, &key)) {
      handle.DecodeFrom(&input);
      handle.set_offset(offset + handle.offset());
      handle.EncodeTo(&handle_encoding);
      index_block_.Add(key, handle_encoding);
      num_index_committed++;
    } else {
      break;
    }
  }

  assert(num_index_committed == num_uncommitted_index_);
  num_uncommitted_data_ = num_uncommitted_index_ = 0;
  uncommitted_indexes_.clear();
  data_block_.buffer_store()->clear();
  data_block_.SwitchBuffer(NULL);
  data_block_.Reset();
}

void TableLogger::Flush() {
  assert(!finished_);               // Finish() has not been called
  if (data_block_.empty()) return;  // Empty block
  if (!ok()) return;                // Abort

  Slice contents = data_block_.Finish();
  const size_t size = contents.size();
  Slice final_contents;
  if (options_.block_padding) {
    final_contents = data_block_.Finalize(options_.block_size);
  } else {
    final_contents = data_block_.Finalize();
  }

  const size_t final_size = final_contents.size();
  const uint64_t offset = data_block_.buffer_store()->size() - final_size;

  if (ok()) {
    data_block_.SwitchBuffer(NULL);
    data_block_.Reset();
    pending_index_handle_.set_size(size);
    pending_index_handle_.set_offset(offset);
    assert(!pending_index_entry_);
    pending_index_entry_ = true;
    num_uncommitted_data_++;
  }
}

void TableLogger::Add(const Slice& key, const Slice& value) {
  assert(!finished_);       // Finish() has not been called
  assert(key.size() != 0);  // Key cannot be empty
  if (!ok()) return;        // Abort

  if (!last_key_.empty()) {
    // Keys within a single table are expected to be added in a sorted order.
    assert(key.compare(last_key_) >= 0);
    if (options_.unique_keys) {
      // Duplicated keys are not allowed
      assert(key.compare(last_key_) != 0);
    }
  }
  if (smallest_key_.empty()) {
    smallest_key_ = key.ToString();
  }
  largest_key_ = key.ToString();

  // Add an index entry if there is one pending insertion
  if (pending_index_entry_) {
    BytewiseComparator()->FindShortestSeparator(&last_key_, key);
    PutLengthPrefixedSlice(&uncommitted_indexes_, last_key_);
    pending_index_handle_.EncodeTo(&uncommitted_indexes_);
    pending_index_entry_ = false;
    num_uncommitted_index_++;
  }

  // Commit all flushed data blocks
  if (data_block_.buffer_store()->size() >= options_.block_buffer) {
    Commit();
  }

  last_key_ = key.ToString();
  data_block_.Add(key, value);
  if (data_block_.CurrentSizeEstimate() + kBlockTrailerSize >=
      static_cast<uint64_t>(options_.block_size * options_.block_util)) {
    Flush();
  }
}

Status TableLogger::Finish() {
  assert(!finished_);  // Finish() has not been called
  EndEpoch();
  finished_ = true;
  if (!ok()) return status_;
  BlockHandle epoch_index_handle;
  std::string tail;
  Footer footer;

  assert(!pending_meta_entry_);
  Slice contents = meta_block_.Finish();
  const size_t size = contents.size();
  Slice final_contents =
      meta_block_.Finalize();  // No padding is needed for metadata blocks
  const uint64_t offset = meta_sink_->Ltell();
  status_ = meta_sink_->Lwrite(final_contents);
  if (!ok()) return status_;

  epoch_index_handle.set_size(size);
  epoch_index_handle.set_offset(offset);
  footer.set_epoch_index_handle(epoch_index_handle);
  footer.set_num_epoches(num_epochs_);
  footer.EncodeTo(&tail);

  if (options_.tail_padding) {
    // Add enough padding to ensure the final size of the index log
    // is some multiple of the physical write size.
    const uint64_t total_size = meta_sink_->Ltell() + tail.size();
    const size_t overflow = total_size % options_.index_buffer;
    if (overflow != 0) {
      const size_t n = options_.index_buffer - overflow;
      status_ = meta_sink_->Lwrite(std::string(n, 0));
    } else {
      // No need to pad
    }
  }

  if (ok()) {
    status_ = meta_sink_->Lwrite(tail);
    return status_;
  } else {
    return status_;
  }
}

PlfsIoLogger::PlfsIoLogger(const DirOptions& options, port::Mutex* mu,
                           port::CondVar* cv, LogSink* data, LogSink* index,
                           CompactionStats* stats)
    : options_(options),
      mu_(mu),
      bg_cv_(cv),
      data_(data),
      index_(index),
      stats_(stats),
      has_bg_compaction_(false),
      pending_epoch_flush_(false),
      pending_finish_(false),
      table_logger_(options, data, index),
      mem_buf_(NULL),
      imm_buf_(NULL),
      imm_buf_is_epoch_flush_(false),
      imm_buf_is_finish_(false) {
  // Sanity checks
  assert(mu != NULL && cv != NULL);

  // Determine the right table size and bloom filter size.
  // Works best when the key and value sizes are fixed.
  //
  // Otherwise, if the estimated key or value sizes are greater
  // than the real average, filter will be allocated with less bytes
  // and there will be higher false positive rate.
  //
  // On the other hand, if the estimated sizes are less than
  // the real, filter will waste memory and each
  // write buffer will be allocated with
  // less memory.
  size_t overhead_per_entry = static_cast<size_t>(
      4 + VarintLength(options_.key_size) + VarintLength(options_.value_size));
  size_t bytes_per_entry =
      options_.key_size + options_.value_size + overhead_per_entry;

  size_t total_bits_per_entry = 8 * bytes_per_entry + options_.bf_bits_per_key;

  // Estimated amount of entries per table
  entries_per_tb_ = static_cast<uint32_t>(ceil(
      8.0 * double(options_.memtable_buffer) / double(total_bits_per_entry)));
  entries_per_tb_ /= (1 << options_.lg_parts);  // Due to data partitioning

  entries_per_tb_ /= 2;  // Due to double buffering

  tb_bytes_ = entries_per_tb_ * bytes_per_entry;
  // Compute bloom filter size (in both bits and bytes)
  bf_bits_ = entries_per_tb_ * options_.bf_bits_per_key;
  // For small n, we can see a very high false positive rate.
  // Fix it by enforcing a minimum bloom filter length.
  if (bf_bits_ > 0 && bf_bits_ < 64) {
    bf_bits_ = 64;
  }

  bf_bytes_ = (bf_bits_ + 7) / 8;
  bf_bits_ = bf_bytes_ * 8;

#if VERBOSE >= 2
  Verbose(__LOG_ARGS__, 2, "C: plfsdir.memtable.tb_size -> %d x %s",
          2 * (1 << options_.lg_parts), PrettySize(tb_bytes_).c_str());
  Verbose(__LOG_ARGS__, 2, "C: plfsdir.memtable.bf_size -> %d x %s",
          2 * (1 << options_.lg_parts), PrettySize(bf_bytes_).c_str());
#endif

  // Allocate memory
  buf0_.Reserve(entries_per_tb_, tb_bytes_);
  buf1_.Reserve(entries_per_tb_, tb_bytes_);

  mem_buf_ = &buf0_;
}

PlfsIoLogger::~PlfsIoLogger() {
  mu_->AssertHeld();
  while (has_bg_compaction_) {
    bg_cv_->Wait();
  }
}

// Block until compaction finishes.
Status PlfsIoLogger::Wait() {
  mu_->AssertHeld();
  while (has_bg_compaction_) {
    bg_cv_->Wait();
  }

  return Status::OK();
}

// Close log files.
Status PlfsIoLogger::Close() {
  mu_->AssertHeld();
  mu_->Unlock();
  Status s = data_->Lclose();
  if (s.ok()) {
    s = index_->Lclose();
  }
  mu_->Lock();
  return s;
}

// If dry_run is set, we will only perform status checks (which includes write
// errors, buffer space, and compaction queue depth) such that no
// compaction jobs will be scheduled.
Status PlfsIoLogger::Finish(bool dry_run) {
  mu_->AssertHeld();
  while (pending_finish_ ||
         pending_epoch_flush_ ||  // The previous job is still in-progress
         imm_buf_ != NULL) {      // There's an on-going compaction job
    if (dry_run || options_.non_blocking) {
      return Status::BufferFull(Slice());
    } else {
      bg_cv_->Wait();
    }
  }

  Status status;
  if (dry_run) {
    // Status check only
    status = table_logger_.status();
  } else {
    pending_finish_ = true;
    pending_epoch_flush_ = true;
    status = Prepare(pending_epoch_flush_, pending_finish_);
    if (!status.ok()) {
      pending_epoch_flush_ = false;  // Avoid blocking future attempts
      pending_finish_ = false;
    } else if (status.ok() && !options_.non_blocking) {
      while (pending_epoch_flush_ || pending_finish_) {
        bg_cv_->Wait();
      }
    }
  }

  return status;
}

// If dry_run is set, we will only perform status checks (which includes write
// errors, buffer space, and compaction queue depth) such that no
// compaction jobs will be scheduled.
Status PlfsIoLogger::MakeEpoch(bool dry_run) {
  mu_->AssertHeld();
  while (pending_epoch_flush_ ||  // The previous job is still in-progress
         imm_buf_ != NULL) {      // There's an on-going compaction job
    if (dry_run || options_.non_blocking) {
      return Status::BufferFull(Slice());
    } else {
      bg_cv_->Wait();
    }
  }

  Status status;
  if (dry_run) {
    // Status check only
    status = table_logger_.status();
  } else {
    pending_epoch_flush_ = true;
    status = Prepare(pending_epoch_flush_);
    if (!status.ok()) {
      pending_epoch_flush_ = false;  // Avoid blocking future attempts
    } else if (status.ok() && !options_.non_blocking) {
      while (pending_epoch_flush_) {
        bg_cv_->Wait();
      }
    }
  }

  return status;
}

Status PlfsIoLogger::Add(const Slice& key, const Slice& value) {
  mu_->AssertHeld();
  Status status = Prepare();
  if (status.ok()) {
    mem_buf_->Add(key, value);
  }

  return status;
}

Status PlfsIoLogger::Prepare(bool flush, bool finish) {
  mu_->AssertHeld();
  Status status;
  assert(mem_buf_ != NULL);
  while (true) {
    if (!table_logger_.ok()) {
      status = table_logger_.status();
      break;
    } else if (!flush && mem_buf_->CurrentBufferSize() < tb_bytes_) {
      // There is room in current write buffer
      break;
    } else if (imm_buf_ != NULL) {
      if (options_.non_blocking) {
        status = Status::BufferFull(Slice());
        break;
      } else {
        bg_cv_->Wait();
      }
    } else {
      // Attempt to switch to a new write buffer
      assert(imm_buf_ == NULL);
      imm_buf_ = mem_buf_;
      if (flush) imm_buf_is_epoch_flush_ = true;
      flush = false;
      if (finish) imm_buf_is_finish_ = true;
      finish = false;
      WriteBuffer* const current_buf = mem_buf_;
      MaybeScheduleCompaction();
      if (current_buf == &buf0_) {
        mem_buf_ = &buf1_;
      } else {
        mem_buf_ = &buf0_;
      }
    }
  }

  return status;
}

void PlfsIoLogger::MaybeScheduleCompaction() {
  mu_->AssertHeld();

  if (has_bg_compaction_) return;  // Skip if there is one already scheduled
  if (imm_buf_ == NULL) return;    // Nothing to be scheduled

  has_bg_compaction_ = true;

  if (options_.compaction_pool != NULL) {
    options_.compaction_pool->Schedule(PlfsIoLogger::BGWork, this);
  } else {
    Env::Default()->Schedule(PlfsIoLogger::BGWork, this);
  }
}

void PlfsIoLogger::BGWork(void* arg) {
  PlfsIoLogger* ins = reinterpret_cast<PlfsIoLogger*>(arg);
  MutexLock ml(ins->mu_);
  ins->DoCompaction();
}

void PlfsIoLogger::DoCompaction() {
  mu_->AssertHeld();
  assert(has_bg_compaction_);
  assert(imm_buf_ != NULL);
  CompactWriteBuffer();
  imm_buf_->Reset();
  imm_buf_is_epoch_flush_ = false;
  imm_buf_is_finish_ = false;
  imm_buf_ = NULL;
  has_bg_compaction_ = false;
  MaybeScheduleCompaction();
  bg_cv_->SignalAll();
}

void PlfsIoLogger::CompactWriteBuffer() {
  mu_->AssertHeld();
  WriteBuffer* const buffer = imm_buf_;
  assert(buffer != NULL);
  const bool pending_finish = pending_finish_;
  const bool pending_epoch_flush = pending_epoch_flush_;
  const bool is_finish = imm_buf_is_finish_;
  const bool is_epoch_flush = imm_buf_is_epoch_flush_;
  TableLogger* const dest = &table_logger_;
  const size_t bf_bits_per_key = options_.bf_bits_per_key;
  const size_t bf_bytes = bf_bytes_;
  uint64_t data_offset = data_->Ltell();
  uint64_t index_offset = index_->Ltell();
  mu_->Unlock();
  uint64_t start = Env::Default()->NowMicros();
#if VERBOSE >= 3
  Verbose(__LOG_ARGS__, 3, "Compacting write buffer ...");
  unsigned long long key_size = 0;
  unsigned long long val_size = 0;
  unsigned num_keys = 0;
#endif

  BloomBlock* bloom_filter = NULL;
  if (bf_bits_per_key != 0 && bf_bytes != 0) {
    bloom_filter = new BloomBlock(bf_bits_per_key, bf_bytes);
  }
  buffer->Finish();
  Iterator* const iter = buffer->NewIterator();
  iter->SeekToFirst();
  for (; iter->Valid(); iter->Next()) {
#if VERBOSE >= 2
    val_size += iter->value().size();
    key_size += iter->key().size();
    num_keys++;
#endif
    if (bloom_filter != NULL) {
      bloom_filter->AddKey(iter->key());
    }
    dest->Add(iter->key(), iter->value());
    if (!dest->ok()) {
      break;
    }
  }

  if (dest->ok()) {
    dest->EndTable(bloom_filter);
  }
  if (is_epoch_flush) {
    dest->EndEpoch();
  }
  if (is_finish) {
    dest->Finish();
  }

  uint64_t end = Env::Default()->NowMicros();

#if VERBOSE >= 3
  Verbose(__LOG_ARGS__, 3, "Compaction done: %d entries (%d us)", num_keys,
          static_cast<int>(end - start));
#endif

  delete iter;
  delete bloom_filter;
  mu_->Lock();
  stats_->data_size += data_->Ltell() - data_offset;
  stats_->index_size += index_->Ltell() - index_offset;
  stats_->write_micros += end - start;
  if (is_epoch_flush) {
    if (pending_epoch_flush) {
      pending_epoch_flush_ = false;
    }
  }
  if (is_finish) {
    if (pending_finish) {
      pending_finish_ = false;
    }
  }
}

template <typename T>
static Status ReadBlock(LogSource* file, const DirOptions& options,
                        const T& handle, BlockContents* result,
                        bool has_checksums = true) {
  result->data = Slice();
  result->heap_allocated = false;
  result->cachable = false;

  assert(file != NULL);
  size_t n = static_cast<size_t>(handle.size());
  size_t m = n;
  if (has_checksums) {
    m += kBlockTrailerSize;
  }
  char* buf = new char[m];
  Slice contents;
  Status s = file->Read(handle.offset(), m, &contents, buf);
  if (s.ok()) {
    if (contents.size() != m) {
      s = Status::Corruption("truncated block read");
    }
  }
  if (!s.ok()) {
    delete[] buf;
    return s;
  }

  // CRC checks
  const char* data = contents.data();  // Pointer to where read put the data
  if (has_checksums && options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      delete[] buf;
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
  }

  if (data != buf) {
    // File implementation gave us pointer to some other data.
    // Use it directly under the assumption that it will be live
    // while the file is open.
    delete[] buf;
    result->data = Slice(data, n);
    result->heap_allocated = false;
    result->cachable = false;  // Avoid double cache
  } else {
    result->data = Slice(buf, n);
    result->heap_allocated = true;
    result->cachable = true;
  }

  return s;
}

// Retrieve value from a given block and
// call "saver" using the value found.
// In addition, set *eok to true if a larger key has been observed.
// Return OK on success and a non-OK status on errors.
Status PlfsIoReader::Get(const Slice& key, const BlockHandle& handle,
                         Saver saver, void* arg, bool* eok) {
  *eok = false;
  Status status;
  BlockContents contents;
  status = ReadBlock(data_src_, options_, handle, &contents);
#if VERBOSE >= 6
  Verbose(__LOG_ARGS__, 6, "[DBLK] read: (offset=%llu, size=%llu) %s",
          static_cast<unsigned long long>(handle.offset()),
          static_cast<unsigned long long>(handle.size()),
          status.ToString().c_str());
#endif

  if (!status.ok()) {
    return status;
  }

  Block* block = new Block(contents);
  Iterator* const iter = block->NewIterator(BytewiseComparator());
  if (options_.unique_keys) {
    iter->Seek(key);
  } else {
    iter->SeekToFirst();
    while (iter->Valid() && key.compare(iter->key()) > 0) {
      iter->Next();
    }
  }
  while (!(*eok) && iter->Valid()) {
    if (iter->key() == key) {
      saver(arg, key, iter->value());
      if (options_.unique_keys) {
        *eok = true;
      }
    } else {
      *eok = true;
    }

    iter->Next();
  }

  status = iter->status();

  delete iter;
  delete block;
  return status;
}

bool PlfsIoReader::KeyMayMatch(const Slice& key, const BlockHandle& handle) {
  Status status;
  BlockContents contents;
  status = ReadBlock(index_src_, options_, handle, &contents);
#if VERBOSE >= 6
  Verbose(__LOG_ARGS__, 6, "[FBLK] read: (offset=%llu, size=%llu) %s",
          static_cast<unsigned long long>(handle.offset()),
          static_cast<unsigned long long>(handle.size()),
          status.ToString().c_str());
#endif

  if (status.ok()) {
    bool r = BloomKeyMayMatch(key, contents.data);
    if (contents.heap_allocated) {
      delete[] contents.data.data();
    }
    return r;
  } else {
    return true;
  }
}

// Retrieve value from a given table and
// call "saver" using the value found.
// Return OK on success and a non-OK status on errors.
Status PlfsIoReader::Get(const Slice& key, const TableHandle& handle,
                         Saver saver, void* arg) {
  Status status;
  // Check key range and filter
  if (key.compare(handle.smallest_key()) < 0 ||
      key.compare(handle.largest_key()) > 0) {
    return status;
  } else {
    BlockHandle filter;
    filter.set_offset(handle.filter_offset());
    filter.set_size(handle.filter_size());
    if (filter.size() != 0 && !KeyMayMatch(key, filter)) {
      return status;
    }
  }

  BlockContents contents;
  status = ReadBlock(index_src_, options_, handle, &contents);
#if VERBOSE >= 6
  Verbose(__LOG_ARGS__, 6, "[IBLK] read: (offset=%llu, size=%llu) %s",
          static_cast<unsigned long long>(handle.offset()),
          static_cast<unsigned long long>(handle.size()),
          status.ToString().c_str());
#endif
  if (!status.ok()) {
    return status;
  }

  bool eok = false;
  Block* block = new Block(contents);
  Iterator* const iter = block->NewIterator(BytewiseComparator());
  if (options_.unique_keys) {
    iter->Seek(key);
  } else {
    iter->SeekToFirst();
    while (iter->Valid() && key.compare(iter->key()) > 0) {
      iter->Next();
    }
  }
  while (status.ok() && !eok && iter->Valid()) {
    BlockHandle h;
    Slice input = iter->value();
    status = h.DecodeFrom(&input);
    if (status.ok()) {
      status = Get(key, h, saver, arg, &eok);
    }

    iter->Next();
  }

  if (status.ok()) {
    status = iter->status();
  }

  delete iter;
  delete block;
  return status;
}

namespace {
struct SaverState {
  std::string* dst;
  bool found;
};

static void SaveValue(void* arg, const Slice& key, const Slice& value) {
  SaverState* state = reinterpret_cast<SaverState*>(arg);
  state->dst->append(value.data(), value.size());
  state->found = true;
}

static inline Iterator* NewEpochIterator(Block* epoch_index) {
  return epoch_index->NewIterator(BytewiseComparator());
}
}  // namespace

Status PlfsIoReader::Get(const Slice& key, uint32_t epoch, std::string* dst) {
  Status status;
  if (epoch_iter_ == NULL) {
    epoch_iter_ = NewEpochIterator(epoch_index_);
  }
  std::string epoch_key;
  uint32_t table = 0;
  while (status.ok()) {
    SaverState state;
    state.found = false;
    state.dst = dst;
    TableHandle handle;
    epoch_key = EpochKey(epoch, table);
    if (!epoch_iter_->Valid() || epoch_iter_->key() != epoch_key) {
      epoch_iter_->Seek(epoch_key);
      if (!epoch_iter_->Valid()) {
        break;
      } else if (epoch_iter_->key() != epoch_key) {
        break;
      }
    }
    Slice handle_encoding = epoch_iter_->value();
    status = handle.DecodeFrom(&handle_encoding);
    if (status.ok()) {
      status = Get(key, handle, SaveValue, &state);
      if (status.ok() && state.found) {
        if (options_.unique_keys) {
          break;
        }
      }
    }

    epoch_iter_->Next();
    table++;
  }

  if (status.ok()) {
    status = epoch_iter_->status();
  }

  return status;
}

Status PlfsIoReader::Gets(const Slice& key, std::string* dst) {
  Status status;
  if (num_epoches_ != 0) {
    if (epoch_iter_ == NULL) {
      epoch_iter_ = NewEpochIterator(epoch_index_);
    }
    uint32_t epoch = 0;
    while (status.ok()) {
      status = Get(key, epoch, dst);
      if (epoch < num_epoches_ - 1) {
        epoch++;
      } else {
        break;
      }
    }
  }

  return status;
}

PlfsIoReader::PlfsIoReader(const DirOptions& o, LogSource* d, LogSource* i)
    : options_(o),
      num_epoches_(0),
      epoch_iter_(NULL),
      epoch_index_(NULL),
      index_src_(i),
      data_src_(d) {
  assert(index_src_ != NULL && data_src_ != NULL);
  index_src_->Ref();
  data_src_->Ref();
}

PlfsIoReader::~PlfsIoReader() {
  delete epoch_iter_;
  delete epoch_index_;
  index_src_->Unref();
  data_src_->Unref();
}

Status PlfsIoReader::Open(const DirOptions& options, LogSource* data,
                          LogSource* index, PlfsIoReader** result) {
  *result = NULL;
  Status status;
  char space[Footer::kEncodeLength];
  Slice input;
  if (index->Size() >= sizeof(space)) {
    status = index->Read(index->Size() - sizeof(space), sizeof(space), &input,
                         space);
  } else {
    status = Status::Corruption("index too short to be valid");
  }

#if VERBOSE >= 6
  Verbose(__LOG_ARGS__, 6, "[TAIL] read: (size=%llu) %s",
          static_cast<unsigned long long>(sizeof(space)),
          status.ToString().c_str());
#endif

  if (!status.ok()) {
    return status;
  }

  Footer footer;
  status = footer.DecodeFrom(&input);
  if (!status.ok()) {
    return status;
  }

  BlockContents contents;
  const BlockHandle& handle = footer.epoch_index_handle();
  status = ReadBlock(index, options, handle, &contents);
#if VERBOSE >= 6
  Verbose(__LOG_ARGS__, 6, "[EIDX] read: (offset=%llu, size=%llu) %s",
          static_cast<unsigned long long>(handle.offset()),
          static_cast<unsigned long long>(handle.size()),
          status.ToString().c_str());
#endif

  if (!status.ok()) {
    return status;
  }

  PlfsIoReader* reader = new PlfsIoReader(options, data, index);
  reader->num_epoches_ = footer.num_epoches();
  Block* block = new Block(contents);
  reader->epoch_index_ = block;

  *result = reader;
  return status;
}

}  // namespace plfsio
}  // namespace pdlfs
