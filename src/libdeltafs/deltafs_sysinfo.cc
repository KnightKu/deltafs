/*
 * Copyright (c) 2015-2016 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "pdlfs-common/pdlfs_config.h"

#include "pdlfs-common/logging.h"

#include <ctype.h>
#include <stdio.h>
#include <time.h>

namespace pdlfs {

#if defined(__linux)
static Slice TrimSpace(Slice s) {
  size_t start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  size_t limit = s.size();
  while (limit > start && isspace(s[limit - 1])) {
    limit--;
  }
  Slice r = Slice(s.data() + start, limit - start);
  return r;
}
#endif

void PrintSysInfo() {
  Info(__LOG_ARGS__, "===============================================");
  Info(__LOG_ARGS__, "Deltafs:    Version %d.%d.%d (prototype)",
       PDLFS_COMMON_VERSION_MAJOR, PDLFS_COMMON_VERSION_MINOR,
       PDLFS_COMMON_VERSION_PATCH);

#if defined(__linux)
  time_t now = time(NULL);
  Info(__LOG_ARGS__, "Date:       %s", ctime(&now));
  FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
  if (cpuinfo != NULL) {
    char line[1000];
    int num_cpus = 0;
    std::string cpu_type;
    std::string cache_size;
    while (fgets(line, sizeof(line), cpuinfo) != NULL) {
      const char* sep = strchr(line, ':');
      if (sep == NULL) {
        continue;
      }
      Slice key = TrimSpace(Slice(line, sep - 1 - line));
      Slice val = TrimSpace(Slice(sep + 1));
      if (key == "model name") {
        ++num_cpus;
        cpu_type = val.ToString();
      } else if (key == "cache size") {
        cache_size = val.ToString();
      }
    }
    fclose(cpuinfo);
    Info(__LOG_ARGS__, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
    Info(__LOG_ARGS__, "CPUCache:   %s\n", cache_size.c_str());
  }
#endif

#if defined(__INTEL_COMPILER)
  Info(__LOG_ARGS__, "CC:         Intel (icpc) %d.%d.%d %d",
       __INTEL_COMPILER / 100, __INTEL_COMPILER % 100, __INTEL_COMPILER_UPDATE,
       __INTEL_COMPILER_BUILD_DATE);
#elif defined(_CRAYC)
  Info(__LOG_ARGS__, "CC:         CRAY (crayc++) %d.%d", _RELEASE,
       _RELEASE_MINOR);
#elif defined(__GNUC__)
  Info(__LOG_ARGS__, "CC:         GNU (g++) %d.%d.%d", __GNUC__, __GNUC_MINOR__,
       __GNUC_PATCHLEVEL__);
#elif defined(__clang__)
  Info(__LOG_ARGS__, "CC:         Clang (clang++) %d.%d.%d", __clang_major__,
       __clang_minor__, __clang_patchlevel__);
#endif
  Info(__LOG_ARGS__, "===============================================");
}

}  // namespace pdlfs
