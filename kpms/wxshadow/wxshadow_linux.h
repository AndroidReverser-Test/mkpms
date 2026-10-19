/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WXSHADOW_LINUX_BRIDGE_H
#define WXSHADOW_LINUX_BRIDGE_H
#include "wxshadow_uapi.h"

/* Keep this bridge independent of both Linux and KernelPatch struct layouts.
 * The native object discovers the running kernel's layouts before use. */
struct wx_linux_host {
  unsigned long (*lookup)(const char *name);
  void *(*alloc)(unsigned long bytes, unsigned int flags);
  void (*free)(void *pointer);
  unsigned long (*alloc_page)(unsigned int flags, unsigned int order);
  void (*free_page)(unsigned long address, unsigned int order);
  unsigned long (*to_pfn)(void *address);
  void *(*from_pfn)(unsigned long pfn);
  int (*legacy_page)(void *mm, unsigned long address);
  unsigned int mm_pgd_offset;
  unsigned int page_shift, page_levels, kernel_version;
  void *(*current_task)(void);
};

int wx_linux_init(const struct wx_linux_host *host);
int wx_linux_request(int command, void *request, void *mm);
/* Copy a self-mm request without depending on inline/LTO usercopy symbols.
 * write==0 copies user to kernel, write==1 copies kernel to user. */
int wx_linux_user_copy(void *buffer, unsigned long address,
                       unsigned long size, int write);
int wx_linux_fault(void *mm, unsigned long address, int access);
/* A positive result holds the native control lock until after_fork, covering
 * the gap before dup_mmap acquires the old mm's mmap_lock. */
int wx_linux_before_fork(void *mm);
void wx_linux_after_fork(void);
void wx_linux_before_gup(void *vma, unsigned long address);
int wx_linux_owns(void *mm, unsigned long address);
int wx_linux_is_quiesced(void);
int wx_linux_available(void);
int wx_linux_legacy_lock(void);
void wx_linux_legacy_unlock(void);
void wx_protocol_init(void);
long wx_protocol_request(int command, unsigned long request, unsigned long a3,
                         unsigned long a4, unsigned long a5);
int wx_protocol_legacy_conflict(void *mm, unsigned long address);
unsigned long wxshadow_lookup_symbol(const char *name);
#endif
