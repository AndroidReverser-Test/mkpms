/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wxuprobe_client.h"
#include "wxuprobe_kernel_trace.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s (errno=%d %s)\n", __LINE__, #condition, errno, strerror(errno)); \
    exit(1); } } while (0)
#define PAGE 4096U
typedef uint64_t (*target_fn)(uint64_t, uint64_t, uint64_t);
struct fixture { int fd; unsigned char *code; size_t size; char path[100]; };
static wu_u64 module_instance, next_cookie = 0x10000;
extern void wu_capture_call(void *function);
extern void wu_capture_resume(void);

static void fixture_create(struct fixture *fixture, unsigned int pages, int custom)
{
    unsigned int page, index;
    uint32_t *words;
    unsigned char *bytes;
    fixture->size = pages * PAGE;
    snprintf(fixture->path, sizeof(fixture->path), "/data/local/tmp/wxuprobe-fixture-XXXXXX");
    fixture->fd = mkstemp(fixture->path); CHECK(fixture->fd >= 0);
    bytes = calloc(1, fixture->size + PAGE); CHECK(bytes);
    for (page = 0; page < pages; ++page) for (index = 0; index < 128; ++index) {
        words = (uint32_t *)(bytes + PAGE + page * PAGE + index * 32);
        words[0] = 0x91000400; words[1] = 0xd65f03c0; /* add x0,x0,#1; ret */
    }
    if (custom) {
        words = (uint32_t *)(bytes + PAGE + 0x100);
        words[0] = 0x8b010000; words[1] = 0x91000400; words[2] = 0xd65f03c0;
        words = (uint32_t *)(bytes + PAGE + 0x120);
        words[0] = 0x8b020000; words[1] = 0x91000400; words[2] = 0xd65f03c0;
        words = (uint32_t *)(bytes + PAGE + 0x140);
        words[0] = 0x14000002; words[1] = 0xd2800000; words[2] = 0x91000400; words[3] = 0xd65f03c0;
        words = (uint32_t *)(bytes + PAGE + 0x160);
        words[0] = 0x10000080; words[1] = 0xd65f03c0; /* adr x0,.+16; ret */
        words = (uint32_t *)(bytes + PAGE + 0x200);
        for (index = 0; index < 31; ++index) words[index] = 0xf9000000 | (index << 10) | (9 << 5) | index;
        words[31] = 0x910003ea; /* mov x10,sp */
        words[32] = 0xf9000000 | (31 << 10) | (9 << 5) | 10;
        words[33] = 0xd65f03c0;
    }
    CHECK(write(fixture->fd, bytes, fixture->size + PAGE) == (ssize_t)(fixture->size + PAGE));
    free(bytes);
    fixture->code = mmap(NULL, fixture->size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fixture->fd, PAGE);
    CHECK(fixture->code != MAP_FAILED);
    /* Populate source pages before exercising the hook publication path. */
    for (page = 0; page < pages; ++page) CHECK(*(volatile uint32_t *)(fixture->code + page * PAGE));
}
static void fixture_destroy(struct fixture *fixture)
{
    CHECK(munmap(fixture->code, fixture->size) == 0);
    CHECK(close(fixture->fd) == 0); CHECK(unlink(fixture->path) == 0);
}
static uint64_t invoke(struct fixture *fixture, unsigned long offset, uint64_t a, uint64_t b, uint64_t c)
{
    return ((target_fn)(fixture->code + offset))(a, b, c);
}
static uint32_t checksum(const unsigned char *address, size_t size)
{
    const volatile unsigned char *bytes = address;
    uint32_t hash = 2166136261U;
    size_t i;
    for (i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}
static struct wu_request request_for(struct fixture *fixture, unsigned long offset)
{
    struct wu_request request;
    wu_request_init(&request); request.instance = module_instance;
    request.cookie = ++next_cookie; request.address = (uintptr_t)fixture->code + offset;
    snprintf(request.name, sizeof(request.name), "fixture_%u_%lx", getpid(), offset);
    return request;
}
static void install(struct wu_request *request)
{
    int rc = wu_call(PR_WXUPROBE_INSTALL, request);
    if (rc) fprintf(stderr, "install request=%p cookie=0x%llx address=0x%llx state=%u error=%d\n",
                    (void *)request, request->cookie, request->address, request->state, request->last_error);
    CHECK(rc == 0); CHECK(request->state == WU_ACTIVE);
}
static void remove_hook(struct wu_request *request)
{
    CHECK(wu_call(PR_WXUPROBE_REMOVE, request) == 0); CHECK(request->state == WU_REMOVED);
}
static void basic_test(void)
{
    struct fixture fixture;
    struct wu_request request, second;
    uint32_t before;
    fixture_create(&fixture, 1, 1);
    before = checksum(fixture.code, PAGE);
    CHECK(invoke(&fixture, 0x100, 3, 4, 5) == 8);
    request = request_for(&fixture, 0x100);
    request.reg_mask = 3; request.regs[0] = 10; request.regs[1] = 20; request.flags = WU_F_TRACE;
    install(&request); CHECK(request.offset == PAGE + 0x100);
    CHECK(invoke(&fixture, 0x100, 3, 4, 5) == 31);
    CHECK(checksum(fixture.code, PAGE) == before);
    CHECK(*(volatile uint32_t *)(fixture.code + 0x100) == 0x8b010000);
    CHECK(invoke(&fixture, 0x100, 1, 1, 1) == 31);
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.hits == 2);
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == 0); /* retry by cookie */
    second = request_for(&fixture, 0x100);
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &second) == -1 && errno == EEXIST);
    request.regs[0] = 100; request.regs[1] = 7;
    CHECK(wu_call(PR_WXUPROBE_UPDATE, &request) == 0);
    CHECK(invoke(&fixture, 0x100, 1, 1, 1) == 108);
    second = request_for(&fixture, 0x120);
    second.reg_mask = 5; second.regs[0] = 50; second.regs[2] = 9;
    install(&second); CHECK(invoke(&fixture, 0x120, 1, 2, 3) == 60);
    remove_hook(&request);
    CHECK(invoke(&fixture, 0x100, 3, 4, 5) == 8);
    CHECK(invoke(&fixture, 0x120, 1, 2, 3) == 60);
    CHECK(checksum(fixture.code, PAGE) == before);
    second.reg_mask = 0;
    CHECK(wu_call(PR_WXUPROBE_UPDATE, &second) == 0);
    CHECK(invoke(&fixture, 0x120, 1, 2, 3) == 5);
    remove_hook(&second); remove_hook(&second); /* idempotent removal */
    fixture_destroy(&fixture);
    printf("PASS basic: XOL, entry arguments, update, hidden page hash, same-page removal; trace_name=%s\n",
           request.name);
}
static void all_registers_test(void)
{
    struct fixture fixture;
    struct wu_request request;
    uint64_t captured[32] = {0};
    void *stack = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned int i;
    CHECK(stack != MAP_FAILED); fixture_create(&fixture, 1, 1);
    request = request_for(&fixture, 0x200); request.reg_mask = WU_REG_MASK;
    for (i = 0; i < 32; ++i) request.regs[i] = 0x123400 + i;
    request.regs[9] = (uintptr_t)captured;
    request.regs[30] = (uintptr_t)wu_capture_resume;
    request.regs[31] = (uintptr_t)stack + PAGE;
    install(&request);
    wu_capture_call(fixture.code + 0x200);
    for (i = 0; i < 32; ++i) CHECK(captured[i] == request.regs[i]);
    remove_hook(&request); fixture_destroy(&fixture); CHECK(munmap(stack, PAGE) == 0);
    puts("PASS registers: x0-x30 and changed SP observed by the target before its first instruction");
}
static void simulated_and_live_test(void)
{
    struct fixture fixture;
    struct wu_request branch, adr, live;
    uint32_t before;
    fixture_create(&fixture, 1, 1);
    branch = request_for(&fixture, 0x140); branch.reg_mask = 1; branch.regs[0] = 40;
    install(&branch); CHECK(invoke(&fixture, 0x140, 1, 0, 0) == 41);
    adr = request_for(&fixture, 0x160);
    install(&adr); CHECK(invoke(&fixture, 0x160, 0, 0, 0) == (uintptr_t)fixture.code + 0x170);
    remove_hook(&branch); remove_hook(&adr);
    /* Emulate runtime decryption: MAP_PRIVATE code differs from the file. */
    CHECK(mprotect(fixture.code, PAGE, PROT_READ | PROT_WRITE) == 0);
    *(uint32_t *)(fixture.code + 0x100) = 0xcb010000; /* sub x0,x0,x1 */
    __builtin___clear_cache((char *)fixture.code, (char *)fixture.code + PAGE);
    CHECK(mprotect(fixture.code, PAGE, PROT_READ | PROT_EXEC) == 0);
    before = checksum(fixture.code, PAGE);
    live = request_for(&fixture, 0x100); live.reg_mask = 3; live.regs[0] = 30; live.regs[1] = 5;
    install(&live); CHECK(invoke(&fixture, 0x100, 1, 2, 3) == 26);
    CHECK(checksum(fixture.code, PAGE) == before);
    remove_hook(&live); CHECK(invoke(&fixture, 0x100, 10, 4, 0) == 7);
    fixture_destroy(&fixture);
    puts("PASS instructions: simulated B/ADR, live decrypted opcode differs from on-disk opcode");
}
static void validation_test(void)
{
    struct fixture fixture;
    struct wu_request request;
    struct wu_request *readonly;
    fixture_create(&fixture, 1, 1);
    CHECK(wu_call(PR_WXUPROBE_QUERY, (void *)1) == -1 && errno == EFAULT);
    request = request_for(&fixture, 0x100); request.size--;
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EINVAL);
    request = request_for(&fixture, 0x100); request.instance ^= 1;
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == ESTALE);
    request = request_for(&fixture, 0x101);
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EINVAL);
    request = request_for(&fixture, 0x100); request.offset = 0x100;
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EINVAL);
    request = request_for(&fixture, 0x100); request.reg_mask = 1ULL << 32;
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EINVAL);
    request = request_for(&fixture, 0x100); request.reg_mask = 1ULL << WU_REG_SP; request.regs[31] = 17;
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EINVAL);
    request = request_for(&fixture, 0x100); request.reserved[0] = 1;
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EINVAL);
    readonly = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(readonly != MAP_FAILED);
    request = request_for(&fixture, 0x100); request.reg_mask = 1; request.regs[0] = 90;
    *readonly = request;
    CHECK(mprotect(readonly, PAGE, PROT_READ) == 0);
    CHECK(wu_call(PR_WXUPROBE_INSTALL, readonly) == -1 && errno == EFAULT);
    /* Installation committed before output failed: recover with the cookie. */
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.state == WU_ACTIVE);
    CHECK(invoke(&fixture, 0x100, 3, 4, 0) == 95);
    remove_hook(&request); CHECK(munmap(readonly, PAGE) == 0);
    fixture_destroy(&fixture);
    puts("PASS validation: pointers, ABI, instance, alignment, offsets, masks, lost-output recovery");
}

struct concurrent_context { struct fixture *fixture; uint32_t hash; atomic_int failed; };
static void *execute_thread(void *argument)
{
    struct concurrent_context *ctx = argument;
    unsigned int i;
    for (i = 0; i < 400; ++i)
        if (invoke(ctx->fixture, 0x100, i, i, i) != 31) atomic_store(&ctx->failed, 1);
    return NULL;
}
static void *reader_thread(void *argument)
{
    struct concurrent_context *ctx = argument;
    unsigned int i;
    for (i = 0; i < 200; ++i)
        if (checksum(ctx->fixture->code, PAGE) != ctx->hash) atomic_store(&ctx->failed, 1);
    return NULL;
}
static void concurrency_test(void)
{
    struct fixture fixture;
    struct wu_request request;
    struct concurrent_context ctx;
    pthread_t first, second, reader;
    fixture_create(&fixture, 1, 1);
    ctx.fixture = &fixture; ctx.hash = checksum(fixture.code, PAGE); atomic_init(&ctx.failed, 0);
    request = request_for(&fixture, 0x100); request.reg_mask = 3; request.regs[0] = 10; request.regs[1] = 20;
    install(&request);
    CHECK(pthread_create(&first, NULL, execute_thread, &ctx) == 0);
    CHECK(pthread_create(&second, NULL, execute_thread, &ctx) == 0);
    CHECK(pthread_create(&reader, NULL, reader_thread, &ctx) == 0);
    CHECK(pthread_join(first, NULL) == 0); CHECK(pthread_join(second, NULL) == 0); CHECK(pthread_join(reader, NULL) == 0);
    CHECK(!atomic_load(&ctx.failed));
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.state == WU_ACTIVE && request.hits == 800);
    remove_hook(&request); fixture_destroy(&fixture);
    puts("PASS concurrency: 800 hooked calls and 200 whole-page reads on three threads");
}
static void legacy_test(void)
{
    struct fixture fixture;
    struct trace_init_info init;
    struct uprobe_item_info first, second;
    struct uprobe_reg_info registers = {0};
    char empty[] = "", first_name[] = "legacy_first", second_name[] = "legacy_second";
    char long_name[150];
    uint32_t hash;
    fixture_create(&fixture, 1, 1); hash = checksum(fixture.code, PAGE);
    init.uid = getuid(); init.module_base = (unsigned long)fixture.code;
    init.tfile_name = fixture.path; init.fix_file_name = empty;
    CHECK(trace_init(&init) == SET_TRACE_SUCCESS);
    first.uprobe_offset = PAGE + 0x100; first.fun_offset = 0x100; first.fun_name = first_name;
    second.uprobe_offset = PAGE + 0x120; second.fun_offset = 0x120; second.fun_name = second_name;
    CHECK(set_fun_info(&first) == SET_TRACE_SUCCESS); CHECK(set_fun_info(&second) == SET_TRACE_SUCCESS);
    registers.fun_offset = 0x100; registers.reg_mask = 3; registers.regs[0] = 21; registers.regs[1] = 6;
    CHECK(set_fun_regs(&registers) == SET_TRACE_SUCCESS);
    CHECK(invoke(&fixture, 0x100, 1, 2, 3) == 28);
    CHECK(checksum(fixture.code, PAGE) == hash);
    CHECK(del_fun_info(0x100) == SET_TRACE_SUCCESS);
    CHECK(invoke(&fixture, 0x100, 3, 4, 5) == 8);
    CHECK(invoke(&fixture, 0x120, 1, 2, 3) == 5);
    CHECK(clear_all_uprobes() == SET_TRACE_SUCCESS);
    memset(long_name, 'L', sizeof(long_name) - 1); long_name[sizeof(long_name) - 1] = 0;
    first.fun_name = long_name;
    CHECK(set_fun_info(&first) == SET_TRACE_SUCCESS); /* reinstall + legacy long name */
    CHECK(clear_all_uprobes() == SET_TRACE_SUCCESS);
    fixture_destroy(&fixture);
    puts("PASS Kernel-Trace ABI: trace_init/set_fun_info/clear + set_fun_regs/del_fun_info");
}
static void lifecycle_test(void)
{
    struct fixture fixture;
    struct wu_request request;
    pid_t child;
    int status, memory;
    uint32_t word = 0;
    fixture_create(&fixture, 1, 1);
    request = request_for(&fixture, 0x100); request.reg_mask = 1; request.regs[0] = 90;
    install(&request);
    CHECK(mprotect(fixture.code, PAGE, PROT_READ) == 0);
    CHECK(mprotect(fixture.code, PAGE, PROT_READ | PROT_EXEC) == 0);
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.state == WU_RETIRED);
    CHECK(invoke(&fixture, 0x100, 3, 4, 0) == 8); remove_hook(&request);
    request = request_for(&fixture, 0x100); request.reg_mask = 1; request.regs[0] = 90;
    install(&request);
    child = fork(); CHECK(child >= 0);
    if (!child) _exit(invoke(&fixture, 0x100, 3, 4, 0) == 8 ? 0 : 3);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.state == WU_RETIRED);
    CHECK(invoke(&fixture, 0x100, 3, 4, 0) == 8); remove_hook(&request);
    request = request_for(&fixture, 0x100); install(&request);
    memory = open("/proc/self/mem", O_RDONLY); CHECK(memory >= 0);
    CHECK(pread(memory, &word, sizeof(word), (off_t)(uintptr_t)(fixture.code + 0x100)) == sizeof(word));
    CHECK(word == 0x8b010000); CHECK(close(memory) == 0);
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.state == WU_RETIRED);
    remove_hook(&request);
    request = request_for(&fixture, 0x100); install(&request);
    fixture_destroy(&fixture);
    CHECK(wu_call(PR_WXUPROBE_STATUS, &request) == 0 && request.state == WU_RETIRED);
    remove_hook(&request);
    puts("PASS lifecycle: mprotect, fork, GUP read, unmap retire hooks and restore original mappings");
}
static void remote_test(void)
{
    struct fixture fixture;
    struct wu_request request;
    int input[2], output[2], status;
    uint64_t value;
    char go = 1;
    pid_t child;
    fixture_create(&fixture, 1, 1); CHECK(pipe(input) == 0 && pipe(output) == 0);
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(input[1]); close(output[0]);
        if (read(input[0], &go, 1) != 1) _exit(4);
        value = invoke(&fixture, 0x100, 3, 4, 0);
        if (write(output[1], &value, sizeof(value)) != sizeof(value)) _exit(5);
        if (read(input[0], &go, 1) != 1) _exit(6);
        _exit(0);
    }
    close(input[0]); close(output[1]);
    request = request_for(&fixture, 0x100); request.pid = child;
    request.reg_mask = 3; request.regs[0] = 70; request.regs[1] = 9;
    if (geteuid() == 0) install(&request);
    else CHECK(wu_call(PR_WXUPROBE_INSTALL, &request) == -1 && errno == EPERM);
    CHECK(write(input[1], &go, 1) == 1);
    CHECK(read(output[0], &value, sizeof(value)) == sizeof(value));
    CHECK(value == (geteuid() == 0 ? 80 : 8));
    CHECK(invoke(&fixture, 0x100, 3, 4, 0) == 8); /* another mm remains untouched */
    if (geteuid() == 0) remove_hook(&request);
    CHECK(write(input[1], &go, 1) == 1);
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(input[1]); close(output[0]); fixture_destroy(&fixture);
    puts("PASS process scope: remote root install or unprivileged EPERM; caller mm unchanged");
}
static void mass_test(void)
{
    enum { COUNT = 2000 };
    struct fixture fixture;
    struct wu_request *requests = calloc(COUNT, sizeof(*requests)), extra;
    unsigned int i;
    uint32_t hash;
    CHECK(requests); fixture_create(&fixture, 16, 0); hash = checksum(fixture.code, fixture.size);
    for (i = 0; i < COUNT; ++i) {
        requests[i] = request_for(&fixture, i * 32);
        requests[i].reg_mask = 1; requests[i].regs[0] = 100 + i;
        install(&requests[i]);
    }
    for (i = 0; i < COUNT; ++i) CHECK(invoke(&fixture, i * 32, 1, 0, 0) == 101 + i);
    CHECK(checksum(fixture.code, fixture.size) == hash);
    extra = request_for(&fixture, 4);
    CHECK(wu_call(PR_WXUPROBE_INSTALL, &extra) == -1 && errno == ENOSPC);
    for (i = 0; i < COUNT; i += 2) remove_hook(&requests[i]);
    for (i = 0; i < COUNT; ++i) CHECK(invoke(&fixture, i * 32, 1, 0, 0) == (i % 2 ? 101 + i : 2));
    for (i = 1; i < COUNT; i += 2) remove_hook(&requests[i]);
    CHECK(checksum(fixture.code, fixture.size) == hash);
    free(requests); fixture_destroy(&fixture);
    puts("PASS mass hooks: 2000 entries on 16 pages, page limit, alternating removal, unchanged hash");
}
static void leave_active_test(void)
{
    struct fixture fixture;
    struct wu_request request;
    fixture_create(&fixture, 1, 1); request = request_for(&fixture, 0x100);
    request.reg_mask = 1; request.regs[0] = 80; install(&request);
    CHECK(invoke(&fixture, 0x100, 1, 2, 0) == 83);
    CHECK(close(fixture.fd) == 0 && unlink(fixture.path) == 0);
    puts("PASS exit fixture: intentionally leaving an active consumer for mm-release and QUIESCE");
}

int main(int argc, char **argv)
{
    struct wu_request query;
    wu_request_init(&query);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--expect-unavailable")) {
        CHECK(wu_call(PR_WXUPROBE_QUERY, &query) == -1 && errno == EINVAL);
        puts("PASS module unavailable"); return 0;
    }
    CHECK(wu_call(PR_WXUPROBE_QUERY, &query) == 0);
    CHECK(query.page_size == PAGE && query.capabilities == 0x3f);
    module_instance = query.instance;
    printf("wxuprobe ABI=%u size=%zu instance=0x%llx capabilities=0x%llx uid=%u\n",
           query.version, sizeof(query), module_instance, query.capabilities, getuid());
    if (argc > 1 && !strcmp(argv[1], "--leave-active")) { leave_active_test(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--remote")) { remote_test(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--mass")) { mass_test(); return 0; }
    basic_test();
    if (argc > 1 && !strcmp(argv[1], "--basic")) return 0;
    all_registers_test(); simulated_and_live_test(); validation_test(); concurrency_test();
    legacy_test(); lifecycle_test(); remote_test(); mass_test();
    puts("ALL WXUPROBE TESTS PASSED");
    return 0;
}
