
#include "dobby/dobby_internal.h"

#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <cerrno>
#include "InterceptRouting/TargetPatchBackend.h"

#if !defined(__APPLE__)
PUBLIC int DobbyCodePatch(void *address, uint8_t *buffer, uint32_t buffer_size) {
#if defined(__ANDROID__) || defined(__linux__)
  if (!address || !buffer || !buffer_size || (uintptr_t)address > UINTPTR_MAX - buffer_size) return -EINVAL;
  if (!dobby::SameProcess()) return DOBBY_ERROR_FORK;
  std::lock_guard<std::recursive_mutex> guard(dobby::ManagementMutex());
  if (dobby::IsProtectedRange((uintptr_t)address, buffer_size)) return -EBUSY;
  int page_size = (int)sysconf(_SC_PAGESIZE);
  uintptr_t patch_page = ALIGN_FLOOR(address, page_size);
  uintptr_t patch_end_page = ALIGN_FLOOR((uintptr_t)address + buffer_size - 1, page_size);
  size_t span = patch_end_page - patch_page + page_size;

  // change page permission as rwx
  if (mprotect((void *)patch_page, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return -errno;

  // patch buffer
  memcpy(address, buffer, buffer_size);

  // restore page permission
  int rc = mprotect((void *)patch_page, span, PROT_READ | PROT_EXEC) ? -errno : 0;

  addr_t clear_start_ = (addr_t)address;
  ClearCache((void *)clear_start_, (void *)(clear_start_ + buffer_size));
  return rc ? DOBBY_ERROR_RECOVERY_REQUIRED : 0;
#endif
  return 0;
}

#endif
