/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Transaction-only KPM bridge. KernelPatch supplies the running ARM64 page
 * geometry/current task; the native VM adapter validates all other layouts.
 * Do not link the legacy engine: its abbreviated structs and debug hooks are
 * unnecessary for Dobby's enhanced protocol and vary between kernel builds. */
#include <compiler.h>
#include <kpmodule.h>
#include <kallsyms.h>
#include <hook.h>
#include <syscall.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/mm_types.h>
#include <linux/errno.h>
#include <pgtable.h>
#include <asm/current.h>
#include "wxshadow_linux.h"
#include "../common/wx_arm64_runtime.h"

KPM_NAME("wxshadow");
KPM_VERSION("1.2.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("mkpms");
KPM_DESCRIPTION("Portable W^X transactional Dobby backend");

static unsigned int in_flight;
static int prctl_hooked;
static int started;
static int stopping;
static void *fork_owner;
static wx_u64 instance;
static struct wx_arm64_runtime runtime;
static void *(*get_task_mm_fn)(void *task);
static void (*mmput_fn)(void *mm);

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
unsigned long wxshadow_lookup_symbol(const char *name)
{
    struct symbol_search search = {name, 0};
    /* Exact matches matter: kallsyms_lookup_name can select an LTO clone
     * whose specialized calling convention no longer matches Linux's API. */
    if (kallsyms_on_each_symbol) kallsyms_on_each_symbol(symbol_callback, &search);
    return search.address;
}
static void *current_task(void) { return current; }
static unsigned long to_pfn(void *address)
{
    return wx_arm64_to_pfn(&runtime, address);
}
static void *from_pfn(unsigned long pfn)
{
    return wx_arm64_from_pfn(&runtime, pfn);
}
static int no_legacy_page(void *mm, unsigned long address) { return 0; }

static long protocol_request(int command, unsigned long user_request)
{
    union {
        struct wx_query query;
        struct wx_patch_request patch;
        struct wx_identity identity;
    } req;
    struct wx_request_header header;
    unsigned long size;
    void *mm;
    long rc;

    if (!user_request) return -EINVAL;
    rc = wx_linux_user_copy(&header, user_request, sizeof(header), 0);
    if (rc) return rc;
    size = command == PR_WXSHADOW_QUERY ? sizeof(req.query) :
           command == PR_WXSHADOW_PATCH_EX ? sizeof(req.patch) : sizeof(req.identity);
    if (header.version != WXSHADOW_ABI_VERSION || header.size != size ||
        header.flags || header.reserved) return -EINVAL;
    memset(&req, 0, sizeof(req));
    rc = wx_linux_user_copy(&req, user_request, size, 0);
    if (rc) return rc;
    if (req.query.header.version != header.version || req.query.header.size != size ||
        req.query.header.flags || req.query.header.reserved) return -EINVAL;
    if (command == PR_WXSHADOW_QUERY) {
        if (req.query.reserved[0] || req.query.reserved[1]) return -EINVAL;
        req.query.arch = WXSHADOW_ARCH_AARCH64;
        req.query.page_size = 1U << runtime.page_shift;
        req.query.instance = instance;
        req.query.capabilities = wx_linux_available() ?
            WX_CAP_REQUIRED | WX_CAP_FORK : 0;
        return wx_linux_user_copy(&req, user_request, size, 1);
    }
    if (req.identity.instance != instance) return -ESTALE;
    if (command == PR_WXSHADOW_PATCH_EX ? req.patch.reserved : req.identity.reserved)
        return -EINVAL;
    mm = get_task_mm_fn(current);
    if (!mm) return -ESRCH;
    rc = wx_linux_request(command, &req, mm);
    mmput_fn(mm);
    if (rc) return rc;
    /* A transaction can commit even if the response page becomes unwritable.
     * Preserve EFAULT so the client can resolve its cookie using QUERY_PATCH. */
    return wx_linux_user_copy(&req, user_request, size, 1);
}

static void prctl_before(hook_fargs5_t *args, void *data)
{
    int command = (int)syscall_argn(args, 0);
    if (command < PR_WXSHADOW_QUERY || command > PR_WXSHADOW_QUIESCE) return;
    enter();
    if (syscall_argn(args, 2) || syscall_argn(args, 3) || syscall_argn(args, 4))
        args->ret = -EINVAL;
    else args->ret = protocol_request(command, syscall_argn(args, 1));
    args->skip_origin = 1;
    leave();
}
static void fault_before(hook_fargs3_t *args, void *data)
{
    unsigned int esr = (unsigned int)args->arg1;
    unsigned int ec = esr >> 26;
    int access;
    void *mm;
    /* Only EL0 permission aborts belong to this protocol. Ignore S1PTW;
     * cache maintenance reads the original page even when WnR is also set. */
    if ((esr & 0x3c) != 0x0c) return;
    if (ec == 0x20) access = 1;
    else if (ec == 0x24 && !(esr & (1U << 7)))
        access = (esr & (1U << 8)) ? 2 : (esr & (1U << 6)) ? 3 : 2;
    else return;
    enter();
    mm = get_task_mm_fn(current);
    if (mm) {
        /* FAR can carry a top-byte tag for data accesses. */
        unsigned long address = args->arg0 & 0x00ffffffffffffffUL;
        if (wx_linux_fault(mm, address, access)) {
            args->ret = 0;
            args->skip_origin = 1;
        }
        mmput_fn(mm);
    }
    leave();
}
static void fork_before(hook_fargs2_t *args, void *data)
{
    args->local.data0 = 0;
    enter();
    if (__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) { leave(); return; }
    args->local.data0 = wx_linux_before_fork((void *)args->arg1);
    if (!args->local.data0) leave();
}
static void fork_after(hook_fargs2_t *args, void *data)
{
    if (args->local.data0) { wx_linux_after_fork(); leave(); }
}
/* LTO may inline dup_mmap itself. Linux's uprobe start/end functions bracket
 * every exit of its complete mmap-copy interval, outside the mmap locks.
 * The control mutex serializes successful starts, so a single owner is
 * sufficient; the end hook ignores forks that started before publication. */
static void fork_pair_start(hook_fargs0_t *args, void *data)
{
    void *mm;
    int held;
    enter();
    if (__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) { leave(); return; }
    mm = get_task_mm_fn(current);
    if (!mm) { leave(); return; }
    held = wx_linux_before_fork(mm);
    mmput_fn(mm);
    if (held) __atomic_store_n(&fork_owner, (void *)current, __ATOMIC_RELEASE);
    else leave();
}
static void fork_pair_end(hook_fargs0_t *args, void *data)
{
    void *owner = current;
    if (__atomic_compare_exchange_n(&fork_owner, &owner, (void *)0, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        wx_linux_after_fork();
        leave();
    }
}
static void gup_before(hook_fargs5_t *args, void *data)
{
    enter();
    wx_linux_before_gup((void *)args->arg0, args->arg1);
    leave();
}
static struct {
    const char *name;
    void *address, *before, *after;
    int argc, installed;
} hooks[] = {
    {"do_page_fault", 0, fault_before, 0, 3, 0},
    /* Install the optional end hook before publishing its start hook. */
    {0, 0, 0, fork_pair_end, 0, 0},
    {"dup_mmap", 0, fork_before, fork_after, 2, 0},
    {"follow_page_pte", 0, gup_before, 0, 5, 0},
};
enum { FAULT_HOOK, FORK_END_HOOK, FORK_START_HOOK, GUP_HOOK };

static void remove_one_hook(unsigned int i)
{
    if (hooks[i].installed) {
        hook_unwrap(hooks[i].address, hooks[i].before, hooks[i].after);
        hooks[i].installed = 0;
    }
}

static void remove_hooks(void)
{
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    if (prctl_hooked) inline_unhook_syscalln(__NR_prctl, prctl_before, 0);
    prctl_hooked = 0;
    if (hooks[FORK_END_HOOK].installed) remove_one_hook(FORK_START_HOOK);
    remove_one_hook(GUP_HOOK);
    remove_one_hook(FAULT_HOOK);
    /* Preserve the paired after callback until every held control mutex has
     * been released, including forks running during failed initialization. */
    while (__atomic_load_n(&in_flight, __ATOMIC_ACQUIRE)) asm volatile("yield" ::: "memory");
    remove_one_hook(FORK_START_HOOK);
    remove_one_hook(FORK_END_HOOK);
}
static long module_init(const char *args, const char *event, void *reserved)
{
    struct wx_linux_host host = {
        .lookup = wxshadow_lookup_symbol,
        .to_pfn = to_pfn, .from_pfn = from_pfn, .legacy_page = no_legacy_page,
        .mm_pgd_offset = mm_struct_offset.pgd_offset,
        .current_task = current_task,
    };
    wx_u64 (*random_u64)(void);
    unsigned int i;
    int rc;
    /* Resolve and validate the complete adapter before publishing callbacks.
     * Unsupported layouts/symbol ABIs leave no kernel hook installed. */
    host.alloc = (void *)wxshadow_lookup_symbol("__kmalloc");
    if (!host.alloc) host.alloc = (void *)wxshadow_lookup_symbol("__kmalloc_noprof");
    host.free = (void *)wxshadow_lookup_symbol("kfree");
    host.alloc_page = (void *)wxshadow_lookup_symbol("__get_free_pages");
    host.free_page = (void *)wxshadow_lookup_symbol("free_pages");
    random_u64 = (void *)wxshadow_lookup_symbol("get_random_u64");
    get_task_mm_fn = (void *)wxshadow_lookup_symbol("get_task_mm");
    mmput_fn = (void *)wxshadow_lookup_symbol("mmput");
    if (!host.alloc || !host.free || !host.alloc_page || !host.free_page ||
        !random_u64 || !get_task_mm_fn || !mmput_fn) return -EOPNOTSUPP;
    if (mm_struct_offset.pgd_offset < 0) return -EOPNOTSUPP;
    rc = wx_arm64_runtime_init(&runtime, wxshadow_lookup_symbol,
                                host.alloc_page, host.free_page);
    if (rc) {
        pr_err("wxshadow: runtime page geometry/direct-map validation failed: %d\n", rc);
        return rc;
    }
    host.page_shift = runtime.page_shift;
    host.page_levels = runtime.page_levels;
    host.kernel_version = runtime.kernel_version;
    instance = random_u64();
    if (!instance) return -EAGAIN;
    hooks[FORK_START_HOOK].address = (void *)wxshadow_lookup_symbol("dup_mmap");
    if (!hooks[FORK_START_HOOK].address) {
        hooks[FORK_START_HOOK].name = "uprobe_start_dup_mmap";
        hooks[FORK_START_HOOK].before = fork_pair_start;
        hooks[FORK_START_HOOK].after = 0;
        hooks[FORK_START_HOOK].argc = 0;
        hooks[FORK_END_HOOK].name = "uprobe_end_dup_mmap";
    }
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        if (!hooks[i].name) continue;
        if (!hooks[i].address)
            hooks[i].address = (void *)wxshadow_lookup_symbol(hooks[i].name);
        if (!hooks[i].address) {
            pr_err("wxshadow: missing required symbol %s\n", hooks[i].name);
            return -EOPNOTSUPP;
        }
    }
    rc = wx_linux_init(&host);
    if (rc) { pr_err("wxshadow: native adapter rejected target: %d\n", rc); return rc; }
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        if (!hooks[i].address) continue;
        rc = hook_wrap(hooks[i].address, hooks[i].argc, hooks[i].before, hooks[i].after, 0);
        if (rc) goto fail;
        hooks[i].installed = 1;
    }
    rc = inline_hook_syscalln(__NR_prctl, 5, prctl_before, 0, 0);
    if (rc) goto fail;
    prctl_hooked = 1;
    started = 1;
    pr_info("wxshadow: portable VM adapter ready; ABI %u page=%u levels=%u kernel=%x\n",
            WXSHADOW_ABI_VERSION, 1U << runtime.page_shift, runtime.page_levels,
            runtime.kernel_version);
    return 0;
fail:
    remove_hooks();
    return rc < 0 ? rc : -EOPNOTSUPP;
}
static long module_control(const char *args, char *out, int size)
{
    pr_info("wxshadow: quiesced=%d; complete enhanced-protocol QUIESCE before unload\n",
            wx_linux_is_quiesced());
    return 0;
}
static long module_exit(void *reserved)
{
    /* The KP loader holds RCU and ignores exit errors. Sleeping notifier
     * teardown must already have completed in the QUIESCE prctl callback. */
    if (started && !wx_linux_is_quiesced())
        pr_err("wxshadow: UNSAFE UNLOAD: QUIESCE was not completed\n");
    remove_hooks();
    pr_info("wxshadow: unloaded\n");
    return 0;
}
KPM_INIT(module_init);
KPM_CTL0(module_control);
KPM_EXIT(module_exit);
