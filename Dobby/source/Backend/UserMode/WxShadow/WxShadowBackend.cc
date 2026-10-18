#include "WxShadowInternal.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ctime>
#include <link.h>

#ifdef DOBBY_WXSHADOW_TESTING
extern __attribute__((visibility("default"))) int dobby_wx_test_transport(int, void *);
#endif
namespace dobby {
namespace {
template <class T> T request() {
  T value{}; value.header.version = WXSHADOW_ABI_VERSION;
  value.header.size = sizeof(T); return value;
}
int transport(int command, void *data) {
#ifdef DOBBY_WXSHADOW_TESTING
  return dobby_wx_test_transport(command, data);
#else
  int rc = prctl(command, (unsigned long)data, 0UL, 0UL, 0UL);
  return rc == -1 ? -errno : rc;
#endif
}
int probe(wx_query *out) {
  if (sysconf(_SC_PAGESIZE) != 4096) return -ENOTSUP;
  *out = request<wx_query>();
  int rc = transport(PR_WXSHADOW_QUERY, out);
  if (rc) return rc;
  if (out->header.version != WXSHADOW_ABI_VERSION || out->header.size != sizeof(*out) ||
      out->arch != WXSHADOW_ARCH_AARCH64 || out->page_size != 4096 || !out->instance ||
      (out->capabilities & WX_CAP_REQUIRED) != WX_CAP_REQUIRED) return -ENOTSUP;
  return 0;
}
uint64_t next_cookie() {
  static uint64_t sequence = 0;
  static bool exhausted = false;
  if (exhausted) return 0;
  if (!sequence) {
#ifdef SYS_getrandom
    if (syscall(SYS_getrandom, &sequence, sizeof(sequence), 0) != sizeof(sequence))
#endif
      sequence = (uint64_t(time(nullptr)) << 32) ^ uintptr_t(&sequence) ^ uint64_t(getpid());
    sequence |= 1;
  }
  uint64_t result = sequence++;
  if (!sequence) exhausted = true;
  return result;
}
bool executable_readonly(uintptr_t address) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps) return false;
  char line[1024], permission[8];
  bool valid = false;
  while (fgets(line, sizeof(line), maps)) {
    unsigned long long begin, end;
    if (sscanf(line, "%llx-%llx %7s", &begin, &end, permission) == 3 &&
        address >= begin && address + 4 <= end) {
      valid = permission[0] == 'r' && permission[1] != 'w' && permission[2] == 'x' && permission[3] == 'p';
      break;
    }
  }
  fclose(maps); return valid;
}
bool critical_page(uintptr_t page) {
  const uintptr_t functions[] = {(uintptr_t)&DobbyHook, (uintptr_t)&DobbyDestroy,
    (uintptr_t)&dobby_set_hook_backend, (uintptr_t)&WxHook, (uintptr_t)&WxRemove,
    (uintptr_t)&WxRefresh, (uintptr_t)&WxBuildRouting, (uintptr_t)&WxPublish,
    (uintptr_t)&WxAllocateNear, (uintptr_t)&transport, (uintptr_t)&probe,
    (uintptr_t)&prctl, (uintptr_t)&syscall, (uintptr_t)&mmap, (uintptr_t)&mprotect,
    (uintptr_t)&munmap, (uintptr_t)&memcpy, (uintptr_t)&malloc, (uintptr_t)&free,
    (uintptr_t)&pthread_mutex_lock, (uintptr_t)&pthread_mutex_unlock,
    (uintptr_t)&fopen, (uintptr_t)&fgets, (uintptr_t)&fclose};
  for (auto address : functions) if ((address & ~uintptr_t(4095)) == page) return true;
  return false;
}
struct PropertyCheck { uintptr_t target; bool unsupported; };
int check_properties(dl_phdr_info *info, size_t, void *data) {
  auto *check = (PropertyCheck *)data;
  bool contains = false;
  for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
    const auto &p = info->dlpi_phdr[i];
    if (p.p_type == PT_LOAD && check->target >= info->dlpi_addr + p.p_vaddr &&
        check->target < info->dlpi_addr + p.p_vaddr + p.p_memsz) contains = true;
  }
  if (!contains) return 0;
  for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
    const auto &p = info->dlpi_phdr[i];
    if (p.p_type != 0x6474e553) continue; // PT_GNU_PROPERTY
    const uint8_t *cursor = (const uint8_t *)(info->dlpi_addr + p.p_vaddr);
    const uint8_t *end = cursor + p.p_memsz;
    while (end - cursor >= 12) {
      uint32_t namesz, descsz, type;
      memcpy(&namesz, cursor, 4); memcpy(&descsz, cursor + 4, 4); memcpy(&type, cursor + 8, 4);
      cursor += 12;
      size_t padded_name = (size_t(namesz) + 3) & ~size_t(3);
      size_t padded_desc = (size_t(descsz) + 3) & ~size_t(3);
      if (padded_name > size_t(end - cursor) || padded_desc > size_t(end - cursor) - padded_name) break;
      bool gnu = namesz == 4 && !memcmp(cursor, "GNU", 4) && type == 5;
      const uint8_t *prop = cursor + padded_name, *prop_end = prop + descsz;
      if (gnu) while (prop_end - prop >= 8) {
        uint32_t kind, bytes; memcpy(&kind, prop, 4); memcpy(&bytes, prop + 4, 4); prop += 8;
        size_t padded = (size_t(bytes) + 7) & ~size_t(7);
        if (padded > size_t(prop_end - prop)) break;
        if (kind == 0xc0000000 && bytes == 4) {
          uint32_t feature; memcpy(&feature, prop, 4);
          if (feature & 3) check->unsupported = true;
        }
        prop += padded;
      }
      cursor += padded_name + padded_desc;
    }
  }
  return 1;
}
int validate(uintptr_t source, uintptr_t replacement) {
  if (!source || !replacement || ((source | replacement) & 3)) return -EINVAL;
  auto page = source & ~uintptr_t(4095);
  if (IsProtectedRange(replacement, 4)) return -ENOTSUP;
  for (auto *e : gInterceptor.entries)
    if ((e->fake_func_addr & ~addr_t(4095)) == page) return -ENOTSUP;
  if ((replacement & ~uintptr_t(4095)) == page || IsHelperPage(page) || critical_page(page)) return -ENOTSUP;
  if (!executable_readonly(source) || !executable_readonly(replacement)) return -ENOTSUP;
  PropertyCheck property{source, false};
  dl_iterate_phdr(check_properties, &property);
  if (property.unsupported) return -ENOTSUP;
  property = {replacement, false}; dl_iterate_phdr(check_properties, &property);
  return property.unsupported ? -ENOTSUP : 0;
}
int query(Interceptor::Entry *e, uint32_t *state) {
  wx_query module;
  int rc = probe(&module);
  if (rc) return rc;
  if (module.instance != e->instance) return DOBBY_ERROR_BACKEND_LOST;
  auto id = request<wx_identity>();
  id.instance = e->instance; id.cookie = e->cookie; id.address = e->addr; id.generation = e->generation;
  rc = transport(PR_WXSHADOW_QUERY_PATCH, &id);
  if (rc) return rc;
  if (id.instance != e->instance || id.cookie != e->cookie || id.address != e->addr ||
      (e->generation && id.generation && e->generation != id.generation) || id.state > WX_PATCH_CONFLICT)
    return DOBBY_ERROR_RECOVERY_REQUIRED;
  if (id.generation) e->generation = id.generation;
  *state = id.state; return 0;
}
void retain(Interceptor::Entry *e) {
  e->ever_published = true;
  for (auto mapping : e->helper_mappings) RetainHelper(mapping);
}
}
int WxProbe() { wx_query module; return probe(&module); }
void WxDiscard(void *pointer) {
  auto *e = (Interceptor::Entry *)pointer;
  if (!e->ever_published) for (auto mapping : e->helper_mappings) WxFree(mapping);
}
int WxHook(void *address, void *replacement, void **original) {
  if (gInterceptor.find((addr_t)address)) return -EEXIST;
  wx_query module;
  int rc = probe(&module);
  if (rc) return rc;
  for (auto *e : gInterceptor.entries) {
    if (e->instance != module.instance) {
      e->state = DOBBY_HOOK_RECOVERY_REQUIRED; e->last_error = DOBBY_ERROR_BACKEND_LOST;
      return DOBBY_ERROR_BACKEND_LOST;
    }
  }
  rc = validate((uintptr_t)address, (uintptr_t)replacement);
  if (rc) return rc;
  auto *e = new (std::nothrow) Interceptor::Entry((addr_t)address);
  if (!e) return -ENOMEM;
  e->backend = DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED;
  e->fake_func_addr = (addr_t)replacement; e->instance = module.instance;
  e->cookie = next_cookie(); e->owner_pid = getpid();
  if (!e->cookie) { delete e; return -EOVERFLOW; }
  for (auto *existing : gInterceptor.entries)
    if ((existing->addr & ~addr_t(4095)) == (e->addr & ~addr_t(4095))) {
      uint32_t state;
      rc = query(existing, &state);
      if (rc || state != WX_PATCH_ACTIVE) { delete e; return rc ? rc : -ESTALE; }
      e->generation = existing->generation;
    }
  // Read without mprotect; an existing source page faults to the original PFN.
  memcpy(&e->wx_original, address, 4);
  rc = WxBuildRouting(e);
  if (rc) { WxDiscard(e); delete e; return rc; }
  auto patch = request<wx_patch_request>();
  patch.instance = e->instance; patch.cookie = e->cookie; patch.address = e->addr;
  patch.generation = e->generation; patch.expected = e->wx_original; patch.replacement = e->wx_branch;
  e->state = DOBBY_HOOK_PREPARED;
  if (original) *original = (void *)e->relocated.addr();
  gInterceptor.add(e); // Protect the page before the first possible publication.
  rc = transport(PR_WXSHADOW_PATCH_EX, &patch);
  uint32_t state = WX_PATCH_ABSENT;
  int resolved = query(e, &state); // Also resolves lost syscall result copies.
  if (!resolved && state == WX_PATCH_ACTIVE) {
    e->state = DOBBY_HOOK_ACTIVE; e->last_error = 0; retain(e); return 0;
  }
  if (!resolved && state == WX_PATCH_ABSENT && rc) {
    gInterceptor.remove(e->addr); if (original) *original = nullptr;
    WxDiscard(e); delete e; return rc;
  }
  // A retired cookie may have been reachable before invalidation. Keep code.
  retain(e); e->state = DOBBY_HOOK_RECOVERY_REQUIRED;
  e->last_error = resolved ? resolved : (rc ? rc : -ESTALE);
  return resolved == DOBBY_ERROR_BACKEND_LOST ? resolved : DOBBY_ERROR_RECOVERY_REQUIRED;
}
int WxRefresh(void *pointer) {
  auto *e = (Interceptor::Entry *)pointer;
  uint32_t state;
  int rc = query(e, &state);
  if (rc || state == WX_PATCH_CONFLICT) {
    e->state = DOBBY_HOOK_RECOVERY_REQUIRED; e->last_error = rc ? rc : -EEXIST;
    return e->last_error;
  }
  e->state = state == WX_PATCH_ACTIVE ? DOBBY_HOOK_ACTIVE : DOBBY_HOOK_RETIRED;
  e->last_error = 0; return 0;
}
int WxRemove(void *pointer) {
  auto *e = (Interceptor::Entry *)pointer;
  uint32_t state;
  int rc = query(e, &state);
  if (rc) return rc;
  if (state == WX_PATCH_ABSENT || state == WX_PATCH_RETIRED) return 0;
  if (state != WX_PATCH_ACTIVE) return -EEXIST;
  auto id = request<wx_identity>();
  id.instance = e->instance; id.cookie = e->cookie; id.address = e->addr; id.generation = e->generation;
  int release = transport(PR_WXSHADOW_RELEASE_EX, &id);
  rc = query(e, &state);
  if (!rc && (state == WX_PATCH_ABSENT || state == WX_PATCH_RETIRED)) return 0;
  return rc ? rc : release ? release : DOBBY_ERROR_RECOVERY_REQUIRED;
}
}
