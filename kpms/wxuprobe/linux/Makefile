# This produces a relocatable KPM object, not a Linux loadable module.
obj-m := wxuprobe_linux.o
ccflags-y += -I$(src)/../../../shared -fno-stack-protector -fno-builtin -mno-outline-atomics -ffixed-x18
KASAN_SANITIZE := n
KCSAN_SANITIZE := n
UBSAN_SANITIZE := n
KCOV_INSTRUMENT := n
GCOV_PROFILE := n
CFLAGS_REMOVE_wxuprobe_linux.o += -pg $(CC_FLAGS_LTO) $(CC_FLAGS_CFI) $(CC_FLAGS_SCS)
