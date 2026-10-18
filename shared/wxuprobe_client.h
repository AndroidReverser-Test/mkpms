/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WXUPROBE_CLIENT_H
#define WXUPROBE_CLIENT_H
#include "wxuprobe_uapi.h"
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline void wu_request_init(struct wu_request *request)
{
    memset(request, 0, sizeof(*request));
    request->version = WXUPROBE_ABI_VERSION;
    request->size = sizeof(*request);
    request->offset = WU_OFFSET_AUTO;
}

/* Returns 0 or -1 with errno, like prctl. A failed output copy may follow a
 * committed install: query STATUS with the same instance/cookie/address. */
static inline int wu_call(int command, struct wu_request *request)
{
    return (int)syscall(SYS_prctl, command, request, 0UL, 0UL, 0UL);
}
#endif
