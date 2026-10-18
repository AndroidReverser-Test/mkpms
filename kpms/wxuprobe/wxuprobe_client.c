/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wxuprobe_client.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int number(const char *text, wu_u64 *value)
{
    char *end;
    errno = 0;
    *value = strtoull(text, &end, 0);
    return *text && !*end && !errno;
}
static int register_value(struct wu_request *request, const char *text)
{
    char *end;
    unsigned long index;
    const char *equal = strchr(text, '=');
    wu_u64 value;
    if (!equal || !number(equal + 1, &value)) return 0;
    if (equal - text == 2 && !strncmp(text, "sp", 2)) index = WU_REG_SP;
    else if (text[0] == 'x') {
        index = strtoul(text + 1, &end, 10);
        if (end != equal || end == text + 1 || index > 30) return 0;
    } else return 0;
    request->reg_mask |= 1ULL << index; request->regs[index] = value;
    return 1;
}
static int usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s query|install|update|status|remove|clear|quiesce [options]\n"
        "  -p PID          target process (default self; remote needs root)\n"
        "  -a ADDRESS      runtime function address\n"
        "  -c COOKIE       caller chosen nonzero cookie (install/status/update/remove)\n"
        "  -o FILE_OFFSET  verify computed uprobe offset (default automatic)\n"
        "  -r xN=VALUE     set x0..x30 or sp; repeat for multiple registers\n"
        "  -n NAME         trace label, at most 63 bytes\n"
        "  --trace         emit +Test-Log+ records to tracefs\n"
        "  --instance ID   require this specific loaded module instance\n"
        "update replaces the whole register mask; omit -r to clear edits.\n", program);
    return 2;
}
int main(int argc, char **argv)
{
    struct wu_request request, query;
    int command, i;
    wu_u64 value, expected_instance = 0;
    if (argc < 2) return usage(argv[0]);
    if (!strcmp(argv[1], "query")) command = PR_WXUPROBE_QUERY;
    else if (!strcmp(argv[1], "install")) command = PR_WXUPROBE_INSTALL;
    else if (!strcmp(argv[1], "update")) command = PR_WXUPROBE_UPDATE;
    else if (!strcmp(argv[1], "status")) command = PR_WXUPROBE_STATUS;
    else if (!strcmp(argv[1], "remove")) command = PR_WXUPROBE_REMOVE;
    else if (!strcmp(argv[1], "clear")) command = PR_WXUPROBE_CLEAR;
    else if (!strcmp(argv[1], "quiesce")) command = PR_WXUPROBE_QUIESCE;
    else return usage(argv[0]);
    wu_request_init(&request);
    for (i = 2; i < argc; ++i) {
        const char *option = argv[i];
        if (!strcmp(option, "--trace")) { request.flags |= WU_F_TRACE; continue; }
        if (++i == argc) return usage(argv[0]);
        if (!strcmp(option, "-r")) { if (!register_value(&request, argv[i])) return usage(argv[0]); }
        else if (!strcmp(option, "-n")) {
            if (strlen(argv[i]) >= sizeof(request.name)) return usage(argv[0]);
            strcpy(request.name, argv[i]);
        } else {
            if (!number(argv[i], &value)) return usage(argv[0]);
            if (!strcmp(option, "-a")) request.address = value;
            else if (!strcmp(option, "-c")) request.cookie = value;
            else if (!strcmp(option, "-o")) request.offset = value;
            else if (!strcmp(option, "--instance")) expected_instance = value;
            else if (!strcmp(option, "-p") && value <= 0x7fffffffULL) request.pid = (wu_s32)value;
            else return usage(argv[0]);
        }
    }
    wu_request_init(&query); query.pid = request.pid;
    if (wu_call(PR_WXUPROBE_QUERY, &query)) { perror("wxuprobe query"); return 1; }
    request.instance = expected_instance ? expected_instance : query.instance;
    if (wu_call(command, &request)) { perror("wxuprobe command"); return 1; }
    printf("instance=0x%llx capabilities=0x%llx page=%u active=%u "
           "cookie=0x%llx address=0x%llx offset=0x%llx state=%u hits=%llu error=%d\n",
           request.instance, request.capabilities, request.page_size, request.active_count,
           request.cookie, request.address, request.offset, request.state, request.hits, request.last_error);
    return 0;
}
