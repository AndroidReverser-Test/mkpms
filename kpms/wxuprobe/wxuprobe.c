/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <compiler.h>
#include <kpmodule.h>
#include <kallsyms.h>
#include <hook.h>
#include <syscall.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/mm_types.h>
#include <linux/errno.h>
#include "wxuprobe_bridge.h"

KPM_NAME("wxuprobe");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v3");
KPM_AUTHOR("mkpms");
KPM_DESCRIPTION("Linux uprobes with W^X shadow pages and entry register editing");

static unsigned int in_flight;
static int prctl_hooked, mincore_hooked;
static void enter(void) { __atomic_add_fetch(&in_flight, 1, __ATOMIC_ACQUIRE); }
static void leave(void) { __atomic_sub_fetch(&in_flight, 1, __ATOMIC_RELEASE); }

struct symbol_search { const char *name; unsigned long address; };
static int symbol_callback(void *data, const char *name, struct module *module,
                           unsigned long address)
{
    struct symbol_search *search = data;
    if (!module && !strcmp(name, search->name)) {
        search->address = address;
        return 1;
    }
    return 0;
}
static unsigned long lookup(const char *name)
{
    struct symbol_search search = {name, 0};
    if (kallsyms_on_each_symbol) kallsyms_on_each_symbol(symbol_callback, &search);
    return search.address;
}

static void prctl_before(hook_fargs5_t *args, void *data)
{
    int command = (int)syscall_argn(args, 0);
    if (command < PR_WXUPROBE_QUERY || command > PR_WXUPROBE_QUIESCE) return;
    enter();
    if (syscall_argn(args, 2) || syscall_argn(args, 3) || syscall_argn(args, 4))
        args->ret = -EINVAL;
    else args->ret = wu_linux_request(command, syscall_argn(args, 1));
    args->skip_origin = 1;
    leave();
}
static void mincore_before(hook_fargs3_t *args, void *data)
{
    unsigned long operation = syscall_argn(args, 1);
    if (syscall_argn(args, 0) || operation < 511 || operation > 515) return;
    enter();
    args->ret = wu_linux_legacy((int)operation - 511, syscall_argn(args, 2));
    args->skip_origin = 1;
    leave();
}
static void fault_before(hook_fargs3_t *args, void *data)
{
    enter();
    if (wu_linux_fault(args->arg0, (unsigned int)args->arg1, (void *)args->arg2)) {
        args->ret = 0;
        args->skip_origin = 1;
    }
    leave();
}
static void opcode_before(hook_fargs4_t *args, void *data)
{
    long result;
    enter();
    if (wu_linux_opcode((void *)args->arg0, (void *)args->arg1, args->arg2,
                        (unsigned int)args->arg3, &result)) {
        args->ret = result;
        args->skip_origin = 1;
    }
    leave();
}
static void copy_before(hook_fargs5_t *args, void *data)
{
    long result;
    enter();
    if (wu_linux_copy_insn((void *)args->arg0, (void *)args->arg1, (void *)args->arg2,
                           (int)args->arg3, (long long)args->arg4, &result)) {
        args->ret = result;
        args->skip_origin = 1;
    }
    leave();
}
static void fork_before(hook_fargs2_t *args, void *data)
{
    enter();
    args->local.data0 = wu_linux_before_fork((void *)args->arg1);
    /* The paired after callback also protects the lock spanning dup_mmap. */
    if (!args->local.data0) leave();
}
static void fork_after(hook_fargs2_t *args, void *data)
{
    if (args->local.data0) { wu_linux_after_fork(); leave(); }
}
static void gup_before(hook_fargs5_t *args, void *data)
{
    enter();
    wu_linux_before_gup((void *)args->arg0, args->arg1);
    leave();
}

static struct {
    const char *name;
    void *address, *before, *after;
    int argc, installed;
} hooks[] = {
    {"uprobe_write_opcode", 0, opcode_before, 0, 4, 0},
    {"__copy_insn", 0, copy_before, 0, 5, 0},
    {"do_page_fault", 0, fault_before, 0, 3, 0},
    {"dup_mmap", 0, fork_before, fork_after, 2, 0},
    {"follow_page_pte", 0, gup_before, 0, 5, 0},
};

static void remove_hooks(void)
{
    unsigned int i;
    if (prctl_hooked) inline_unhook_syscalln(__NR_prctl, prctl_before, 0);
    if (mincore_hooked) inline_unhook_syscalln(__NR_mincore, mincore_before, 0);
    prctl_hooked = mincore_hooked = 0;
    for (i = sizeof(hooks) / sizeof(hooks[0]); i; --i)
        if (hooks[i - 1].installed) {
            hook_unwrap(hooks[i - 1].address, hooks[i - 1].before, hooks[i - 1].after);
            hooks[i - 1].installed = 0;
        }
    while (__atomic_load_n(&in_flight, __ATOMIC_ACQUIRE)) asm volatile("yield" ::: "memory");
}
static long module_init(const char *args, const char *event, void *reserved)
{
    unsigned int i;
    int rc;
    struct wu_host host = {lookup, mm_struct_offset.pgd_offset};
    /* Resolve everything before publishing any callback. No registrations or
     * notifier contexts exist during init, so rollback cannot need to sleep. */
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        hooks[i].address = (void *)lookup(hooks[i].name);
        if (!hooks[i].address) {
            pr_err("wxuprobe: missing required symbol %s\n", hooks[i].name);
            return -EOPNOTSUPP;
        }
    }
    rc = wu_linux_init(&host);
    if (rc) { pr_err("wxuprobe: native adapter rejected target: %d\n", rc); return rc; }
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        rc = hook_wrap(hooks[i].address, hooks[i].argc, hooks[i].before, hooks[i].after, 0);
        if (rc) goto fail;
        hooks[i].installed = 1;
    }
    rc = inline_hook_syscalln(__NR_prctl, 5, prctl_before, 0, 0);
    if (rc) goto fail;
    prctl_hooked = 1;
    rc = inline_hook_syscalln(__NR_mincore, 3, mincore_before, 0, 0);
    if (rc) goto fail;
    mincore_hooked = 1;
    wu_linux_start();
    pr_info("wxuprobe: ready; uprobe/XOL + shadow + x0-x30/SP; ABI %u\n", WXUPROBE_ABI_VERSION);
    return 0;
fail:
    remove_hooks();
    return rc ? rc : -EOPNOTSUPP;
}
static long module_control(const char *args, char *out, int size)
{
    pr_info("wxuprobe: quiesced=%d; use wxuprobe_client quiesce before unload\n", wu_linux_quiesced());
    return 0;
}
static long module_exit(void *reserved)
{
    /* The KP loader holds RCU and ignores exit errors. Sleeping teardown MUST
     * already have completed through the ordinary-process QUIESCE command. */
    if (!wu_linux_quiesced())
        pr_err("wxuprobe: UNSAFE UNLOAD: QUIESCE was not completed\n");
    remove_hooks();
    pr_info("wxuprobe: unloaded\n");
    return 0;
}
KPM_INIT(module_init);
KPM_CTL0(module_control);
KPM_EXIT(module_exit);
