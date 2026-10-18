#include "dobby.h"
#include "wxshadow_uapi.h"
#include "wxshadow_arm64.h"
#ifdef TEST_LINK_STATIC
#include "Backend/UserMode/WxShadow/WxShadowInternal.h"
#endif
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t instance = 101, capabilities = WX_CAP_REQUIRED;
static bool missing, lose_patch_reply, query_fails, reject_patch, reject_release;
static int failpoint, allocation_calls;
static void **original_output;
static std::map<uint64_t, wx_patch_request> records;
bool dobby_wx_test_failure(int point) {
  if (failpoint == 3 && point == 1) return ++allocation_calls == 2;
  return failpoint == point;
}
int dobby_wx_test_transport(int command, void *data) {
  if (missing) return -EINVAL;
  if (command == PR_WXSHADOW_QUERY) {
    auto *q = (wx_query *)data;
    q->arch = WXSHADOW_ARCH_AARCH64; q->page_size = 4096;
    q->instance = instance; q->capabilities = capabilities; return 0;
  }
  if (command == PR_WXSHADOW_PATCH_EX) {
    auto *p = (wx_patch_request *)data;
    assert(original_output == nullptr || *original_output != nullptr);
    assert((p->replacement & 0xfc000000) == 0x14000000);
    if (reject_patch) return -ENOMEM;
    if (!p->generation) p->generation = 7;
    p->state = WX_PATCH_ACTIVE; records[p->cookie] = *p;
    return lose_patch_reply ? -EFAULT : 0;
  }
  auto *id = (wx_identity *)data;
  if (query_fails) return -EIO;
  auto found = records.find(id->cookie);
  if (found == records.end()) { id->state = WX_PATCH_ABSENT; return 0; }
  auto &p = found->second;
  if (id->address != p.address || (id->generation && id->generation != p.generation)) return -EEXIST;
  if (command == PR_WXSHADOW_RELEASE_EX) {
    if (reject_release) return -EIO;
    p.state = WX_PATCH_RETIRED;
  }
  id->state = p.state; id->generation = p.generation;
  return 0;
}
static void *code_page(size_t size = 4096) {
  void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(p != MAP_FAILED);
  for (size_t i = 0; i < size; i += 4) { uint32_t nop = 0xd503201f; memcpy((char *)p + i, &nop, 4); }
  assert(!mprotect(p, size, PROT_READ | PROT_EXEC)); return p;
}
static DobbyHookStatus status(void *address) {
  DobbyHookStatus s{}; assert(!dobby_get_hook_status(address, &s)); return s;
}
int main() {
#if defined(__x86_64__)
  // Native regression executes real machine code, including a saved original
  // after removal and the optional null original output.
  auto native_source = (uint8_t *)code_page();
  auto native_dest = (uint8_t *)code_page();
  const uint8_t increment[] = {0x8d, 0x47, 0x01, 0xc3};
  const uint8_t replacement_code[] = {0x8d, 0x47, 0x0a, 0xc3};
  assert(!mprotect(native_source,4096,PROT_READ|PROT_WRITE));
  assert(!mprotect(native_dest,4096,PROT_READ|PROT_WRITE));
  memset(native_source,0x90,64); memcpy(native_source,increment,4); memcpy(native_dest,replacement_code,4);
  assert(!mprotect(native_source,4096,PROT_READ|PROT_EXEC));
  assert(!mprotect(native_dest,4096,PROT_READ|PROT_EXEC));
  auto fn=(int(*)(int))native_source; int (*saved)(int)=nullptr;
  assert(fn(5)==6 && !DobbyHook(native_source,native_dest,(void **)&saved));
  assert(fn(5)==15 && saved(5)==6 && !DobbyDestroy(native_source));
  assert(fn(5)==6 && saved(5)==6 && !DobbyHook(native_source,native_dest,nullptr));
  assert(fn(5)==15 && !DobbyDestroy(native_source) && fn(5)==6);
#endif
  uint32_t b;
  assert(wx_a64_branch(0x10000000, 0x08000000, &b) && b == 0x16000000);
  assert(wx_a64_branch(0x10000000, 0x17fffffc, &b) && b == 0x15ffffff);
  assert(!wx_a64_branch(0x10000000, 0x18000000, &b));
  assert(!wx_a64_branch(0x10000000, 0x07fffffc, &b));
  assert(!wx_a64_branch(0x10000000, 0x10000002, &b));
  assert(wx_a64_special_entry(0xd503233f) && wx_a64_special_entry(0xd503237f));
  assert(wx_a64_special_entry(0xd503245f) && !wx_a64_special_entry(0xd503201f));
#ifdef TEST_LINK_STATIC
  uint8_t output[128]; size_t length = 0;
  assert(!dobby::WxBuildOriginal(0xd503201f, 0x10000000, 0x10010000, output, &length) && length == 8);
  assert(!dobby::WxBuildOriginal(0x90000000, 0x10000000, 0x10010000, output, &length) && length == 20);
  assert(dobby::WxBuildOriginal(0x58000000, 0x10000000, 0x10010000, output, &length) == -ENOTSUP);
  assert(dobby::WxBuildOriginal(0x94000000, 0x10000000, 0x10010000, output, &length) == -ENOTSUP);
  assert(dobby::WxBuildOriginal(0xd503237f, 0x10000000, 0x10010000, output, &length) == -ENOTSUP);
#endif
  missing = true;
  assert(dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) != 0);
  missing = false; capabilities = WX_CAP_REQUIRED & ~WX_CAP_FAULT;
  assert(dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) == -ENOTSUP);
  capabilities = WX_CAP_REQUIRED;
  assert(!dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED));
  void *source = code_page(8192), *replacement = code_page();
  missing = true;
  assert(DobbyHook(source,replacement,nullptr) != 0); // strict selection remains set
  missing = false;
  uint8_t before[8192]; memcpy(before, source, sizeof(before));
  void *original = nullptr; original_output = &original;
  failpoint = 1;
  assert(DobbyHook(source, replacement, &original) == -ENOMEM);
  failpoint = 2;
  assert(DobbyHook(source, replacement, &original) == -EIO);
  failpoint = 0; reject_patch = true;
  assert(DobbyHook(source, replacement, &original) == -ENOMEM && original == nullptr);
  assert(!memcmp(before, source, sizeof(before)));
  reject_patch = false; lose_patch_reply = true;
  assert(!DobbyHook(source, replacement, &original));
  assert(original && status(source).state == DOBBY_HOOK_ACTIVE);
  assert(DobbyHook(source, replacement, nullptr) == -EEXIST);
  assert(dobby_set_hook_backend(DOBBY_HOOK_BACKEND_NATIVE) == -EBUSY);
  uint8_t nop[4] = {0x1f, 0x20, 0x03, 0xd5};
  assert(DobbyCodePatch((char *)source + 64, nop, 4) == -EBUSY);
  assert(DobbyInstrument(source, [](void *, DobbyRegisterContext *) {}) == -ENOTSUP);
  assert(DobbyHook((char *)source + 4, (char *)source + 128, nullptr) == -ENOTSUP);
  lose_patch_reply = false; original_output = nullptr;
  assert(!DobbyHook((char *)source + 4, replacement, nullptr));
  assert(!DobbyHook((char *)source + 4092, replacement, nullptr));
  assert(!DobbyDestroy((char *)source + 4));
  assert(status(source).state == DOBBY_HOOK_ACTIVE);
  assert(!DobbyDestroy((char *)source + 4092));
  reject_release = true;
  assert(DobbyDestroy(source) == -EIO);
  reject_release = false; assert(!DobbyDestroy(source));
  assert(!memcmp(before, source, sizeof(before)));
  query_fails = true;
  assert(DobbyHook(source, replacement, &original) == DOBBY_ERROR_RECOVERY_REQUIRED);
  assert(status(source).state == DOBBY_HOOK_RECOVERY_REQUIRED);
  query_fails = false; assert(!DobbyDestroy(source));
  auto *far = mmap((void *)0x2000000000ULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  assert(far != MAP_FAILED && !wx_a64_branch((uintptr_t)source+4096,(uintptr_t)far,&b));
  memcpy(far,nop,4); assert(!mprotect(far,4096,PROT_READ|PROT_EXEC));
  failpoint=3; allocation_calls=0;
  assert(DobbyHook((char *)source+4096,far,nullptr) == -ENOMEM);
  failpoint=0;
  assert(!DobbyHook((char *)source+4096,far,nullptr));
  assert(!DobbyDestroy((char *)source+4096));
  pid_t child = fork(); assert(child >= 0);
  if (!child) _exit(dobby_set_hook_backend(DOBBY_HOOK_BACKEND_NATIVE) == DOBBY_ERROR_FORK ? 0 : 1);
  int exit_status; assert(waitpid(child, &exit_status, 0) == child && WIFEXITED(exit_status) && !WEXITSTATUS(exit_status));
  DobbyHookStatistics stats{};
  assert(!dobby_get_hook_statistics(&stats));
  assert(!stats.active_entries && !stats.recovery_entries && stats.retained_mappings >= 4 && stats.retained_bytes >= 4 * 4096);
  assert(!DobbyHook(source, replacement, nullptr));
  ++instance; records.clear();
  assert(DobbyDestroy(source) == DOBBY_ERROR_BACKEND_LOST);
  assert(status(source).state == DOBBY_HOOK_RECOVERY_REQUIRED);
  puts("wxshadow: ABI, routing, page end, failures, lost replies, recovery, fork, reload passed");
  return 0;
}
