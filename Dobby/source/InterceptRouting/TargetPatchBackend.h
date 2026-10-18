#pragma once
#include "dobby.h"
#include <cstddef>
#include <cstdint>
#include <mutex>

struct Interceptor;
namespace dobby {
// One process-wide lock and registry, defined in TargetPatchBackend.cc.
std::recursive_mutex &ManagementMutex();
int CurrentBackend();
bool SameProcess();
bool IsProtectedRange(uintptr_t address, size_t size);
bool IsHelperPage(uintptr_t page);
void RegisterHelper(void *address, size_t size);
void UnregisterHelper(void *address);
void RetainHelper(void *address);
int WxProbe();
int WxHook(void *address, void *replacement, void **original);
int WxRemove(void *entry);
int WxRefresh(void *entry);
void WxDiscard(void *entry);
}
