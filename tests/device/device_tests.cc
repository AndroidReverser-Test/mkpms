#include "dobby.h"
#include "wxshadow_uapi.h"
#include "wxshadow_arm64.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

// Only code owned by this process is hooked. Installation/removal happens
// before workers start / after they join; no live instruction rewrite test.
#define CHECK(expr) do { if (!(expr)) { \
  std::fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #expr, errno); std::exit(1); \
} } while (0)
using Function = int (*)(int);
static Function original_a, original_b, original_c;
__attribute__((noinline)) static int replacement_a(int x) { return original_a(x) + 100; }
__attribute__((noinline)) static int replacement_b(int x) { return original_b(x) + 200; }
__attribute__((noinline)) static int replacement_c(int x) { return original_c(x) + 300; }
static void write_word(void *p, uint32_t value) { std::memcpy(p, &value, 4); }
static uint32_t crc32(const void *p, size_t count) {
  const auto *bytes = static_cast<const volatile uint8_t *>(p);
  uint32_t crc = ~0U;
  for (size_t i = 0; i < count; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
template <typename T> static T request() {
  T r{}; r.header = {WXSHADOW_ABI_VERSION, sizeof(r), 0, 0}; return r;
}
static DobbyHookStatus status(void *p) {
  DobbyHookStatus s{}; CHECK(!dobby_get_hook_status(p, &s)); return s;
}
static void *far_code(uintptr_t source) {
  for (uintptr_t hint : {0x1000000000ULL, 0x2000000000ULL, 0x3000000000ULL}) {
    void *p = mmap(reinterpret_cast<void *>(hint), 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) continue;
    uint32_t b;
    if (wx_a64_branch(source, uintptr_t(p), &b)) { munmap(p,4096); continue; }
    write_word(p, 0x528009a0); // mov w0, #77
    write_word(static_cast<char *>(p) + 4, 0xd65f03c0);
    __builtin___clear_cache(static_cast<char *>(p), static_cast<char *>(p) + 8);
    CHECK(!mprotect(p,4096,PROT_READ | PROT_EXEC)); return p;
  }
  return nullptr;
}
static void protocol_copy_failures(char *source, uint32_t crc) {
  auto q = request<wx_query>();
  CHECK(!prctl(PR_WXSHADOW_QUERY,(unsigned long)&q,0UL,0UL,0UL));
  auto bad = q; bad.header.flags = 1; errno = 0;
  CHECK(prctl(PR_WXSHADOW_QUERY,(unsigned long)&bad,0UL,0UL,0UL) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(prctl(PR_WXSHADOW_QUERY,1UL,0UL,0UL,0UL) == -1 && errno == EFAULT);
  auto *p = static_cast<wx_patch_request *>(mmap(nullptr,4096,PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS,-1,0));
  CHECK(p != MAP_FAILED); *p = request<wx_patch_request>();
  p->instance = q.instance; p->cookie = 0x7478000000000000ULL | unsigned(getpid());
  p->address = uintptr_t(source + 256); p->expected = 0x11001400; // add w0,w0,#5
  CHECK(wx_a64_branch(p->address,p->address + 4,&p->replacement));
  wx_identity id = request<wx_identity>();
  id.instance=p->instance; id.cookie=p->cookie; id.address=p->address;
  CHECK(!mprotect(p,4096,PROT_READ)); errno=0;
  CHECK(prctl(PR_WXSHADOW_PATCH_EX,(unsigned long)p,0UL,0UL,0UL) == -1 && errno == EFAULT);
  CHECK(!prctl(PR_WXSHADOW_QUERY_PATCH,(unsigned long)&id,0UL,0UL,0UL) && id.state == WX_PATCH_ACTIVE);
  CHECK(reinterpret_cast<Function>(source+256)(10) == 10 && crc32(source,8192) == crc);
  CHECK(!mprotect(p,4096,PROT_READ | PROT_WRITE)); std::memcpy(p,&id,sizeof(id));
  CHECK(!mprotect(p,4096,PROT_READ)); errno=0;
  CHECK(prctl(PR_WXSHADOW_RELEASE_EX,(unsigned long)p,0UL,0UL,0UL) == -1 && errno == EFAULT);
  CHECK(!prctl(PR_WXSHADOW_QUERY_PATCH,(unsigned long)&id,0UL,0UL,0UL) && id.state == WX_PATCH_RETIRED);
  CHECK(reinterpret_cast<Function>(source+256)(10) == 15 && crc32(source,8192) == crc);
  CHECK(!munmap(p,4096));
  std::puts("PASS protocol validation and committed requests with lost result copies");
}
int main(int argc, char **argv) {
  setvbuf(stdout,nullptr,_IONBF,0);
  CHECK(sysconf(_SC_PAGESIZE) == 4096);
  bool unavailable = argc == 2 && !std::strcmp(argv[1],"--expect-unavailable");
  bool native = argc == 2 && !std::strcmp(argv[1],"--native");
  CHECK(argc == 1 || unavailable || native);
  auto *source = static_cast<char *>(mmap(nullptr,8192,PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS,-1,0));
  CHECK(source != MAP_FAILED);
  for (size_t i=0;i<8192;i+=4) write_word(source+i,0xd503201f);
  for (auto pair : {std::pair<size_t,int>{0,1}, {64,2}, {4092,3}, {128,4}, {256,5}}) {
    write_word(source+pair.first,0x11000000 | (pair.second << 10));
    write_word(source+pair.first+4,0xd65f03c0);
  }
  __builtin___clear_cache(source,source+8192);
  CHECK(!mprotect(source,8192,PROT_READ | PROT_EXEC));
  uint32_t crc = crc32(source,8192);
  auto a = reinterpret_cast<Function>(source), b = reinterpret_cast<Function>(source+64);
  auto c = reinterpret_cast<Function>(source+4092);
  CHECK(a(10)==11 && b(10)==12 && c(10)==13);
  int selected = dobby_set_hook_backend(native ? DOBBY_HOOK_BACKEND_NATIVE : DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED);
  if (unavailable) {
    CHECK(selected != 0);
    CHECK(DobbyHook(source,reinterpret_cast<void *>(replacement_a),reinterpret_cast<void **>(&original_a)) != 0);
    CHECK(!original_a && a(10)==11 && crc32(source,8192)==crc);
    std::puts("PASS unavailable backend: strict failure, original bytes unchanged"); return 0;
  }
  CHECK(!selected);
  CHECK(!DobbyHook(source,reinterpret_cast<void *>(replacement_a),reinterpret_cast<void **>(&original_a)));
  CHECK(a(10)==111 && original_a(10)==11);
  if (native) {
    CHECK(!DobbyDestroy(source) && a(10)==11 && original_a(10)==11);
    CHECK(crc32(source,8192)==crc);
    std::puts("PASS native ARM64 replacement/original/destroy regression"); return 0;
  }
  CHECK(status(source).state == DOBBY_HOOK_ACTIVE && crc32(source,8192)==crc);
  CHECK(!DobbyHook(source+64,reinterpret_cast<void *>(replacement_b),reinterpret_cast<void **>(&original_b)));
  CHECK(!DobbyHook(source+4092,reinterpret_cast<void *>(replacement_c),reinterpret_cast<void **>(&original_c)));
  CHECK(b(10)==212 && c(10)==313 && original_b(10)==12 && original_c(10)==13);
  CHECK(status(source).generation == status(source+64).generation);
  CHECK(DobbyHook(source,reinterpret_cast<void *>(replacement_a),nullptr) == -EEXIST);
  uint8_t nop[] = {0x1f,0x20,0x03,0xd5};
  CHECK(DobbyCodePatch(source+128,nop,4) == -EBUSY);
  CHECK(DobbyInstrument(source,[](void *,DobbyRegisterContext *){}) == -ENOTSUP);
  void *far = far_code(uintptr_t(source+128)); CHECK(far);
  CHECK(!DobbyHook(source+128,far,nullptr));
  CHECK(reinterpret_cast<Function>(source+128)(10)==77 && crc32(source,8192)==crc);
  std::puts("PASS same-page hooks, original calls, CRC, page-end entry, far relay");

  std::atomic<int> failures{0}; std::vector<std::thread> workers;
  for (int i=0;i<3;++i) workers.emplace_back([&] {
    for (int j=0;j<200;++j) if (a(j)!=j+101 || b(j)!=j+202 || c(j)!=j+303) ++failures;
  });
  workers.emplace_back([&] { for(int j=0;j<50;++j) if(crc32(source,8192)!=crc) ++failures; });
  for(auto &worker:workers) worker.join(); CHECK(!failures);
  CHECK(!DobbyDestroy(source) && a(10)==11 && b(10)==212 && c(10)==313);
  CHECK(!DobbyDestroy(source+64) && b(10)==12 && c(10)==313);
  CHECK(!DobbyDestroy(source+4092) && c(10)==13);
  CHECK(!DobbyDestroy(source+128) && reinterpret_cast<Function>(source+128)(10)==14);
  CHECK(original_a(10)==11 && original_b(10)==12 && original_c(10)==13 && crc32(source,8192)==crc);
  std::puts("PASS concurrent execution/read verification and precise removal");

  CHECK(!DobbyHook(source,reinterpret_cast<void *>(replacement_a),reinterpret_cast<void **>(&original_a)));
  CHECK(!mprotect(source,8192,PROT_READ));
  CHECK(!mprotect(source,8192,PROT_READ | PROT_EXEC));
  CHECK(status(source).state == DOBBY_HOOK_RETIRED);
  CHECK(!DobbyDestroy(source) && a(10)==11);
  CHECK(!DobbyHook(source,reinterpret_cast<void *>(replacement_a),reinterpret_cast<void **>(&original_a)));
  pid_t child = fork(); CHECK(child >= 0);
  if (!child) { DobbyHookStatistics s{}; _exit(dobby_get_hook_statistics(&s) == DOBBY_ERROR_FORK ? 0 : 1); }
  int exit_status; CHECK(waitpid(child,&exit_status,0)==child && WIFEXITED(exit_status) && !WEXITSTATUS(exit_status));
  CHECK(status(source).state == DOBBY_HOOK_RETIRED && !DobbyDestroy(source));
  CHECK(a(10)==11 && crc32(source,8192)==crc);
  std::puts("PASS mprotect/fork retirement and child API rejection");
  protocol_copy_failures(source,crc);
  DobbyHookStatistics stats{}; CHECK(!dobby_get_hook_statistics(&stats));
  CHECK(!stats.active_entries && !stats.recovery_entries && stats.retained_mappings);
  std::printf("PASS all device tests; retained mappings=%u bytes=%llu\n",stats.retained_mappings,
              static_cast<unsigned long long>(stats.retained_bytes));
  // Published helper mappings remain alive, including the far replacement.
  CHECK(!munmap(source,8192)); return 0;
}
