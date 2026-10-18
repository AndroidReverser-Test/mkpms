/* SPDX-License-Identifier: MIT */
#ifndef WXSHADOW_UAPI_H
#define WXSHADOW_UAPI_H

/* No libc/kernel types: the wire layout is identical in C, C++ and KPMs. */
typedef unsigned int wx_u32;
typedef unsigned long long wx_u64;
#define WXSHADOW_ABI_VERSION 1U
#define WXSHADOW_ARCH_AARCH64 1U
#define WXSHADOW_PAGE_BYTES 4096U
#define PR_WXSHADOW_QUERY       0x57580100
#define PR_WXSHADOW_PATCH_EX    0x57580101
#define PR_WXSHADOW_RELEASE_EX  0x57580102
#define PR_WXSHADOW_QUERY_PATCH 0x57580103
#define PR_WXSHADOW_QUIESCE     0x57580104

#define WX_CAP_FAULT       (1ULL << 0)
#define WX_CAP_TLB         (1ULL << 1)
#define WX_CAP_COOKIE      (1ULL << 2)
#define WX_CAP_TRANSACTION (1ULL << 3)
#define WX_CAP_VM_NOTIFY   (1ULL << 4)
#define WX_CAP_GUP         (1ULL << 5)
#define WX_CAP_FORK        (1ULL << 6)
#define WX_CAP_REQUIRED (WX_CAP_FAULT | WX_CAP_TLB | WX_CAP_COOKIE | WX_CAP_TRANSACTION | WX_CAP_VM_NOTIFY)

enum wx_patch_state { WX_PATCH_ABSENT = 0, WX_PATCH_ACTIVE = 1,
                      WX_PATCH_RETIRED = 2, WX_PATCH_CONFLICT = 3 };
/* All commands: prctl(command, request_pointer, 0UL, 0UL, 0UL).
 * Only 64-bit self-mm callers. Unknown flags/reserved fields are errors.
 * PATCH_EX atomically captures a generation when generation==0 (first patch).
 * Existing-page installs must supply the generation returned by QUERY_PATCH.
 */
struct wx_request_header {
  wx_u32 version, size, flags, reserved;
};
struct wx_query {
  struct wx_request_header header;
  wx_u32 arch, page_size;
  wx_u64 instance, capabilities;
  wx_u64 reserved[2];
};
struct wx_patch_request {
  struct wx_request_header header;
  wx_u64 instance, cookie, address, generation;
  wx_u32 expected, replacement; /* one aligned A64 instruction, inline data */
  wx_u32 state, reserved;      /* output state; ignored on retry */
};
/* RELEASE_EX / QUERY_PATCH use the same identity and output structure. */
struct wx_identity {
  struct wx_request_header header;
  wx_u64 instance, cookie, address, generation;
  wx_u32 state, reserved;
};

#if defined(__cplusplus)
static_assert(sizeof(wx_query) == 56 && sizeof(wx_patch_request) == 64 && sizeof(wx_identity) == 56, "wxshadow ABI layout");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct wx_query) == 56 && sizeof(struct wx_patch_request) == 64 && sizeof(struct wx_identity) == 56, "wxshadow ABI layout");
#endif
#endif
