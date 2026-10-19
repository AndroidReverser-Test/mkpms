// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Runtime-validated implementation. Page publication, retirement and TLB/cache
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
#include "../../common/wx_btf.h"

#if !defined(CONFIG_ARM64_4K_PAGES) || !defined(CONFIG_MMU_NOTIFIER) || !defined(CONFIG_UPROBES)
#error "wxuprobe requires ARM64 4K pages, MMU_NOTIFIER and UPROBES"
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0) || LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
#error "wxuprobe supports the audited Linux 5.10 through 6.6 VM/uprobe APIs"
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
/* Kernel-owned objects use target BTF sizes and offsets, never the compiler's
 * configuration-dependent layout. The union supplies natural alignment. */
#define WU_OPAQUE_MAX 512U
union wu_opaque { u64 align; unsigned char bytes[WU_OPAQUE_MAX]; };
static union wu_opaque control_storage, state_storage, control_key, state_key;
static struct mutex *control_mutex = (void *)&control_storage;
static struct mutex *state_mutex = (void *)&state_storage;
#define control_lock (*control_mutex)
#define state_lock (*state_mutex)
static struct wx_btf btf;
static bool dynamic_layout;
static unsigned int notifier_size, consumer_size;
static union wu_opaque notifier_ops_storage;
struct wu_offsets {
    unsigned int mm_pgd, mm_lock, mm_count, mm_task_size;
    unsigned int task_mm, task_tgid, task_cred, task_flags, cred_uid;
    unsigned int vma_start, vma_end, vma_flags, vma_file, vma_offset, vma_mm;
    unsigned int file_inode, file_count, inode_mode, inode_mapping;
    unsigned int notifier_ops, range_start, range_end, range_flags;
    unsigned int ops_release, ops_invalidate;
    unsigned int consumer_handler, consumer_filter, consumer_next;
    unsigned int arch_insn;
    unsigned int uprobe_arch, uprobe_inode, uprobe_offset, uprobe_consumers, uprobe_flags;
} offsets;
#define WU_FIELD(type, object, member) (*(type *)((char *)(object) + offsets.member))
#define MM_PGD(mm) WU_FIELD(void *, mm, mm_pgd)
#define MM_LOCK(mm) ((struct rw_semaphore *)((char *)(mm) + offsets.mm_lock))
#define MM_COUNT(mm) WU_FIELD(int, mm, mm_count)
#define MM_TASK_SIZE(mm) WU_FIELD(unsigned long, mm, mm_task_size)
#define TASK_MM(task) WU_FIELD(struct mm_struct *, task, task_mm)
#define TASK_TGID(task) WU_FIELD(int, task, task_tgid)
#define TASK_CRED(task) WU_FIELD(const struct cred *, task, task_cred)
#define CRED_UID(cred) WU_FIELD(unsigned int, cred, cred_uid)
#define VMA_START(vma) WU_FIELD(unsigned long, vma, vma_start)
#define VMA_END(vma) WU_FIELD(unsigned long, vma, vma_end)
#define VMA_FLAGS(vma) WU_FIELD(unsigned long, vma, vma_flags)
#define VMA_FILE(vma) WU_FIELD(struct file *, vma, vma_file)
#define VMA_OFFSET(vma) WU_FIELD(unsigned long, vma, vma_offset)
#define VMA_MM(vma) WU_FIELD(struct mm_struct *, vma, vma_mm)
#define FILE_INODE(file) WU_FIELD(struct inode *, file, file_inode)
#define FILE_COUNT(file) WU_FIELD(long, file, file_count)
#define INODE_MODE(inode) WU_FIELD(umode_t, inode, inode_mode)
#define INODE_MAPPING(inode) WU_FIELD(void *, inode, inode_mapping)
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
static long (*gup6_fn)(struct mm_struct *, unsigned long, unsigned long,
                      unsigned int, struct page **, int *);
static struct page *(*normal_page_fn)(struct vm_area_struct *, unsigned long, pte_t);
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
static int (*trace_puts_fn)(unsigned long, const char *, int);
static int (*format_fn)(char *, size_t, const char *, ...);
static int (*message_fn)(const char *, ...);

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
    struct uprobe_consumer *consumer;
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
    struct mmu_notifier *notifier;
    struct mm_struct *mm;
    bool released;
    unsigned long legacy_base;
    struct file *legacy_file;
};
/* Owned by control_lock. Accessed by opcode/copy callbacks under state_lock.
 * The task check prevents unrelated registrations from using live bytes. */
static struct wu_probe *operation;
static struct task_struct *operation_task;
/* Only accessed while owning control_lock, except the end's owner check.
 * The global control lock permits exactly one paired fork window at a time. */
static struct task_struct *fork_owner;

static bool is_ready(void) { return __atomic_load_n(&ready, __ATOMIC_ACQUIRE); }
static bool trace_supported(void) { return (trace_puts_fn && format_fn) || trace_fn; }
static bool compat_task(void)
{
    return (WU_FIELD(unsigned long, current, task_flags) & (1UL << TIF_32BIT)) != 0;
}
/* Run outside control/state/mmap locks: GUP can fault, break COW and invoke
 * our retirement callback. No FOLL_FORCE: normal user VMA permissions apply.
 * Calling native access_process_vm also preserves the kernel's PAN/uaccess
 * handling, without embedding unprocessed Linux alternatives in the KPM. */
static int copy_user_buffer(void *buffer, unsigned long address, size_t size, bool write)
{
    struct mm_struct *mm = TASK_MM(current);
    /* Android's allocator uses tagged heap pointers even without MTE. */
    address = untagged_addr(address);
    if (!mm || !address || address >= MM_TASK_SIZE(mm) || size > MM_TASK_SIZE(mm) - address ||
        size > INT_MAX) return -EFAULT;
    return access_vm_fn(current, address, buffer, (int)size, write ? FOLL_WRITE : 0) == size ? 0 : -EFAULT;
}
static void *from_pfn(unsigned long pfn)
{
    return host.from_pfn(pfn);
}
static unsigned long to_pfn(void *address)
{
    return host.to_pfn(address);
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
            normal_page_fn(find_vma_fn(page->context->mm, address), address, __pte(old)) != page->pin)
            return -ESTALE;
#ifdef PTE_GP
        if (old & PTE_GP) return -EOPNOTSUPP;
#endif
        page->pfn = pte_pfn(__pte(old));
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
    u64 *table = (u64 *)MM_PGD(page->context->mm);
    int rc, level;
    /* Linux 5.10's existing-page walker BUG_ONs on blocks/huge pages. */
    for (level = host.page_levels - 1; level > 0; --level) {
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
    struct wu_context *ctx;
    struct wu_page *page;
    if (!(WU_FIELD(unsigned int, range, range_flags) & MMU_NOTIFIER_RANGE_BLOCKABLE)) return -EAGAIN;
    lock_fn(&state_lock);
    list_for_each_entry(ctx, &contexts, node) if (ctx->notifier == notifier) goto found;
    unlock_fn(&state_lock); return 0;
found:
    list_for_each_entry(page, &ctx->pages, node)
        if (page->active && page->address < WU_FIELD(unsigned long, range, range_end) &&
            page->address + PAGE_SIZE > WU_FIELD(unsigned long, range, range_start))
            retire_page(page, -ESTALE);
    unlock_fn(&state_lock);
    return 0;
}
static void mm_release(struct mmu_notifier *notifier, struct mm_struct *mm)
{
    struct wu_context *ctx;
    struct wu_page *page;
    lock_fn(&state_lock);
    list_for_each_entry(ctx, &contexts, node) if (ctx->notifier == notifier) goto found;
    unlock_fn(&state_lock); return;
found:
    list_for_each_entry(page, &ctx->pages, node) if (page->active) retire_page(page, -ESRCH);
    ctx->released = true;
    unlock_fn(&state_lock);
}
static void drop_mm(struct mm_struct *mm)
{
    if (__atomic_sub_fetch(&MM_COUNT(mm), 1, __ATOMIC_ACQ_REL) == 0) mmdrop_fn(mm);
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
    ctx->notifier = alloc_zero(notifier_size);
    if (!ctx->notifier) { free_fn(ctx); return NULL; }
    WU_FIELD(void *, ctx->notifier, notifier_ops) = &notifier_ops_storage;
    ctx->mm = mm;
    __atomic_add_fetch(&MM_COUNT(mm), 1, __ATOMIC_RELAXED);
    if (notifier_register_fn(ctx->notifier, mm)) {
        drop_mm(mm); free_fn(ctx->notifier); free_fn(ctx); return NULL;
    }
    lock_fn(&state_lock); link_tail(&ctx->node, &contexts); ++context_count; unlock_fn(&state_lock);
    return ctx;
}
static bool valid_vma(struct vm_area_struct *vma, unsigned long address)
{
    unsigned long unsupported = VM_WRITE | VM_SHARED | VM_IO | VM_PFNMAP | VM_MIXEDMAP | VM_HUGETLB;
#ifdef VM_ARM64_BTI
    unsupported |= VM_ARM64_BTI;
#endif
    return vma && VMA_START(vma) <= address && VMA_END(vma) - address >= 4 && VMA_FILE(vma) &&
        S_ISREG(INODE_MODE(FILE_INODE(VMA_FILE(vma)))) &&
        (VMA_FLAGS(vma) & (VM_READ | VM_EXEC)) == (VM_READ | VM_EXEC) && !(VMA_FLAGS(vma) & unsupported);
}
static void hold_file(struct file *file)
{
    __atomic_add_fetch(&FILE_COUNT(file), 1, __ATOMIC_RELAXED);
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
    struct wu_context *ctx;
    struct wu_probe *probe;
    bool accept = false;
    /* A remap/new mapping is a new generation and requires explicit install.
     * Fork inherits the clean page after before_fork retires the parent. */
    lock_fn(&state_lock);
    list_for_each_entry(ctx, &contexts, node)
        list_for_each_entry(probe, &ctx->probes, node)
            if (probe->consumer == consumer) {
                accept = where != UPROBE_FILTER_MMAP && mm == ctx->mm &&
                         !ctx->released && probe->registered;
                goto out;
            }
out:
    unlock_fn(&state_lock);
    return accept;
}
static int entry_handler(struct uprobe_consumer *consumer, struct pt_regs *regs)
{
    struct wu_context *ctx;
    struct wu_probe *probe = NULL, *candidate;
    bool trace = false;
    unsigned int i;
    lock_fn(&state_lock);
    list_for_each_entry(ctx, &contexts, node)
        list_for_each_entry(candidate, &ctx->probes, node)
            if (candidate->consumer == consumer) { probe = candidate; goto found; }
found:
    if (!probe) { unlock_fn(&state_lock); return 0; }
    if (TASK_MM(current) == probe->context->mm && regs->pc == probe->info.address &&
        probe->info.state == WU_ACTIVE && probe->page->active) {
        for (i = 0; i < 31; ++i)
            if (probe->info.reg_mask & (1ULL << i)) regs->regs[i] = probe->info.regs[i];
        if (probe->info.reg_mask & (1ULL << WU_REG_SP)) regs->sp = probe->info.regs[WU_REG_SP];
        ++probe->info.hits;
        trace = probe->info.flags & WU_F_TRACE;
    }
    unlock_fn(&state_lock);
    if (trace && trace_puts_fn && format_fn) {
        char message[256];
        int length = format_fn(message, sizeof(message),
                "+Test-Log+ wxuprobe fun_name:%s,offset:0x%llx,pc:0x%llx,x0:0x%llx\n",
                probe->info.name, probe->info.offset, probe->info.address, regs->regs[0]);
        /* __trace_puts copies the text into the ring buffer. Unlike
         * __trace_printk it needs no loader-created per-CPU formatting buffer,
         * so a KPM can use it without initializing or enabling global tracing. */
        if (length >= sizeof(message)) length = sizeof(message) - 1;
        if (length > 0) trace_puts_fn(0, message, length);
    } else if (trace && trace_fn)
        trace_fn(0, "+Test-Log+ wxuprobe fun_name:%s,offset:0x%llx,pc:0x%llx,x0:0x%llx\n",
                 probe->info.name, probe->info.offset, probe->info.address, regs->regs[0]);
    return 0;
}
static int valid_registers(struct mm_struct *mm, const struct wu_request *request)
{
    if (request->reg_mask & ~WU_REG_MASK) return -EINVAL;
    if ((request->reg_mask & (1ULL << WU_REG_SP)) &&
        ((request->regs[WU_REG_SP] & 15) || !request->regs[WU_REG_SP] ||
         request->regs[WU_REG_SP] >= MM_TASK_SIZE(mm))) return -EINVAL;
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
        request->address >= MM_TASK_SIZE(ctx->mm)) return -EINVAL;
    rc = valid_registers(ctx->mm, request);
    if (rc) return rc;
    if ((request->flags & WU_F_TRACE) && !trace_supported()) return -EOPNOTSUPP;
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
    probe->consumer = alloc_zero(consumer_size);
    if (!probe->consumer) { free_fn(probe); return -ENOMEM; }
    read_lock_fn(MM_LOCK(ctx->mm));
    vma = find_vma_fn(ctx->mm, request->address);
    if (!valid_vma(vma, request->address)) { rc = -EOPNOTSUPP; goto unlock_read; }
    file = VMA_FILE(vma); hold_file(file);
    offset = ((loff_t)VMA_OFFSET(vma) << PAGE_SHIFT) + request->address - VMA_START(vma);
    if ((request->offset != WU_OFFSET_AUTO && request->offset != offset) ||
        (legacy && (!ctx->legacy_file || FILE_INODE(ctx->legacy_file) != FILE_INODE(file)))) {
        rc = -EINVAL; goto unlock_read;
    }
    if (!page) {
        got = gup6_fn ? gup6_fn(ctx->mm, request->address & PAGE_MASK, 1, 0, &pin, NULL) :
              gup_fn(ctx->mm, request->address & PAGE_MASK, 1, 0, &pin, NULL, NULL);
        if (got != 1) { rc = got < 0 ? got : -EFAULT; goto unlock_read; }
    }
    read_unlock_fn(MM_LOCK(ctx->mm));
    if (!page) {
        page = alloc_zero(sizeof(*page));
        if (!page) { rc = -ENOMEM; goto fail; }
        page->context = ctx; page->pin = pin;
        page->address = request->address & PAGE_MASK;
        fresh = true;
    }
    write_lock_fn(MM_LOCK(ctx->mm));
    lock_fn(&state_lock);
    vma = find_vma_fn(ctx->mm, request->address);
    if (ctx->released || !valid_vma(vma, request->address) || VMA_FILE(vma) != file ||
        (((loff_t)VMA_OFFSET(vma) << PAGE_SHIFT) + request->address - VMA_START(vma)) != offset) {
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
    WU_FIELD(void *, probe->consumer, consumer_handler) = entry_handler;
    WU_FIELD(void *, probe->consumer, consumer_filter) = consumer_filter;
    if (fresh) { link_tail(&page->node, &ctx->pages); ++retained_pages; }
    link_tail(&probe->node, &ctx->probes); ++probe_count;
    operation = probe; operation_task = current;
    unlock_fn(&state_lock); write_unlock_fn(MM_LOCK(ctx->mm));
    rc = uprobe_register_fn(FILE_INODE(file), offset, probe->consumer);
    write_lock_fn(MM_LOCK(ctx->mm));
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
    unlock_fn(&state_lock); write_unlock_fn(MM_LOCK(ctx->mm));
    if (rc && probe->registered) {
        uprobe_unregister_fn(FILE_INODE(file), offset, probe->consumer);
        lock_fn(&state_lock);
        probe->registered = false; probe->info.state = WU_REMOVED; probe->info.last_error = rc;
        fill_request(request, probe); unlock_fn(&state_lock);
    }
    return rc;
unlock_write:
    unlock_fn(&state_lock); write_unlock_fn(MM_LOCK(ctx->mm));
    if (fresh) free_fn(page);
    goto fail;
unlock_read:
    read_unlock_fn(MM_LOCK(ctx->mm));
fail:
    if (pin) release_pages_fn(&pin, 1);
    if (file) fput_fn(file);
    free_fn(probe->consumer);
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
        mapping == INODE_MAPPING(FILE_INODE(probe->file)) && offset == probe->info.offset) {
        handled = 1; *result = -EOPNOTSUPP;
        if (size == 4) {
            copy_fn(destination, &probe->instruction, 4);
            probe->arch = (void *)((char *)destination - offsets.arch_insn);
            probe->copied = true; *result = 0;
        }
    }
    unlock_fn(&state_lock);
    return handled;
}

/* LTO may inline __copy_insn. The architecture decode entry remains available
 * and is called under the uprobe consumer write lock, before COPY_INSN is set.
 * Only a new, exclusively owned uprobe may receive the resident instruction.
 * Existing decoded uprobes never reach this callback and remain rejected by
 * the copied/arch checks in wu_linux_opcode. */
int wu_linux_analyze(void *arch, void *mm, unsigned long address, long *result)
{
    struct wu_probe *probe;
    void *uprobe;
    int handled = 0;
    if (!is_ready() || !host.copy_via_analyze || !dynamic_layout) return 0;
    lock_fn(&state_lock);
    probe = operation;
    if (!probe || operation_task != current || probe->info.state != WU_INSTALLING) goto out;
    uprobe = (char *)arch - offsets.uprobe_arch;
    if (WU_FIELD(struct inode *, uprobe, uprobe_inode) != FILE_INODE(probe->file) ||
        WU_FIELD(loff_t, uprobe, uprobe_offset) != probe->info.offset) goto out;
    if (mm != probe->context->mm || address != probe->info.address ||
        WU_FIELD(void *, uprobe, uprobe_consumers) != probe->consumer ||
        WU_FIELD(void *, probe->consumer, consumer_next) ||
        (WU_FIELD(unsigned long, uprobe, uprobe_flags) & 1UL)) {
        handled = 1; *result = -EBUSY; goto out;
    }
    /* The generic trap test already examined the disk instruction, before
     * this fallback runs. Apply that test to the resident replacement too. */
    if (probe->instruction == UPROBE_SWBP_INSN) {
        handled = 1; *result = -EOPNOTSUPP; goto out;
    }
    copy_fn((char *)arch + offsets.arch_insn, &probe->instruction, 4);
    probe->arch = arch; probe->copied = true;
    /* The original function still performs instruction rejection/decoding. */
out:
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
        copy_fn(&instruction, (char *)arch + offsets.arch_insn, 4);
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
    uprobe_unregister_fn(FILE_INODE(probe->file), probe->info.offset, probe->consumer);
    write_lock_fn(MM_LOCK(probe->context->mm));
    lock_fn(&state_lock);
    operation = NULL; operation_task = NULL;
    /* uprobe_unregister has a void result. If it did not remove our shadow
     * (e.g. mapping disappearance), retire the whole page before returning. */
    if (probe->info.state == WU_ACTIVE) retire_page(probe->page, -ESTALE);
    rc = probe->info.last_error;
    if (rc == -ESTALE || rc == -ESRCH) rc = 0;
    probe->registered = false; probe->info.state = WU_REMOVED; probe->info.last_error = rc;
    unlock_fn(&state_lock); write_unlock_fn(MM_LOCK(probe->context->mm));
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
        notifier_unregister_fn(ctx->notifier, ctx->mm);
        lock_fn(&state_lock); unlink_node(&ctx->node); unlock_fn(&state_lock);
        list_for_each_entry_safe(probe, rnext, &ctx->probes, node) {
            fput_fn(probe->file); free_fn(probe->consumer); free_fn(probe);
        }
        list_for_each_entry_safe(page, pnext, &ctx->pages, node) {
            release_pages_fn(&page->pin, 1); free_fn(page);
        }
        list_for_each_entry_safe(shadow, snext, &ctx->shadows, node) free_shadow(shadow);
        if (ctx->legacy_file) fput_fn(ctx->legacy_file);
        drop_mm(ctx->mm); free_fn(ctx->notifier); free_fn(ctx);
    }
    context_count = retained_pages = probe_count = shadow_count = 0;
    return 0;
}
static struct mm_struct *target_mm(int pid, int *error)
{
    struct mm_struct *mm = NULL;
    struct task_struct *task;
    if (pid < 0) { *error = -EINVAL; return NULL; }
    if (!pid || pid == TASK_TGID(current)) return get_mm_fn(current);
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
    if (!is_ready() || compat_task()) return -EOPNOTSUPP;
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
            if (!rc && (request.flags & WU_F_TRACE) && !trace_supported()) rc = -EOPNOTSUPP;
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
    mm = TASK_MM(current);
    if (!mm) return 0;
    read_lock_fn(MM_LOCK(mm));
    lock_fn(&state_lock); ctx = find_context(mm); page = ctx ? find_page(ctx, address) : NULL;
    if (page) {
        if (write) retire_page(page, -ESTALE);
        else {
            rc = page_operation(page, execute ? page->execute_pte : page->read_pte, false);
            if (!rc) handled = 1;
            else retire_page(page, rc);
        }
    }
    unlock_fn(&state_lock); read_unlock_fn(MM_LOCK(mm));
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
    write_lock_fn(MM_LOCK(mm));
    lock_fn(&state_lock); ctx = find_context(mm);
    if (ctx) list_for_each_entry(page, &ctx->pages, node) if (page->active) retire_page(page, -ESTALE);
    unlock_fn(&state_lock); write_unlock_fn(MM_LOCK(mm));
    return 1;
}
void wu_linux_after_fork(void) { unlock_fn(&control_lock); }
int wu_linux_before_current_fork(void)
{
    struct mm_struct *mm = TASK_MM(current);
    if (!mm || !wu_linux_before_fork(mm)) return 0;
    WRITE_ONCE(fork_owner, current);
    return 1;
}
int wu_linux_after_current_fork(void)
{
    if (READ_ONCE(fork_owner) != current) return 0;
    WRITE_ONCE(fork_owner, NULL);
    wu_linux_after_fork();
    return 1;
}
void wu_linux_before_gup(void *vma_pointer, unsigned long address)
{
    struct vm_area_struct *vma = vma_pointer;
    struct wu_context *ctx;
    struct wu_page *page;
    if (!is_ready() || READ_ONCE(stopped) || !vma) return;
    lock_fn(&state_lock); ctx = find_context(VMA_MM(vma)); page = ctx ? find_page(ctx, address) : NULL;
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
    if (!is_ready() || compat_task()) return 1001;
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
        if (!init.base || init.base >= MM_TASK_SIZE(mm) ||
            init.uid != CRED_UID(TASK_CRED(current))) { rc = -EINVAL; goto out; }
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
        if (item.function_offset > MM_TASK_SIZE(mm) - ctx->legacy_base) { rc = -EINVAL; goto out; }
        request.address = ctx->legacy_base + item.function_offset;
        request.offset = item.offset; request.cookie = (1ULL << 63) | ++legacy_cookie;
        request.flags = WU_F_TRACE;
        /* Original Kernel-Trace accepts 149-byte names; trace labels use 63. */
        copy_fn(request.name, name, sizeof(request.name) - 1);
        rc = install_probe(ctx, &request, true);
        goto out;
    }
    if (registers.function_offset > MM_TASK_SIZE(mm) - ctx->legacy_base) { rc = -EINVAL; goto out; }
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

/* The fallback keeps the original exact-kernel contract for kernels built
 * without BTF. A present but incompatible BTF is always an error. */
static int native_layout(void)
{
    const char expected[] = "Linux version " UTS_RELEASE " ";
    const char *banner = (const char *)host.lookup("linux_banner");
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    /* KPM storage is outside Linux's static/module key registration. */
    return -EOPNOTSUPP;
#endif
    if (host.copy_via_analyze) return -EOPNOTSUPP;
    if (!banner || compare_fn(banner, expected, sizeof(expected) - 1) ||
        host.mm_pgd_offset != offsetof(struct mm_struct, pgd)) return -EPROTO;
#define NATIVE(field, type, member) offsets.field = offsetof(struct type, member)
    NATIVE(mm_pgd, mm_struct, pgd); NATIVE(mm_lock, mm_struct, mmap_lock);
    NATIVE(mm_count, mm_struct, mm_count); NATIVE(mm_task_size, mm_struct, task_size);
    NATIVE(task_mm, task_struct, mm); NATIVE(task_tgid, task_struct, tgid);
    NATIVE(task_cred, task_struct, cred); NATIVE(cred_uid, cred, uid);
    offsets.task_flags = offsetof(struct task_struct, thread_info) + offsetof(struct thread_info, flags);
    NATIVE(vma_start, vm_area_struct, vm_start); NATIVE(vma_end, vm_area_struct, vm_end);
    NATIVE(vma_flags, vm_area_struct, vm_flags); NATIVE(vma_file, vm_area_struct, vm_file);
    NATIVE(vma_offset, vm_area_struct, vm_pgoff); NATIVE(vma_mm, vm_area_struct, vm_mm);
    NATIVE(file_inode, file, f_inode); NATIVE(file_count, file, f_count);
    NATIVE(inode_mode, inode, i_mode); NATIVE(inode_mapping, inode, i_mapping);
    NATIVE(notifier_ops, mmu_notifier, ops);
    NATIVE(range_start, mmu_notifier_range, start); NATIVE(range_end, mmu_notifier_range, end);
    NATIVE(range_flags, mmu_notifier_range, flags);
    NATIVE(ops_release, mmu_notifier_ops, release);
    NATIVE(ops_invalidate, mmu_notifier_ops, invalidate_range_start);
    NATIVE(consumer_handler, uprobe_consumer, handler);
    NATIVE(consumer_filter, uprobe_consumer, filter); NATIVE(consumer_next, uprobe_consumer, next);
    NATIVE(arch_insn, arch_uprobe, insn);
#undef NATIVE
    notifier_size = sizeof(struct mmu_notifier); consumer_size = sizeof(struct uprobe_consumer);
    if (sizeof(struct mutex) > WU_OPAQUE_MAX || sizeof(struct mmu_notifier_ops) > WU_OPAQUE_MAX)
        return -EOPNOTSUPP;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    gup6_fn = (void *)gup_fn; gup_fn = NULL;
#endif
    return 0;
}

static int target_layout(void)
{
    unsigned int mutex_size, ops_size, sem_size, thread_size, thread_offset, flags_offset;
    unsigned int offset, map_size, arch_size;
    int rc, nargs;
    rc = wx_btf_init(&btf, host.lookup, (void *)alloc_fn, (void *)free_fn, GFP_KERNEL);
    if (rc == -ENOENT && !host.lookup("__start_BTF") && !host.lookup("__stop_BTF"))
        return native_layout();
    if (rc) return rc;
#define SIZE(type, destination) do { \
    rc = wx_btf_size(&btf, #type, &(destination)); if (rc) goto out; \
} while (0)
#define MEMBER(field, type, member, bytes) do { \
    rc = wx_btf_member(&btf, #type, #member, bytes, &offsets.field); \
    if (rc) { if (message_fn) message_fn("\0013wxuprobe: incompatible BTF %s.%s: %d\n", #type, #member, rc); goto out; } \
} while (0)
    SIZE(mutex, mutex_size); SIZE(mmu_notifier_ops, ops_size); SIZE(rw_semaphore, sem_size);
    SIZE(mmu_notifier, notifier_size); SIZE(uprobe_consumer, consumer_size);
    SIZE(thread_info, thread_size);
    /* PREEMPT_RT replaces the ordinary mutex implementation. Its locking
     * semantics have not been audited for these callbacks. */
    rc = wx_btf_member(&btf, "mutex", "owner", 8, &offset);
    if (rc) goto out;
    rc = wx_btf_member(&btf, "mutex", "wait_list", 16, &offset);
    if (rc) goto out;
    rc = wx_btf_size(&btf, "lockdep_map", &map_size);
    if (rc && rc != -ENOENT) goto out;
    if (!rc && map_size) {
        rc = wx_btf_member(&btf, "mutex", "dep_map", map_size, &offset);
        if (!rc) { rc = -EOPNOTSUPP; goto out; }
        if (rc != -ENOENT) goto out;
    }
    if (!mutex_size || mutex_size > WU_OPAQUE_MAX || !ops_size || ops_size > WU_OPAQUE_MAX ||
        !notifier_size || notifier_size > WU_OPAQUE_MAX || !consumer_size || consumer_size > WU_OPAQUE_MAX) {
        rc = -EOPNOTSUPP; goto out;
    }
    MEMBER(mm_pgd, mm_struct, pgd, 8); MEMBER(mm_lock, mm_struct, mmap_lock, sem_size);
    MEMBER(mm_count, mm_struct, mm_count, 4); MEMBER(mm_task_size, mm_struct, task_size, 8);
    MEMBER(task_mm, task_struct, mm, 8); MEMBER(task_tgid, task_struct, tgid, 4);
    MEMBER(task_cred, task_struct, cred, 8); MEMBER(cred_uid, cred, uid, 4);
    rc = wx_btf_member(&btf, "task_struct", "thread_info", thread_size, &thread_offset);
    if (rc) goto out;
    rc = wx_btf_member(&btf, "thread_info", "flags", 8, &flags_offset);
    if (rc) goto out;
    offsets.task_flags = thread_offset + flags_offset;
    MEMBER(vma_start, vm_area_struct, vm_start, 8); MEMBER(vma_end, vm_area_struct, vm_end, 8);
    MEMBER(vma_flags, vm_area_struct, vm_flags, 8); MEMBER(vma_file, vm_area_struct, vm_file, 8);
    MEMBER(vma_offset, vm_area_struct, vm_pgoff, 8); MEMBER(vma_mm, vm_area_struct, vm_mm, 8);
    MEMBER(file_inode, file, f_inode, 8); MEMBER(file_count, file, f_count, 8);
    MEMBER(inode_mode, inode, i_mode, 2); MEMBER(inode_mapping, inode, i_mapping, 8);
    MEMBER(notifier_ops, mmu_notifier, ops, 8);
    MEMBER(range_start, mmu_notifier_range, start, 8); MEMBER(range_end, mmu_notifier_range, end, 8);
    MEMBER(range_flags, mmu_notifier_range, flags, 4);
    MEMBER(ops_release, mmu_notifier_ops, release, 8);
    MEMBER(ops_invalidate, mmu_notifier_ops, invalidate_range_start, 8);
    MEMBER(consumer_handler, uprobe_consumer, handler, 8);
    MEMBER(consumer_filter, uprobe_consumer, filter, 8); MEMBER(consumer_next, uprobe_consumer, next, 8);
    MEMBER(arch_insn, arch_uprobe, insn, 4);
    if (host.copy_via_analyze) {
        SIZE(arch_uprobe, arch_size);
        MEMBER(uprobe_arch, uprobe, arch, arch_size);
        MEMBER(uprobe_inode, uprobe, inode, 8); MEMBER(uprobe_offset, uprobe, offset, 8);
        MEMBER(uprobe_consumers, uprobe, consumers, 8); MEMBER(uprobe_flags, uprobe, flags, 8);
    }
    /* pt_regs is accessed by the architecture's register helpers; prove its
     * used members retain the layout with which this translation unit built. */
#define REGS(member, bytes) do { \
    rc = wx_btf_member(&btf, "pt_regs", #member, bytes, &offset); \
    if (rc || offset != offsetof(struct pt_regs, member)) { rc = -EPROTO; goto out; } \
} while (0)
    REGS(regs, 31 * 8); REGS(sp, 8); REGS(pc, 8); REGS(pstate, 8);
#undef REGS
    if (offsets.mm_pgd != host.mm_pgd_offset) { rc = -EPROTO; goto out; }
#define ARITY(name, expected) do { \
    if (wx_btf_func_arity(&btf, name) != expected) { \
        if (message_fn) message_fn("\0013wxuprobe: incompatible BTF prototype %s\n", name); \
        rc = -EPROTO; goto out; \
    } \
} while (0)
#define PARAM(name, index, type) do { \
    rc = wx_btf_func_param_named(&btf, name, index, #type); if (rc) goto out; \
} while (0)
    /* Some compilers omit BTF FUNC records for these static functions while
     * retaining their callable kallsyms symbols. Their signatures have been
     * audited across the bounded supported kernel range. Existing records
     * must still match; malformed BTF never selects this exception. */
    nargs = wx_btf_func_arity(&btf, "__copy_insn");
    if (nargs != -ENOENT) {
        ARITY("__copy_insn", 5); PARAM("__copy_insn", 0, address_space); PARAM("__copy_insn", 1, file);
    }
    if (host.copy_via_analyze) {
        ARITY("arch_uprobe_analyze_insn", 3);
        PARAM("arch_uprobe_analyze_insn", 0, arch_uprobe);
        PARAM("arch_uprobe_analyze_insn", 1, mm_struct);
    }
    ARITY("uprobe_write_opcode", 4); PARAM("uprobe_write_opcode", 0, arch_uprobe);
    PARAM("uprobe_write_opcode", 1, mm_struct);
    ARITY("uprobe_register", 3); PARAM("uprobe_register", 0, inode); PARAM("uprobe_register", 2, uprobe_consumer);
    ARITY("uprobe_unregister", 3); PARAM("uprobe_unregister", 2, uprobe_consumer);
    ARITY("do_page_fault", 3);
    nargs = wx_btf_func_arity(&btf, "dup_mmap");
    if (nargs != -ENOENT) { ARITY("dup_mmap", 2); PARAM("dup_mmap", 1, mm_struct); }
    if (host.fork_uprobe_pair) {
        ARITY("uprobe_start_dup_mmap", 0); ARITY("uprobe_end_dup_mmap", 0);
    }
    ARITY("follow_page_pte", 5); PARAM("follow_page_pte", 0, vm_area_struct);
    ARITY("vm_normal_page", 3); ARITY("apply_to_existing_page_range", 5);
    ARITY("mmu_notifier_register", 2); ARITY("mmu_notifier_unregister", 2);
    ARITY("__mutex_init", 3); ARITY("release_pages", 2);
    nargs = wx_btf_func_arity(&btf, "get_user_pages_remote");
    PARAM("get_user_pages_remote", 0, mm_struct);
    if (nargs == 6) { gup6_fn = (void *)gup_fn; gup_fn = NULL; }
    else if (nargs != 7) { rc = -EPROTO; goto out; }
    dynamic_layout = true;
    rc = 0;
out:
    wx_btf_destroy(&btf);
    return rc;
#undef PARAM
#undef ARITY
#undef MEMBER
#undef SIZE
}

int wu_linux_init(const struct wu_host *new_host)
{
    u64 (*random_fn)(void);
    void (*mutex_init_fn)(void *, const char *, void *);
    int rc;
    host = *new_host;
    if (!host.from_pfn || !host.to_pfn || host.page_shift != PAGE_SHIFT ||
        host.page_levels < 3 || host.page_levels > 4 ||
        host.kernel_version < KERNEL_VERSION(5, 10, 0) ||
        host.kernel_version >= KERNEL_VERSION(6, 7, 0)) return -EOPNOTSUPP;
    message_fn = (void *)host.lookup("printk");
    if (!message_fn) message_fn = (void *)host.lookup("_printk");
    RESOLVE(compare_fn, "memcmp"); RESOLVE(copy_fn, "memcpy"); RESOLVE(zero_fn, "memset");
    RESOLVE(alloc_fn, "__kmalloc"); RESOLVE(free_fn, "kfree");
    RESOLVE(alloc_pages_fn, "__get_free_pages"); RESOLVE(free_pages_fn, "free_pages");
    RESOLVE(lock_fn, "mutex_lock"); RESOLVE(unlock_fn, "mutex_unlock");
    RESOLVE(read_lock_fn, "down_read"); RESOLVE(read_unlock_fn, "up_read");
    RESOLVE(write_lock_fn, "down_write"); RESOLVE(write_unlock_fn, "up_write");
    RESOLVE(get_mm_fn, "get_task_mm"); RESOLVE(mmput_fn, "mmput"); RESOLVE(mmdrop_fn, "__mmdrop");
    RESOLVE(find_task_fn, "find_task_by_vpid"); RESOLVE(rcu_lock_fn, "__rcu_read_lock"); RESOLVE(rcu_unlock_fn, "__rcu_read_unlock");
    RESOLVE(capable_fn, "capable"); RESOLVE(find_vma_fn, "find_vma");
    RESOLVE(gup_fn, "get_user_pages_remote"); RESOLVE(release_pages_fn, "release_pages");
    RESOLVE(normal_page_fn, "vm_normal_page"); RESOLVE(mutex_init_fn, "__mutex_init");
    RESOLVE(walk_fn, "apply_to_existing_page_range");
    RESOLVE(notifier_register_fn, "mmu_notifier_register"); RESOLVE(notifier_unregister_fn, "mmu_notifier_unregister");
    RESOLVE(uprobe_register_fn, "uprobe_register"); RESOLVE(uprobe_unregister_fn, "uprobe_unregister");
    RESOLVE(access_vm_fn, "access_process_vm");
    RESOLVE(open_fn, "filp_open"); RESOLVE(fput_fn, "fput");
    rc = target_layout();
    if (rc) return rc;
    WU_FIELD(void *, &notifier_ops_storage, ops_release) = mm_release;
    WU_FIELD(void *, &notifier_ops_storage, ops_invalidate) = invalidate_start;
    mutex_init_fn(control_mutex, "wxuprobe.control", &control_key);
    mutex_init_fn(state_mutex, "wxuprobe.state", &state_key);
    RESOLVE(random_fn, "get_random_u64"); instance = random_fn();
    if (!instance) return -EIO;
    trace_fn = (void *)host.lookup("__trace_printk");
    trace_puts_fn = (void *)host.lookup("__trace_puts");
    format_fn = (void *)host.lookup("scnprintf");
    if (!format_fn) format_fn = (void *)host.lookup("snprintf");
    if (message_fn) message_fn("\0016wxuprobe: %s kernel layout, GUP %u arguments\n",
                              dynamic_layout ? "BTF" : "exact", gup6_fn ? 6 : 7);
    if (message_fn && host.copy_via_analyze)
        message_fn("\0016wxuprobe: resident instruction supplied before architecture decode (LTO fallback)\n");
    return 0;
}
void wu_linux_start(void)
{
    /* Publish the API only after every required hook was installed. */
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}
