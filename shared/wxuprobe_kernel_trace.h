/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WXUPROBE_KERNEL_TRACE_H
#define WXUPROBE_KERNEL_TRACE_H
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "wxuprobe_uapi.h"

/* Binary compatible with Kernel-Trace/user/uprobe_trace_user.h on ARM64.
 * Use this header instead of that header, not alongside it. */
#define TRACE_FLAG 511
#define MAX_HOOK_NUM 2000
#define SET_TRACE_SUCCESS 1000
#define SET_TRACE_ERROR 1001
enum trace_info { SET_TRACE_INFO, SET_FUN_INFO, CLEAR_UPROBE, SET_FUN_REGS, DEL_FUN_INFO };
struct trace_init_info {
    uid_t uid;
    unsigned long module_base;
    char *tfile_name;
    char *fix_file_name;
};
struct uprobe_item_info {
    unsigned long uprobe_offset;
    unsigned long fun_offset;
    char *fun_name;
};
struct uprobe_reg_info {
    unsigned long fun_offset;
    wu_u64 reg_mask;
    wu_u64 regs[32];
};
static inline int clear_all_uprobes(void)
{
    return (int)syscall(SYS_mincore, 0UL, TRACE_FLAG + CLEAR_UPROBE, 0UL);
}
static inline int trace_init(struct trace_init_info *info)
{
    int rc = clear_all_uprobes();
    if (rc != SET_TRACE_SUCCESS) return rc;
    return (int)syscall(SYS_mincore, 0UL, TRACE_FLAG + SET_TRACE_INFO, info);
}
static inline int set_fun_info(struct uprobe_item_info *info)
{
    return (int)syscall(SYS_mincore, 0UL, TRACE_FLAG + SET_FUN_INFO, info);
}
static inline int set_fun_regs(struct uprobe_reg_info *info)
{
    return (int)syscall(SYS_mincore, 0UL, TRACE_FLAG + SET_FUN_REGS, info);
}
static inline int del_fun_info(unsigned long fun_offset)
{
    struct uprobe_reg_info info = {0};
    info.fun_offset = fun_offset;
    return (int)syscall(SYS_mincore, 0UL, TRACE_FLAG + DEL_FUN_INFO, &info);
}
#endif
