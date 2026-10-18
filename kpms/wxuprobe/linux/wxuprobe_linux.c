// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Exact-kernel implementation. Page publication, retirement and TLB/cache
 * handling follow kpms/wxshadow/linux/wxshadow_linux_adapter.c (GPL-2.0+).
 * Linux owns uprobe decoding, BRK delivery, XOL slots and single stepping.
 * We redirect only our uprobe's opcode publication to an execute-only page.
 *
 * Lock order: control -> kernel uprobe locks -> mmap_lock -> state -> PTL.
 * Never call uprobe_register/unregister with state or mmap_lock held.
 * GUP runs before state is acquired. Notifiers take state -> PTL only.
 */
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uprobes.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/sched/mm.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/rcupdate.h>
#include <generated/utsrelease.h>
#include <asm/pgtable.h>
#include "../wxuprobe_bridge.h"

#if !defined(CONFIG_ARM64_4K_PAGES) || !defined(CONFIG_MMU_NOTIFIER) || !defined(CONFIG_UPROBES)
#error "wxuprobe requires ARM64 4K pages, MMU_NOTIFIER and UPROBES"
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0) || LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#error "wxuprobe currently supports the audited Linux 5.10 VM/uprobe APIs"
#endif
#if CONFIG_ARM64_PA_BITS > 48 || CONFIG_ARM64_VA_BITS > 48 || defined(CONFIG_PREEMPT_RT) || defined(CONFIG_GCC_PLUGIN_RANDSTRUCT) || defined(CONFIG_RANDSTRUCT)
#error "unsupported physical/virtual address width, RT locking or randomized layout"
#endif

#define WU_MAX_CONTEXTS 64U
#define WU_MAX_PAGES 1024U
#define WU_MAX_PROBES 8192U
#define WU_MAX_PAGE_PROBES 128U
#define WU_MAX_SHADOWS 8192U

static struct wu_host host;
static DEFINE_MUTEX(control_lock);
static DEFINE_MUTEX(state_lock);
static LIST_HEAD(contexts);
static bool ready, stopped;
static u64 instance, legacy_cookie;
static unsigned int context_count, retained_pages, probe_count, shadow_count;

static void *(*alloc_fn)(size_t, gfp_t);
static void (*free_fn)(const void *);
static unsigned long (*alloc_pages_fn)(gfp_t, unsigned int);
static void (*free_pages_fn)(unsigned long, unsigned int);
static void *(*copy_fn)(void *, const void *, size_t);
static void *(*zero_fn)(void *, int, size_t);
static int (*compare_fn)(const void *, const void *, size_t);
static void (*lock_fn)(struct mutex *);
static void (*unlock_fn)(struct mutex *);
static void (*read_lock_fn)(struct rw_semaphore *);
static void (*read_unlock_fn)(struct rw_semaphore *);
static void (*write_lock_fn)(struct rw_semaphore *);
static void (*write_unlock_fn)(struct rw_semaphore *);
static struct mm_struct *(*get_mm_fn)(struct task_struct *);
static void (*mmput_fn)(struct mm_struct *);
static void (*mmdrop_fn)(struct mm_struct *);
static struct task_struct *(*find_task_fn)(pid_t);
static void (*rcu_lock_fn)(void);
static void (*rcu_unlock_fn)(void);
static bool (*capable_fn)(int);
static struct vm_area_struct *(*find_vma_fn)(struct mm_struct *, unsigned long);
static long (*gup_fn)(struct mm_struct *, unsigned long, unsigned long,
                      unsigned int, struct page **, struct vm_area_struct **, int *);
static void (*release_pages_fn)(struct page **, int);
static int (*walk_fn)(struct mm_struct *, unsigned long, unsigned long, pte_fn_t, void *);
static int (*notifier_register_fn)(struct mmu_notifier *, struct mm_struct *);
static void (*notifier_unregister_fn)(struct mmu_notifier *, struct mm_struct *);
static int (*uprobe_register_fn)(struct inode *, loff_t, struct uprobe_consumer *);
static void (*uprobe_unregister_fn)(struct inode *, loff_t, struct uprobe_consumer *);
static int (*access_vm_fn)(struct task_struct *, unsigned long, void *, int, unsigned int);
static struct file *(*open_fn)(const char *, int, umode_t);
static void (*fput_fn)(struct file *);
static int (*trace_fn)(unsigned long, const char *, ...);
static int (*message_fn)(const char *, ...);
static s64 *wu_memstart_addr;
#define memstart_addr (*wu_memstart_addr)

struct wu_context;
struct wu_page {
    struct list_head node;
    struct wu_context *context;
    unsigned long address, pfn;
    struct page *pin;
    u64 original_pte, read_pte, execute_pte;
    bool active;
};
struct wu_shadow { struct list_head node; void *address; };
struct wu_probe {
    struct list_head node;
    struct uprobe_consumer consumer;
    struct wu_context *context;
    struct wu_page *page;
    struct file *file;
    struct arch_uprobe *arch;
    struct wu_request info;
    u32 instruction;
    bool registered, copied, legacy;
};
struct wu_context {
    struct list_head node, pages, probes, shadows;
    struct mmu_notifier notifier;
    struct mm_struct *mm;
    bool released;
    unsigned long legacy_base;
    struct file *legacy_file;
};
/* Owned by control_lock. Accessed by opcode/copy callbacks under state_lock.
 * The task check prevents unrelated registrations from using live bytes. */
static struct wu_probe *operation;
static struct task_struct *operation_task;

static bool is_ready(void) { return __atomic_load_n(&ready, __ATOMIC_ACQUIRE); }
/* Run outside control/state/mmap locks: GUP can fault, break COW and invoke
 * our retirement callback. No FOLL_FORCE: normal user VMA permissions apply.
 * Calling native access_process_vm also preserves the kernel's PAN/uaccess
 * handling, without embedding unprocessed Linux alternatives in the KPM. */
static int copy_user_buffer(void *buffer, unsigned long address, size_t size, bool write)
{
    struct mm_struct *mm = current->mm;
    /* Android's allocator uses tagged heap pointers even without MTE. */
    address = untagged_addr(address);
    if (!mm || !address || address >= mm->task_size || size > mm->task_size - address ||
        size > INT_MAX) return -EFAULT;
    return access_vm_fn(current, address, buffer, (int)size, write ? FOLL_WRITE : 0) == size ? 0 : -EFAULT;
}
static void *from_pfn(unsigned long pfn)
{
    return (void *)(((pfn << PAGE_SHIFT) - *wu_memstart_addr) | PAGE_OFFSET);
}
static unsigned long to_pfn(void *address)
{
    return (((unsigned long)address & ~PAGE_OFFSET) + *wu_memstart_addr) >> PAGE_SHIFT;
}
static void *alloc_zero(size_t size)
{
    void *p = alloc_fn(size, GFP_KERNEL);
    if (p) zero_fn(p, 0, size);
    return p;
}
/* Private lists: avoid CONFIG_DEBUG_LIST out-of-line Linux references. */
static void link_tail(struct list_head *node, struct list_head *head)
{
    node->prev = head->prev; node->next = head;
    WRITE_ONCE(head->prev->next, node); head->prev = node;
}
static void unlink_node(struct list_head *node)
{
    node->next->prev = node->prev; WRITE_ONCE(node->prev->next, node->next);
}
static struct wu_context *find_context(struct mm_struct *mm)
{
    struct wu_context *ctx;
    list_for_each_entry(ctx, &contexts, node) if (ctx->mm == mm) return ctx;
    return NULL;
}
static struct wu_page *find_page(struct wu_context *ctx, unsigned long address)
{
    struct wu_page *page;
    list_for_each_entry(page, &ctx->pages, node)
        if (page->active && page->address == (address & PAGE_MASK)) return page;
    return NULL;
}
static struct wu_probe *find_cookie(struct wu_context *ctx, u64 cookie)
{
    struct wu_probe *probe;
    list_for_each_entry(probe, &ctx->probes, node)
        if (probe->info.cookie == cookie) return probe;
    return NULL;
}
static unsigned int active_count(struct wu_context *ctx)
{
    struct wu_probe *probe;
    unsigned int count = 0;
    if (ctx) list_for_each_entry(probe, &ctx->probes, node)
        if (probe->info.state == WU_ACTIVE) ++count;
    return count;
}
static unsigned int page_probes(struct wu_page *page, struct wu_probe *skip)
{
    struct wu_probe *probe;
    unsigned int count = 0;
    list_for_each_entry(probe, &page->context->probes, node)
        if (probe != skip && probe->page == page && probe->info.state == WU_ACTIVE) ++count;
    return count;
}
static void sync_code(void *address)
{
    u64 ctr;
    unsigned long cursor, step;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    step = 4UL << ((ctr >> 16) & 15);
    for (cursor = (unsigned long)address; cursor < (unsigned long)address + PAGE_SIZE; cursor += step)
        asm volatile("dc cvau, %0" :: "r"(cursor) : "memory");
    asm volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}
static void replace_pte(pte_t *ptep, unsigned long address, u64 value)
{
    WRITE_ONCE(ptep->pte, 0);
    /* Break-before-make, both KPTI ASIDs, repeat-TLBI workaround. */
    asm volatile("dsb ishst\n\ttlbi vaale1is, %0\n\tdsb ish\n\ttlbi vaale1is, %0\n\tdsb ish\n\tisb"
                 :: "r"(address >> 12) : "memory");
    WRITE_ONCE(ptep->pte, value);
    asm volatile("dsb ishst\n\tisb" ::: "memory");
}
struct pte_operation { struct wu_page *page; u64 value; bool capture, visited, wrote; };
static int operate_pte(pte_t *ptep, unsigned long address, void *argument)
{
    struct pte_operation *op = argument;
    struct wu_page *page = op->page;
    u64 old = pte_val(READ_ONCE(*ptep));
    op->visited = true;
    if (op->capture) {
        if (!pte_valid(__pte(old)) || !(old & PTE_USER) || pte_special(__pte(old)) ||
            (old & (PTE_UXN | PTE_CONT)) || !(old & PTE_RDONLY) ||
            pte_pfn(__pte(old)) != page->pfn) return -ESTALE;
#ifdef PTE_GP
        if (old & PTE_GP) return -EOPNOTSUPP;
#endif
        page->original_pte = old; page->read_pte = old | PTE_UXN;
        return 0;
    }
    if (old != page->original_pte && old != page->read_pte && old != page->execute_pte) {
        if (page->execute_pte && op->value == page->original_pte &&
            pte_valid(__pte(old)) && pte_pfn(__pte(old)) == pte_pfn(__pte(page->execute_pte))) {
            replace_pte(ptep, address, (old & ~PTE_ADDR_MASK) | (page->original_pte & PTE_ADDR_MASK));
            op->wrote = true; return 0;
        }
        return -ESTALE;
    }
    if (old != op->value) replace_pte(ptep, address, op->value);
    op->wrote = true;
    return 0;
}
/* Caller holds mmap_lock (or mm-release's stable VM context) and state_lock. */
static int page_operation(struct wu_page *page, u64 value, bool capture)
{
    struct pte_operation op = {.page = page, .value = value, .capture = capture};
    u64 *table = (u64 *)page->context->mm->pgd;
    int rc, level;
    /* Linux 5.10's existing-page walker BUG_ONs on blocks/huge pages. */
    for (level = CONFIG_PGTABLE_LEVELS - 1; level > 0; --level) {
        unsigned int index = (page->address >> (PAGE_SHIFT + level * 9)) & 511;
        u64 descriptor = READ_ONCE(table[index]);
        if ((descriptor & 3) != 3) return -EOPNOTSUPP;
        table = from_pfn((descriptor & PTE_ADDR_MASK) >> PAGE_SHIFT);
    }
    rc = walk_fn(page->context->mm, page->address, PAGE_SIZE, operate_pte, &op);
    return op.wrote ? 0 : rc ? rc : !op.visited ? -ESTALE : 0;
}
static void retire_page(struct wu_page *page, int error)
{
    struct wu_probe *probe;
    if (page->active) page_operation(page, page->original_pte, false);
    page->active = false;
    list_for_each_entry(probe, &page->context->probes, node)
        if (probe->page == page && (probe->info.state == WU_ACTIVE || probe->info.state == WU_INSTALLING)) {
            probe->info.state = WU_RETIRED;
            probe->info.last_error = error;
        }
}
static int invalidate_start(struct mmu_notifier *notifier, const struct mmu_notifier_range *range)
{
    struct wu_context *ctx = container_of(notifier, struct wu_context, notifier);
    struct wu_page *page;
    if (!mmu_notifier_range_blockable(range)) return -EAGAIN;
    lock_fn(&state_lock);
    list_for_each_entry(page, &ctx->pages, node)
        if (page->active && page->address < range->end && page->address + PAGE_SIZE > range->start)
            retire_page(page, -ESTALE);
    unlock_fn(&state_lock);
    return 0;
}
static void mm_release(struct mmu_notifier *notifier, struct mm_struct *mm)
{
    struct wu_context *ctx = container_of(notifier, struct wu_context, notifier);
    struct wu_page *page;
    lock_fn(&state_lock);
    list_for_each_entry(page, &ctx->pages, node) if (page->active) retire_page(page, -ESRCH);
    ctx->released = true;
    unlock_fn(&state_lock);
}
static const struct mmu_notifier_ops notifier_ops = {
    .release = mm_release, .invalidate_range_start = invalidate_start,
};
static void drop_mm(struct mm_struct *mm)
{
    if (__atomic_sub_fetch(&mm->mm_count.counter, 1, __ATOMIC_ACQ_REL) == 0) mmdrop_fn(mm);
}
/* control_lock held, no mmap/state lock across notifier registration. */
static struct wu_context *get_context(struct mm_struct *mm)
{
    struct wu_context *ctx;
    lock_fn(&state_lock); ctx = find_context(mm); unlock_fn(&state_lock);
    if (ctx) return ctx;
    if (context_count >= WU_MAX_CONTEXTS) return NULL;
    ctx = alloc_zero(sizeof(*ctx));
    if (!ctx) return NULL;
    INIT_LIST_HEAD(&ctx->pages); INIT_LIST_HEAD(&ctx->probes); INIT_LIST_HEAD(&ctx->shadows);
    ctx->notifier.ops = &notifier_ops; ctx->mm = mm;
    __atomic_add_fetch(&mm->mm_count.counter, 1, __ATOMIC_RELAXED);
    if (notifier_register_fn(&ctx->notifier, mm)) { drop_mm(mm); free_fn(ctx); return NULL; }
    lock_fn(&state_lock); link_tail(&ctx->node, &contexts); ++context_count; unlock_fn(&state_lock);
    return ctx;
}
static bool valid_vma(struct vm_area_struct *vma, unsigned long address)
{
    unsigned long unsupported = VM_WRITE | VM_SHARED | VM_IO | VM_PFNMAP | VM_MIXEDMAP | VM_HUGETLB;
#ifdef VM_ARM64_BTI
    unsupported |= VM_ARM64_BTI;
#endif
    return vma && vma->vm_start <= address && vma->vm_end - address >= 4 && vma->vm_file &&
        S_ISREG(vma->vm_file->f_inode->i_mode) &&
        (vma->vm_flags & (VM_READ | VM_EXEC)) == (VM_READ | VM_EXEC) && !(vma->vm_flags & unsupported);
}
static void hold_file(struct file *file)
{
    __atomic_add_fetch(&file->f_count.counter, 1, __ATOMIC_RELAXED);
}
static struct wu_shadow *make_shadow(struct wu_page *page, struct wu_probe *add, struct wu_probe *skip)
{
    struct wu_shadow *shadow;
    struct wu_probe *probe;
    u32 opcode = UPROBE_SWBP_INSN;
    shadow = alloc_zero(sizeof(*shadow));
    if (!shadow) return NULL;
    shadow->address = (void *)alloc_pages_fn(GFP_KERNEL | __GFP_ZERO, 0);
    if (!shadow->address) { free_fn(shadow); return NULL; }
    copy_fn(shadow->address, from_pfn(page->pfn), PAGE_SIZE);
    list_for_each_entry(probe, &page->context->probes, node)
        if (probe != skip && probe->page == page && probe->info.state == WU_ACTIVE)
            copy_fn((char *)shadow->address + (probe->info.address & ~PAGE_MASK), &opcode, 4);
    if (add) copy_fn((char *)shadow->address + (add->info.address & ~PAGE_MASK), &opcode, 4);
    sync_code(shadow->address);
    return shadow;
}
static void free_shadow(struct wu_shadow *shadow)
{
    if (!shadow) return;
    free_pages_fn((unsigned long)shadow->address, 0); free_fn(shadow);
}
static int publish_shadow(struct wu_page *page, struct wu_shadow *shadow)
{
    u64 pte = page->original_pte;
    int rc;
    pte &= ~(PTE_ADDR_MASK | PTE_USER | PTE_UXN);
    pte |= ((u64)to_pfn(shadow->address) << PAGE_SHIFT) & PTE_ADDR_MASK;
    pte |= PTE_RDONLY | PTE_PXN;
    rc = page_operation(page, pte, false);
    if (rc) return rc;
    page->execute_pte = pte; page->active = true;
    link_tail(&shadow->node, &page->context->shadows); ++shadow_count;
    return 0;
}

static bool consumer_filter(struct uprobe_consumer *consumer, enum uprobe_filter_ctx where,
                            struct mm_struct *mm)
{
    struct wu_probe *probe = container_of(consumer, struct wu_probe, consumer);
    /* A remap/new mapping is a new generation and requires explicit install.
     * Fork inherits the clean page after before_fork retires the parent. */
    return where != UPROBE_FILTER_MMAP && mm == probe->context->mm &&
           !READ_ONCE(probe->context->released) && READ_ONCE(probe->registered);
}
static int entry_handler(struct uprobe_consumer *consumer, struct pt_regs *regs)
{
    struct wu_probe *probe = container_of(consumer, struct wu_probe, consumer);
    bool trace = false;
    unsigned int i;
    lock_fn(&state_lock);
    if (current->mm == probe->context->mm && regs->pc == probe->info.address &&
        probe->info.state == WU_ACTIVE && probe->page->active) {
        for (i = 0; i < 31; ++i)
            if (probe->info.reg_mask & (1ULL << i)) regs->regs[i] = probe->info.regs[i];
        if (probe->info.reg_mask & (1ULL << WU_REG_SP)) regs->sp = probe->info.regs[WU_REG_SP];
        ++probe->info.hits;
        trace = probe->info.flags & WU_F_TRACE;
    }
    unlock_fn(&state_lock);
    if (trace && trace_fn)
        trace_fn(0, "+Test-Log+ wxuprobe fun_name:%s,offset:0x%llx,pc:0x%llx,x0:0x%llx\n",
                 probe->info.name, probe->info.offset, probe->info.address, regs->regs[0]);
    return 0;
}
static int valid_registers(struct mm_struct *mm, const struct wu_request *request)
{
    if (request->reg_mask & ~WU_REG_MASK) return -EINVAL;
    if ((request->reg_mask & (1ULL << WU_REG_SP)) &&
        ((request->regs[WU_REG_SP] & 15) || !request->regs[WU_REG_SP] ||
         request->regs[WU_REG_SP] >= mm->task_size)) return -EINVAL;
    return 0;
}
static void fill_request(struct wu_request *request, struct wu_probe *probe)
{
    request->address = probe->info.address; request->offset = probe->info.offset;
    request->cookie = probe->info.cookie; request->state = probe->info.state;
    request->reg_mask = probe->info.reg_mask; request->flags = probe->info.flags;
    request->hits = probe->info.hits; request->last_error = probe->info.last_error;
    copy_fn(request->regs, probe->info.regs, sizeof(request->regs));
    copy_fn(request->name, probe->info.name, sizeof(request->name));
    request->active_count = active_count(probe->context);
}
/* control_lock held. The record and its pages remain allocated until QUIESCE,
 * including failed/removed records, so STATUS can resolve lost output copies. */
static int install_probe(struct wu_context *ctx, struct wu_request *request, bool legacy)
{
    struct wu_probe *probe, *existing;
    struct wu_page *page = NULL;
    struct vm_area_struct *vma;
    struct file *file = NULL;
    struct page *pin = NULL;
    loff_t offset;
    bool fresh = false;
    unsigned int i;
    int rc;
    long got;
    if (!request->cookie || !request->address || (request->address & 3) ||
        request->address >= ctx->mm->task_size) return -EINVAL;
    rc = valid_registers(ctx->mm, request);
    if (rc) return rc;
    if ((request->flags & WU_F_TRACE) && !trace_fn) return -EOPNOTSUPP;
    lock_fn(&state_lock);
    existing = find_cookie(ctx, request->cookie);
    if (existing) {
        rc = existing->info.address != request->address ||
             (request->offset != WU_OFFSET_AUTO && existing->info.offset != request->offset) ||
             existing->info.reg_mask != request->reg_mask || existing->info.flags != request->flags ||
             compare_fn(existing->info.regs, request->regs, sizeof(request->regs)) ||
             compare_fn(existing->info.name, request->name, sizeof(request->name)) ? -EEXIST :
             existing->info.state != WU_ACTIVE ? -ESTALE : 0;
        fill_request(request, existing); unlock_fn(&state_lock); return rc;
    }
    list_for_each_entry(existing, &ctx->probes, node)
        if (existing->registered && existing->info.address == request->address) {
            unlock_fn(&state_lock); return -EEXIST;
        }
    page = find_page(ctx, request->address);
    unlock_fn(&state_lock);
    if (probe_count >= WU_MAX_PROBES || shadow_count >= WU_MAX_SHADOWS ||
        (!page && retained_pages >= WU_MAX_PAGES)) return -ENOSPC;
    probe = alloc_zero(sizeof(*probe));
    if (!probe) return -ENOMEM;
    read_lock_fn(&ctx->mm->mmap_lock);
    vma = find_vma_fn(ctx->mm, request->address);
    if (!valid_vma(vma, request->address)) { rc = -EOPNOTSUPP; goto unlock_read; }
    file = vma->vm_file; hold_file(file);
    offset = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) + request->address - vma->vm_start;
    if ((request->offset != WU_OFFSET_AUTO && request->offset != offset) ||
        (legacy && (!ctx->legacy_file || ctx->legacy_file->f_inode != file->f_inode))) {
        rc = -EINVAL; goto unlock_read;
    }
    if (!page) {
        got = gup_fn(ctx->mm, request->address & PAGE_MASK, 1, 0, &pin, NULL, NULL);
        if (got != 1) { rc = got < 0 ? got : -EFAULT; goto unlock_read; }
        if (PageCompound(pin)) { rc = -EOPNOTSUPP; goto unlock_read; }
    }
    read_unlock_fn(&ctx->mm->mmap_lock);
    if (!page) {
        page = alloc_zero(sizeof(*page));
        if (!page) { rc = -ENOMEM; goto fail; }
        page->context = ctx; page->pin = pin; page->pfn = page_to_pfn(pin);
        page->address = request->address & PAGE_MASK;
        fresh = true;
    }
    write_lock_fn(&ctx->mm->mmap_lock);
    lock_fn(&state_lock);
    vma = find_vma_fn(ctx->mm, request->address);
    if (ctx->released || !valid_vma(vma, request->address) || vma->vm_file != file ||
        (((loff_t)vma->vm_pgoff << PAGE_SHIFT) + request->address - vma->vm_start) != offset) {
        rc = -ESTALE; goto unlock_write;
    }
    if (fresh) {
        rc = page_operation(page, 0, true);
        if (rc) goto unlock_write;
        /* Do not take ownership of a page already containing ordinary uprobe
         * traps. Its other consumers could otherwise write through our PFN. */
        for (i = 0; i < PAGE_SIZE / 4; ++i)
            if (((u32 *)from_pfn(page->pfn))[i] == UPROBE_SWBP_INSN) {
                rc = -EBUSY; goto unlock_write;
            }
    } else if (!page->active) { rc = -ESTALE; goto unlock_write; }
    if (page_probes(page, NULL) >= WU_MAX_PAGE_PROBES) { rc = -ENOSPC; goto unlock_write; }
    copy_fn(&probe->instruction, (char *)from_pfn(page->pfn) + (request->address & ~PAGE_MASK), 4);
    /* BTI/PAC entry semantics need separate support; retain the established
     * wxshadow restriction instead of silently altering those entry paths. */
    if ((probe->instruction & 0xffffff1fU) == 0xd503241fU ||
        (probe->instruction & 0xffffff00U) == 0xd5032300U) { rc = -EOPNOTSUPP; goto unlock_write; }
    request->offset = offset; request->state = WU_INSTALLING; request->last_error = 0;
    probe->info = *request; probe->context = ctx; probe->page = page; probe->file = file;
    probe->legacy = legacy; probe->registered = true;
    probe->consumer.handler = entry_handler; probe->consumer.filter = consumer_filter;
    if (fresh) { link_tail(&page->node, &ctx->pages); ++retained_pages; }
    link_tail(&probe->node, &ctx->probes); ++probe_count;
    operation = probe; operation_task = current;
    unlock_fn(&state_lock); write_unlock_fn(&ctx->mm->mmap_lock);
    rc = uprobe_register_fn(file->f_inode, offset, &probe->consumer);
    write_lock_fn(&ctx->mm->mmap_lock);
    lock_fn(&state_lock);
    operation = NULL; operation_task = NULL;
    if (rc) {
        /* The kernel has already removed the consumer on registration error. */
        probe->registered = false;
        if (probe->info.state == WU_ACTIVE) retire_page(page, rc);
        probe->info.state = WU_REMOVED; probe->info.last_error = rc;
    } else if (probe->info.state != WU_ACTIVE) {
        /* No eligible VMA was actually published. Remove the consumer below. */
        rc = -ESTALE;
    }
    fill_request(request, probe);
    unlock_fn(&state_lock); write_unlock_fn(&ctx->mm->mmap_lock);
    if (rc && probe->registered) {
        uprobe_unregister_fn(file->f_inode, offset, &probe->consumer);
        lock_fn(&state_lock);
        probe->registered = false; probe->info.state = WU_REMOVED; probe->info.last_error = rc;
        fill_request(request, probe); unlock_fn(&state_lock);
    }
    return rc;
unlock_write:
    unlock_fn(&state_lock); write_unlock_fn(&ctx->mm->mmap_lock);
    if (fresh) free_fn(page);
    goto fail;
unlock_read:
    read_unlock_fn(&ctx->mm->mmap_lock);
fail:
    if (pin) release_pages_fn(&pin, 1);
    if (file) fput_fn(file);
    free_fn(probe);
    return rc;
}

int wu_linux_copy_insn(void *mapping, void *file, void *destination,
                       int size, long long offset, long *result)
{
    struct wu_probe *probe;
    int handled = 0;
    if (!is_ready()) return 0;
    lock_fn(&state_lock);
    probe = operation;
    if (probe && operation_task == current && probe->info.state == WU_INSTALLING &&
        mapping == probe->file->f_inode->i_mapping && offset == probe->info.offset) {
        handled = 1; *result = -EOPNOTSUPP;
        if (size == 4) {
            copy_fn(destination, &probe->instruction, 4);
            probe->arch = container_of(destination, struct arch_uprobe, insn);
            probe->copied = true; *result = 0;
        }
    }
    unlock_fn(&state_lock);
    return handled;
}
int wu_linux_opcode(void *arch_pointer, void *mm_pointer, unsigned long address,
                    unsigned int opcode, long *result)
{
    struct arch_uprobe *arch = arch_pointer;
    struct mm_struct *mm = mm_pointer;
    struct wu_context *ctx;
    struct wu_probe *probe = NULL, *candidate;
    struct wu_page *page;
    struct wu_shadow *shadow;
    u32 instruction;
    int rc = 0, handled = 0;
    if (!is_ready()) return 0;
    lock_fn(&state_lock);
    /* Detect another consumer of our inode/offset even in a different mm. */
    list_for_each_entry(ctx, &contexts, node)
        list_for_each_entry(candidate, &ctx->probes, node)
            if (candidate->registered && candidate->arch == arch) { probe = candidate; goto found; }
found:
    if (!probe && operation && operation_task == current && operation->context->mm == mm)
        probe = operation;
    if (!probe) {
        ctx = find_context(mm); page = ctx ? find_page(ctx, address) : NULL;
        if (page) { handled = 1; rc = -EBUSY; }
        goto out;
    }
    handled = 1; page = probe->page;
    if (opcode == UPROBE_SWBP_INSN) {
        /* Exclusivity avoids changing global uprobe instruction semantics for
         * existing consumers. copied proves this is a newly decoded probe. */
        if (operation != probe || operation_task != current || probe->context->mm != mm ||
            address != probe->info.address || !probe->copied || probe->arch != arch ||
            probe->info.state != WU_INSTALLING) { rc = -EBUSY; goto out; }
        copy_fn(&instruction, arch->insn, 4);
        if (instruction != probe->instruction ||
            compare_fn(&instruction, (char *)from_pfn(page->pfn) + (address & ~PAGE_MASK), 4)) {
            rc = -ESTALE; goto out;
        }
        shadow = make_shadow(page, probe, NULL);
        if (!shadow) { rc = -ENOMEM; goto out; }
        rc = publish_shadow(page, shadow);
        if (rc) free_shadow(shadow);
        else probe->info.state = WU_ACTIVE;
    } else {
        /* Rollback/unregister may visit other aliases or other mms. They were
         * never patched by us, and must never fall through to a real write. */
        if (probe->context->mm != mm || address != probe->info.address) goto out;
        if (page->active && probe->info.state == WU_ACTIVE) {
            if (page_probes(page, probe)) {
                shadow = make_shadow(page, NULL, probe);
                if (!shadow) { rc = -ENOMEM; retire_page(page, rc); goto out; }
                rc = publish_shadow(page, shadow);
                if (rc) { free_shadow(shadow); retire_page(page, rc); }
            } else {
                rc = page_operation(page, page->original_pte, false);
                page->active = false;
            }
        }
        probe->info.state = WU_REMOVED;
        probe->info.last_error = rc;
    }
out:
    *result = rc;
    unlock_fn(&state_lock);
    return handled;
}

static int remove_probe(struct wu_probe *probe)
{
    int rc;
    if (!probe->registered) return 0;
    lock_fn(&state_lock); operation = probe; operation_task = current; unlock_fn(&state_lock);
    uprobe_unregister_fn(probe->file->f_inode, probe->info.offset, &probe->consumer);
    write_lock_fn(&probe->context->mm->mmap_lock);
    lock_fn(&state_lock);
    operation = NULL; operation_task = NULL;
    /* uprobe_unregister has a void result. If it did not remove our shadow
     * (e.g. mapping disappearance), retire the whole page before returning. */
    if (probe->info.state == WU_ACTIVE) retire_page(probe->page, -ESTALE);
    rc = probe->info.last_error;
    if (rc == -ESTALE || rc == -ESRCH) rc = 0;
    probe->registered = false; probe->info.state = WU_REMOVED; probe->info.last_error = rc;
    unlock_fn(&state_lock); write_unlock_fn(&probe->context->mm->mmap_lock);
    return rc;
}
static int clear_context(struct wu_context *ctx)
{
    struct wu_probe *probe;
    int rc = 0, result;
    if (!ctx) return 0;
    list_for_each_entry(probe, &ctx->probes, node) {
        result = remove_probe(probe);
        if (result && !rc) rc = result;
    }
    return rc;
}
static int quiesce(void)
{
    struct wu_context *ctx, *next;
    struct wu_page *page, *pnext;
    struct wu_probe *probe, *rnext;
    struct wu_shadow *shadow, *snext;
    if (!capable_fn(CAP_SYS_ADMIN)) return -EPERM;
    list_for_each_entry(ctx, &contexts, node) clear_context(ctx);
    lock_fn(&state_lock); stopped = true; unlock_fn(&state_lock);
    /* This synchronizes SRCU and may sleep. Called only from prctl, never
     * from KPM ctl0/exit, which the pinned KP loader invokes under RCU. */
    list_for_each_entry_safe(ctx, next, &contexts, node) {
        notifier_unregister_fn(&ctx->notifier, ctx->mm);
        lock_fn(&state_lock); unlink_node(&ctx->node); unlock_fn(&state_lock);
        list_for_each_entry_safe(probe, rnext, &ctx->probes, node) {
            fput_fn(probe->file); free_fn(probe);
        }
        list_for_each_entry_safe(page, pnext, &ctx->pages, node) {
            release_pages_fn(&page->pin, 1); free_fn(page);
        }
        list_for_each_entry_safe(shadow, snext, &ctx->shadows, node) free_shadow(shadow);
        if (ctx->legacy_file) fput_fn(ctx->legacy_file);
        drop_mm(ctx->mm); free_fn(ctx);
    }
    context_count = retained_pages = probe_count = shadow_count = 0;
    return 0;
}
static struct mm_struct *target_mm(int pid, int *error)
{
    struct mm_struct *mm = NULL;
    struct task_struct *task;
    if (pid < 0) { *error = -EINVAL; return NULL; }
    if (!pid || pid == current->tgid) return get_mm_fn(current);
    if (!capable_fn(CAP_SYS_ADMIN)) { *error = -EPERM; return NULL; }
    rcu_lock_fn();
    task = find_task_fn(pid);
    if (task) mm = get_mm_fn(task);
    rcu_unlock_fn();
    if (!mm) *error = -ESRCH;
    return mm;
}
long wu_linux_request(int command, unsigned long user_request)
{
    struct wu_request request;
    struct wu_context *ctx = NULL;
    struct wu_probe *probe;
    struct mm_struct *mm = NULL;
    unsigned int i;
    int rc = 0;
    if (!is_ready() || is_compat_task()) return -EOPNOTSUPP;
    if (copy_user_buffer(&request, user_request, sizeof(request), false)) return -EFAULT;
    if (request.version != WXUPROBE_ABI_VERSION || request.size != sizeof(request) ||
        (request.flags & ~WU_F_TRACE)) return -EINVAL;
    for (i = 0; i < ARRAY_SIZE(request.reserved); ++i) if (request.reserved[i]) return -EINVAL;
    if (command != PR_WXUPROBE_QUERY && request.instance != instance) return -ESTALE;
    lock_fn(&control_lock);
    if (command == PR_WXUPROBE_QUIESCE) { rc = quiesce(); goto output; }
    if (stopped && command != PR_WXUPROBE_QUERY) { rc = -ESHUTDOWN; goto output; }
    mm = target_mm(request.pid, &rc);
    if (!mm) { if (!rc) rc = -ESRCH; goto output; }
    lock_fn(&state_lock); ctx = find_context(mm); unlock_fn(&state_lock);
    if (command == PR_WXUPROBE_QUERY) {
        lock_fn(&state_lock);
        request.instance = instance;
        request.capabilities = stopped ? 0 : WU_CAP_UPROBE | WU_CAP_SHADOW | WU_CAP_REGS |
            WU_CAP_LIVE_INSN | WU_CAP_LEGACY | WU_CAP_REMOTE;
        request.page_size = PAGE_SIZE; request.active_count = active_count(ctx);
        unlock_fn(&state_lock);
    } else if (command == PR_WXUPROBE_INSTALL) {
        if (request.name[WU_NAME_LEN - 1]) { rc = -EINVAL; goto output; }
        if (!ctx) ctx = get_context(mm);
        rc = ctx ? install_probe(ctx, &request, false) : -ENOSPC;
    } else if (command == PR_WXUPROBE_CLEAR) {
        rc = clear_context(ctx); request.active_count = 0;
    } else {
        if (!request.cookie) { rc = -EINVAL; goto output; }
        lock_fn(&state_lock);
        probe = ctx ? find_cookie(ctx, request.cookie) : NULL;
        if (probe && request.address && request.address != probe->info.address) probe = NULL;
        if (!probe) {
            request.state = WU_ABSENT;
            if (command == PR_WXUPROBE_UPDATE) rc = -ENOENT;
            unlock_fn(&state_lock); goto output;
        }
        if (command == PR_WXUPROBE_UPDATE) {
            rc = valid_registers(mm, &request);
            if (!rc && probe->info.state != WU_ACTIVE) rc = -ESTALE;
            if (!rc && (request.flags & WU_F_TRACE) && !trace_fn) rc = -EOPNOTSUPP;
            if (!rc) {
                probe->info.reg_mask = request.reg_mask; probe->info.flags = request.flags;
                copy_fn(probe->info.regs, request.regs, sizeof(request.regs));
            }
        }
        unlock_fn(&state_lock);
        if (command == PR_WXUPROBE_REMOVE) rc = remove_probe(probe);
        lock_fn(&state_lock); fill_request(&request, probe); unlock_fn(&state_lock);
    }
output:
    if (mm) mmput_fn(mm);
    unlock_fn(&control_lock);
    if (copy_user_buffer(&request, user_request, sizeof(request), true)) return -EFAULT;
    return rc;
}

int wu_linux_fault(unsigned long address, unsigned int esr, void *registers)
{
    struct pt_regs *regs = registers;
    struct mm_struct *mm;
    struct wu_context *ctx;
    struct wu_page *page;
    unsigned int ec = (esr >> 26) & 63;
    bool execute = ec == 0x20, write = !execute && (esr & (1U << 6));
    int handled = 0, rc;
    if (!is_ready() || READ_ONCE(stopped) || !regs || !user_mode(regs) ||
        (ec != 0x20 && ec != 0x24) || (esr & 0x3c) != 0x0c) return 0;
    mm = current->mm;
    if (!mm) return 0;
    read_lock_fn(&mm->mmap_lock);
    lock_fn(&state_lock); ctx = find_context(mm); page = ctx ? find_page(ctx, address) : NULL;
    if (page) {
        if (write) retire_page(page, -ESTALE);
        else {
            rc = page_operation(page, execute ? page->execute_pte : page->read_pte, false);
            if (!rc) handled = 1;
            else retire_page(page, rc);
        }
    }
    unlock_fn(&state_lock); read_unlock_fn(&mm->mmap_lock);
    return handled;
}
int wu_linux_before_fork(void *mm_pointer)
{
    struct mm_struct *mm = mm_pointer;
    struct wu_context *ctx;
    struct wu_page *page;
    /* Hooks are installed only after lock_fn is resolved. Hold control even
     * before API activation, so a fork crossing activation excludes install. */
    if (READ_ONCE(stopped)) return 0;
    /* dup_mmap has not acquired dup_mmap_sem/mmap_lock yet. Holding control
     * through the original prevents publication into the fork copy gap. */
    lock_fn(&control_lock);
    if (stopped) { unlock_fn(&control_lock); return 0; }
    write_lock_fn(&mm->mmap_lock);
    lock_fn(&state_lock); ctx = find_context(mm);
    if (ctx) list_for_each_entry(page, &ctx->pages, node) if (page->active) retire_page(page, -ESTALE);
    unlock_fn(&state_lock); write_unlock_fn(&mm->mmap_lock);
    return 1;
}
void wu_linux_after_fork(void) { unlock_fn(&control_lock); }
void wu_linux_before_gup(void *vma_pointer, unsigned long address)
{
    struct vm_area_struct *vma = vma_pointer;
    struct wu_context *ctx;
    struct wu_page *page;
    if (!is_ready() || READ_ONCE(stopped) || !vma) return;
    lock_fn(&state_lock); ctx = find_context(vma->vm_mm); page = ctx ? find_page(ctx, address) : NULL;
    if (page) retire_page(page, -ESTALE);
    unlock_fn(&state_lock);
}
int wu_linux_quiesced(void)
{
    return !is_ready() || !READ_ONCE(context_count);
}

struct legacy_init_info { u32 uid; unsigned long base, file, fix_file; };
struct legacy_probe_info { unsigned long offset, function_offset, name; };
struct legacy_reg_info { unsigned long function_offset; u64 mask, regs[32]; };
static_assert(sizeof(struct wu_request) == 440);
static_assert(sizeof(struct legacy_init_info) == 32);
static_assert(sizeof(struct legacy_probe_info) == 24);
static_assert(sizeof(struct legacy_reg_info) == 272);
static_assert(UPROBE_SWBP_INSN_SIZE == 4);
static int copy_user_string(char *destination, size_t capacity, unsigned long source)
{
    size_t i;
    zero_fn(destination, 0, capacity);
    if (!source) return -EFAULT;
    for (i = 0; i < capacity; ++i) {
        if (source + i < source || copy_user_buffer(destination + i, source + i, 1, false)) return -EFAULT;
        if (!destination[i]) return 0;
    }
    destination[capacity - 1] = 0;
    return -ENAMETOOLONG;
}
long wu_linux_legacy(int number, unsigned long user_request)
{
    struct legacy_init_info init;
    struct legacy_probe_info item;
    struct legacy_reg_info registers;
    struct wu_request request;
    struct wu_context *ctx;
    struct wu_probe *probe, *found = NULL;
    struct mm_struct *mm;
    struct file *file;
    char path[300], fix_path[300], name[150];
    unsigned long address;
    int rc = 0;
    if (!is_ready() || is_compat_task()) return 1001;
    /* Snapshot user input before taking locks; faults may retire source pages. */
    if (number == 0) {
        if (copy_user_buffer(&init, user_request, sizeof(init), false)) return 1001;
        rc = copy_user_string(path, sizeof(path), init.file);
        if (!rc && init.fix_file) rc = copy_user_string(fix_path, sizeof(fix_path), init.fix_file);
    } else if (number == 1) {
        if (copy_user_buffer(&item, user_request, sizeof(item), false)) return 1001;
        rc = copy_user_string(name, sizeof(name), item.name);
    } else if (number == 3 || number == 4) {
        zero_fn(&registers, 0, sizeof(registers));
        rc = copy_user_buffer(&registers, user_request,
                              number == 3 ? sizeof(registers) : sizeof(unsigned long), false);
    } else if (number != 2) return 1001;
    if (rc) return 1001;
    lock_fn(&control_lock);
    if (stopped) { rc = -ESHUTDOWN; goto unlock; }
    mm = get_mm_fn(current);
    if (!mm) { rc = -ESRCH; goto unlock; }
    lock_fn(&state_lock); ctx = find_context(mm); unlock_fn(&state_lock);
    if (number == 2) { rc = clear_context(ctx); goto out; }
    if (number == 0) {
        if (!init.base || init.base >= mm->task_size ||
            init.uid != __kuid_val(current->cred->uid)) { rc = -EINVAL; goto out; }
        /* Live resident bytes replace Kernel-Trace's global read-cache hook.
         * The legacy fix_file pointer was validated above; no file is needed. */
        if (ctx) list_for_each_entry(probe, &ctx->probes, node)
            if (probe->registered) { rc = -EBUSY; goto out; }
        file = open_fn(path, O_RDONLY | O_LARGEFILE, 0);
        if (IS_ERR(file)) { rc = PTR_ERR(file); goto out; }
        if (!ctx) ctx = get_context(mm);
        if (!ctx) { fput_fn(file); rc = -ENOSPC; goto out; }
        if (ctx->legacy_file) fput_fn(ctx->legacy_file);
        ctx->legacy_file = file; ctx->legacy_base = init.base;
        goto out;
    }
    if (!ctx || !ctx->legacy_base || !ctx->legacy_file) { rc = -EINVAL; goto out; }
    zero_fn(&request, 0, sizeof(request));
    request.version = WXUPROBE_ABI_VERSION; request.size = sizeof(request); request.instance = instance;
    if (number == 1) {
        if (item.function_offset > mm->task_size - ctx->legacy_base) { rc = -EINVAL; goto out; }
        request.address = ctx->legacy_base + item.function_offset;
        request.offset = item.offset; request.cookie = (1ULL << 63) | ++legacy_cookie;
        request.flags = WU_F_TRACE;
        /* Original Kernel-Trace accepts 149-byte names; trace labels use 63. */
        copy_fn(request.name, name, sizeof(request.name) - 1);
        rc = install_probe(ctx, &request, true);
        goto out;
    }
    if (registers.function_offset > mm->task_size - ctx->legacy_base) { rc = -EINVAL; goto out; }
    address = ctx->legacy_base + registers.function_offset;
    lock_fn(&state_lock);
    list_for_each_entry(probe, &ctx->probes, node)
        if (probe->legacy && probe->registered && probe->info.address == address) { found = probe; break; }
    if (!found) rc = -ENOENT;
    else if (number == 3) {
        request.reg_mask = registers.mask; copy_fn(request.regs, registers.regs, sizeof(request.regs));
        rc = valid_registers(mm, &request);
        if (!rc && found->info.state != WU_ACTIVE) rc = -ESTALE;
        if (!rc) { found->info.reg_mask = request.reg_mask; copy_fn(found->info.regs, request.regs, sizeof(request.regs)); }
    }
    unlock_fn(&state_lock);
    if (!rc && number == 4) rc = remove_probe(found);
out:
    mmput_fn(mm);
unlock:
    unlock_fn(&control_lock);
    if (rc && message_fn) message_fn("\0013wxuprobe: legacy operation %d failed: %d\n", number, rc);
    return rc ? 1001 : 1000;
}

#define RESOLVE(field, name) do { \
    field = (void *)host.lookup(name); \
    if (!field) { if (message_fn) message_fn("\0013wxuprobe: missing %s\n", name); return -EOPNOTSUPP; } \
} while (0)
int wu_linux_init(const struct wu_host *new_host)
{
    const char expected[] = "Linux version " UTS_RELEASE " ";
    const char *banner;
    u64 (*random_fn)(void);
    host = *new_host;
    message_fn = (void *)host.lookup("printk");
    RESOLVE(compare_fn, "memcmp"); RESOLVE(copy_fn, "memcpy"); RESOLVE(zero_fn, "memset");
    banner = (const char *)host.lookup("linux_banner");
    if (!banner || compare_fn(banner, expected, sizeof(expected) - 1) ||
        host.mm_pgd_offset != offsetof(struct mm_struct, pgd)) return -EPROTO;
    RESOLVE(wu_memstart_addr, "memstart_addr");
    RESOLVE(alloc_fn, "__kmalloc"); RESOLVE(free_fn, "kfree");
    RESOLVE(alloc_pages_fn, "__get_free_pages"); RESOLVE(free_pages_fn, "free_pages");
    RESOLVE(lock_fn, "mutex_lock"); RESOLVE(unlock_fn, "mutex_unlock");
    RESOLVE(read_lock_fn, "down_read"); RESOLVE(read_unlock_fn, "up_read");
    RESOLVE(write_lock_fn, "down_write"); RESOLVE(write_unlock_fn, "up_write");
    RESOLVE(get_mm_fn, "get_task_mm"); RESOLVE(mmput_fn, "mmput"); RESOLVE(mmdrop_fn, "__mmdrop");
    RESOLVE(find_task_fn, "find_task_by_vpid"); RESOLVE(rcu_lock_fn, "__rcu_read_lock"); RESOLVE(rcu_unlock_fn, "__rcu_read_unlock");
    RESOLVE(capable_fn, "capable"); RESOLVE(find_vma_fn, "find_vma");
    RESOLVE(gup_fn, "get_user_pages_remote"); RESOLVE(release_pages_fn, "release_pages");
    RESOLVE(walk_fn, "apply_to_existing_page_range");
    RESOLVE(notifier_register_fn, "mmu_notifier_register"); RESOLVE(notifier_unregister_fn, "mmu_notifier_unregister");
    RESOLVE(uprobe_register_fn, "uprobe_register"); RESOLVE(uprobe_unregister_fn, "uprobe_unregister");
    RESOLVE(access_vm_fn, "access_process_vm");
    RESOLVE(open_fn, "filp_open"); RESOLVE(fput_fn, "fput");
    RESOLVE(random_fn, "get_random_u64"); instance = random_fn();
    if (!instance) return -EIO;
    trace_fn = (void *)host.lookup("__trace_printk");
    return 0;
}
void wu_linux_start(void)
{
    /* Publish the API only after every required hook was installed. */
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}
