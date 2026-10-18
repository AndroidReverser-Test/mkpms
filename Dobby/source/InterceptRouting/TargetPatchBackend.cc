#include "TargetPatchBackend.h"
#include "Interceptor.h"
#include <cerrno>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

Interceptor gInterceptor;
namespace dobby {
static std::recursive_mutex mutex;
static int backend = DOBBY_HOOK_BACKEND_NATIVE;
static const int owner_pid = getpid();
struct Helper { uintptr_t address; size_t size; bool retained; };
static std::vector<Helper> helpers;
std::recursive_mutex &ManagementMutex() { return mutex; }
bool SameProcess() { return owner_pid == getpid(); }
int CurrentBackend() { return backend; }
bool IsHelperPage(uintptr_t page) {
  for (const auto &h : helpers)
    if (page >= h.address && page < h.address + h.size) return true;
  return false;
}
bool IsProtectedRange(uintptr_t address, size_t size) {
  if (!size || address > UINTPTR_MAX - (size - 1)) return size != 0;
  for (auto *e : gInterceptor.entries) {
    if (e->backend != DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) continue;
    auto page = e->addr & ~uintptr_t(4095);
    if (address <= page + 4095 && address + size - 1 >= page) return true;
  }
  return false;
}
void RegisterHelper(void *address, size_t size) { helpers.push_back({uintptr_t(address), size, false}); }
void UnregisterHelper(void *address) {
  for (auto i = helpers.begin(); i != helpers.end(); ++i)
    if (i->address == uintptr_t(address)) { helpers.erase(i); return; }
}
void RetainHelper(void *address) {
  for (auto &h : helpers) if (h.address == uintptr_t(address)) h.retained = true;
}
#ifndef DOBBY_WITH_WXSHADOW
int WxProbe() { return -ENOTSUP; }
int WxHook(void *, void *, void **) { return -ENOTSUP; }
int WxRemove(void *) { return -ENOTSUP; }
int WxRefresh(void *) { return -ENOTSUP; }
void WxDiscard(void *) {}
#endif
}

PUBLIC int dobby_set_hook_backend(DobbyHookBackend value) {
  if (!dobby::SameProcess()) return DOBBY_ERROR_FORK;
  std::lock_guard<std::recursive_mutex> guard(dobby::ManagementMutex());
  if (value != DOBBY_HOOK_BACKEND_NATIVE && value != DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) return -EINVAL;
  if (gInterceptor.count()) return -EBUSY;
  // Keep a requested strict mode even if probing fails. Ignoring this return
  // value must not turn a subsequent DobbyHook into a native source write.
  dobby::backend = value;
  if (value == DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) {
    int rc = dobby::WxProbe();
    if (rc) return rc;
  }
  return 0;
}

PUBLIC int dobby_get_hook_status(void *address, DobbyHookStatus *out) {
  if (!out || !address) return -EINVAL;
  if (!dobby::SameProcess()) return DOBBY_ERROR_FORK;
  std::lock_guard<std::recursive_mutex> guard(dobby::ManagementMutex());
  auto *e = gInterceptor.find((addr_t)address);
  if (!e) return -ENOENT;
  if (e->backend == DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) dobby::WxRefresh(e);
  *out = {sizeof(*out), (uint32_t)e->backend, (uint32_t)e->state, e->last_error,
          (uint64_t)e->addr, e->instance, e->generation, e->cookie};
  return 0;
}

PUBLIC int dobby_get_hook_statistics(DobbyHookStatistics *out) {
  if (!out) return -EINVAL;
  if (!dobby::SameProcess()) return DOBBY_ERROR_FORK;
  std::lock_guard<std::recursive_mutex> guard(dobby::ManagementMutex());
  *out = {};
  out->size = sizeof(*out);
  for (auto *e : gInterceptor.entries) {
    if (e->state == DOBBY_HOOK_RECOVERY_REQUIRED) ++out->recovery_entries;
    else if (e->state == DOBBY_HOOK_ACTIVE) ++out->active_entries;
  }
  for (const auto &h : dobby::helpers) if (h.retained) {
    ++out->retained_mappings; out->retained_bytes += h.size;
  }
  return 0;
}
