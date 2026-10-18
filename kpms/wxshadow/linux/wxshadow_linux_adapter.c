// SPDX-License-Identifier: GPL-2.0-or-later
/* Built separately by Kbuild, using the exact target kernel's configuration.
 * Never include KernelPatch's abbreviated Linux structs in this translation
 * unit. All non-inline kernel calls go through resolved function pointers.
 * Lock order: control -> mmap_lock -> state_lock -> kernel PTE lock.
 * Notifiers already run in VM context and only take state_lock -> PTE lock.
 */
#ifdef WXSHADOW_ADAPTER_TEST
#include "adapter_test_env.h"
#else
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/capability.h>
#include <linux/sched/mm.h>
#include <linux/compat.h>
#include <generated/utsrelease.h>
#include <asm/pgtable.h>
#include "../wxshadow_linux.h"

#if !defined(CONFIG_ARM64) || !defined(CONFIG_ARM64_4K_PAGES) || !defined(CONFIG_MMU_NOTIFIER)
#error "wxshadow requires ARM64 4K pages and MMU_NOTIFIER"
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0) || LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#error "VM adapter currently supports Linux 5.10; audit other VM/GUP APIs first"
#endif
#ifdef CONFIG_PREEMPT_RT
#error "PREEMPT_RT requires a separate locking audit"
#endif
#if CONFIG_ARM64_PA_BITS > 48 || defined(CONFIG_GCC_PLUGIN_RANDSTRUCT) || defined(CONFIG_RANDSTRUCT)
#error "wxshadow requires <=48-bit physical addresses and non-randomized struct layouts"
#endif
#endif

static struct wx_linux_host host;
static DEFINE_MUTEX(control_lock);
static DEFINE_MUTEX(state_lock);
static LIST_HEAD(contexts);
static bool stopped;
static bool ready;
static bool is_ready(void) { return __atomic_load_n(&ready, __ATOMIC_ACQUIRE); }
static u64 next_generation;
/* Bound retained storage. Removal can use additional shadow pages so hitting
 * the installation budget can never prevent removing the remaining hooks. */
static unsigned int context_count, retained_page_count, record_count, shadow_count;
#define WX_MAX_CONTEXTS 64U
#define WX_MAX_PAGES 1024U
#define WX_MAX_RECORDS 8192U
#define WX_INSTALL_SHADOW_BUDGET 4096U
static void (*lock_fn)(struct mutex *);
static void (*unlock_fn)(struct mutex *);
static void (*read_lock_fn)(struct rw_semaphore *);
static void (*read_unlock_fn)(struct rw_semaphore *);
static void (*write_lock_fn)(struct rw_semaphore *);
static void (*write_unlock_fn)(struct rw_semaphore *);
static int (*walk_fn)(struct mm_struct *, unsigned long, unsigned long, pte_fn_t, void *);
static long (*gup_fn)(unsigned long, unsigned long, unsigned int, struct page **, struct vm_area_struct **);
static void (*release_pages_fn)(struct page **, int);
static struct vm_area_struct *(*find_vma_fn)(struct mm_struct *, unsigned long);
static int (*register_fn)(struct mmu_notifier *, struct mm_struct *);
static void (*unregister_fn)(struct mmu_notifier *, struct mm_struct *);
static void (*mmdrop_fn)(struct mm_struct *);
static bool (*capable_fn)(int);
static void *(*memcpy_fn)(void *, const void *, size_t);
static int (*memcmp_fn)(const void *, const void *, size_t);
static void *(*memset_fn)(void *, int, size_t);
static s64 *wx_memstart_addr;
/* page_to_pfn's vmemmap macro uses the running memstart_addr. Avoid an
 * unresolved variable reference in the relocatable KPM object. */
#define memstart_addr (*wx_memstart_addr)

static void drop_mm(struct mm_struct *mm)
{
    if (__atomic_sub_fetch(&mm->mm_count.counter, 1, __ATOMIC_ACQ_REL) == 0) mmdrop_fn(mm);
}

struct tx_context;
struct tx_page {
    struct list_head node;
    unsigned long address;
    u64 generation;
    struct page *pin;
    unsigned long pfn;
    u64 original_pte, shadow_pte, read_pte;
    void *shadow;
    bool active;
};
struct tx_record {
    struct list_head node;
    struct tx_page *page;
    struct wx_patch_request request;
    unsigned int state;
};
struct tx_shadow {
    struct list_head node;
    void *address;
};
struct tx_context {
    struct list_head node, pages, records, shadows;
    struct mmu_notifier notifier;
    struct mm_struct *mm;
    bool released;
};

/* These private lists are always protected by state/control locks. Avoid
 * CONFIG_DEBUG_LIST's out-of-line helpers: KPM cannot relocate ordinary Linux
 * extern references. No kernel-owned list is modified through these helpers. */
static void tx_link_tail(struct list_head *node, struct list_head *head)
{
    node->prev = head->prev; node->next = head;
    WRITE_ONCE(head->prev->next, node); head->prev = node;
}
static void tx_unlink(struct list_head *node)
{
    node->next->prev = node->prev; WRITE_ONCE(node->prev->next, node->next);
}

static void *alloc_zero(size_t bytes)
{
    void *p = host.alloc(bytes, GFP_KERNEL);
    if (p) memset_fn(p, 0, bytes);
    return p;
}
static struct tx_context *find_context(struct mm_struct *mm)
{
    struct tx_context *ctx;
    list_for_each_entry(ctx, &contexts, node) if (ctx->mm == mm) return ctx;
    return NULL;
}
static struct tx_page *find_page(struct tx_context *ctx, unsigned long address)
{
    struct tx_page *page;
    list_for_each_entry(page, &ctx->pages, node)
        if (page->active && page->address == (address & PAGE_MASK)) return page;
    return NULL;
}
static struct tx_record *find_record(struct tx_context *ctx, u64 cookie)
{
    struct tx_record *record;
    list_for_each_entry(record, &ctx->records, node)
        if (record->request.cookie == cookie) return record;
    return NULL;
}
static void retire(struct tx_context *ctx, struct tx_page *page)
{
    struct tx_record *record;
    page->active = false;
    list_for_each_entry(record, &ctx->records, node)
        if (record->page == page) record->state = WX_PATCH_RETIRED;
}

static void sync_code(void *address)
{
#ifndef WXSHADOW_ADAPTER_TEST
    u64 ctr;
    unsigned long cursor, step;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    step = 4UL << ((ctr >> 16) & 15);
    for (cursor = (unsigned long)address; cursor < (unsigned long)address + PAGE_SIZE; cursor += step)
        asm volatile("dc cvau, %0" :: "r"(cursor) : "memory");
    asm volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
#endif
}
static void replace_pte(pte_t *ptep, unsigned long address, u64 value)
{
    /* Break-before-make under the Linux PTL. Broadcast covers KPTI ASID pairs;
     * no guessed mm->context.id or unverified kernel TLB function signature.
     * Old shadow storage is retained until QUIESCE, never freed here.
     */
    WRITE_ONCE(ptep->pte, 0);
#ifndef WXSHADOW_ADAPTER_TEST
    /* Repeat the invalidation unconditionally for CPUs needing the ARM64
     * repeat-TLBI workaround; this object cannot use Linux alternatives. */
    asm volatile("dsb ishst\n\ttlbi vaale1is, %0\n\tdsb ish\n\ttlbi vaale1is, %0\n\tdsb ish\n\tisb"
                 :: "r"(address >> 12) : "memory");
    WRITE_ONCE(ptep->pte, value);
    asm volatile("dsb ishst\n\tisb" ::: "memory");
#else
    WRITE_ONCE(ptep->pte, value);
#endif
}
struct pte_operation {
    struct tx_page *page;
    u64 value;
    bool capture, wrote, visited;
};
static int operate_pte(pte_t *ptep, unsigned long address, void *argument)
{
    struct pte_operation *op = argument;
    struct tx_page *page = op->page;
    u64 old = pte_val(READ_ONCE(*ptep));
    op->visited = true;
    if (op->capture) {
        if (!pte_valid(__pte(old)) || !(old & PTE_USER) || pte_special(__pte(old)) ||
            (old & (PTE_UXN | PTE_CONT)) || !(old & PTE_RDONLY) ||
            pte_pfn(__pte(old)) != page->pfn) return -ESTALE;
#ifdef PTE_GP
        if (old & PTE_GP) return -EOPNOTSUPP;
#endif
        page->original_pte = old;
        page->read_pte = old | PTE_UXN;
        return 0;
    }
    /* Never restore a snapshot into a new or externally modified mapping. */
    if (old != page->original_pte && old != page->read_pte && old != page->shadow_pte)
    {
        if (page->shadow_pte && op->value == page->original_pte &&
            pte_valid(__pte(old)) && pte_pfn(__pte(old)) == pte_pfn(__pte(page->shadow_pte))) {
            // Preserve an external permission change while detaching our PFN.
            u64 restore = (old & ~PTE_ADDR_MASK) | (page->original_pte & PTE_ADDR_MASK);
            replace_pte(ptep, address, restore); op->wrote = true; return 0;
        }
        return -ESTALE;
    }
    if (old != op->value) replace_pte(ptep, address, op->value);
    op->wrote = true;
    return 0;
}
static int page_operation(struct tx_context *ctx, struct tx_page *page, u64 value, bool capture)
{
    struct pte_operation op = { .page = page, .value = value, .capture = capture };
    int rc, level;
#ifndef WXSHADOW_ADAPTER_TEST
    u64 *table = (u64 *)ctx->mm->pgd;
    /* apply_to_existing_page_range BUG_ONs on huge/block mappings in 5.10.
     * Check each upper descriptor first. from_pfn uses the running direct-map
     * translation; compiled-in __va helpers can depend on unrelocated globals.
     * Actual PTE access and modification still happen in the kernel walker/PTL.
     */
    for (level = CONFIG_PGTABLE_LEVELS - 1; level > 0; --level) {
        unsigned int index = (page->address >> (PAGE_SHIFT + level * 9)) & 511;
        u64 descriptor = READ_ONCE(table[index]);
        if ((descriptor & 3) != 3) return -EOPNOTSUPP;
        table = host.from_pfn((descriptor & PTE_ADDR_MASK) >> PAGE_SHIFT);
    }
#endif
    rc = walk_fn(ctx->mm, page->address, PAGE_SIZE, operate_pte, &op);
    if (op.wrote) return 0; // a completed publication is authoritative
    return rc ? rc : !op.visited ? -ESTALE : 0;
}
static void restore_and_retire(struct tx_context *ctx, struct tx_page *page)
{
    if (!page->active) return;
    /* An absent/changed PTE is already invalidated; do not resurrect it. */
    page_operation(ctx, page, page->original_pte, false);
    retire(ctx, page);
}

static int invalidate_start(struct mmu_notifier *notifier, const struct mmu_notifier_range *range)
{
    struct tx_context *ctx = container_of(notifier, struct tx_context, notifier);
    struct tx_page *page;
    /* Atomic invalidations must retry in a blockable context. We do not take a
     * sleeping lock or a VM walk from the nonblockable callback. */
    if (!mmu_notifier_range_blockable(range)) return -EAGAIN;
    lock_fn(&state_lock);
    list_for_each_entry(page, &ctx->pages, node)
        if (page->active && page->address < range->end && page->address + PAGE_SIZE > range->start)
            restore_and_retire(ctx, page);
    unlock_fn(&state_lock);
    return 0;
}
static void mm_release(struct mmu_notifier *notifier, struct mm_struct *mm)
{
    struct tx_context *ctx = container_of(notifier, struct tx_context, notifier);
    struct tx_page *page;
    /* exit_mmap invokes release before freeing page tables. mm_users is zero,
     * so no user can mutate this address space; do not reacquire mmap_lock. */
    lock_fn(&state_lock);
    list_for_each_entry(page, &ctx->pages, node) restore_and_retire(ctx, page);
    ctx->released = true;
    unlock_fn(&state_lock);
}
static const struct mmu_notifier_ops notifier_ops = {
    .release = mm_release, .invalidate_range_start = invalidate_start,
};

/* control_lock held. Registration takes mmap_lock internally; no state lock. */
static struct tx_context *get_context(struct mm_struct *mm)
{
    struct tx_context *ctx;
    lock_fn(&state_lock); ctx = find_context(mm); unlock_fn(&state_lock);
    if (ctx) return ctx;
    if (context_count >= WX_MAX_CONTEXTS) return NULL;
    ctx = alloc_zero(sizeof(*ctx));
    if (!ctx) return NULL;
    INIT_LIST_HEAD(&ctx->pages); INIT_LIST_HEAD(&ctx->records); INIT_LIST_HEAD(&ctx->shadows);
    ctx->notifier.ops = &notifier_ops; ctx->mm = mm;
    /* LL/SC builtins avoid depending on Linux alternatives/static-key tables
     * that the KPM loader does not relocate like the Linux module loader. */
    __atomic_add_fetch(&mm->mm_count.counter, 1, __ATOMIC_RELAXED);
    if (register_fn(&ctx->notifier, mm)) { drop_mm(mm); host.free(ctx); return NULL; }
    lock_fn(&state_lock); tx_link_tail(&ctx->node, &contexts); ++context_count; unlock_fn(&state_lock);
    return ctx;
}
static int valid_vma(struct mm_struct *mm, unsigned long address)
{
    struct vm_area_struct *vma = find_vma_fn(mm, address);
    unsigned long unsupported = VM_WRITE | VM_SHARED | VM_IO | VM_PFNMAP | VM_MIXEDMAP | VM_HUGETLB;
#ifdef VM_ARM64_BTI
    unsupported |= VM_ARM64_BTI;
#endif
    return vma && vma->vm_start <= address && vma->vm_end - address >= 4 &&
           (vma->vm_flags & (VM_READ | VM_EXEC)) == (VM_READ | VM_EXEC) &&
           !(vma->vm_flags & unsupported);
}
static struct tx_shadow *prepare_shadow(struct tx_context *ctx, struct tx_page *page,
                                        struct tx_record *skip, const struct wx_patch_request *add)
{
    struct tx_shadow *shadow;
    struct tx_record *record;
    if (add && shadow_count >= WX_INSTALL_SHADOW_BUDGET) return NULL;
    shadow = alloc_zero(sizeof(*shadow));
    if (!shadow) return NULL;
    shadow->address = (void *)(host.alloc_page)(GFP_KERNEL | __GFP_ZERO, 0);
    if (!shadow->address) { host.free(shadow); return NULL; }
    memcpy_fn(shadow->address, host.from_pfn(page->pfn), PAGE_SIZE);
    list_for_each_entry(record, &ctx->records, node)
        if (record != skip && record->page == page && record->state == WX_PATCH_ACTIVE)
            memcpy_fn((char *)shadow->address + (record->request.address & ~PAGE_MASK), &record->request.replacement, 4);
    if (add) memcpy_fn((char *)shadow->address + (add->address & ~PAGE_MASK), &add->replacement, 4);
    sync_code(shadow->address);
    return shadow;
}
static int commit_shadow(struct tx_context *ctx, struct tx_page *page, struct tx_shadow *shadow)
{
    u64 pte = page->original_pte;
    int rc;
    pte &= ~(PTE_ADDR_MASK | PTE_USER | PTE_UXN);
    pte |= ((u64)host.to_pfn(shadow->address) << PAGE_SHIFT) & PTE_ADDR_MASK;
    pte |= PTE_RDONLY | PTE_PXN;
    rc = page_operation(ctx, page, pte, false);
    if (rc) return rc;
    page->shadow_pte = pte; page->shadow = shadow->address; page->active = true;
    tx_link_tail(&shadow->node, &ctx->shadows);
    ++shadow_count;
    return 0;
}
static void free_shadow(struct tx_shadow *shadow)
{
    if (shadow) { (host.free_page)((unsigned long)shadow->address, 0); host.free(shadow); }
}
static bool same_identity(const struct tx_record *record, const struct wx_identity *id)
{
    return record->request.address == id->address &&
           (!id->generation || record->page->generation == id->generation);
}

static int install(struct tx_context *ctx, struct wx_patch_request *req)
{
    struct tx_record *existing, *record = NULL;
    struct tx_page *page = NULL;
    struct tx_shadow *shadow = NULL;
    struct page *pin = NULL;
    bool fresh = false;
    int rc = 0;
    unsigned int expected;
    unsigned int count = 0;
    if (host.legacy_page(ctx->mm, req->address)) return -EBUSY;
    lock_fn(&state_lock);
    existing = find_record(ctx, req->cookie);
    if (existing) {
        if (existing->request.address != req->address || existing->request.expected != req->expected ||
            existing->request.replacement != req->replacement ||
            (req->generation && req->generation != existing->page->generation)) rc = -EEXIST;
        else if (existing->state != WX_PATCH_ACTIVE) rc = -ESTALE;
        else { req->generation = existing->page->generation; req->state = WX_PATCH_ACTIVE; }
        unlock_fn(&state_lock); return rc;
    }
    page = find_page(ctx, req->address);
    unlock_fn(&state_lock);
    if (record_count >= WX_MAX_RECORDS || (!page && retained_page_count >= WX_MAX_PAGES)) return -ENOSPC;
    if (!page) {
        long got;
        /* get_user_pages requires the caller to hold mmap_lock. Do not hold
         * state_lock: its follow_page_pte hook also takes that lock. Capture
         * revalidates the VMA and PFN later under the write lock. */
        read_lock_fn(&ctx->mm->mmap_lock);
        got = gup_fn(req->address & PAGE_MASK, 1, 0, &pin, NULL);
        read_unlock_fn(&ctx->mm->mmap_lock);
        if (got != 1) return got < 0 ? got : -EFAULT;
        if (PageCompound(pin)) { release_pages_fn(&pin, 1); return -EOPNOTSUPP; }
        page = alloc_zero(sizeof(*page));
        if (!page) { release_pages_fn(&pin, 1); return -ENOMEM; }
        page->pin = pin; page->pfn = page_to_pfn(pin); page->address = req->address & PAGE_MASK;
        fresh = true;
    }
    record = alloc_zero(sizeof(*record));
    if (!record) { rc = -ENOMEM; goto out; }
    write_lock_fn(&ctx->mm->mmap_lock);
    lock_fn(&state_lock);
    if (ctx->released || !valid_vma(ctx->mm, req->address) || host.legacy_page(ctx->mm, req->address)) {
        rc = -EOPNOTSUPP; goto unlock;
    }
    if (fresh) {
        if (req->generation) { rc = -ESTALE; goto unlock; }
        if (next_generation == ~0ULL) { rc = -EOVERFLOW; goto unlock; }
        page->generation = ++next_generation;
        rc = page_operation(ctx, page, 0, true);
        if (rc) goto unlock;
    } else if (!page->active || req->generation != page->generation) { rc = -ESTALE; goto unlock; }
    list_for_each_entry(existing, &ctx->records, node) {
        if (existing->state != WX_PATCH_ACTIVE || existing->page != page) continue;
        if (existing->request.address == req->address) { rc = -EEXIST; goto unlock; }
        ++count;
    }
    if (count >= 128) { rc = -ENOSPC; goto unlock; }
    memcpy_fn(&expected, (char *)host.from_pfn(page->pfn) + (req->address & ~PAGE_MASK), 4);
    if (expected != req->expected) { rc = -ESTALE; goto unlock; }
    shadow = prepare_shadow(ctx, page, NULL, req);
    if (!shadow) { rc = -ENOMEM; goto unlock; }
    rc = commit_shadow(ctx, page, shadow);
    if (rc) { if (!fresh && rc == -ESTALE) restore_and_retire(ctx, page); goto unlock; }
    shadow = NULL;
    if (fresh) { tx_link_tail(&page->node, &ctx->pages); ++retained_page_count; }
    req->generation = page->generation; req->state = WX_PATCH_ACTIVE;
    record->request = *req; record->state = WX_PATCH_ACTIVE; record->page = page;
    tx_link_tail(&record->node, &ctx->records); record = NULL;
    ++record_count;
unlock:
    unlock_fn(&state_lock);
    write_unlock_fn(&ctx->mm->mmap_lock);
out:
    free_shadow(shadow); host.free(record);
    if (rc && fresh) { release_pages_fn(&page->pin, 1); host.free(page); }
    return rc;
}
static int identify(struct tx_context *ctx, struct wx_identity *id, bool remove)
{
    struct tx_record *record, *other;
    struct tx_page *page;
    struct tx_shadow *shadow = NULL;
    unsigned int remaining = 0;
    int rc = 0;
    if (remove) write_lock_fn(&ctx->mm->mmap_lock);
    lock_fn(&state_lock);
    record = find_record(ctx, id->cookie);
    if (!record) { id->state = WX_PATCH_ABSENT; goto out; }
    if (!same_identity(record, id)) { id->state = WX_PATCH_CONFLICT; rc = -EEXIST; goto out; }
    page = record->page; id->generation = page->generation; id->state = record->state;
    if (!remove || record->state != WX_PATCH_ACTIVE) goto out;
    list_for_each_entry(other, &ctx->records, node)
        if (other != record && other->page == page && other->state == WX_PATCH_ACTIVE) ++remaining;
    if (remaining) {
        shadow = prepare_shadow(ctx, page, record, NULL);
        if (!shadow) { rc = -ENOMEM; goto out; }
        rc = commit_shadow(ctx, page, shadow);
        if (rc) goto out;
        shadow = NULL;
    } else {
        rc = page_operation(ctx, page, page->original_pte, false);
        if (rc == -ESTALE) { retire(ctx, page); rc = 0; }
        if (rc) goto out;
        page->active = false;
    }
    record->state = id->state = WX_PATCH_RETIRED;
out:
    unlock_fn(&state_lock);
    if (remove) write_unlock_fn(&ctx->mm->mmap_lock);
    free_shadow(shadow);
    return rc;
}

static int quiesce(void)
{
    struct tx_context *ctx, *next;
    struct tx_page *page, *pnext;
    struct tx_record *record, *rnext;
    struct tx_shadow *shadow, *snext;
    if (!capable_fn(CAP_SYS_ADMIN)) return -EPERM;
    lock_fn(&state_lock);
    list_for_each_entry(ctx, &contexts, node)
        list_for_each_entry(page, &ctx->pages, node)
            if (page->active) { unlock_fn(&state_lock); return -EBUSY; }
    stopped = true;
    unlock_fn(&state_lock);
    /* Unregister can sleep and synchronize SRCU: this MUST run in prctl,
     * never inside KP's unload callback (the pinned loader holds RCU). */
    list_for_each_entry_safe(ctx, next, &contexts, node) {
        unregister_fn(&ctx->notifier, ctx->mm);
        lock_fn(&state_lock); tx_unlink(&ctx->node); unlock_fn(&state_lock);
        list_for_each_entry_safe(record, rnext, &ctx->records, node) host.free(record);
        list_for_each_entry_safe(page, pnext, &ctx->pages, node) {
            release_pages_fn(&page->pin, 1); host.free(page);
        }
        list_for_each_entry_safe(shadow, snext, &ctx->shadows, node) free_shadow(shadow);
        drop_mm(ctx->mm); host.free(ctx);
    }
    context_count = retained_page_count = record_count = shadow_count = 0;
    return 0;
}
int wx_linux_request(int command, void *request, void *mm)
{
    struct wx_identity *id = request;
    struct wx_patch_request *patch = request;
    struct tx_context *ctx;
    int rc;
    if (!is_ready() || is_compat_task()) return -EOPNOTSUPP;
    if (command != PR_WXSHADOW_QUIESCE &&
        (!id->cookie || !id->address || (id->address & 3) || id->address >= ((struct mm_struct *)mm)->task_size)) return -EINVAL;
    if (command == PR_WXSHADOW_PATCH_EX && (patch->replacement & 0xfc000000U) != 0x14000000U) return -EINVAL;
    lock_fn(&control_lock);
    if (command == PR_WXSHADOW_QUIESCE) { rc = quiesce(); goto out; }
    if (stopped) { rc = -ESHUTDOWN; goto out; }
    lock_fn(&state_lock); ctx = find_context(mm); unlock_fn(&state_lock);
    if (!ctx && command == PR_WXSHADOW_PATCH_EX) ctx = get_context(mm);
    if (!ctx) {
        id->state = WX_PATCH_ABSENT;
        rc = command == PR_WXSHADOW_PATCH_EX ? -ENOMEM : 0;
    } else if (command == PR_WXSHADOW_PATCH_EX) rc = install(ctx, patch);
    else rc = identify(ctx, id, command == PR_WXSHADOW_RELEASE_EX);
out:
    unlock_fn(&control_lock); return rc;
}
int wx_linux_fault(void *mm, unsigned long address, int access)
{
    struct tx_context *ctx;
    struct tx_page *page;
    int handled = 0;
    if (!is_ready()) return 0;
    read_lock_fn(&((struct mm_struct *)mm)->mmap_lock);
    lock_fn(&state_lock);
    ctx = find_context(mm); page = ctx ? find_page(ctx, address) : NULL;
    if (page) {
        if (access == 3) restore_and_retire(ctx, page); // allow the real write fault
        else {
            u64 value = access == 1 ? page->shadow_pte : page->read_pte;
            int rc = page_operation(ctx, page, value, false);
            if (!rc) handled = 1;
            else retire(ctx, page);
        }
    }
    unlock_fn(&state_lock);
    read_unlock_fn(&((struct mm_struct *)mm)->mmap_lock);
    return handled;
}
int wx_linux_before_fork(void *mm)
{
    struct tx_context *ctx;
    struct tx_page *page;
    if (!is_ready()) return 0;
    /* Keep installations out until dup_mmap finishes copying page tables.
     * Merely restoring and dropping mmap_lock here leaves a publication gap
     * before the original dup_mmap acquires that lock itself. */
    lock_fn(&control_lock);
    if (stopped) { unlock_fn(&control_lock); return 0; }
    write_lock_fn(&((struct mm_struct *)mm)->mmap_lock);
    lock_fn(&state_lock); ctx = find_context(mm);
    if (ctx) list_for_each_entry(page, &ctx->pages, node) restore_and_retire(ctx, page);
    unlock_fn(&state_lock);
    write_unlock_fn(&((struct mm_struct *)mm)->mmap_lock);
    return 1;
}
void wx_linux_after_fork(void) { unlock_fn(&control_lock); }
void wx_linux_before_gup(void *vma_pointer, unsigned long address)
{
    struct vm_area_struct *vma = vma_pointer;
    struct tx_context *ctx;
    struct tx_page *page;
    if (!is_ready() || !vma) return;
    /* follow_page_pte is entered with mmap_lock held and before its PTL. */
    lock_fn(&state_lock); ctx = find_context(vma->vm_mm);
    page = ctx ? find_page(ctx, address) : NULL;
    if (page) restore_and_retire(ctx, page);
    unlock_fn(&state_lock);
}
int wx_linux_owns(void *mm, unsigned long address)
{
    struct tx_context *ctx;
    struct tx_page *page;
    int result = 0;
    if (!is_ready()) return 0;
    lock_fn(&state_lock); ctx = find_context(mm);
    if (ctx) list_for_each_entry(page, &ctx->pages, node)
        if (page->active && (!address || page->address == (address & PAGE_MASK))) { result = 1; break; }
    unlock_fn(&state_lock); return result;
}
int wx_linux_is_quiesced(void) { return !is_ready() || READ_ONCE(stopped); }
int wx_linux_available(void) { return is_ready() && !is_compat_task() && !READ_ONCE(stopped); }
int wx_linux_legacy_lock(void)
{
    if (!is_ready()) return 0;
    lock_fn(&control_lock);
    return 1;
}
void wx_linux_legacy_unlock(void) { unlock_fn(&control_lock); }

#define RESOLVE(field, symbol) do { field = (void *)host.lookup(symbol); if (!field) return -EOPNOTSUPP; } while (0)
int wx_linux_init(const struct wx_linux_host *h)
{
    const char *banner;
    const char expected[] = "Linux version " UTS_RELEASE " ";
    host = *h;
    RESOLVE(wx_memstart_addr, "memstart_addr");
    RESOLVE(memcpy_fn, "memcpy"); RESOLVE(memcmp_fn, "memcmp"); RESOLVE(memset_fn, "memset");
    banner = (const char *)host.lookup("linux_banner");
    if (!banner || memcmp_fn(banner, expected, sizeof(expected) - 1) ||
        h->mm_pgd_offset != offsetof(struct mm_struct, pgd)) return -EPROTO;
    RESOLVE(lock_fn, "mutex_lock"); RESOLVE(unlock_fn, "mutex_unlock");
    RESOLVE(read_lock_fn, "down_read"); RESOLVE(read_unlock_fn, "up_read");
    RESOLVE(write_lock_fn, "down_write"); RESOLVE(write_unlock_fn, "up_write");
    RESOLVE(walk_fn, "apply_to_existing_page_range"); RESOLVE(gup_fn, "get_user_pages");
    RESOLVE(release_pages_fn, "release_pages"); RESOLVE(find_vma_fn, "find_vma");
    RESOLVE(register_fn, "mmu_notifier_register"); RESOLVE(unregister_fn, "mmu_notifier_unregister");
    RESOLVE(mmdrop_fn, "__mmdrop"); RESOLVE(capable_fn, "capable");
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
    return 0;
}
