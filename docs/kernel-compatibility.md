# Dobby / wxuprobe 跨内核兼容

更新：2026-10-19。本文取代旧文档中“必须针对目标 Linux 5.10 完整配置编译”的限制；旧设备记录仍保留为历史记录。

## 自动适配方式

用户态 Dobby API、wxuprobe ABI 和 Kernel-Trace 兼容 ABI 不变。基准内核只用于编译一次适配对象；目标提供内嵌 BTF 时，加载同一个 KPM 即可自动适配，无需填写内核版本、结构偏移或设备配置，也无需把 BTF 文件推到设备。

| 差异 | 当前处理 |
|---|---|
| `mm_struct`、VMA、task、file/inode 等布局 | 读取运行内核的 `__start_BTF` / `__stop_BTF`，验证字段宽度及边界后保存偏移 |
| mutex、notifier、notifier ops、uprobe consumer | 按目标尺寸和字段偏移构造，使用目标 `__mutex_init`；不将基准内核对象布局交给目标内核 |
| GUP 参数变化 | BTF 检查 `get_user_pages` 的 4/5 参数、`get_user_pages_remote` 的 6/7 参数，选择对应调用 |
| notifier range 字段变化 | `start`、`end`、`flags` 从目标 BTF 解析，包括 6.6 移除 `vma` 首字段后的布局 |
| 页表层数、地址转换 | 读取 ARM64 TCR，识别 4KB 页和 3/4 级页表；分配真实页并用 AT/PAR 验证虚实地址转换 |
| `struct page` / vmemmap 布局 | 用目标 `vm_normal_page` 校验已 pin 页面与当前 PTE 的对应关系，从 PTE 得到 PFN |
| `dup_mmap` 被 LTO 内联 | 自动改用无参数的 `uprobe_start_dup_mmap` / `uprobe_end_dup_mmap`，控制锁覆盖整个复制窗口 |
| wxuprobe 的 `__copy_insn` 被内联 | 自动在 `arch_uprobe_analyze_insn` 前提供实时原指令，随后由 Linux 完成解码、模拟和 XOL |

完整 BTF 解析有长度、类型、递归深度、成员宽度及 bitfield 校验。遇到不匹配的函数原型或结构时拒绝加载。仅“版本号相近”或 `mm.pgd` 偏移相同，不视为结构兼容。

wxuprobe 的架构解码回退只接受尚未解码、inode/文件偏移正确且 consumer 独占的新探针；不会覆盖已缓存或其他消费者的指令。文件页缓存读取失败、磁盘入口为 uprobe BRK 时，此回退路径拒绝安装。

Dobby 的 portable KPM 仅链接增强协议桥接层和事务适配层，不加载原来的 legacy BRK/step/偏移扫描引擎。指定适配对象的新构建默认启用 `WXSHADOW_PORTABLE=ON`；旧构建目录若缓存了 OFF，须显式改为 ON。legacy BP/PATCH 命令仍由显式关闭 portable 的原模块提供，不属于跨版本保证。

## 兼容边界

- 已审查的接口范围为 **Linux 5.10–6.6、Android ARM64、4KB 页、3/4 级页表、VA/PA ≤48 位**。范围内仍须通过实际类型、函数和地址转换校验，不保证所有厂商修改都兼容。范围外目前拒绝加载。
- Dobby portable 需要内核内嵌 BTF、MMU_NOTIFIER 和 fork/GUP hook 入口。wxuprobe 还需要 UPROBES。BTF 存在但校验失败时不会改用静态偏移。
- wxuprobe 在没有 BTF 时保留原有精确内核模式，须使用目标源码和完整配置编译；LTO 解码回退需要 BTF。Dobby portable 没有无 BTF 的猜测模式。
- PREEMPT_RT 和带 mutex lockdep 元数据的调试内核拒绝加载。页大小、PAC/BTI、同页自读、安装/撤销时协调页访问等原有限制继续适用。
- GUP、写访问、mprotect、unmap 和 fork 会退役 hook；没有把退役解释为“继续保持隐藏”。卸载仍须先撤销所有 hook，再在普通进程上下文 QUIESCE，最后卸载 KPM。

## 构建

一次基准构建可供不同目标复用。基准树需要已完成 Kbuild 准备，启用 ARM64 4K、MMU_NOTIFIER，wxuprobe 还需 UPROBES。下面示例在 Linux/WSL 的仓库根目录执行；工具链与基准树配套即可，不要求目标设备源码。

```sh
WX_REPO="$PWD"
WX_KERNEL=/absolute/path/to/prepared-baseline-kernel
make -C "$WX_KERNEL" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  LLVM=1 LLVM_IAS=1 M="$WX_REPO/kpms/wxshadow/linux" wxshadow_linux_adapter.o
make -C "$WX_KERNEL" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  LLVM=1 LLVM_IAS=1 M="$WX_REPO/kpms/wxuprobe/linux" wxuprobe_linux.o

cmake -S . -B build/portable \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DWXSHADOW_PORTABLE=ON \
  -DWXSHADOW_LINUX_ADAPTER_OBJECT="$WX_REPO/kpms/wxshadow/linux/wxshadow_linux_adapter.o" \
  -DWXUPROBE_LINUX_ADAPTER_OBJECT="$WX_REPO/kpms/wxuprobe/linux/wxuprobe_linux.o"
cmake --build build/portable --target wxshadow.kpm wxuprobe.kpm -j6
```

Windows CMake 使用原有 GNU ARM 工具链，把适配对象路径换成 Windows 绝对路径即可。Dobby 的 `DOBBY_WITH_WXSHADOW=ON`、NDK 构建与调用方式见[使用文档](dobby-wxshadow-usage.md)；wxuprobe 客户端见[模块文档](../kpms/wxuprobe/README.md)。同一时刻只加载一种测试模块。

## 本次验证

当前设备：Pixel 6 / oriole，Android 16，ARM64、4096 字节页，内核 `6.1.134-android14-11-g66e758f7d0c0-ab13748739`。适配对象使用旧设备的 Android 5.10 基准树 `d262b276948e382dbaa38474845bac692bae4236`、Clang r498229b 构建，再由 GNU ARM GCC 14.2 链接 KPM。没有下载、准备或使用当前设备的 6.1 源码/配置，也未修改 KernelPatch。

设备实际选择 BTF 布局、uprobe fork 开始/结束回退；wxuprobe 选择架构解码回退。

| 验证 | 结果 |
|---|---|
| Dobby 静态库 / 动态库 | REQUIRED 能力查询 0x5f，hook / original / destroy、同页多入口、页末入口、远跳板通过 |
| Dobby 并发与生命周期 | 执行/整页 CRC 并发、mprotect/fork 退役、子进程拒绝、丢失输出后的 cookie 恢复通过 |
| ART JNI | 静态/动态库 × native/WxShadow 四组，在 `-Xcheck:jni` 下通过参数快照、original 与撤销 |
| wxuprobe 功能 | XOL、B/ADR 模拟、实时解密指令、x0–x30/SP、寄存器更新、原 Kernel-Trace ABI 通过 |
| wxuprobe 并发与批量 | 800 次命中 + 200 次整页读取，16 页共 2000 个活动入口、交错撤销与页容量检查通过 |
| wxuprobe 生命周期与权限 | fork/mprotect/GUP/unmap 退役、root 跨进程与 shell EPERM、进程退出清理通过 |
| tracefs 与卸载 | shell/root 两次独立名称日志均找到；tracing_on 恢复；两个模块分别 QUIESCE、卸载，列表为空 |
| 主机 | 原 Dobby/VM/协议回归及 BTF 解析测试 5/5；BTF 损坏输入在 ASan/UBSan 下通过 |
| 6.6 头文件 | 两个适配对象编译验证；不是 6.6 设备运行结果 |

本地 6.6 配置未启用 UPROBES；wxuprobe 的隔离头文件编译临时定义该选项，仅检查 C/API 兼容性，未修改内核配置或将此对象用于设备测试。

wxuprobe 日志优先用 `scnprintf` + `__trace_puts` 复制到内核 ring buffer。这样不会依赖由 Linux 模块加载器初始化的 `trace_printk` 临时缓冲，也不改变用户的 tracing 选项。Dobby/JNI 和 wxuprobe 测试时间段的 dmesg 均未出现 BUG、WARNING、Call trace、Internal error 或 UNSAFE UNLOAD。

本机忽略目录中的复现证据：`build/compat-dobby-device.log`、`build/compat-jni-device.log`、`build/compat-dobby-dmesg.log`、`build/compat-wxuprobe-device/{device,trace,dmesg}.log`。测试结束后模块均已卸载。

设备脚本仍使用环境变量 `KP_SUPERKEY` 或临时 `-SuperKey` 参数，不把凭据写入文档或日志：

```powershell
./tests/device/run-device-tests.ps1 -Serial <serial> -Module <portable-wxshadow.kpm>
./tests/jni/run-device-tests.ps1 -Serial <serial> -Module <portable-wxshadow.kpm>
./tests/wxuprobe/run-device-tests.ps1 -Serial <serial> -Module <wxuprobe.kpm>
```

历史 5.10 设备结果不等同于本次新产物已在 5.10 设备复测。当前真实运行验证来自上述一台 6.1 设备；6.6 和其他厂商设备尚需实测。

实现依据：[Linux BTF 格式](https://docs.kernel.org/bpf/btf.html)、[Linux 6.1 fork](https://github.com/torvalds/linux/blob/v6.1/kernel/fork.c)、[6.1 uprobes](https://github.com/torvalds/linux/blob/v6.1/kernel/events/uprobes.c)、[6.6 GUP](https://github.com/torvalds/linux/blob/v6.6/mm/gup.c)、[6.6 MMU notifier](https://github.com/torvalds/linux/blob/v6.6/include/linux/mmu_notifier.h)。
