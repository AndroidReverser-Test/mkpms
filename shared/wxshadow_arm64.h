/* SPDX-License-Identifier: MIT */
#ifndef WXSHADOW_ARM64_H
#define WXSHADOW_ARM64_H
#include "wxshadow_uapi.h"

static inline int wx_a64_branch(wx_u64 from, wx_u64 to, wx_u32 *out) {
  wx_u64 distance;
  if ((from | to) & 3) return 0;
  if (to >= from) {
    distance = to - from;
    if (distance > 134217724ULL) return 0;
  } else {
    distance = from - to;
    if (distance > 134217728ULL) return 0;
    distance = 0ULL - distance;
  }
  *out = 0x14000000U | ((wx_u32)(distance >> 2) & 0x03ffffffU);
  return 1;
}

static inline int wx_a64_special_entry(wx_u32 insn) {
  /* BTI c/j/jc, PACIASP/PACIBSP, PACIA1716/PACIB1716, authenticated branches. */
  return (insn & 0xffffff1fU) == 0xd503241fU ||
         (insn & 0xffffff00U) == 0xd5032300U ||
         (insn & 0xffffff00U) == 0xd5032100U ||
         insn == 0xd50320ffU ||
         (insn & 0xfe000000U) == 0xd6000000U;
}
#endif
