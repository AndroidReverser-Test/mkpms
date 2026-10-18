/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef WXSHADOW_PROTOCOL_TEST
#include "protocol_test_env.h"
#else
#include "wxshadow_internal.h"
#endif
#include "wxshadow_linux.h"

static unsigned long (*wx_copy_from_user)(void *, const void *, unsigned long);
static unsigned long (*wx_copy_to_user)(void *, const void *, unsigned long);
static int wx_native_ready;
static wx_u64 wx_instance;

#ifndef WXSHADOW_NATIVE_ADAPTER
int wx_linux_init(const struct wx_linux_host *host) { return -EOPNOTSUPP; }
int wx_linux_request(int command, void *request, void *mm) { return -EOPNOTSUPP; }
int wx_linux_fault(void *mm, unsigned long address, int access) { return 0; }
int wx_linux_before_fork(void *mm) { return 0; }
void wx_linux_after_fork(void) {}
void wx_linux_before_gup(void *vma, unsigned long address) {}
int wx_linux_owns(void *mm, unsigned long address) { return 0; }
int wx_linux_is_quiesced(void) { return 1; }
int wx_linux_available(void) { return 0; }
int wx_linux_legacy_lock(void) { return 0; }
void wx_linux_legacy_unlock(void) {}
#endif

static unsigned long wx_to_pfn(void *address) { return kaddr_to_pfn((unsigned long)address); }
static int wx_legacy_page(void *mm, unsigned long address)
{
    struct wxshadow_page *page = wxshadow_find_page(mm, address);
    if (!page) return 0;
    wxshadow_page_put(page);
    return 1;
}

void wx_protocol_init(void)
{
    wx_u64 (*random_u64)(void) = (void *)wxshadow_lookup_symbol("get_random_u64");
    struct wx_linux_host host = {
        .lookup = wxshadow_lookup_symbol, .alloc = (void *)kfunc_kzalloc,
        .free = kfunc_kfree, .alloc_page = kfunc___get_free_pages,
        .free_page = kfunc_free_pages, .to_pfn = wx_to_pfn,
        .from_pfn = pfn_to_kaddr, .legacy_page = wx_legacy_page,
        .mm_pgd_offset = mm_struct_offset.pgd_offset,
    };
    /* kzalloc/_copy_*_user are inline on ARM64. GKI LTO emits local clones
     * with specialized calling conventions under those exact names. Resolve
     * exported functions whose prototypes are preserved instead. Nofault
     * copies also keep user faults from recursively entering our VM hooks.
     * Requests must be resident; EFAULT remains resolvable using the cookie. */
    host.alloc = (void *)wxshadow_lookup_symbol("__kmalloc");
    wx_copy_from_user = (void *)wxshadow_lookup_symbol("copy_from_user_nofault");
    wx_copy_to_user = (void *)wxshadow_lookup_symbol("copy_to_user_nofault");
    /* Refuse to invent a reusable instance ID on kernels without entropy. */
    wx_instance = random_u64 ? random_u64() : 0;
    if (wx_instance && host.alloc && wx_copy_from_user && wx_copy_to_user &&
        wx_page_shift == 12 && kfunc_do_page_fault && kfunc_dup_mmap && kfunc_follow_page_pte)
        wx_native_ready = wx_linux_init(&host) == 0;
    pr_info("wxshadow: ABI v1 native VM adapter %s\n", wx_native_ready ? "ready" : "unavailable");
}

int wx_protocol_legacy_conflict(void *mm, unsigned long address)
{
    return wx_native_ready && wx_linux_owns(mm, address);
}

long wx_protocol_request(int command, unsigned long user_request,
                         unsigned long a3, unsigned long a4, unsigned long a5)
{
    union {
        struct wx_query query;
        struct wx_patch_request patch;
        struct wx_identity identity;
    } req;
    struct wx_request_header header;
    unsigned long size;
    void *mm;
    long ret;

    if (a3 || a4 || a5 || !user_request || !wx_copy_from_user || !wx_copy_to_user) {
#ifdef WXSHADOW_DEBUG_PROTOCOL
        pr_info("wxshadow: protocol rejected args %lx %lx %lx\n", a3, a4, a5);
#endif
        return -EINVAL;
    }
    if (wx_copy_from_user(&header, (void *)user_request, sizeof(header))) return -EFAULT;
    size = command == PR_WXSHADOW_QUERY ? sizeof(req.query) :
           command == PR_WXSHADOW_PATCH_EX ? sizeof(req.patch) : sizeof(req.identity);
    if (header.version != WXSHADOW_ABI_VERSION || header.size != size || header.flags || header.reserved) {
#ifdef WXSHADOW_DEBUG_PROTOCOL
        pr_info("wxshadow: protocol rejected header v=%u size=%u/%lu flags=%u reserved=%u\n",
                header.version, header.size, size, header.flags, header.reserved);
#endif
        return -EINVAL;
    }
    memset(&req, 0, sizeof(req));
    if (wx_copy_from_user(&req, (void *)user_request, size)) return -EFAULT;
    if (req.query.header.version != header.version || req.query.header.size != size ||
        req.query.header.flags || req.query.header.reserved) return -EINVAL;
    if (command == PR_WXSHADOW_QUERY) {
        if (req.query.reserved[0] || req.query.reserved[1]) return -EINVAL;
        req.query.arch = WXSHADOW_ARCH_AARCH64;
        req.query.page_size = 1U << wx_page_shift;
        req.query.instance = wx_instance;
        req.query.capabilities = wx_native_ready && wx_linux_available() ? WX_CAP_REQUIRED | WX_CAP_FORK : 0;
        return wx_copy_to_user((void *)user_request, &req, size) ? -EFAULT : 0;
    }
    if (!wx_native_ready) return -EOPNOTSUPP;
    if (req.identity.instance != wx_instance) return -ESTALE;
    if (command == PR_WXSHADOW_PATCH_EX ? req.patch.reserved : req.identity.reserved) return -EINVAL;
    mm = kfunc_get_task_mm(current);
    if (!mm) return -ESRCH;
    ret = wx_linux_request(command, &req, mm);
    kfunc_mmput(mm);
    if (ret) return ret;
    /* Commit may already have happened. The caller resolves EFAULT by cookie. */
    return wx_copy_to_user((void *)user_request, &req, size) ? -EFAULT : 0;
}
