#include "WxShadowInternal.h"
#include <cerrno>
#include <cstring>

namespace dobby {
namespace {
struct Emitter {
  uint8_t *code; size_t size = 0;
  void word(uint32_t value) { memcpy(code + size, &value, 4); size += 4; }
  void mov(unsigned reg, uint64_t value) {
    word(0xd2800000 | ((value & 0xffff) << 5) | reg);
    for (unsigned i = 1; i != 4; ++i)
      word(0xf2800000 | (i << 21) | (((value >> (i * 16)) & 0xffff) << 5) | reg);
  }
  void jump(uintptr_t at, uintptr_t to) {
    uint32_t b;
    if (wx_a64_branch(at, to, &b)) word(b);
    else { mov(17, to); word(0xd61f0220); } // x17 is an ABI scratch register.
  }
};
int64_t sign_extend(uint32_t value, unsigned bits) {
  return (int64_t(value) ^ (int64_t(1) << (bits - 1))) - (int64_t(1) << (bits - 1));
}
}
int WxBuildOriginal(uint32_t insn, uintptr_t source, uintptr_t output, uint8_t *code, size_t *length) {
  if (wx_a64_special_entry(insn)) return -ENOTSUP;
  if ((insn & 0xff000000) == 0x54000000 && ((insn & 0x10) || (insn & 15) >= 14)) return -ENOTSUP;
  Emitter e{code};
  if ((insn & 0x7c000000) == 0x14000000) {
    // A BL at the entry changes the observable LR; defer that uncommon entry.
    if (insn & 0x80000000) return -ENOTSUP;
    uintptr_t destination = source + sign_extend(insn & 0x3ffffff, 26) * 4;
    if (destination == source) destination = output;
    e.jump(output, destination);
  } else if ((insn & 0x1f000000) == 0x10000000) {
    int64_t delta = sign_extend(((insn >> 5) & 0x7ffff) << 2 | ((insn >> 29) & 3), 21);
    uintptr_t destination = (insn & 0x80000000) ? (source & ~uintptr_t(4095)) + delta * 4096 : source + delta;
    e.mov(insn & 31, destination);
  } else if ((insn & 0x3b000000) == 0x18000000) {
    // Literal loads from the protected source page violate the page contract.
    uintptr_t destination = source + sign_extend((insn >> 5) & 0x7ffff, 19) * 4;
    if ((destination & ~uintptr_t(4095)) == (source & ~uintptr_t(4095))) return -ENOTSUP;
    unsigned opc = insn >> 30, rt = insn & 31;
    uint32_t load;
    if (insn & (1U << 26)) {
      if (opc == 3) return -ENOTSUP;
      load = opc == 0 ? 0xbd400000 : opc == 1 ? 0xfd400000 : 0x3dc00000;
    } else {
      load = opc == 0 ? 0xb9400000 : opc == 1 ? 0xf9400000 : opc == 2 ? 0xb9800000 : 0xf9800000;
    }
    e.mov(17, destination); e.word(load | (17 << 5) | rt);
  } else if ((insn & 0xff000010) == 0x54000000 ||
             (insn & 0x7e000000) == 0x34000000 ||
             (insn & 0x7e000000) == 0x36000000) {
    bool condition = (insn & 0xff000010) == 0x54000000;
    bool test = (insn & 0x7e000000) == 0x36000000;
    unsigned bits = test ? 14 : 19;
    uint32_t mask = ((1U << bits) - 1) << 5;
    uintptr_t destination = source + sign_extend((insn & mask) >> 5, bits) * 4;
    if (destination == source) destination = output;
    uint32_t direct;
    unsigned jump_size = wx_a64_branch(output + 4, destination, &direct) ? 4 : 20;
    uint32_t inverted = insn ^ (condition ? 1U : 1U << 24);
    e.word((inverted & ~mask) | (((4 + jump_size) / 4) << 5));
    e.jump(output + e.size, destination);
  } else {
    // All remaining base A64 instructions are position independent. Reject
    // exception-generating and reserved instructions, rather than executing
    // them from a different PC with different signal semantics.
    if (!insn || (insn & 0xff000000) == 0xd4000000) return -ENOTSUP;
    e.word(insn);
  }
  uint32_t back;
  if (!wx_a64_branch(output + e.size, source + 4, &back)) return -ERANGE;
  e.word(back);
  *length = e.size;
  return 0;
}

int WxBuildRouting(Interceptor::Entry *entry) {
  void *original = WxAllocateNear(entry->addr);
  if (!original) return -ENOMEM;
  entry->helper_mappings.push_back(original);
  uint8_t code[128]{};
  size_t length = 0;
  int rc = WxBuildOriginal(entry->wx_original, entry->addr, (uintptr_t)original, code, &length);
  if (rc) return rc;
  rc = WxPublish(original, code, length);
  if (rc) return rc;
  entry->patched = MemBlock(entry->addr, 4);
  entry->relocated = MemBlock((addr_t)original, length);
  if (wx_a64_branch(entry->addr, entry->fake_func_addr, &entry->wx_branch)) return 0;
  void *relay = WxAllocateNear(entry->addr);
  if (!relay) return -ENOMEM;
  entry->helper_mappings.push_back(relay);
  // This literal lives in an independently owned readable RX page.
  uint32_t instructions[] = {0x58000051, 0xd61f0220};
  memcpy(code, instructions, 8);
  uint64_t destination = entry->fake_func_addr;
  memcpy(code + 8, &destination, 8);
  rc = WxPublish(relay, code, 16);
  if (rc) return rc;
  return wx_a64_branch(entry->addr, (uintptr_t)relay, &entry->wx_branch) ? 0 : -ERANGE;
}
}
