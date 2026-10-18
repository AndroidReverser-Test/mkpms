#include "WxShadowInternal.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <vector>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifdef DOBBY_WXSHADOW_TESTING
extern __attribute__((visibility("default"))) bool dobby_wx_test_failure(int point);
#endif
namespace dobby {
void *WxAllocateNear(uintptr_t target) {
#ifdef DOBBY_WXSHADOW_TESTING
  if (dobby_wx_test_failure(1)) return nullptr;
#endif
  // Each mapping has one publication. No MAP_FIXED and no existing code caves.
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps) return nullptr;
  uintptr_t low = target > 134217728 ? target - 134217728 : WxPageSize;
  low = (low + WxPageSize - 1) & ~(WxPageSize - 1);
  uintptr_t high = target > UINTPTR_MAX - 134217724 ? UINTPTR_MAX : target + 134217724;
  high &= ~(WxPageSize - 1);
  std::vector<uintptr_t> candidates;
  uintptr_t previous = low;
  char line[1024];
  while (fgets(line, sizeof(line), maps)) {
    unsigned long long begin, end;
    if (sscanf(line, "%llx-%llx", &begin, &end) != 2) continue;
    if (begin > previous && previous <= high) {
      uintptr_t last = std::min<uintptr_t>(high, begin - WxPageSize);
      if (last >= previous) {
        uintptr_t preferred = std::max(previous, std::min(target & ~(WxPageSize - 1), last));
        candidates.push_back(preferred);
        if (previous != preferred) candidates.push_back(previous);
        if (last != preferred) candidates.push_back(last);
      }
    }
    previous = std::max<uintptr_t>(previous, end);
    if (previous > high) break;
  }
  if (previous <= high) candidates.push_back(previous);
  fclose(maps);
  std::sort(candidates.begin(), candidates.end(), [target](uintptr_t a, uintptr_t b) {
    auto da = a > target ? a - target : target - a;
    auto db = b > target ? b - target : target - b;
    return da < db;
  });
  for (auto candidate : candidates) {
    if (IsHelperPage(candidate) || IsProtectedRange(candidate, WxPageSize)) continue;
    void *mapping = mmap((void *)candidate, WxPageSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapping == MAP_FAILED && errno == EINVAL)
      mapping = mmap((void *)candidate, WxPageSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) continue;
    uint32_t ignored;
    // Older kernels may ignore NOREPLACE. Never trust the requested address.
    if ((uintptr_t)mapping != candidate ||
        !wx_a64_branch(target, (uintptr_t)mapping, &ignored) ||
        !wx_a64_branch((uintptr_t)mapping, target + 4, &ignored) ||
        IsProtectedRange((uintptr_t)mapping, WxPageSize)) {
      munmap(mapping, WxPageSize); continue;
    }
    RegisterHelper(mapping, WxPageSize);
    return mapping;
  }
  return nullptr;
}
int WxPublish(void *mapping, const void *code, size_t length) {
#ifdef DOBBY_WXSHADOW_TESTING
  if (dobby_wx_test_failure(2)) return -EIO;
#endif
  if (!mapping || !code || !length || length > WxPageSize) return -EINVAL;
  memcpy(mapping, code, length);
  __builtin___clear_cache((char *)mapping, (char *)mapping + length);
  if (mprotect(mapping, WxPageSize, PROT_READ | PROT_EXEC)) return -errno;
  return 0;
}
void WxFree(void *mapping) {
  if (mapping) { UnregisterHelper(mapping); munmap(mapping, WxPageSize); }
}
}
