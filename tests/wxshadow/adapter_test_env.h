/* Minimal VM test double. The adapter's real transaction/notifier code runs
 * unchanged; only Linux primitives and ARM64 hardware operations are replaced. */
#ifndef WX_ADAPTER_TEST_ENV_H
#define WX_ADAPTER_TEST_ENV_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "../../kpms/wxshadow/wxshadow_linux.h"
typedef uint64_t u64;
typedef int64_t s64;
typedef struct { u64 pte; } pte_t;
typedef struct { int counter; } atomic_t;
struct mutex { int locked; };
struct rw_semaphore { int readers, writer; };
struct mmu_notifier;
struct mm_struct {
  void *pgd; unsigned long task_size; atomic_t mm_count;
  struct rw_semaphore mmap_lock;
  struct mmu_notifier *notifier;
  bool exiting;
};
struct vm_area_struct { unsigned long vm_start, vm_end, vm_flags; struct mm_struct *vm_mm; };
struct page { unsigned long pfn; int refs; bool compound; };
struct mmu_notifier_range { unsigned long start, end; bool blockable; };
struct mmu_notifier_ops {
  void (*release)(struct mmu_notifier *, struct mm_struct *);
  int (*invalidate_range_start)(struct mmu_notifier *, const struct mmu_notifier_range *);
};
struct mmu_notifier { const struct mmu_notifier_ops *ops; struct mm_struct *mm; };
typedef int (*pte_fn_t)(pte_t *, unsigned long, void *);
struct list_head { struct list_head *next, *prev; };
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define LIST_HEAD(name) struct list_head name = {&name, &name}
#define DEFINE_MUTEX(name) struct mutex name = {0}
#define INIT_LIST_HEAD(p) do { (p)->next = (p); (p)->prev = (p); } while (0)
static inline void list_add_tail(struct list_head *n, struct list_head *h) {
  n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static inline void list_del(struct list_head *n) { n->prev->next = n->next; n->next->prev = n->prev; }
#define list_for_each_entry(p,h,m) for (p=container_of((h)->next, __typeof__(*p), m); &p->m!=(h); p=container_of(p->m.next,__typeof__(*p),m))
#define list_for_each_entry_safe(p,n,h,m) for(p=container_of((h)->next,__typeof__(*p),m),n=container_of(p->m.next,__typeof__(*p),m); &p->m!=(h); p=n,n=container_of(n->m.next,__typeof__(*n),m))
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~4095UL)
#define PTE_ADDR_MASK 0x0000fffffffff000ULL
#define PTE_USER (1ULL << 6)
#define PTE_RDONLY (1ULL << 7)
#define PTE_UXN (1ULL << 54)
#define PTE_PXN (1ULL << 53)
#define PTE_CONT (1ULL << 52)
#define PTE_GP (1ULL << 50)
#define VM_READ 1
#define VM_WRITE 2
#define VM_EXEC 4
#define VM_SHARED 8
#define VM_IO 16
#define VM_PFNMAP 32
#define VM_MIXEDMAP 64
#define VM_HUGETLB 128
#define VM_ARM64_BTI 256
#define GFP_KERNEL 0
#define __GFP_ZERO 0
#define CAP_SYS_ADMIN 21
#define UTS_RELEASE "wxshadow-test"
#define READ_ONCE(v) (v)
#define WRITE_ONCE(p,v) ((p)=(v))
#define __pte(v) ((pte_t){v})
#define pte_val(p) ((p).pte)
#define pte_pfn(p) (((p).pte & PTE_ADDR_MASK) >> 12)
#define pte_valid(p) (((p).pte & 3) == 3)
#define pte_special(p) false
#define page_to_pfn(p) ((p)->pfn)
#define PageCompound(p) ((p)->compound)
#define mmu_notifier_range_blockable(r) ((r)->blockable)
extern bool wx_test_compat;
#define is_compat_task() wx_test_compat
#endif
