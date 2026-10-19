/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WX_ARM64_RUNTIME_H
#define WX_ARM64_RUNTIME_H

/* KernelPatch's page geometry variables are not part of its exported KPM ABI.
 * Discover the running geometry directly and verify the Linux direct map
 * before publishing any callback. No kernel struct layouts are used here. */
struct wx_arm64_runtime {
    unsigned int page_shift, page_levels, kernel_version;
    unsigned long linear_offset;
};

static inline unsigned int wx_arm64_kernel_version(const char *banner)
{
    const char prefix[] = "Linux version ";
    unsigned int parts[3] = {0, 0, 0}, i;
    if (!banner) return 0;
    for (i = 0; prefix[i]; ++i)
        if (banner[i] != prefix[i]) return 0;
    banner += sizeof(prefix) - 1;
    for (i = 0; i < 3; ++i) {
        unsigned int digits = 0;
        while (*banner >= '0' && *banner <= '9') {
            if (++digits > 6) return 0;
            parts[i] = parts[i] * 10 + (unsigned int)(*banner++ - '0');
        }
        if (!digits || (i < 2 && *banner++ != '.')) return 0;
    }
    if (!parts[0] || parts[0] > 255 || parts[1] > 255) return 0;
    if (parts[2] > 255) parts[2] = 255;
    return parts[0] << 16 | parts[1] << 8 | parts[2];
}

static inline unsigned long wx_arm64_to_pfn(const struct wx_arm64_runtime *runtime,
                                            const void *address)
{
    return ((unsigned long)address - runtime->linear_offset) >> runtime->page_shift;
}
static inline void *wx_arm64_from_pfn(const struct wx_arm64_runtime *runtime,
                                      unsigned long pfn)
{
    return (void *)((pfn << runtime->page_shift) + runtime->linear_offset);
}

static inline int wx_arm64_runtime_init(struct wx_arm64_runtime *runtime,
    unsigned long (*lookup)(const char *),
    unsigned long (*alloc_page)(unsigned int, unsigned int),
    void (*free_page)(unsigned long, unsigned int))
{
#ifdef __aarch64__
    unsigned long tcr, probe, physical, par, saved_par, saved_daif;
    unsigned long page_offset;
    const long *memstart;
    unsigned int user_bits, kernel_bits, kernel_levels;
    if (!runtime || !lookup || !alloc_page || !free_page) return -22;
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    /* TG0=00 and TG1=10 encode 4K. IPS=110 permits 52-bit PAs,
     * which the current PTE/TLBI implementation deliberately rejects. */
    if (((tcr >> 14) & 3) != 0 || ((tcr >> 30) & 3) != 2 ||
        ((tcr >> 32) & 7) > 5) return -95;
    user_bits = 64 - (tcr & 63);
    kernel_bits = 64 - ((tcr >> 16) & 63);
    if (user_bits < 30 || user_bits > 48 || kernel_bits < 30 || kernel_bits > 48)
        return -95;
    runtime->page_shift = 12;
    runtime->page_levels = (user_bits - 4) / 9;
    kernel_levels = (kernel_bits - 4) / 9;
    if (runtime->page_levels < 3 || runtime->page_levels > 4 ||
        kernel_levels < 3 || kernel_levels > 4) return -95;
    runtime->kernel_version = wx_arm64_kernel_version((const char *)lookup("linux_banner"));
    if (!runtime->kernel_version) return -71;
    memstart = (const long *)lookup("memstart_addr");
    if (!memstart) return -95;
    /* Since Linux 5.4 PAGE_OFFSET is -(1UL << VA_BITS); the boundary
     * -(1UL << (VA_BITS - 1)) is PAGE_END, not the direct-map base.
     * memstart_addr includes the running kernel's linear-map randomization. */
    page_offset = ~0UL << kernel_bits;
    runtime->linear_offset = page_offset - (unsigned long)*memstart;
    probe = alloc_page(0xcc0U, 0); /* GFP_KERNEL, unchanged in audited kernels */
    if (!probe) return -12;
    /* Keep the AT/PAR pair on one CPU and preserve PAR for interrupted code.
     * This also rejects a mismatched compile-time VA_BITS configuration. */
    asm volatile("mrs %0, daif\n\tmsr daifset, #0xf\n\tmrs %1, par_el1\n\t"
                 "at s1e1r, %3\n\tisb\n\tmrs %2, par_el1\n\t"
                 "msr par_el1, %1\n\tmsr daif, %0"
                 : "=&r"(saved_daif), "=&r"(saved_par), "=&r"(par)
                 : "r"(probe) : "memory");
    physical = (par & 0x0000fffffffff000UL) | (probe & 4095UL);
    free_page(probe, 0);
    if ((par & 1) || probe - runtime->linear_offset != physical) return -71;
    return 0;
#else
    (void)runtime;
    (void)lookup;
    (void)alloc_page;
    (void)free_page;
    return -95;
#endif
}
#endif
