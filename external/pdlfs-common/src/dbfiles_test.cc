/*
 * Copyright (c) 2019 Carnegie Mellon University,
 * Copyright (c) 2019 Triad National Security, LLC, as operator of
 *     Los Alamos National Laboratory.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

/*
 * Copyright (c) 2011 The LevelDB Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found at https://github.com/google/leveldb.
 */
#include "pdlfs-common/dbfiles.h"
#include "pdlfs-common/strutil.h"
#include "pdlfs-common/testharness.h"

namespace pdlfs {

class FileNameTest {};

/* clang-format off */
TEST(FileNameTest, Parse) {
  Slice db;
  FileType type;
  uint64_t number;

  // Successful parses
  static struct {
    const char* fname;
    uint64_t number;
    FileType type;
  } cases[] = {
    { "100.log",            100,   kLogFile },
    { "0.log",              0,     kLogFile },
    { "0.sst",              0,     kTableFile },
    { "0.ldb",              0,     kTableFile },
    { "CURRENT",            0,     kCurrentFile },
    { "LOCK",               0,     kDBLockFile },
    { "MANIFEST-2",         2,     kDescriptorFile },
    { "MANIFEST-7",         7,     kDescriptorFile },
    { "LOG",                0,     kInfoLogFile },
    { "LOG.old",            0,     kInfoLogFile },
    { "18446744073709551615.log", 18446744073709551615ull, kLogFile },
  };
  for (int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    std::string f = cases[i].fname;
    ASSERT_TRUE(ParseFileName(f, &number, &type)) << f;
    ASSERT_EQ(cases[i].type, type) << f;
    ASSERT_EQ(cases[i].number, number) << f;
  }

  // Errors
  static const char* errors[] = {
    "",
    "foo",
    "foo-dx-100.log",
    ".log",
    "",
    "manifest",
    "CURREN",
    "CURRENTX",
    "MANIFES",
    "MANIFEST",
    "MANIFEST-",
    "XMANIFEST-3",
    "MANIFEST-3x",
    "LOC",
    "LOCKx",
    "LO",
    "LOGx",
    "18446744073709551616.log",
    "184467440737095516150.log",
    "100",
    "100.",
    "100.lop"
  };
  for (int i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
    std::string f = errors[i];
    ASSERT_TRUE(!ParseFileName(f, &number, &type)) << f;
  }
}
/* clang-format on */

TEST(FileNameTest, Construction) {
  uint64_t number;
  FileType type;
  std::string fname;

  fname = CurrentFileName("foo");
  ASSERT_EQ("foo/", std::string(fname.data(), 4));
  ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
  ASSERT_EQ(0, number);
  ASSERT_EQ(kCurrentFile, type);

  fname = LockFileName("foo");
  ASSERT_EQ("foo/", std::string(fname.data(), 4));
  ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
  ASSERT_EQ(0, number);
  ASSERT_EQ(kDBLockFile, type);

  fname = LogFileName("foo", 192);
  ASSERT_EQ("foo/", std::string(fname.data(), 4));
  ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
  ASSERT_EQ(192, number);
  ASSERT_EQ(kLogFile, type);

  fname = TableFileName("bar", 200);
  ASSERT_EQ("bar/", std::string(fname.data(), 4));
  ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
  ASSERT_EQ(200, number);
  ASSERT_EQ(kTableFile, type);

  fname = DescriptorFileName("bar", 100);
  ASSERT_EQ("bar/", std::string(fname.data(), 4));
  ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
  ASSERT_EQ(100, number);
  ASSERT_EQ(kDescriptorFile, type);

  fname = TempFileName("tmp", 999);
  ASSERT_EQ("tmp/", std::string(fname.data(), 4));
  ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
  ASSERT_EQ(999, number);
  ASSERT_EQ(kTempFile, type);
}

}  // namespace pdlfs

int main(int argc, char** argv) {
  return ::pdlfs::test::RunAllTests(&argc, &argv);
}
