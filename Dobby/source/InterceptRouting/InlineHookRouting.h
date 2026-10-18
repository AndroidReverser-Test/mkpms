#pragma once

#include "dobby/common.h"
#include "InterceptRouting/InterceptRouting.h"
#include "TrampolineBridge/ClosureTrampolineBridge/ClosureTrampoline.h"
#include "TargetPatchBackend.h"
#include <cerrno>

struct InlineHookRouting : InterceptRouting {
  addr_t fake_func;

  InlineHookRouting(Interceptor::Entry *entry, addr_t fake_func) : InterceptRouting(entry), fake_func(fake_func) {
  }

  ~InlineHookRouting() = default;

  addr_t TrampolineTarget() override {
    return fake_func;
  }

  void BuildRouting() {
    __FUNC_CALL_TRACE__();

    if (!GenerateTrampoline()) return;

    GenerateRelocatedCode();
    if (error) return;

    BackupOriginCode();
  }
};

#ifdef DOBBY_API_IMPLEMENTATION
PUBLIC int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
  __FUNC_CALL_TRACE__();
  if (!address || !fake_func) {
    ERROR_LOG("address is 0x0");
    return -1;
  }

  features::apple::arm64e_pac_strip(address);
  features::apple::arm64e_pac_strip(fake_func);
  if (!dobby::SameProcess()) return DOBBY_ERROR_FORK;
  std::lock_guard<std::recursive_mutex> guard(dobby::ManagementMutex());
  if (dobby::CurrentBackend() == DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED)
    return dobby::WxHook(address, fake_func, out_origin_func);
  if (!features::android::make_memory_readable(address, 4)) return -errno;

  DEBUG_LOG("----- [DobbyHook: %p] -----", address);

  auto entry = gInterceptor.find((addr_t)address);
  if (entry) {
    ERROR_LOG("%p already been hooked.", address);
    return -1;
  }

  entry = new (std::nothrow) Interceptor::Entry((addr_t)address);
  if (!entry) return -ENOMEM;
  entry->fake_func_addr = (addr_t)fake_func;

  auto routing = new (std::nothrow) InlineHookRouting(entry, (addr_t)fake_func);
  if (!routing) { delete entry; return -ENOMEM; }
  routing->BuildRouting();
  entry->routing = routing;

  if (routing->error) {
    ERROR_LOG("build routing error.");
    delete routing; delete entry;
    return -1;
  }

  if (out_origin_func) {
    *out_origin_func = (void *)entry->relocated.addr();
    features::apple::arm64e_pac_strip_and_sign(*out_origin_func);
  }
  entry->state = DOBBY_HOOK_PREPARED;
  gInterceptor.add(entry);
  routing->Active();
#if defined(__ANDROID__) || defined(__linux__)
  if (routing->error && routing->error != DOBBY_ERROR_RECOVERY_REQUIRED) {
    // The POSIX writer failed before copying any entry bytes.
    int rc = routing->error;
    gInterceptor.remove(entry->addr);
    if (out_origin_func) *out_origin_func = nullptr;
    delete routing; delete entry;
    return rc;
  }
#endif
  entry->last_error = routing->error;
  entry->state = routing->error ? DOBBY_HOOK_RECOVERY_REQUIRED : DOBBY_HOOK_ACTIVE;
  entry->ever_published = true;
  return routing->error ? DOBBY_ERROR_RECOVERY_REQUIRED : 0;
}
#endif
