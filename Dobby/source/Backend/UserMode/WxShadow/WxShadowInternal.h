#pragma once
#include "Interceptor.h"
#include "InterceptRouting/TargetPatchBackend.h"
#include "wxshadow_uapi.h"
#include "wxshadow_arm64.h"

namespace dobby {
constexpr size_t WxPageSize = 4096;
void *WxAllocateNear(uintptr_t target);
int WxPublish(void *mapping, const void *code, size_t length);
void WxFree(void *mapping);
int WxBuildOriginal(uint32_t instruction, uintptr_t source, uintptr_t output, uint8_t *code, size_t *length);
int WxBuildRouting(Interceptor::Entry *entry);
}
