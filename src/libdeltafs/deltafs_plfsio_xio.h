/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#pragma once

#include "pdlfs-common/env.h"
#include "pdlfs-common/env_files.h"
#include "pdlfs-common/port.h"

#include <string>

// This module provides the abstraction for accessing data stored in
// an underlying storage using a log-structured format. Data is written,
// append-only, into a "sink", and is read from a "source".

namespace pdlfs {
namespace plfsio {

namespace xio {

// Log types
enum LogType {
  // Default type, contains data blocks
  kData = 0x00,  // Optimized for random read access

  // Index log with table indexes, filters, and other index blocks
  kIndex = 0x01  // Sequential reads expected
};

// Log rotation types
enum RotationType {
  // Do not rotate log files
  kNoRotation = 0x00,
  // Log rotation is controlled by external user code
  kUsrCtrl = 0x01
};

// Options for naming, write buffering, and rolling.
struct LogOptions {
  // Rank # of the calling process
  int rank;

  // Sub-partition index # of the log
  // Set to "-1" to indicate there is no sub-partitions
  int sub_partition;

  // Max write buffering in bytes
  // Set to "0" to disable
  size_t max_buf;

  // Min write buffering in bytes
  // Set to "0" to disable
  size_t min_buf;

  // Log rotation
  RotationType rotation;

  // Type of the log
  LogType type;

  // Allow synchronization among multiple threads
  port::Mutex* mu;

  // Enable I/O monitoring
  WritableFileStats* stats;

  // Low-level storage abstraction
  Env* env;
};

typedef MinMaxBufferedWritableFile BufferedLogFile;

class RollingLogFile;

// Abstraction for writing data to storage.
// Implementation is not thread-safe. External synchronization is needed for
// multi-threaded access.
class LogSink {
 public:
  LogSink(const LogOptions& options, const std::string& prefix,
          BufferedLogFile* buf, RollingLogFile* vf)
      : options_(options),
        prefix_(prefix),
        mu_(options_.mu),
        buf_(buf),
        vf_(vf),
        env_(options_.env),
        offset_(0),
        file_(NULL),
        refs_(0) {}

  // Return the current logic write offset.
  uint64_t Ltell() const {
    if (mu_ != NULL) mu_->AssertHeld();
    return offset_;
  }

  void Lock() {
    if (mu_ != NULL) {
      mu_->Lock();
    }
  }

  // Append data into the storage.
  // Return OK on success, or a non-OK status on errors.
  // May lose data until the next Lsync().
  // REQUIRES: Lclose() has not been called.
  Status Lwrite(const Slice& data) {
    if (file_ == NULL) {
      return Status::AssertionFailed("Log already closed", LogName());
    } else {
      if (mu_ != NULL) {
        mu_->AssertHeld();
      }
      Status result = file_->Append(data);
      if (result.ok()) {
        // File implementation may delay the writing
        result = file_->Flush();
        if (result.ok()) {
          offset_ += data.size();
        }
      }
      return result;
    }
  }

  // Force data to be written to storage.
  // Return OK on success, or a non-OK status on errors.
  // Data previously buffered will be forcefully flushed out.
  Status Lsync() {
    Status status;
    if (file_ != NULL) {
      if (mu_ != NULL) mu_->AssertHeld();
      status = file_->Sync();
    }
    return status;
  }

  void Unlock() {
    if (mu_ != NULL) {
      mu_->Unlock();
    }
  }

  // Open a sink object according to the given set of options.
  // Return OK on success, or a non-OK status on errors.
  static Status Open(
      LogSink** result, const LogOptions& options, Env* env,
      const std::string& prefix,  // Parent directory name
      port::Mutex* io_mutex =
          NULL,  // Allow synchronization among multiple threads
      std::vector<std::string*>* io_bufs =
          NULL,  // Enable memory consumption monitoring
      WritableFileStats* io_stats = NULL  // Enable I/O monitoring
      );
  // Close the log so no further writes will be accepted.
  // Return OK on success, or a non-OK status on errors.
  // If sync is set to true, will force data sync before closing the log.
  Status Lclose(bool sync = false);
  // Flush and close the current log file and redirect
  // all future writes to a new log file.
  Status Lrotate(int index, bool sync = false);
  uint64_t Ptell() const;  // Return the current physical log offset
  void Ref() { refs_++; }
  void Unref();

 private:
  ~LogSink();  // Triggers Finish()
  // No copying allowed
  void operator=(const LogSink&);
  LogSink(const LogSink&);
  std::string LogName() const;
  Status Finish();  // Internally used by Lclose()

  // Constant after construction
  const LogOptions options_;
  const std::string prefix_;  // Parent directory name
  port::Mutex* const mu_;
  BufferedLogFile* const buf_;  // NULL is write buffering is disabled
  // NULL if log rotation is disabled
  RollingLogFile* const vf_;
  Env* const env_;

  // State below is protected by mu_
  Status finish_status_;
  uint64_t offset_;  // Logic write offset, monotonically increasing
  uint64_t prev_offset_;
  // NULL if Finish() has been called
  WritableFile* file_;
  uint32_t refs_;
};
}

}  // namespace plfsio
}  // namespace pdlfs
