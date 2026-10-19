/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WXUPROBE_BRIDGE_H
#define WXUPROBE_BRIDGE_H
#include "wxuprobe_uapi.h"
/* Keep Linux/KP layouts on opposite sides of this bridge. */
struct wu_host {
    unsigned long (*lookup)(const char *);
    unsigned int mm_pgd_offset;
    unsigned int page_shift, page_levels;
    unsigned int kernel_version;
    unsigned int copy_via_analyze;
    unsigned int fork_uprobe_pair;
    void *(*from_pfn)(unsigned long);
    unsigned long (*to_pfn)(const void *);
};
int wu_linux_init(const struct wu_host *host);
void wu_linux_start(void);
long wu_linux_request(int command, unsigned long user_request);
long wu_linux_legacy(int operation, unsigned long user_request);
int wu_linux_fault(unsigned long address, unsigned int esr, void *regs);
int wu_linux_opcode(void *arch, void *mm, unsigned long address,
                    unsigned int opcode, long *result);
int wu_linux_copy_insn(void *mapping, void *file, void *destination,
                       int size, long long offset, long *result);
int wu_linux_analyze(void *arch, void *mm, unsigned long address, long *result);
int wu_linux_before_fork(void *mm);
void wu_linux_after_fork(void);
int wu_linux_before_current_fork(void);
int wu_linux_after_current_fork(void);
void wu_linux_before_gup(void *vma, unsigned long address);
int wu_linux_quiesced(void);
#endif
