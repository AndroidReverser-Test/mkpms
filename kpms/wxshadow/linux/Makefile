# Exact target kernel build tree required. This is a KPM object, not an LKM.
obj-m := wxshadow_linux_adapter.o
ccflags-y += -I$(src)/../../../shared -fno-stack-protector -fno-builtin -mno-outline-atomics -ffixed-x18
KASAN_SANITIZE := n
KCSAN_SANITIZE := n
UBSAN_SANITIZE := n
KCOV_INSTRUMENT := n
GCOV_PROFILE := n
# KPM relocates native ELF objects, not LLVM bitcode or Linux CFI/SCS runtime
# metadata. Keep autoconf/struct layouts intact and remove instrumentation only
# for this translation unit. KernelPatch supplies its existing KPM callback CFI
# handling; this adapter does not patch the kernel's CFI machinery.
CFLAGS_REMOVE_wxshadow_linux_adapter.o += -pg $(CC_FLAGS_LTO) $(CC_FLAGS_CFI) $(CC_FLAGS_SCS)
