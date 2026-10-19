/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runtime layout, never a guessed offset from a kernel release number. */
#include "../../common/wx_btf.h"
static struct {
    unsigned int mm_pgd, mm_lock, mm_count, mm_task_size;
    unsigned int task_mm, task_flags;
    unsigned int vma_start, vma_end, vma_flags, vma_mm;
    unsigned int range_start, range_end, range_flags;
    unsigned int notifier_size, notifier_ops, ops_size, ops_release, ops_start;
    unsigned int gup_arity;
} layout;
#define WX_FIELD(type, object, offset) (*(type *)((char *)(object) + (offset)))
#define WX_MM_LOCK(mm) ((struct rw_semaphore *)((char *)(mm) + layout.mm_lock))
#define WX_MM_COUNT(mm) WX_FIELD(int, mm, layout.mm_count)
#define WX_VMA(type, vma, member) WX_FIELD(type, vma, layout.vma_##member)
static u64 control_storage[64], state_storage[64], control_key[64], state_key[64];
static u64 notifier_ops_storage[64];
#define control_lock (*(struct mutex *)control_storage)
#define state_lock (*(struct mutex *)state_storage)
static struct page *(*normal_page_fn)(struct vm_area_struct *, unsigned long, pte_t);
static int (*access_vm_fn)(struct task_struct *, unsigned long, void *, int, unsigned int);

static int init_layout(void)
{
    struct wx_btf btf;
    unsigned int size, thread_offset, flags_offset, ignored;
    void (*init_mutex)(struct mutex *, const char *, void *);
    int rc = wx_btf_init(&btf, host.lookup, host.alloc, host.free, GFP_KERNEL);
    if (rc) return rc;
    if (host.kernel_version < KERNEL_VERSION(5, 10, 0) ||
        host.kernel_version >= KERNEL_VERSION(6, 7, 0)) { rc = -EOPNOTSUPP; goto out; }
#define MEMBER(s, m, bytes, output) do { rc = wx_btf_member(&btf, s, m, bytes, &(output)); if (rc) goto out; } while (0)
#define SIZE(s, output) do { rc = wx_btf_size(&btf, s, &(output)); if (rc) goto out; } while (0)
    MEMBER("mm_struct", "pgd", 8, layout.mm_pgd);
    SIZE("rw_semaphore", size);
    MEMBER("mm_struct", "mmap_lock", size, layout.mm_lock);
    MEMBER("mm_struct", "mm_count", 4, layout.mm_count);
    MEMBER("mm_struct", "task_size", 8, layout.mm_task_size);
    MEMBER("task_struct", "mm", 8, layout.task_mm);
    SIZE("thread_info", size);
    MEMBER("task_struct", "thread_info", size, thread_offset);
    MEMBER("thread_info", "flags", 8, flags_offset);
    layout.task_flags = thread_offset + flags_offset;
    MEMBER("vm_area_struct", "vm_start", 8, layout.vma_start);
    MEMBER("vm_area_struct", "vm_end", 8, layout.vma_end);
    MEMBER("vm_area_struct", "vm_flags", 8, layout.vma_flags);
    MEMBER("vm_area_struct", "vm_mm", 8, layout.vma_mm);
    MEMBER("mmu_notifier_range", "start", 8, layout.range_start);
    MEMBER("mmu_notifier_range", "end", 8, layout.range_end);
    MEMBER("mmu_notifier_range", "flags", 4, layout.range_flags);
    SIZE("mmu_notifier", layout.notifier_size);
    MEMBER("mmu_notifier", "ops", 8, layout.notifier_ops);
    SIZE("mmu_notifier_ops", layout.ops_size);
    MEMBER("mmu_notifier_ops", "release", 8, layout.ops_release);
    MEMBER("mmu_notifier_ops", "invalidate_range_start", 8, layout.ops_start);
    SIZE("mutex", size);
    if (size > sizeof(control_storage) || layout.ops_size > sizeof(notifier_ops_storage) ||
        !layout.notifier_size || layout.notifier_size > 4096 || layout.mm_pgd != host.mm_pgd_offset) {
        rc = -EPROTO; goto out;
    }
    /* PREEMPT_RT mutexes have different semantics and no owner field. */
    MEMBER("mutex", "owner", 8, ignored);
    MEMBER("mutex", "wait_list", 16, ignored);
    if (wx_btf_size(&btf, "lockdep_map", &size) == 0 && size &&
        wx_btf_member(&btf, "mutex", "dep_map", size, &ignored) == 0) {
        /* KPM memory is not registered as Linux module static key storage. */
        rc = -EOPNOTSUPP; goto out;
    }
    /* The bridge interprets the common ARM64 exception frame. */
#define REGS(m, bytes) do { MEMBER("pt_regs", #m, bytes, ignored); if (ignored != offsetof(struct pt_regs, m)) { rc = -EPROTO; goto out; } } while (0)
    REGS(regs, 31 * 8); REGS(sp, 8); REGS(pc, 8); REGS(pstate, 8);
#undef REGS
    if (wx_btf_func_arity(&btf, "__mutex_init") != 3 ||
        wx_btf_func_arity(&btf, "vm_normal_page") != 3 ||
        wx_btf_func_arity(&btf, "apply_to_existing_page_range") != 5 ||
        wx_btf_func_arity(&btf, "do_page_fault") != 3 ||
        wx_btf_func_arity(&btf, "follow_page_pte") != 5) { rc = -EPROTO; goto out; }
    /* Optimized static dup_mmap lacks a FUNC record on some Android GKI
     * builds. Its audited two-argument entry is still required by the bridge. */
    rc = wx_btf_optional_arity(&btf, "dup_mmap", 2);
    if (rc) goto out;
    if (!host.lookup("dup_mmap") &&
        (wx_btf_func_arity(&btf, "uprobe_start_dup_mmap") != 0 ||
         wx_btf_func_arity(&btf, "uprobe_end_dup_mmap") != 0)) { rc = -EPROTO; goto out; }
    if (wx_btf_func_arity(&btf, "mmu_notifier_register") != 2 ||
        wx_btf_func_arity(&btf, "mmu_notifier_unregister") != 2 ||
        wx_btf_func_arity(&btf, "release_pages") != 2 ||
        wx_btf_func_arity(&btf, "access_process_vm") != 5) { rc = -EPROTO; goto out; }
    rc = wx_btf_func_arity(&btf, "get_user_pages");
    if (rc != 4 && rc != 5) { rc = -EPROTO; goto out; }
    layout.gup_arity = rc;
    rc = wx_btf_func_param_named(&btf, "get_user_pages", 3, "page");
    if (rc) goto out;
    init_mutex = (void *)host.lookup("__mutex_init");
    normal_page_fn = (void *)host.lookup("vm_normal_page");
    access_vm_fn = (void *)host.lookup("access_process_vm");
    if (!init_mutex || !normal_page_fn || !access_vm_fn || !host.current_task ||
        host.page_shift != 12 || host.page_levels < 3 || host.page_levels > 4) { rc = -EOPNOTSUPP; goto out; }
    init_mutex(&control_lock, "wxshadow.control", control_key);
    init_mutex(&state_lock, "wxshadow.state", state_key);
    rc = 0;
out:
    wx_btf_destroy(&btf);
    return rc;
#undef MEMBER
#undef SIZE
}
static bool wx_compat_task(void)
{
    /* TIF_32BIT is architectural ARM64 bit 22 in the supported kernels. */
    return (WX_FIELD(unsigned long, host.current_task(), layout.task_flags) & (1UL << 22)) != 0;
}
