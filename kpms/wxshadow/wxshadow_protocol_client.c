/* SPDX-License-Identifier: MIT */
#include "wxshadow_uapi.h"
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <errno.h>

int main(int argc, char **argv)
{
    struct wx_query query = { .header = {WXSHADOW_ABI_VERSION, sizeof(query), 0, 0} };
    if (prctl(PR_WXSHADOW_QUERY, (unsigned long)&query, 0UL, 0UL, 0UL)) {
        perror("wxshadow QUERY"); return 1;
    }
    printf("abi=%u arch=%u page=%u instance=%llx capabilities=%llx required=%llx\n",
           query.header.version, query.arch, query.page_size, query.instance,
           query.capabilities, WX_CAP_REQUIRED);
    if (argc == 2 && !strcmp(argv[1], "--require")) {
        if (query.arch != WXSHADOW_ARCH_AARCH64 || query.page_size != WXSHADOW_PAGE_BYTES ||
            !query.instance || (query.capabilities & WX_CAP_REQUIRED) != WX_CAP_REQUIRED) {
            fputs("The transactional ARM64 VM backend is unavailable.\n", stderr); return 1;
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "quiesce")) {
        struct wx_identity id = { .header = {WXSHADOW_ABI_VERSION, sizeof(id), 0, 0}, .instance = query.instance };
        if (prctl(PR_WXSHADOW_QUIESCE, (unsigned long)&id, 0UL, 0UL, 0UL)) {
            perror("wxshadow QUIESCE (destroy hooks in every client first; CAP_SYS_ADMIN required)"); return 1;
        }
        puts("VM notifiers drained. The KPM can now be unloaded.");
    } else if (argc > 1) {
        fprintf(stderr, "usage: %s [--require | quiesce]\n", argv[0]); return 2;
    }
    return 0;
}
