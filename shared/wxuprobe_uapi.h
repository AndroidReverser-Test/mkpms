/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WXUPROBE_UAPI_H
#define WXUPROBE_UAPI_H

/* Fixed width ABI shared by KPM, Android clients and the native VM object. */
typedef unsigned int wu_u32;
typedef int wu_s32;
typedef unsigned long long wu_u64;

#define WXUPROBE_ABI_VERSION 1U
#define PR_WXUPROBE_QUERY    0x57550001
#define PR_WXUPROBE_INSTALL  0x57550002
#define PR_WXUPROBE_UPDATE   0x57550003
#define PR_WXUPROBE_REMOVE   0x57550004
#define PR_WXUPROBE_STATUS   0x57550005
#define PR_WXUPROBE_CLEAR    0x57550006
#define PR_WXUPROBE_QUIESCE  0x57550007
#define WU_OFFSET_AUTO (~0ULL)
#define WU_F_TRACE 1U
#define WU_REG_SP 31U
#define WU_REG_MASK 0xffffffffULL
#define WU_NAME_LEN 64U
#define WU_CAP_UPROBE    (1ULL << 0)
#define WU_CAP_SHADOW    (1ULL << 1)
#define WU_CAP_REGS      (1ULL << 2)
#define WU_CAP_LIVE_INSN (1ULL << 3)
#define WU_CAP_LEGACY    (1ULL << 4)
#define WU_CAP_REMOTE    (1ULL << 5)

enum wu_state {
    WU_ABSENT = 0,
    WU_INSTALLING = 1,
    WU_ACTIVE = 2,
    WU_RETIRED = 3,
    WU_REMOVED = 4,
};

struct wu_request {
    wu_u32 version;
    wu_u32 size;
    wu_u64 instance;
    wu_u64 cookie;        /* Client chosen, unique within target mm. */
    wu_u64 address;       /* Runtime function address, A64 aligned. */
    wu_u64 offset;        /* File offset, or WU_OFFSET_AUTO on install. */
    wu_u64 reg_mask;      /* Bits 0..30: x0..x30; bit 31: SP. */
    wu_u64 regs[32];
    wu_u64 hits;
    wu_u64 capabilities;
    wu_s32 pid;           /* 0: caller; another process requires CAP_SYS_ADMIN. */
    wu_u32 flags;
    wu_u32 state;
    wu_u32 page_size;
    wu_u32 active_count;
    wu_s32 last_error;
    char name[WU_NAME_LEN];
    wu_u64 reserved[4];
};

#if defined(__cplusplus)
static_assert(sizeof(wu_request) == 440, "wxuprobe requires the ARM64 ABI layout");
#else
_Static_assert(sizeof(struct wu_request) == 440, "wxuprobe requires the ARM64 ABI layout");
#endif

#endif
