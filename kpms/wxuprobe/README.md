# wxuprobe：读执行分离的 Linux uprobe KPM

独立模块 `wxuprobe.kpm`，结合本项目的 wxshadow 页面机制和 Kernel-Trace 使用的 Linux uprobe。保留内核的指令解码、模拟及 XOL 单步，在函数首条原指令执行前修改 **x0–x30 和 SP**。执行页包含 `BRK #5`，用户态直接读取目标页时得到安装前的内容。

- [两个项目的实现、集成设计和实机验证](../../docs/kernel-trace-wxuprobe.md)
- 用户接口：[原生 ABI](../../shared/wxuprobe_uapi.h)、[C/C++ 客户端](../../shared/wxuprobe_client.h)、[Kernel-Trace 兼容头](../../shared/wxuprobe_kernel_trace.h)
- [设备测试及自动清理脚本](../../tests/wxuprobe/run-device-tests.ps1)

## 支持范围

当前版本实际验证：Pixel 6 / Android 16 / ARM64 / 4KB 页，内核
`6.1.134-android14-11-g66e758f7d0c0-ab13748739`，使用 5.10 基准对象构建；完整脚本、tracefs 及安全卸载均通过。[跨内核兼容与验证](../../docs/kernel-compatibility.md)。旧版 Android 12 / 5.10.66 的记录保留在原验证文档。

模块支持审查过的 **Linux 5.10–6.6** VM/uprobe 接口。目标内核提供内嵌 BTF 时，同一个 KPM 在加载时自动读取目标结构布局、初始化对应尺寸的 mutex/notifier/consumer，并识别 `get_user_pages_remote` 的 6/7 参数版本；用户不需要配置偏移或内核版本。内核函数原型、字段宽度和 KernelPatch 探测的 `mm.pgd` 偏移不符合要求时拒绝加载，不通过猜测偏移继续运行。

LTO 将 `__copy_insn` 或 `dup_mmap` 内联时，自动使用 Linux 保留的架构解码入口和 uprobe fork 开始/结束入口。解码入口只在 BTF 证明该探针尚未解码、inode/偏移正确且仅属于本模块时替换原指令，随后仍由 Linux 解码。此路径要求文件页缓存读取成功，且磁盘入口不是 uprobe BRK；内存已解密而磁盘仍不可读或是 BRK 的入口会被拒绝。

要求 `CONFIG_UPROBES`、`CONFIG_MMU_NOTIFIER`、ARM64 4KB 页，无 PREEMPT_RT 或 `CONFIG_DEBUG_LOCK_ALLOC`，VA/PA 不超过 48 位。缺少 BTF 时保留精确内核模式，仍须使用目标源码和完整配置编译，且 release 与 `mm.pgd` 偏移必须匹配。BTF 存在但校验失败时不会降级到精确内核模式。版本范围表示接口适配范围；实际设备验证范围以[验证记录](../../docs/kernel-trace-wxuprobe.md#5-实机验证)为准。

目标必须是私有、可读可执行、不可写的普通文件映射，入口按 A64 的 4 字节对齐。文件偏移由 VMA 自动计算，支持同页多个入口。运行时解密后私有页与文件内容不同时，使用安装时的内存原指令。

限制：

- **同页自读代码不受支持**：一条指令需要同时从被保护页取指和读取该页数据时，读执行切换无法完成。整页内其他函数也受此限制。
- 不支持匿名/JIT、自修改、共享、可写、特殊、大页、原生 XOM，以及 BTI/PAC 入口；其余指令还须通过 Linux uprobe 的指令检查。
- 不修改 PC、PSTATE、FP/SIMD 寄存器，不提供返回探针。修改值先于原指令生效；原指令仍可覆盖这些值。SP 必须非零、16 字节对齐且位于用户地址范围，实际栈空间由调用方保证。
- 安装和撤销前，调用方须停止整个目标页的执行与读取；模块不自动暂停进程。UPDATE 串行替换整份寄存器配置。
- mprotect、unmap、fork、写访问或 GUP 访问会让相关页的 hook 退役并恢复原映射；没有自动重新挂载。fork 同时退役父进程的相关 hook，子进程得到干净页。
- 每个 `inode + file_offset` 在注册期间独占，不能与其他 uprobe 消费者共用；受保护页上的外部 uprobe 写入也会被拒绝。同一进程内的文件别名映射可能使安装被拒绝。
- 不要同时加载原 `kernel_trace`，两者使用相同的兼容 mincore 命令。本模块独立运行，测试脚本也要求先卸载 `wxshadow`。
- 此机制只解决目标代码页读取与执行的差异。Linux 的 uprobe/XOL 等状态依然存在，不表示任意检测都不可见。

## 构建

### 1. 用 Kbuild 编译 VM/uprobe 实现

在 Linux/WSL 的仓库根目录执行。可用 Linux 5.10–6.6 的已准备源码和配置编译基准对象；有 BTF 的目标由运行时适配，无需为每个目标重新构建对象。没有 BTF 的目标仍要求源码和配置精确匹配。内核准备方式可参考 [现有 VM 构建说明](../../docs/dobby-wxshadow-usage.md#31-准备与设备完全匹配的内核树)，不要修改 release 冒充精确匹配。

```sh
WU_REPO="$PWD"
WU_KERNEL=/absolute/path/to/prepared-target-kernel
WU_SUFFIX=-android12-9-00001-gd262b276948e-ab8291727
# PATH 中放入对应 Android Clang；本次使用 clang-r498229b。
make -C "$WU_KERNEL" M="$WU_REPO/kpms/wxuprobe/linux" \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LLVM=1 LLVM_IAS=1 LOCALVERSION="$WU_SUFFIX" wxuprobe_linux.o
llvm-nm -u kpms/wxuprobe/linux/wxuprobe_linux.o
```

得到 `kpms/wxuprobe/linux/wxuprobe_linux.o`，本次只有 `memcpy` 未解析，由 KPM 内的 runtime 提供。Makefile 仅对本对象禁用 LTO/CFI/SCS 和诊断插桩，保留内核配置对应的结构布局。不生成或加载 Linux `.ko`。

### 2. 链接新 KPM

本机 PowerShell 构建命令，工具路径按实际环境修改：

```powershell
& 'D:/androidSDK/cmake/3.22.1/bin/cmake.exe' -S . -B build/wxuprobe-kpm -G Ninja `
  '-DCMAKE_C_COMPILER=D:/arm_gnu/14.2 rel1/bin/aarch64-none-elf-gcc.exe' `
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY `
  '-DCMAKE_MAKE_PROGRAM=D:/androidSDK/cmake/3.22.1/bin/ninja.exe' `
  "-DWXUPROBE_LINUX_ADAPTER_OBJECT=$PWD/kpms/wxuprobe/linux/wxuprobe_linux.o"
& 'D:/androidSDK/cmake/3.22.1/bin/cmake.exe' --build build/wxuprobe-kpm --target wxuprobe.kpm -j6
```

产物：`build/wxuprobe-kpm/kpms/wxuprobe/wxuprobe.kpm`。未指定适配对象时不生成此模块。没有修改 `.kp` 框架。

### 3. 编译 Android 客户端和设备测试

```powershell
& 'D:/androidSDK/cmake/3.22.1/bin/cmake.exe' -S tests/wxuprobe -B build/wxuprobe-device -G Ninja `
  '-DCMAKE_TOOLCHAIN_FILE=D:/androidSDK/ndk/25.1.8937393/build/cmake/android.toolchain.cmake' `
  '-DCMAKE_MAKE_PROGRAM=D:/androidSDK/cmake/3.22.1/bin/ninja.exe' `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
& 'D:/androidSDK/cmake/3.22.1/bin/cmake.exe' --build build/wxuprobe-device -j6
```

得到 `wxuprobe_client`、`wxuprobe_device_tests`。这两个程序动态链接系统 Bionic，无额外共享库。

## 自动实机测试

在当前 PowerShell 会话中设置 `KP_SUPERKEY`，然后执行：

```powershell
./tests/wxuprobe/run-device-tests.ps1 -Serial <adb-serial>
```

脚本从环境读取密钥，也支持 `-SuperKey`，不把密钥写入文件。它使用 `/data/local/tmp/kpatch`，要求普通 ADB shell 的 uid 为 2000，并拒绝接管已加载的 `wxuprobe`、`wxshadow` 或 `kernel_trace`。

流程：未加载检查 → 部署/加载 → shell 自进程 hook 和跨进程权限检查 → root 完整测试（含 2000 个 hook）→ 带活动消费者的进程退出 → 检查本次 tracefs 记录 → QUIESCE → 卸载 → 再次检查模块不可用。

脚本读取 trace 快照，保留原有内容和 `tracing_on` 状态。日志保存在 `build/wxuprobe-device/logs/`：`device.log`、`trace.log`、`dmesg.log`。即使测试失败也会尝试清理；**QUIESCE 失败时保留模块，不继续卸载**。

## CLI 使用

推送文件并为客户端设置执行权限：

```sh
adb shell mkdir -p /data/local/tmp/wxuprobe-test
adb push build/wxuprobe-kpm/kpms/wxuprobe/wxuprobe.kpm /data/local/tmp/wxuprobe-test/
adb push build/wxuprobe-device/wxuprobe_client /data/local/tmp/wxuprobe-test/
adb shell chmod 755 /data/local/tmp/wxuprobe-test/wxuprobe_client
```

用 `kpatch <superkey> kpm load /data/local/tmp/wxuprobe-test/wxuprobe.kpm` 加载。以下命令在设备 root shell 内执行；PID、地址和 cookie 替换为实际值，并协调目标页访问：

```sh
cd /data/local/tmp/wxuprobe-test
./wxuprobe_client query
./wxuprobe_client install -p 1234 -a 0x7000011000 -c 1 -r x0=10 -r x1=20 -n target --trace
./wxuprobe_client status  -p 1234 -c 1
./wxuprobe_client update  -p 1234 -c 1 -r x0=100
./wxuprobe_client update  -p 1234 -c 1
./wxuprobe_client remove  -p 1234 -c 1
```

第二次 UPDATE 没有 `-r`，清除全部寄存器修改。UPDATE 也替换 trace 开关：需要继续记录时加 `--trace`。`-o` 可显式核对文件偏移，默认自动计算。`--instance` 可绑定特定模块实例。

普通进程通过库接口操作自己的 `mm`；指定其他进程需要 `CAP_SYS_ADMIN`。独立 CLI 的 `-p 0` 表示 CLI 自己的进程，不是被调试应用。

trace 输出在 `/sys/kernel/tracing/trace` 或 `trace_pipe`，需要开启 `tracing_on`。例如：

```text
+Test-Log+ wxuprobe fun_name:target,offset:0x1100,pc:0x...,x0:0xa
```

## C/C++ 原生接口

将 `shared/` 加入 include 路径，包含 `wxuprobe_client.h`：

```c
int install_entry(void *target, struct wu_request *handle)
{
    wu_request_init(handle);
    if (wu_call(PR_WXUPROBE_QUERY, handle)) return -1;
    handle->cookie = 1; /* 在目标 mm 内唯一，重试同一安装时保持不变。 */
    handle->address = (unsigned long)target;
    handle->reg_mask = (1ULL << 0) | (1ULL << 1);
    handle->regs[0] = 10;
    handle->regs[1] = 20;
    return wu_call(PR_WXUPROBE_INSTALL, handle);
}
/* 后续对同一 handle 调用 STATUS、UPDATE 或 REMOVE。 */
```

固定 ARM64 ABI 大小为 440 字节；所有保留字段须为 0，命令通过 `prctl(command, request, 0, 0, 0)` 传递，返回 0 或 `-1/errno`。

| 命令 | 行为 |
|---|---|
| QUERY | 返回本次加载的随机 instance、能力位、页大小和目标 mm 活动数 |
| INSTALL | 安装；相同 cookie 和配置可幂等重试，冲突返回 EEXIST |
| UPDATE | 按 cookie 替换寄存器 mask/值及 flags，仅接受 ACTIVE |
| STATUS | 返回状态、命中数、配置和 last_error；未知 cookie 返回 ABSENT |
| REMOVE | 精确撤销，重复撤销成功；未知 cookie 返回 ABSENT |
| CLEAR | 撤销目标 mm 的全部登记，包括兼容接口创建的 hook |
| QUIESCE | 管理员清理所有 mm；此后仅允许 QUERY 和重复 QUIESCE，其他管理命令返回 ESHUTDOWN |

状态：ABSENT=0、INSTALLING=1、ACTIVE=2、RETIRED=3、REMOVED=4。位 0–30 对应 x0–x30，位 31 对应 SP。

请求复制支持缺页、fork COW 和 Android 带标签的堆指针，并遵守读写权限。**EFAULT 可能发生在安装已提交后的结果复制阶段**：保持原 instance/cookie，使用另一块可写请求内存调用 STATUS，确认状态再重试或撤销。相同 cookie 的记录保留到 QUIESCE；REMOVED/RETIRED 的 cookie 不能用于一次新安装。目标进程已退出时 STATUS 可返回 ESRCH，使用管理员 QUIESCE 清理保留登记。

## Kernel-Trace 接入

现有 ARM64 `trace_init` / `set_fun_info` / `clear_all_uprobes` 二进制接口保持兼容。应用使用新增的 `wxuprobe_kernel_trace.h` 替换原 `uprobe_trace_user.h`，不要同时包含二者：

```c
struct trace_init_info init = {
    .uid = getuid(), .module_base = load_bias,
    .tfile_name = so_path, .fix_file_name = NULL
};
struct uprobe_item_info item = {
    .uprobe_offset = file_offset, .fun_offset = target_address - load_bias,
    .fun_name = "target"
};
if (trace_init(&init) != SET_TRACE_SUCCESS) return -1;
if (set_fun_info(&item) != SET_TRACE_SUCCESS) return -1;
struct uprobe_reg_info edit = {0};
edit.fun_offset = item.fun_offset;
edit.reg_mask = (1ULL << 0) | (1ULL << 1);
edit.regs[0] = 10; edit.regs[1] = 20;
if (set_fun_regs(&edit) != SET_TRACE_SUCCESS) return -1;
/* del_fun_info(item.fun_offset) 单独删除；clear_all_uprobes() 全部撤销。 */
```

兼容操作通过 `mincore(0, 511 + op, pointer)` 传递：0 初始化、1 添加、2 清除、3 修改寄存器、4 单独删除；返回原有的 1000/1001。初始化只接受当前调用者 UID，并绑定当前 `mm`；没有跨进程全局 UID 配置。`trace_init()` 会先清除该 mm 的全部 hook。

`uprobe_offset` 是 **文件偏移**，不等于 `fun_offset`：

```text
file_offset = function_address - vma_start + maps_file_offset
fun_offset  = function_address - module_base
```

模块会核对文件 inode 和偏移。`fix_file_name` 非空时只校验字符串可读性；无需提供替代文件，直接使用目标映射的原指令。兼容名称最多接受 149 字节，trace 标签截为 63 字节。兼容 CLEAR 会同时清除该 mm 的原生 API hook。

## 资源与卸载

上限：64 个保留的 mm、1024 个保留源页、8192 条保留记录、每个活动页 128 个 hook。安装时受 8192 个保留影子页预算约束，撤销路径允许额外分配以完成清理。删除和退役不立即释放旧影子页或 cookie；它们在 QUIESCE 时统一回收，避免并发执行和失败重试引用已释放对象。长期反复注册须规划实例清理。

卸载顺序：停止所有客户端/目标页访问 → 撤销 hook 或让目标进程退出 → 在普通 root 进程执行 `wxuprobe_client quiesce` → 确认成功 → `kpatch <superkey> kpm unload wxuprobe`。

**必须先 QUIESCE。** 当前 KernelPatch 装载器在 RCU 临界区调用 exit，并忽略 exit 返回值而释放模块，无法用 exit 错误阻止误卸载。KPM ctl0 只输出状态，不能代替会休眠的清理。绕过 QUIESCE 会留下消费者或 notifier 回调指向已释放代码，不属于受支持的卸载方式。
