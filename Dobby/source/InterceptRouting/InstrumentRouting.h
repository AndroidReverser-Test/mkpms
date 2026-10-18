#pragma once

#include "dobby/common.h"
#include "InterceptRouting/InterceptRouting.h"
#include "TrampolineBridge/ClosureTrampolineBridge/ClosureTrampoline.h"
#include "TargetPatchBackend.h"
#include <cerrno>

struct InstrumentRouting : InterceptRouting {
  ClosureTrampoline *instrument_tramp = nullptr;

  InstrumentRouting(Interceptor::Entry *entry, dobby_instrument_callback_t pre_handler) : InterceptRouting(entry) {
  }

  ~InstrumentRouting() {
    if (instrument_tramp) {
      // TODO: free code block
      delete instrument_tramp;
    }
  }

  addr_t TrampolineTarget() override {
    return instrument_tramp->addr();
  }

  void GenerateInstrumentClosureTrampoline() {
    __FUNC_CALL_TRACE__();
    instrument_tramp = ::GenerateInstrumentClosureTrampoline(entry);
  }

  void BuildRouting() {
    __FUNC_CALL_TRACE__();

    GenerateInstrumentClosureTrampoline();
    if (!instrument_tramp || !instrument_tramp->addr()) { error = -ENOMEM; return; }

    if (!GenerateTrampoline()) return;

    GenerateRelocatedCode();
    if (error) return;

    BackupOriginCode();
  }
};

#ifdef DOBBY_API_IMPLEMENTATION
PUBLIC int DobbyInstrument(void *address, dobby_instrument_callback_t pre_handler) {
  __FUNC_CALL_TRACE__();
  if (!address || !pre_handler) {
    ERROR_LOG("address is 0x0.");
    return -1;
  }

  features::apple::arm64e_pac_strip(address);
  if (!dobby::SameProcess()) return DOBBY_ERROR_FORK;
  std::lock_guard<std::recursive_mutex> guard(dobby::ManagementMutex());
  if (dobby::CurrentBackend() == DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED) return -ENOTSUP;
  if (!features::android::make_memory_readable(address, 4)) return -errno;

  DEBUG_LOG("----- [DobbyInstrument: %p] -----", address);

  auto entry = gInterceptor.find((addr_t)address);
  if (entry) {
    ERROR_LOG("%s already been instrumented.", address);
    return -1;
  }

  entry = new (std::nothrow) Interceptor::Entry((addr_t)address);
  if (!entry) return -ENOMEM;
  entry->pre_handler = pre_handler;

  auto routing = new (std::nothrow) InstrumentRouting(entry, pre_handler);
  if (!routing) { delete entry; return -ENOMEM; }
  routing->BuildRouting();
  entry->routing = routing;

  if (routing->error) {
    ERROR_LOG("build routing error.");
    delete routing; delete entry;
    return -1;
  }
  gInterceptor.add(entry);
  entry->state = DOBBY_HOOK_PREPARED;
  routing->Active();
#if defined(__ANDROID__) || defined(__linux__)
  if (routing->error && routing->error != DOBBY_ERROR_RECOVERY_REQUIRED) {
    int rc = routing->error;
    gInterceptor.remove(entry->addr);
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
