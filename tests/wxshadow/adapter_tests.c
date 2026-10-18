#include "adapter_test_env.h"
#include <stdio.h>

bool wx_test_compat;
static struct mm_struct mm = {.task_size = 1UL << 47, .mm_count = {1}};
static struct vm_area_struct vma = {.vm_start = 0x10000000, .vm_end = 0x10001000,
                                   .vm_flags = VM_READ | VM_EXEC, .vm_mm = &mm};
static struct page original;
static pte_t pte;
static int allocations, pages_allocated, fail_after = -1, walk_failure, held_mutexes;
static bool admin = true, legacy;
static s64 memstart;
static void lock(struct mutex *m) { assert(!m->locked); m->locked = 1; ++held_mutexes; }
static void unlock(struct mutex *m) { assert(m->locked); m->locked = 0; --held_mutexes; }
static void read_lock(struct rw_semaphore *s) { assert(!s->writer); ++s->readers; }
static void read_unlock(struct rw_semaphore *s) { assert(s->readers); --s->readers; }
static void write_lock(struct rw_semaphore *s) { assert(!s->writer && !s->readers); s->writer = 1; }
static void write_unlock(struct rw_semaphore *s) { assert(s->writer); s->writer = 0; }
static int walk(struct mm_struct *m, unsigned long addr, unsigned long size, pte_fn_t fn, void *data) {
  assert(m == &mm && (m->mmap_lock.readers || m->mmap_lock.writer || m->exiting));
  assert(addr == vma.vm_start && size == 4096);
  if (walk_failure) return -EIO;
  return pte.pte ? fn(&pte, addr, data) : 0;
}
static long get_pages(unsigned long addr, unsigned long count, unsigned int flags, struct page **out, struct vm_area_struct **unused) {
  assert(mm.mmap_lock.readers && !mm.mmap_lock.writer);
  assert(addr == vma.vm_start && count == 1); *out = &original; ++original.refs; return 1;
}
static void release_pages(struct page **page, int count) { assert(count == 1 && *page == &original && original.refs > 0); --original.refs; }
static struct vm_area_struct *find_vma(struct mm_struct *m, unsigned long addr) { return &vma; }
static int register_notifier(struct mmu_notifier *n, struct mm_struct *m) {
  assert(!m->notifier); m->notifier = n; n->mm = m; ++m->mm_count.counter; return 0;
}
static void unregister_notifier(struct mmu_notifier *n, struct mm_struct *m) {
  n->ops->release(n, m); assert(m->notifier == n); m->notifier = NULL; --m->mm_count.counter;
}
static void mmdrop(struct mm_struct *m) { assert(m->mm_count.counter == 0); }
static bool capable(int cap) { return admin; }
static unsigned long lookup(const char *name) {
#define SYMBOL(n,f) if (!strcmp(name,n)) return (unsigned long)(f)
  SYMBOL("linux_banner", "Linux version " UTS_RELEASE " test"); SYMBOL("memstart_addr", &memstart);
  SYMBOL("memcpy", memcpy); SYMBOL("memcmp", memcmp); SYMBOL("memset", memset);
  SYMBOL("mutex_lock", lock); SYMBOL("mutex_unlock", unlock);
  SYMBOL("down_read", read_lock); SYMBOL("up_read", read_unlock);
  SYMBOL("down_write", write_lock); SYMBOL("up_write", write_unlock);
  SYMBOL("apply_to_existing_page_range", walk); SYMBOL("get_user_pages", get_pages);
  SYMBOL("release_pages", release_pages); SYMBOL("find_vma", find_vma);
  SYMBOL("mmu_notifier_register", register_notifier); SYMBOL("mmu_notifier_unregister", unregister_notifier);
  SYMBOL("__mmdrop", mmdrop); SYMBOL("capable", capable); return 0;
}
static bool fail_alloc(void) { if (fail_after < 0) return false; return fail_after-- == 0; }
static void *allocate(unsigned long size, unsigned int flags) {
  if (fail_alloc()) return NULL;
  void *p = calloc(1, size); if (p) ++allocations; return p;
}
static void deallocate(void *p) { if (p) { --allocations; free(p); } }
static unsigned long allocate_page(unsigned int flags, unsigned int order) {
  if (fail_alloc()) return 0;
  void *p = aligned_alloc(4096, 4096); if (p) { ++pages_allocated; memset(p,0,4096); } return (unsigned long)p;
}
static void free_page(unsigned long address, unsigned int order) { --pages_allocated; free((void *)address); }
static unsigned long to_pfn(void *address) { return (unsigned long)address >> 12; }
static void *from_pfn(unsigned long pfn) { return (void *)(pfn << 12); }
static int legacy_page(void *m, unsigned long address) { return legacy; }
static struct wx_patch_request patch(unsigned long cookie, unsigned long offset, unsigned long generation) {
  struct wx_patch_request r = {.cookie = cookie, .address = vma.vm_start + offset,
      .generation = generation, .expected = 0xd503201f, .replacement = 0x14000001}; return r;
}
static struct wx_identity identity(const struct wx_patch_request *r) {
  struct wx_identity id = {.cookie=r->cookie, .address=r->address, .generation=r->generation}; return id;
}
static void expect_state(struct wx_patch_request *r, unsigned int state) {
  struct wx_identity id = identity(r);
  assert(!wx_linux_request(PR_WXSHADOW_QUERY_PATCH, &id, &mm) && id.state == state);
}
int main(void) {
  void *bytes = aligned_alloc(4096,4096);
  for (int i=0;i<1024;i++) ((unsigned int *)bytes)[i] = 0xd503201f;
  original.pfn = to_pfn(bytes); pte.pte = (original.pfn<<12) | 3 | PTE_USER | PTE_RDONLY | PTE_PXN;
  u64 old = pte.pte;
  struct wx_linux_host h = {lookup, allocate, deallocate, allocate_page, free_page, to_pfn, from_pfn, legacy_page, offsetof(struct mm_struct,pgd)};
  assert(!wx_linux_init(&h));
  wx_test_compat = true; assert(!wx_linux_available());
  wx_test_compat = false; assert(wx_linux_available());
  struct wx_patch_request a = patch(1,0,0);
  assert(!wx_linux_request(PR_WXSHADOW_PATCH_EX,&a,&mm));
  assert(a.generation && a.state == WX_PATCH_ACTIVE && pte.pte != old);
  assert(!wx_linux_request(PR_WXSHADOW_PATCH_EX,&a,&mm));
  struct wx_patch_request conflict = a; conflict.replacement++;
  assert(wx_linux_request(PR_WXSHADOW_PATCH_EX,&conflict,&mm) == -EEXIST);
  conflict = patch(2,0,a.generation);
  assert(wx_linux_request(PR_WXSHADOW_PATCH_EX,&conflict,&mm) == -EEXIST);
  struct wx_patch_request b = patch(3,4092,a.generation);
  u64 active_pte = pte.pte;
  for (int i=0;i<3;i++) {
    fail_after=i;
    assert(wx_linux_request(PR_WXSHADOW_PATCH_EX,&b,&mm) == -ENOMEM);
    assert(pte.pte == active_pte); expect_state(&a,WX_PATCH_ACTIVE);
  }
  fail_after=-1; walk_failure=1;
  assert(wx_linux_request(PR_WXSHADOW_PATCH_EX,&b,&mm) == -EIO && pte.pte == active_pte);
  walk_failure=0; assert(!wx_linux_request(PR_WXSHADOW_PATCH_EX,&b,&mm));
  struct wx_identity id = identity(&a);
  fail_after=0; assert(wx_linux_request(PR_WXSHADOW_RELEASE_EX,&id,&mm) == -ENOMEM);
  fail_after=-1; expect_state(&a,WX_PATCH_ACTIVE); expect_state(&b,WX_PATCH_ACTIVE);
  assert(!wx_linux_request(PR_WXSHADOW_RELEASE_EX,&id,&mm));
  expect_state(&a,WX_PATCH_RETIRED); expect_state(&b,WX_PATCH_ACTIVE);
  assert(!wx_linux_request(PR_WXSHADOW_RELEASE_EX,&id,&mm));
  assert(wx_linux_fault(&mm,vma.vm_start,2) == 1 && (pte.pte & PTE_UXN));
  assert(wx_linux_fault(&mm,vma.vm_start,1) == 1 && !(pte.pte & PTE_USER));
  struct mmu_notifier_range range = {vma.vm_start,vma.vm_end,false};
  assert(mm.notifier->ops->invalidate_range_start(mm.notifier,&range) == -EAGAIN);
  expect_state(&b,WX_PATCH_ACTIVE);
  range.blockable=true; write_lock(&mm.mmap_lock);
  assert(!mm.notifier->ops->invalidate_range_start(mm.notifier,&range)); write_unlock(&mm.mmap_lock);
  assert(pte.pte == old); expect_state(&b,WX_PATCH_RETIRED);
  struct wx_patch_request c=patch(4,4,0); assert(!wx_linux_request(PR_WXSHADOW_PATCH_EX,&c,&mm));
  assert(c.generation != a.generation);
  // Unexpected new mapping: QUERY/teardown must not put the old PTE back.
  pte.pte = old + 4096; range.blockable=true; write_lock(&mm.mmap_lock);
  assert(!mm.notifier->ops->invalidate_range_start(mm.notifier,&range)); write_unlock(&mm.mmap_lock);
  assert(pte.pte == old+4096); expect_state(&c,WX_PATCH_RETIRED);
  pte.pte=old; struct wx_patch_request d=patch(5,8,0);
  assert(!wx_linux_request(PR_WXSHADOW_PATCH_EX,&d,&mm));
  struct wx_identity stop={0};
  assert(wx_linux_request(PR_WXSHADOW_QUIESCE,&stop,&mm) == -EBUSY);
  assert(wx_linux_before_fork(&mm) == 1);
  assert(held_mutexes == 1 && !mm.mmap_lock.writer);
  // The control lock must span the original dup_mmap, even after mmap_lock
  // was dropped by the before callback. Query is legal after the after hook.
  wx_linux_after_fork(); assert(!held_mutexes);
  expect_state(&d,WX_PATCH_RETIRED); assert(pte.pte == old);
  admin=false; assert(wx_linux_request(PR_WXSHADOW_QUIESCE,&stop,&mm) == -EPERM);
  admin=true; assert(!wx_linux_request(PR_WXSHADOW_QUIESCE,&stop,&mm));
  assert(wx_linux_is_quiesced() && !mm.notifier && mm.mm_count.counter == 1);
  assert(!allocations && !pages_allocated && !original.refs);
  free(bytes); puts("VM transactions: ownership, rollback, retention, page end, invalidation, fork, quiesce passed");
}
