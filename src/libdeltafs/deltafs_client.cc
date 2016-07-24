/*
 * Copyright (c) 2014-2016 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_client.h"
#include "deltafs_conf_loader.h"
#if defined(PDLFS_PLATFORM_POSIX)
#include <sys/types.h>
#include <unistd.h>
#endif

#include "pdlfs-common/rpc.h"
#include "pdlfs-common/strutil.h"

namespace pdlfs {

Client::~Client() {
  delete mdscli_;
  delete mdsfty_;
  delete blkdb_;
  delete db_;
}

// Open a file for writing. Return OK on success.
// If the file already exists, it is not truncated.
// If the file doesn't exist, it will be created.
Status Client::Wopen(const Slice& path, int mode, FileInfo* info) {
  Status s;
  Fentry ent;
  s = mdscli_->Fcreat(path, true, mode, &ent);
  if (s.IsAlreadyExists()) {
    s = mdscli_->Fstat(path, &ent);
  }

  uint64_t mtime;
  uint64_t size;
  int fd;
  if (s.ok()) {
    s = blkdb_->Open(ent, true,  // create if missing
                     false,      // error if exists
                     &mtime, &size, &fd);
#if 0
    if (s.ok()) {
      if (size != ent.stat.FileSize()) {
        // FIXME
      }
    }
#endif
    if (s.ok()) {
      info->size = size;
      info->fd = fd;
    }
  }
  return s;
}

// Write a chunk of data at a specified offset. Return OK on success.
Status Client::Pwrite(int fd, const Slice& data, uint64_t off) {
  return blkdb_->Pwrite(fd, data, off);
}

// Open a file for reading. Return OK on success.
// If the file doesn't exist, it won't be created.
Status Client::Ropen(const Slice& path, FileInfo* info) {
  Status s;
  Fentry ent;
  s = mdscli_->Fstat(path, &ent);

  uint64_t mtime;
  uint64_t size;
  int fd;
  if (s.ok()) {
    // Create the file if missing since the MDS says it exists.
    s = blkdb_->Open(ent, true,  // create if missing
                     false,      // error if exists
                     &mtime, &size, &fd);
#if 0
    if (s.ok()) {
      if (size != ent.stat.FileSize()) {
        // FIXME
      }
    }
#endif
    if (s.ok()) {
      info->size = size;
      info->fd = fd;
    }
  }
  return s;
}

// Read a chunk of data at a specified offset. Return OK on success.
Status Client::Pread(int fd, Slice* result, uint64_t off, uint64_t size,
                     char* buf) {
  return blkdb_->Pread(fd, result, off, size, buf);
}

Status Client::Fdatasync(int fd) {
  Fentry ent;
  uint64_t mtime;
  uint64_t size;
  bool dirty;
  Status s = blkdb_->GetInfo(fd, &ent, &dirty, &mtime, &size);
  if (s.ok()) {
    s = blkdb_->Flush(fd, true);  // Force sync
    if (s.ok() && dirty) {
      s = mdscli_->Ftruncate(ent, mtime, size);
    }
  }
  return s;
}

Status Client::Flush(int fd) {
  Fentry ent;
  uint64_t mtime;
  uint64_t size;
  bool dirty;
  Status s = blkdb_->GetInfo(fd, &ent, &dirty, &mtime, &size);
  if (s.ok() && dirty) {
    s = blkdb_->Flush(fd);
    if (s.ok()) {
      s = mdscli_->Ftruncate(ent, mtime, size);
    }
  }
  return s;
}

// REQUIRES: Flush(...) has been called on the same fd.
Status Client::Close(int fd) {
  blkdb_->Close(fd);
  return Status::OK();
}

Status Client::Mkfile(const Slice& path, int mode) {
  Status s;
  Fentry ent;
  const bool error_if_exists = true;
  s = mdscli_->Fcreat(path, error_if_exists, mode, &ent);
  return s;
}

Status Client::Mkdir(const Slice& path, int mode) {
  Status s;
  s = mdscli_->Mkdir(path, mode);
  return s;
}

class Client::Builder {
 public:
  explicit Builder()
      : env_(NULL), mdsfty_(NULL), mdscli_(NULL), db_(NULL), blkdb_(NULL) {}
  ~Builder() {}

  Status status() const { return status_; }
  Client* BuildClient();

 private:
  static int FetchUid() {
#if defined(PDLFS_PLATFORM_POSIX)
    return getuid();
#else
    return 0;
#endif
  }

  static int FetchGid() {
#if defined(PDLFS_PLATFORM_POSIX)
    return getgid();
#else
    return 0;
#endif
  }

  void LoadIds();
  void LoadMDSTopology();
  void OpenSession();
  void OpenDB();
  void OpenMDSCli();

  Status status_;
  bool ok() const { return status_.ok(); }
  Env* env_;
  MDSTopology mdstopo_;
  MDSFactoryImpl* mdsfty_;
  MDSCliOptions mdscliopts_;
  MDSClient* mdscli_;
  DBOptions dbopts_;
  DB* db_;
  BlkDBOptions blkdbopts_;
  BlkDB* blkdb_;
  int cli_id_;
  int session_id_;
  int uid_;
  int gid_;
};

void Client::Builder::LoadIds() {
  uid_ = FetchUid();
  gid_ = FetchGid();

  uint64_t cli_id;
  status_ = config::LoadInstanceId(&cli_id);
  if (ok()) {
    cli_id_ = cli_id;
  }
}

void Client::Builder::LoadMDSTopology() {
  uint64_t num_vir_srvs;
  uint64_t num_srvs;

  if (ok()) {
    status_ = config::LoadNumOfVirMetadataSrvs(&num_vir_srvs);
    if (ok()) {
      status_ = config::LoadNumOfMetadataSrvs(&num_srvs);
      if (ok()) {
        std::string addrs = config::MetadataSrvAddrs();
        size_t num_addrs = SplitString(addrs, ';', &mdstopo_.srv_addrs);
        if (num_addrs < num_srvs) {
          status_ = Status::InvalidArgument("Not enough addrs");
        } else if (num_addrs > num_srvs) {
          status_ = Status::InvalidArgument("Too many addrs");
        }
      }
    }
  }

  if (ok()) {
    status_ = config::LoadRPCTracing(&mdstopo_.rpc_tracing);
  }

  if (ok()) {
    mdstopo_.rpc_proto = config::RPCProto();
    num_vir_srvs = std::max(num_vir_srvs, num_srvs);
    mdstopo_.num_vir_srvs = num_vir_srvs;
    mdstopo_.num_srvs = num_srvs;
  }

  if (ok()) {
    MDSFactoryImpl* fty = new MDSFactoryImpl;
    status_ = fty->Init(mdstopo_);
    if (ok()) {
      status_ = fty->Start();
    }
    if (ok()) {
      mdsfty_ = fty;
    } else {
      delete fty;
    }
  }
}

// REQUIRES: both LoadIds() and LoadMDSTopology() have been called.
void Client::Builder::OpenSession() {
  std::string env_name;
  std::string env_conf;

  if (ok()) {
    assert(mdsfty_ != NULL);
    MDS* mds = mdsfty_->Get(cli_id_ % mdstopo_.num_srvs);
    assert(mds != NULL);
    MDS::OpensessionOptions options;
    options.dir_id = DirId(0, 0, 0);
    MDS::OpensessionRet ret;
    status_ = mds->Opensession(options, &ret);
    if (ok()) {
      session_id_ = ret.session_id;
      env_name = ret.env_name;
      env_conf = ret.env_conf;
    }
  }

  if (ok()) {
    env_ = Env::Default();  // FIXME
  }
}

// REQUIRES: OpenSession() has been called.
void Client::Builder::OpenDB() {
  std::string output_root;

  if (ok()) {
    assert(mdsfty_ != NULL);
    MDS* mds = mdsfty_->Get(session_id_ % mdstopo_.num_srvs);
    assert(mds != NULL);
    MDS::GetoutputOptions options;
    options.dir_id = DirId(0, 0, 0);
    MDS::GetoutputRet ret;
    status_ = mds->Getoutput(options, &ret);
    if (ok()) {
      output_root = ret.info;
    }
  }

  if (ok()) {
    status_ = config::LoadVerifyChecksums(&blkdbopts_.verify_checksum);
  }

  if (ok()) {
    dbopts_.create_if_missing = true;
    dbopts_.compression = kNoCompression;
    dbopts_.disable_compaction = true;
    dbopts_.env = env_;
  }

  if (ok()) {
    std::string dbhome = output_root;
    char tmp[30];
    snprintf(tmp, sizeof(tmp), "/data_%d", session_id_);
    dbhome += tmp;
    status_ = DB::Open(dbopts_, dbhome, &db_);
    if (ok()) {
      blkdbopts_.db = db_;
      blkdbopts_.session_id = session_id_;
      blkdb_ = new BlkDB(blkdbopts_);
    }
  }
}

// REQUIRES: OpenSession() has been called.
void Client::Builder::OpenMDSCli() {
  uint64_t idx_cache_sz;
  uint64_t lookup_cache_sz;

  if (ok()) {
    status_ = config::LoadSizeOfCliIndexCache(&idx_cache_sz);
    if (ok()) {
      status_ = config::LoadSizeOfCliLookupCache(&lookup_cache_sz);
    }
  }

  if (ok()) {
    status_ = config::LoadAtomicPathRes(&mdscliopts_.atomic_path_resolution);
    if (ok()) {
      status_ = config::LoadParanoidChecks(&mdscliopts_.paranoid_checks);
    }
  }

  if (ok()) {
    mdscliopts_.env = env_;
    mdscliopts_.factory = mdsfty_;
    mdscliopts_.index_cache_size = idx_cache_sz;
    mdscliopts_.lookup_cache_size = lookup_cache_sz;
    mdscliopts_.num_virtual_servers = mdstopo_.num_vir_srvs;
    mdscliopts_.num_servers = mdstopo_.num_srvs;
    mdscliopts_.session_id = session_id_;
    mdscliopts_.cli_id = cli_id_;
    mdscliopts_.uid = uid_;
    mdscliopts_.gid = gid_;
  }

  if (ok()) {
    mdscli_ = MDSClient::Open(mdscliopts_);
  }
}

Client* Client::Builder::BuildClient() {
  LoadIds();
  LoadMDSTopology();
  OpenSession();
  OpenDB();
  OpenMDSCli();

  if (ok()) {
    Client* cli = new Client;
    cli->mdscli_ = mdscli_;
    cli->mdsfty_ = mdsfty_;
    cli->blkdb_ = blkdb_;
    cli->db_ = db_;
    return cli;
  } else {
    delete mdscli_;
    delete mdsfty_;
    delete blkdb_;
    delete db_;
    return NULL;
  }
}

Status Client::Open(Client** cliptr) {
  Builder builder;
  *cliptr = builder.BuildClient();
  return builder.status();
}

}  // namespace pdlfs
