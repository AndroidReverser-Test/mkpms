#pragma once

#include "dobby/common.h"
#include "MemoryAllocator/MemoryAllocator.h"

#include "TrampolineBridge/Trampoline/Trampoline.h"
#include <vector>
#include <new>

typedef enum { kFunctionInlineHook, kInstructionInstrument } InterceptRoutingType;

struct InterceptRouting;
struct Interceptor {
  struct Entry {
    uint32_t id = 0;

    struct {
      bool arm_thumb_mode;
    } features{};

    addr_t fake_func_addr = 0;
    dobby_instrument_callback_t pre_handler = nullptr;
    dobby_instrument_callback_t post_handler = nullptr;

    addr_t addr;

    MemBlock patched;
    MemBlock relocated;

    InterceptRouting *routing = nullptr;
    DobbyHookBackend backend = DOBBY_HOOK_BACKEND_NATIVE;
    DobbyHookState state = DOBBY_HOOK_PREPARING;
    uint64_t instance = 0, generation = 0, cookie = 0;
    int owner_pid = 0, last_error = 0;
    bool ever_published = false;
    uint32_t wx_original = 0, wx_branch = 0;
    std::vector<void *> helper_mappings;

    uint8_t *origin_code_ = 0;

    Entry(addr_t addr) {
      this->addr = addr;
    }

    ~Entry() { operator delete(origin_code_); }

    bool backup_orig_code() {
      __FUNC_CALL_TRACE__();
      auto orig = (uint8_t *)this->addr;
      uint32_t tramp_size = this->patched.size;
      origin_code_ = (uint8_t *)operator new(tramp_size, std::nothrow);
      if (!origin_code_) return false;
      memcpy(origin_code_, orig, tramp_size);
      return true;
    }

    int restore_orig_code() {
      __FUNC_CALL_TRACE__();
      return DobbyCodePatch((void *)patched.addr(), origin_code_, patched.size);
    }

    void feature_set_arm_thumb(bool thumb) {
      features.arm_thumb_mode = thumb;
    }
  };

  stl::vector<Entry *> entries;

  static Interceptor *Shared();

  Entry *find(addr_t addr) {
    for (auto *entry : entries) {
      if (entry->addr == addr || entry->patched.addr() == addr) {
        return entry;
      }
    }
    return nullptr;
  }

  Entry *remove(addr_t addr) {
    for (auto iter = entries.begin(); iter != entries.end(); iter++) {
      Entry *entry = *iter;
      if (entry->addr == addr || entry->patched.addr() == addr) {
        entries.erase(iter);
        return entry;
      }
    }
    return nullptr;
  }

  void add(Entry *entry) {
    entries.push_back(entry);
  }

  const Entry *get(int i) {
    return entries[i];
  }

  int count() const {
    return entries.size();
  }
};

extern Interceptor gInterceptor;

inline Interceptor *Interceptor::Shared() {
  return &gInterceptor;
}
