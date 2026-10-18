# Dobby WxShadow 后端：构建与使用

实现说明：[设计与实现](dobby-wxshadow-design.md)。测试证据：[验证记录](dobby-wxshadow-validation.md)。

Native 回调中的 Java 参数读取见 [JNI 解析接入](dobby-jni-usage.md)，可通过 `DOBBY_WITH_JNI=ON` 与本后端一起使用。

## 1. 工作方式与支持范围

应用链接本仓库的 Dobby，显式选择 `DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED` 后，继续使用 `DobbyHook` / `DobbyDestroy`。Dobby 创建独立 RX 的 original 和 relay，通过 prctl 将源入口的单条 B 指令提交给 wxshadow。用户读取源页得到原始内容，执行时切换至 shadow。

普通后端默认启用。REQUIRED 模式下模块缺失、能力不足、提交失败或近地址内存不足均返回错误；不会改用普通入口写入。选择 REQUIRED 时即使探测失败，配置仍保持 REQUIRED，避免调用方忽略返回值后发生普通写入。

当前支持：

- Android ARM64，当前进程，4KB 页，标准函数调用 ABI，私有且可读、可执行、不可写的源映射。
- 内核 VM 对象使用目标 Linux **5.10** 源码和完整配置编译；最多 48 位物理地址，MMU_NOTIFIER，无 PREEMPT_RT、无随机结构布局。
- 每个入口替换一条对齐的 4 字节指令。原始首条指令的重定位支持普通位置无关指令、ADR/ADRP、B、常见条件分支和跨源页 literal load。
- 同页多 hook、超出 B 范围的 replacement、可选的空 original 输出、按 cookie 精确撤销。

下列情况返回不支持或属于调用方必须排除的条件：

- `DobbyInstrument`、BTI/PAC 标记的 ELF/VMA/入口、原生 XOM、共享/可写/特殊/大页映射。
- 首条 BL、间接跳转/返回、异常指令、同源页 literal load。首版对部分普通短函数的 RET/BR 首条也保守拒绝。
- JIT、自修改代码、目标页内取指和读数据同时发生的自读代码。仅检查首条指令无法自动证明整页无自读，调用方必须了解页面行为。
- replacement、辅助代码和已知后端依赖入口位于受保护源页。额外的间接依赖也须由调用方排除；页内其他函数同样受页面切换影响。
- fork 子进程继续使用继承的 Dobby 状态。子进程管理 API 返回 `DOBBY_ERROR_FORK`，应 exec。

**安装和撤销前，调用方须停止目标页的执行与读取。** 管理锁只串行化 API；没有自动暂停其他线程。安装后的并发读取和执行已在测试设备上验证；未验证执行线程不停机时修改入口。

## 2. 构建 Dobby

以下示例在仓库根目录执行，NDK 路径按本机修改：

```sh
cmake -S Dobby -B build/dobby-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DDOBBY_WITH_WXSHADOW=ON \
  -DDOBBY_WXSHADOW_UAPI_DIR="$PWD/shared"
cmake --build build/dobby-android -j6
```

产物为 `libdobby.so` 和 `libdobby.a`。只需要普通后端时设 `DOBBY_WITH_WXSHADOW=OFF`；新 API 仍存在，但 REQUIRED 返回不支持。不要在设备构建中启用 `DOBBY_WXSHADOW_TESTING`。

## 3. 构建内核 VM 适配对象和 KPM

### 3.1 准备与设备完全匹配的内核树

```sh
adb shell uname -r
adb pull /proc/config.gz build/device-config.gz
gzip -dc build/device-config.gz > build/device.config
git submodule update --init .kp
```

取得对应内核提交，并用该配置准备内核构建目录。Linux 头文件的字段布局依赖完整配置，不能用发行版 headers、通用 defconfig 或只改 UTS_RELEASE 冒充目标环境。运行时会比较 release 和 mm.pgd 偏移，但不会证明其他字段完全相同。

本次设备对应官方 Android common 提交 `d262b276948e382dbaa38474845bac692bae4236`，release 为 `5.10.66-android12-9-00001-gd262b276948e-ab8291727`。源码可从 [Android common 对应提交](https://android.googlesource.com/kernel/common/+/d262b276948e382dbaa38474845bac692bae4236) 获取。构建示例：

```sh
WX_REPO="$PWD"
WX_KERNEL=/absolute/path/to/android-common-d262b276948e
WX_RELEASE_SUFFIX=-android12-9-00001-gd262b276948e-ab8291727
# 把 Android Clang 工具链 bin 放入 PATH；本次使用 clang-r498229b。
cp "$WX_REPO/build/device.config" "$WX_KERNEL/.config"
make -C "$WX_KERNEL" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CLANG_TRIPLE=aarch64-linux-gnu- LLVM=1 LLVM_IAS=1 \
  LOCALVERSION="$WX_RELEASE_SUFFIX" olddefconfig modules_prepare
make -C "$WX_KERNEL" M="$WX_REPO/kpms/wxshadow/linux" \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  LLVM=1 LLVM_IAS=1 LOCALVERSION="$WX_RELEASE_SUFFIX" wxshadow_linux_adapter.o
llvm-nm -u kpms/wxshadow/linux/wxshadow_linux_adapter.o
```

最后一步应无输出。检查 `include/generated/utsrelease.h` 与设备一致。若配置中的 UNUSED_KSYMS_WHITELIST 指向原构建机路径，应替换为本机有效的符号表路径；仅准备头文件和此对象时可使用本地空普通文件，因为没有生成内核或 Linux LKM。该项不改变结构布局。不要修改内存、锁、调试、CFI 等影响布局的配置来绕过编译错误。

Kbuild Makefile 只对适配对象移除 LTO/CFI/SCS 等运行时插桩，保留 autoconf 配置和真实结构。KPM 自身禁用 SIMD 并保留 x18；编译器可能产生的 memcpy/memset/memmove 由模块内小型实现提供。CFI 内核依赖 KernelPatch 已有的 KPM 回调支持，本模块不改写 CFI 机制。

### 3.2 链接进 KPM

```sh
cmake -S . -B build/kpm-device -G Ninja \
  -DCMAKE_C_COMPILER=aarch64-none-elf-gcc \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DWXSHADOW_LINUX_ADAPTER_OBJECT="$PWD/kpms/wxshadow/linux/wxshadow_linux_adapter.o"
cmake --build build/kpm-device --target wxshadow.kpm -j6
```

产物：`build/kpm-device/kpms/wxshadow/wxshadow.kpm`。`WXSHADOW_DEBUG_PROTOCOL=ON` 可打印协议诊断；发布构建默认 OFF。

未指定 `WXSHADOW_LINUX_ADAPTER_OBJECT` 时仍可编译旧功能，但增强能力为 0，Dobby REQUIRED 不可用。KernelPatch 框架目录保持只读，根 CMake 自动处理 Windows 的 `kernel` 符号链接占位文件。

## 4. 应用代码

```cpp
#include <dobby.h>

using Target = int (*)(int);
static Target original;
static int replacement(int value) { return original(value) + 100; }

int install(void *target) {
  int rc = dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED);
  if (rc) return rc;
  // 此时调用方已停止整个目标页的访问；original 在入口发布前填入。
  return DobbyHook(target, reinterpret_cast<void *>(replacement),
                   reinterpret_cast<void **>(&original));
}

int remove(void *target) {
  // 同样先停止目标页访问，成功后才恢复执行。
  return DobbyDestroy(target);
}
```

不需要运行 wxshadow_client。状态检查：

```cpp
DobbyHookStatus status{};
int rc = dobby_get_hook_status(target, &status);
// rc==0 只表示查询完成，还须检查 status.state 和 status.last_error。
DobbyHookStatistics statistics{};
dobby_get_hook_statistics(&statistics);
```

`active_entries` / `recovery_entries` 是最近已知状态的数量；可先通过状态 API 刷新。`retained_mappings` / `retained_bytes` 包含活动和已撤销 hook 发布过的 WxShadow 辅助映射，不统计普通后端分配池。

## 5. 错误、恢复与生命周期

| 返回值 | 处理 |
|---|---|
| 普通负 errno | 检查参数、映射、能力或资源；确认无 Entry 后可按失败安装处理 |
| `DOBBY_ERROR_RECOVERY_REQUIRED` (-10001) | 安装/撤销结果无法确认；保留 original 和相关库、目标页，继续维持协调状态，查询状态并重试 DobbyDestroy |
| `DOBBY_ERROR_BACKEND_LOST` (-10002) | 模块实例变化；旧 Entry 保留，禁止按新实例撤销旧 cookie |
| `DOBBY_ERROR_FORK` (-10003) | fork 子进程拒绝管理；exec 后重新初始化 |
| `-EBUSY` | 仍有登记 Entry 时不能切后端；受保护源页也拒绝 DobbyCodePatch |

模块的 copy_to_user 即使失败也可能已经提交。Dobby 自动使用预先保存的 cookie 查询实际结果；查询失败时不会丢弃 Entry 或回收可能可达代码。重复地址和不同归属记录冲突不会覆盖已有 hook。

页失效、解除映射、权限修改、写入、GUP 访问以及 fork 会使增强 hook 恢复原映射并退役，不自动重装。`WX_CAP_GUP` 未置位，不能把本实现描述为跨进程读内存时 hook 始终保持活动。

撤销成功只撤销入口。发布后的 original/relay 保留至进程退出，以支持在途调用和保存的 original 指针；原始源映射及相关库仍须存活，目标解除映射后不能再调用旧 original。

模块保留的页引用、shadow 和 cookie tombstone 在 QUIESCE 统一释放。上限：64 个 mm、1024 个源页代次、8192 条记录、每源页 128 个活动入口，安装 shadow 预算 4096；撤销可以使用额外 shadow，避免预算耗尽后无法撤销。

## 6. 部署与正确卸载

协议客户端通过第 7 节设备构建生成。以环境变量保存测试设备凭据，不写入仓库：

```sh
adb push build/kpm-device/kpms/wxshadow/wxshadow.kpm /data/local/tmp/
adb shell /data/local/tmp/kpatch "$KP_SUPERKEY" kpm load /data/local/tmp/wxshadow.kpm
adb shell /data/local/tmp/wxshadow-dobby-test/wxshadow_protocol_client --require
# 应报告 arch=1 page=4096，capabilities 包含 required=1f；本次设备为 5f。
```

卸载必须按此顺序：

1. 所有客户端协调停止访问并完成 DobbyDestroy；或者进程退出，使 mm notifier 退役其 hook。
2. 管理员在普通进程上下文运行 `wxshadow_protocol_client quiesce`，成功后停止新增强操作并注销全部 notifier。
3. 执行 kpatch kpm unload。

```sh
adb shell /data/local/tmp/kpatch "$KP_SUPERKEY" su -c \
  '/data/local/tmp/wxshadow-dobby-test/wxshadow_protocol_client quiesce'
adb shell /data/local/tmp/kpatch "$KP_SUPERKEY" kpm unload wxshadow
```

QUIESCE 返回 EBUSY 时还有活动 hook；不要卸载。QUIESCE 后需卸载重载才恢复新安装能力。

**已核对的 KernelPatch 装载器不能阻止违规卸载。** 它在 RCU 读侧调用 exit/ctl0，并忽略 exit 错误继续释放模块；因此不能在其中等待 notifier 的 SRCU 注销，也不能通过 exit 返回 EBUSY 阻止释放。必须先走 prctl QUIESCE，否则可能留下悬空内核回调。

## 7. 测试

主机测试（Linux / WSL，有 GCC、CMake）：

```sh
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure
```

四项测试：Dobby static/shared、真实 VM 事务代码配合 VM doubles、真实协议校验和复制错误。VM doubles 不执行 ARM64 PTE/TLB/cache 指令，不能代替设备测试。

构建设备程序（同时构建测试所需 Dobby 两种库）：

```sh
cmake -S tests/device -B build/device -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
cmake --build build/device -j6
```

Windows PowerShell 自动部署、测试及清理：

```powershell
# 由环境提供 KP_SUPERKEY，不把凭据写入脚本。
./tests/device/run-device-tests.ps1 -Serial <adb设备序列号>
```

脚本拒绝接管已有 wxshadow 模块；它先运行模块缺失和普通后端测试，再加载本次产物，测试 static/shared，最后 QUIESCE、卸载并重验模块缺失。QUIESCE 失败时保留已加载模块并报告错误。也可分别执行 `wxshadow_device_static` / `wxshadow_device_shared` 的默认、`--native`、`--expect-unavailable` 模式。

测试只操作测试进程自有的匿名 RX 页面。覆盖内容及实测结果见验证记录；BTI/PAC、任意地址插桩及不协调的实时安装仍未实现。
