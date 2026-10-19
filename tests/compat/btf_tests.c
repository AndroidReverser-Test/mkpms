/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kpms/common/wx_btf.h"
#include "../../kpms/common/wx_arm64_runtime.h"

static unsigned char blob[8192], records[4096];
static char strings[2048];
static unsigned int record_len, string_len, blob_len, allocations;
static int allocation_failure;
static unsigned long symbol_start, symbol_stop;

static void put32(unsigned char *p, unsigned int value)
{
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16); p[3] = (unsigned char)(value >> 24);
}
static unsigned int string(const char *s)
{
    unsigned int off = string_len, len = (unsigned int)strlen(s) + 1;
    assert(string_len + len <= sizeof(strings));
    memcpy(strings + string_len, s, len); string_len += len; return off;
}
static void word(unsigned int value)
{
    assert(record_len + 4 <= sizeof(records));
    put32(records + record_len, value); record_len += 4;
}
static void type(const char *name, unsigned int kind, unsigned int vlen,
                 unsigned int flag, unsigned int size_or_type)
{
    word(string(name)); word((flag << 31) | (kind << 24) | vlen); word(size_or_type);
}
static void member(const char *name, unsigned int id, unsigned int bits)
{
    word(string(name)); word(id); word(bits);
}
static void fixture(void)
{
    unsigned int i;
    memset(blob, 0, sizeof(blob)); memset(records, 0, sizeof(records));
    memset(strings, 0, sizeof(strings)); record_len = 0; string_len = 1;
    type("u32", 1, 0, 0, 4); word(32); /* 1 */
    type("u64", 1, 0, 0, 8); word(64); /* 2 */
    type("vm_area_struct", 4, 2, 0, 16); /* 3 */
    member("vm_start", 2, 0); member("vm_end", 2, 64);
    type("", 2, 0, 0, 3); /* 4 pointer */
    type("", 10, 0, 0, 3); /* 5 const VMA */
    type("", 2, 0, 0, 5); /* 6 const VMA pointer */
    type("", 2, 0, 0, 6); /* 7 double pointer */
    type("", 5, 1, 0, 8); member("value", 2, 0); /* 8 anonymous union */
    type("", 4, 2, 0, 16); /* 9 anonymous struct */
    member("", 8, 0); member("leaf", 1, 64);
    type("holder", 4, 3, 0, 32); /* 10 */
    member("prefix", 1, 0); member("", 9, 64); member("flags", 1, 192);
    type("holder_t", 8, 0, 0, 10); /* 11 */
    type("", 13, 7, 0, 1); /* 12 */
    for (i = 0; i < 7; ++i) { word(0); word(i == 5 ? 7 : 2); }
    type("gup7", 12, 1, 0, 12); /* 13 */
    type("", 13, 6, 0, 1); /* 14 */
    for (i = 0; i < 6; ++i) { word(0); word(2); }
    type("gup6", 12, 1, 0, 14); /* 15 */
    type("bitfield", 4, 1, 1, 4); member("bits", 1, (5U << 24) | 1); /* 16 */
    type("bad_bounds", 4, 1, 0, 4); member("overflow", 2, 0); /* 17 */
    type("cycle", 8, 0, 0, 18); /* 18 */
    type("", 4, 1, 0, 8); member("self", 19, 0); /* 19 */
    type("bad_recursive", 4, 1, 0, 8); member("", 20, 0); /* 20 */
    blob[0] = 0x9f; blob[1] = 0xeb; blob[2] = 1;
    put32(blob + 4, 24); put32(blob + 12, record_len);
    put32(blob + 16, record_len); put32(blob + 20, string_len);
    memcpy(blob + 24, records, record_len);
    memcpy(blob + 24 + record_len, strings, string_len);
    blob_len = 24 + record_len + string_len;
    symbol_start = (unsigned long)blob; symbol_stop = symbol_start + blob_len;
}
static unsigned long lookup(const char *name)
{
    if (!strcmp(name, "__start_BTF")) return symbol_start;
    if (!strcmp(name, "__stop_BTF")) return symbol_stop;
    return 0;
}
static void *allocate(unsigned long bytes, unsigned int flags)
{
    void *p;
    assert(flags == 123);
    if (allocation_failure) return NULL;
    p = malloc(bytes); if (p) ++allocations; return p;
}
static void release(void *p) { assert(allocations); --allocations; free(p); }
static int init(struct wx_btf *b) { return wx_btf_init(b, lookup, allocate, release, 123); }

static void normal_queries(void)
{
    struct wx_btf b;
    unsigned int value = 0;
    fixture(); assert(init(&b) == 0);
    assert(wx_btf_size(&b, "holder", &value) == 0 && value == 32);
    assert(wx_btf_size(&b, "holder_t", &value) == 0 && value == 32);
    assert(wx_btf_member(&b, "holder_t", "leaf", 4, &value) == 0 && value == 16);
    assert(wx_btf_member(&b, "holder", "value", 8, &value) == 0 && value == 8);
    assert(wx_btf_member(&b, "holder", "flags", 4, &value) == 0 && value == 24);
    assert(wx_btf_member(&b, "holder", "flags", 8, &value) == WX_BTF_EPROTO);
    assert(wx_btf_member(&b, "holder", "missing", 4, &value) == WX_BTF_ENOENT);
    assert(wx_btf_member(&b, "bitfield", "bits", 4, &value) == WX_BTF_EPROTO);
    assert(wx_btf_member(&b, "bad_bounds", "overflow", 8, &value) == WX_BTF_EPROTO);
    assert(wx_btf_member(&b, "holder", "flags", 0, &value) == WX_BTF_EINVAL);
    assert(wx_btf_member(&b, "bad_recursive", "missing", 4, &value) == WX_BTF_EPROTO);
    assert(wx_btf_size(&b, "cycle", &value) == WX_BTF_EPROTO);
    assert(wx_btf_func_arity(&b, "gup7") == 7);
    assert(wx_btf_func_arity(&b, "gup6") == 6);
    assert(wx_btf_func_arity(&b, "absent") == WX_BTF_ENOENT);
    assert(wx_btf_optional_arity(&b, "absent", 5) == 0);
    assert(wx_btf_optional_arity(&b, "gup7", 7) == 0);
    assert(wx_btf_optional_arity(&b, "gup7", 6) == WX_BTF_EPROTO);
    assert(wx_btf_func_param_named(&b, "gup7", 5, "vm_area_struct") == 0);
    assert(wx_btf_func_param_named(&b, "gup6", 5, "vm_area_struct") == WX_BTF_EPROTO);
    assert(wx_btf_func_param_named(&b, "gup6", 6, "vm_area_struct") == WX_BTF_EINVAL);
    wx_btf_destroy(&b); wx_btf_destroy(&b); assert(!allocations);
}
static void malformed_blobs(void)
{
    struct wx_btf b;
    unsigned int value;
    fixture(); symbol_start = 0; assert(init(&b) == WX_BTF_ENOENT);
    fixture(); symbol_stop = symbol_start + 23; assert(init(&b) == WX_BTF_EPROTO);
    fixture(); symbol_stop = symbol_start - 1; assert(init(&b) == WX_BTF_EPROTO);
    fixture(); symbol_stop = symbol_start + WX_BTF_MAX_BLOB + 1; assert(init(&b) == WX_BTF_EPROTO);
    fixture(); blob[0] = 0; assert(init(&b) == WX_BTF_EPROTO);
    fixture(); blob[2] = 2; assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 4, ~0U); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 12, ~0U); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 16, 0); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 20, ~0U); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 24, string_len); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); blob[blob_len - 1] = 'x'; assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 28, 31U << 24); assert(init(&b) == WX_BTF_EOPNOTSUPP);
    fixture(); put32(blob + 28, (1U << 24) | 0x10000); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); put32(blob + 12, record_len - 1); assert(init(&b) == WX_BTF_EPROTO);
    fixture(); allocation_failure = 1; assert(init(&b) == WX_BTF_ENOMEM); allocation_failure = 0;
    /* Corrupt a member type ID without permitting an out-of-bounds read. */
    fixture(); put32(blob + 24 + 32 + 12 + 4, ~0U); assert(init(&b) == 0);
    assert(wx_btf_member(&b, "vm_area_struct", "vm_start", 8, &value) == WX_BTF_EPROTO);
    wx_btf_destroy(&b); assert(!allocations);
    fixture(); put32(blob + 24 + 12, (1U << 16) | 5); assert(init(&b) == 0);
    assert(wx_btf_member(&b, "holder", "prefix", 4, &value) == WX_BTF_EPROTO);
    wx_btf_destroy(&b); assert(!allocations);
}
static void mutation_checks(void)
{
    struct wx_btf b;
    unsigned int i, j, value;
    unsigned char original;
    fixture();
    for (i = 0; i < blob_len; ++i) {
        original = blob[i];
        for (j = 0; j < 2; ++j) {
            blob[i] = (unsigned char)(original ^ (j ? 0xffU : 1U));
            if (!init(&b)) {
                (void)wx_btf_size(&b, "holder", &value);
                (void)wx_btf_member(&b, "holder", "leaf", 4, &value);
                (void)wx_btf_func_arity(&b, "gup7");
                (void)wx_btf_func_param_named(&b, "gup7", 5, "vm_area_struct");
                wx_btf_destroy(&b);
            }
            assert(!allocations);
        }
        blob[i] = original;
    }
}
static void runtime_helper_checks(void)
{
    struct wx_arm64_runtime geometry = {12, 3, 0, 0xffffff7fc0000000UL};
    unsigned long pfn;
    assert(wx_arm64_kernel_version("Linux version 5.10.66-android12 build") == 0x050a42U);
    assert(wx_arm64_kernel_version("Linux version 6.1.118-android14 build") == 0x060176U);
    assert(wx_arm64_kernel_version("Linux version 6.6.0") == 0x060600U);
    assert(wx_arm64_kernel_version("Linux version 5.10.300-vendor") == 0x050affU);
    assert(wx_arm64_kernel_version(NULL) == 0);
    assert(wx_arm64_kernel_version("") == 0);
    assert(wx_arm64_kernel_version("Linux version 6.1.") == 0);
    assert(wx_arm64_kernel_version("Linux version 6.x.12") == 0);
    assert(wx_arm64_kernel_version("Linux version 0.1.1") == 0);
    assert(wx_arm64_kernel_version("Linux version 256.1.1") == 0);
    assert(wx_arm64_kernel_version("Linux version 6.256.1") == 0);
    assert(wx_arm64_kernel_version("Linux version 6.1.1000000") == 0);
    assert(wx_arm64_kernel_version("FreeBSD version 6.1.1") == 0);
    for (pfn = 0x40000; pfn < 0x200000; pfn += 0x12345)
        assert(wx_arm64_to_pfn(&geometry, wx_arm64_from_pfn(&geometry, pfn)) == pfn);
}
static void real_kernel_btf(const char *path)
{
    static const char *const names[] = {"mm_struct", "vm_area_struct", "mutex", "mmu_notifier",
        "mmu_notifier_ops", "mmu_notifier_range", "uprobe_consumer", "page", "file", "inode"};
    static const char *const functions[] = {"get_user_pages", "get_user_pages_remote",
        "uprobe_write_opcode", "__copy_insn", "follow_page_pte", "do_page_fault", "dup_mmap", "__mutex_init",
        "uprobe_register", "uprobe_unregister", "vm_normal_page", "apply_to_existing_page_range",
        "mmu_notifier_register", "mmu_notifier_unregister", "release_pages", "access_process_vm",
        "uprobe_start_dup_mmap", "uprobe_end_dup_mmap", "uprobe_dup_mmap", "arch_uprobe_analyze_insn"};
    static const struct { const char *type, *member; unsigned int bytes; } fields[] = {
        {"mm_struct", "task_size", 8}, {"mm_struct", "mm_count", 4}, {"mm_struct", "pgd", 8},
        {"task_struct", "mm", 8}, {"task_struct", "tgid", 4}, {"task_struct", "cred", 8},
        {"thread_info", "flags", 8}, {"cred", "uid", 4},
        {"vm_area_struct", "vm_start", 8}, {"vm_area_struct", "vm_end", 8},
        {"vm_area_struct", "vm_flags", 8}, {"vm_area_struct", "vm_file", 8},
        {"vm_area_struct", "vm_pgoff", 8}, {"vm_area_struct", "vm_mm", 8},
        {"file", "f_inode", 8}, {"file", "f_count", 8},
        {"inode", "i_mode", 2}, {"inode", "i_mapping", 8},
        {"mmu_notifier", "ops", 8}, {"mmu_notifier_range", "start", 8},
        {"mmu_notifier_range", "end", 8}, {"mmu_notifier_range", "flags", 4},
        {"mmu_notifier_ops", "release", 8}, {"mmu_notifier_ops", "invalidate_range_start", 8},
        {"uprobe_consumer", "handler", 8}, {"uprobe_consumer", "filter", 8},
        {"uprobe_consumer", "next", 8}, {"mutex", "owner", 8}, {"mutex", "wait_list", 16},
        {"uprobe", "inode", 8}, {"uprobe", "offset", 8},
        {"uprobe", "consumers", 8}, {"uprobe", "flags", 8}
    };
    struct wx_btf b;
    unsigned char *data;
    FILE *file = fopen(path, "rb");
    long bytes;
    unsigned int i, size, offset;
    assert(file && fseek(file, 0, SEEK_END) == 0);
    bytes = ftell(file); assert(bytes > 0 && bytes < (long)WX_BTF_MAX_BLOB);
    rewind(file); data = malloc((size_t)bytes); assert(data);
    assert(fread(data, 1, (size_t)bytes, file) == (size_t)bytes); fclose(file);
    symbol_start = (unsigned long)data; symbol_stop = symbol_start + (unsigned long)bytes;
    assert(init(&b) == 0);
    printf("Real BTF: %s, %u types\n", path, b.count);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(wx_btf_size(&b, names[i], &size) == 0);
        printf("  sizeof %s = %u\n", names[i], size);
    }
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        assert(wx_btf_member(&b, fields[i].type, fields[i].member, fields[i].bytes, &offset) == 0);
        printf("  %s.%s = %u (%u bytes)\n", fields[i].type, fields[i].member, offset, fields[i].bytes);
    }
    assert(wx_btf_size(&b, "rw_semaphore", &size) == 0);
    assert(wx_btf_member(&b, "mm_struct", "mmap_lock", size, &offset) == 0);
    printf("  mm_struct.mmap_lock = %u (%u bytes)\n", offset, size);
    assert(wx_btf_size(&b, "thread_info", &size) == 0);
    assert(wx_btf_member(&b, "task_struct", "thread_info", size, &offset) == 0);
    printf("  task_struct.thread_info = %u (%u bytes)\n", offset, size);
    assert(wx_btf_size(&b, "arch_uprobe", &size) == 0);
    assert(wx_btf_member(&b, "uprobe", "arch", size, &offset) == 0);
    printf("  uprobe.arch = %u (%u bytes)\n", offset, size);
    if (!wx_btf_member(&b, "pt_regs", "regs", 248, &offset)) {
        assert(offset == 0);
        assert(wx_btf_member(&b, "pt_regs", "sp", 8, &offset) == 0 && offset == 248);
        assert(wx_btf_member(&b, "pt_regs", "pc", 8, &offset) == 0 && offset == 256);
        assert(wx_btf_member(&b, "pt_regs", "pstate", 8, &offset) == 0 && offset == 264);
        assert(wx_btf_member(&b, "arch_uprobe", "insn", 4, &offset) == 0);
        puts("  ARM64 pt_regs and arch_uprobe.insn widths/offsets verified");
    }
    for (i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i)
        printf("  arity %s = %d\n", functions[i], wx_btf_func_arity(&b, functions[i]));
    if (wx_btf_func_arity(&b, "get_user_pages_remote") == 7)
        assert(wx_btf_func_param_named(&b, "get_user_pages_remote", 5, "vm_area_struct") == 0);
    wx_btf_destroy(&b); free(data); assert(!allocations);
}
int main(int argc, char **argv)
{
    normal_queries(); malformed_blobs(); mutation_checks(); runtime_helper_checks();
    if (argc == 2) real_kernel_btf(argv[1]);
    puts("BTF compatibility parser checks passed"); return 0;
}
