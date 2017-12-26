/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "io_client.h"

#include "deltafs/deltafs_api.h"
#include "pdlfs-common/pdlfs_config.h"
#include "pdlfs-common/strutil.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#if defined(PDLFS_GLOG)
#include <glog/logging.h>
#endif

namespace pdlfs {
namespace ioclient {

enum { PLFSDIR_DISABLED, PLFSDIR_READ, PLFSDIR_WRITE };

static int FLAGS_plfsdir = PLFSDIR_DISABLED;

static const mode_t IO_FILEPERMS =
    (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);  // rw-r--r--
static const mode_t IO_DIRPERMS =
    (S_IRWXU | S_IRGRP | S_IROTH | S_IXGRP | S_IXOTH);  // rwxr-x-r-x

static Status IOError(const std::string& target) {
  if (errno != 0) {
    return Status::IOError(target, strerror(errno));
  } else {
    return Status::IOError(target);
  }
}

// Be very verbose
static const bool kVVerbose = false;
// Be verbose
static const bool kVerbose = true;

// IOClient implementation layered on top of deltafs API.
class DeltafsClient : public IOClient {
 public:
  explicit DeltafsClient() {}
  virtual ~DeltafsClient() {}

  struct DeltafsDir : public Dir {
    explicit DeltafsDir(int fd) : fd(fd) {}
    virtual ~DeltafsDir() {}
    int fd;
  };

  static inline DeltafsDir* ToDeltafsDir(Dir* dir) {
    assert(dir != NULL);
#ifndef NDEBUG
    return dynamic_cast<DeltafsDir*>(dir);
#else
    return static_cast<DeltafsDir*>(dir);
#endif
  }

  // Common FS operations
  virtual Status NewFile(const std::string& path);
  virtual Status DelFile(const std::string& path);
  virtual Status MakeDir(const std::string& path);
  virtual Status GetAttr(const std::string& path);
  virtual Status OpenDir(const std::string& path, Dir**);
  virtual Status FlushEpoch(Dir* dir);
  virtual Status CloseDir(Dir* dir);
  virtual Status AppendAt(Dir* dir, const std::string& file, const char* data,
                          size_t size);

  virtual Status Dispose();
  virtual Status Init();
};

// Convenient method for printing status lines
static inline void print(const Status& s) {
  printf("> %s\n", s.ToString().c_str());
}

Status DeltafsClient::Init() {
  Status s;
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_init...\n");
#endif
  int r = deltafs_nonop();
  if (r != 0) {
    s = IOError(".");
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::Dispose() {
#if defined(PDLFS_GLOG)
  google::FlushLogFiles(google::GLOG_INFO);
#endif
  return Status::OK();
}

Status DeltafsClient::NewFile(const std::string& path) {
  const char* p = path.c_str();
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_mkfile %s...\n", p);
#endif
  Status s;
  int r = deltafs_mkfile(p, IO_FILEPERMS);
  if (r != 0) {
    s = IOError(path);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::DelFile(const std::string& path) {
  const char* p = path.c_str();
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_unlink %s...\n", p);
#endif
  Status s;
  int r = deltafs_unlink(p);
  if (r != 0) {
    s = IOError(path);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::MakeDir(const std::string& path) {
  const char* p = path.c_str();
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_mkdir %s...\n", p);
#endif
  Status s;
  int r = deltafs_mkdir(
      p, IO_DIRPERMS |
             (FLAGS_plfsdir != PLFSDIR_DISABLED ? DELTAFS_DIR_PLFS_STYLE : 0));
  if (r != 0) {
    s = IOError(path);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::GetAttr(const std::string& path) {
  const char* p = path.c_str();
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_stat %s...\n", p);
#endif
  Status s;
  struct stat statbuf;
  int r = deltafs_stat(p, &statbuf);
  if (r != 0) {
    s = IOError(path);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::OpenDir(const std::string& path, Dir** dirptr) {
  const char* p = path.c_str();
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_open %s...\n", p);
#endif
  Status s;
  int fd = deltafs_open(
      p,
      O_DIRECTORY |
          (FLAGS_plfsdir != PLFSDIR_DISABLED
               ? (FLAGS_plfsdir == PLFSDIR_WRITE ? O_WRONLY : O_RDONLY)
               : O_RDONLY),
      0);
  if (fd == -1) {
    s = IOError(path);
  } else {
    *dirptr = new DeltafsDir(fd);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::FlushEpoch(Dir* dir) {
  Status s;
  DeltafsDir* const d = ToDeltafsDir(dir);
  char tmp[20];
  snprintf(tmp, sizeof(tmp), "dir#%d", d->fd);
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_epoch_flush %s...\n", tmp);
#endif
  int r = deltafs_epoch_flush(d->fd, NULL);
  if (r != 0) {
    s = IOError(tmp);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::CloseDir(Dir* dir) {
  Status s;
  DeltafsDir* const d = ToDeltafsDir(dir);
  char tmp[20];
  snprintf(tmp, sizeof(tmp), "dir#%d", d->fd);
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_close %s...\n", tmp);
#endif
  deltafs_close(d->fd);
  delete d;
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

Status DeltafsClient::AppendAt(Dir* dir, const std::string& file,
                               const char* data, size_t size) {
  Status s;
  DeltafsDir* const d = ToDeltafsDir(dir);
  char tmp[25];
  snprintf(tmp, sizeof(tmp), "dir#%d + ", d->fd);
  std::string path = tmp;
  path += file;
  const char* p = path.c_str();
#if VERBOSE >= 10
  if (kVVerbose) printf("deltafs_append %s...\n", p);
#endif
  int fd = deltafs_openat(d->fd, file.c_str(), O_WRONLY | O_APPEND | O_CREAT,
                          IO_FILEPERMS);
  if (fd == -1) {
    s = IOError(path);
  } else {
    ssize_t n = deltafs_write(fd, data, size);
    if (n != size) {
      s = IOError(path);
    }
    deltafs_close(fd);
  }
#if VERBOSE >= 10
  if (kVVerbose) print(s);
#endif
  return s;
}

static void MaybeSetVerboseLevel() {
#if defined(PDLFS_GLOG)
  const char* env = getenv("DELTAFS_Verbose");
  if (env != NULL && strlen(env) != 0) {
    FLAGS_v = atoi(env);  // Update log verbose level
  }
#else
// Do nothing
#endif
}

static void MaybeLogToStderr() {
#if defined(PDLFS_GLOG)
  const char* env = getenv("DELTAFS_LogToStderr");
  if (env != NULL && strlen(env) != 0) {
    FLAGS_logtostderr = true;
  }
#else
// Do nothing
#endif
}

static void MaybeEnablePLFSDir() {
  const char* env = getenv("DELTAFS_PLFSDir");
  if (env != NULL && strlen(env) != 0) {
    if (strcmp(env, "read") != 0 && strcmp(env, "write") != 0) {
      FLAGS_plfsdir = PLFSDIR_DISABLED;
    } else if (env[0] == 'w') {
      FLAGS_plfsdir = PLFSDIR_WRITE;
    } else {
      FLAGS_plfsdir = PLFSDIR_READ;
    }
  }
}

static void InstallDeltafsOpts(const IOClientOptions& options) {
  std::vector<std::string> confs;
  SplitString(&confs, options.conf_str.c_str(), '|');
  for (size_t i = 0; i < confs.size(); i++) {
    std::vector<std::string> kv;
    SplitString(&kv, confs[i].c_str(), '?', 1);
    if (kv.size() == 2) {
      const char* k = kv[0].c_str();
      const char* v = kv[1].c_str();
      setenv(k, v, 1);  // Override the existing one

#if VERBOSE >= 2
      if (options.rank == 0) {
        if (kVerbose) {
          printf("%s -> %s\n", k, v);
        }
      }
#endif
    }
  }

  MaybeEnablePLFSDir();
  // XXX: must run before glog is initialized
  MaybeSetVerboseLevel();
  MaybeLogToStderr();
}

IOClient* IOClient::Deltafs(const IOClientOptions& options) {
  DeltafsClient* cli = new DeltafsClient;
  InstallDeltafsOpts(options);
#if defined(PDLFS_GLOG)
  const char* argv0 = "io_deltafs";
  if (options.argc > 0 && options.argv != NULL) {
    argv0 = options.argv[0];
  }

  // XXX: Deltafs relies on glog to print important messages so we
  // setup it here.
  google::InitGoogleLogging(argv0);
  google::InstallFailureSignalHandler();
#endif
  return cli;
}

}  // namespace ioclient
}  // namespace pdlfs
