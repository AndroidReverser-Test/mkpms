# Dobby WxShadow 验证记录

2026-10-19 新增：使用 5.10 基准对象构建的 portable KPM 已在 Pixel 6 / Android 16 / Linux 6.1.134 上通过静态/动态 Dobby 与四组 ART JNI 回归、QUIESCE 和卸载。完整兼容说明与新测试矩阵见[跨内核兼容记录](kernel-compatibility.md#本次验证)。以下保留 2026-10-18 的旧版测试证据。

日期：2026-10-18。测试代码在 `tests/`；接口与构建说明见 [使用文档](dobby-wxshadow-usage.md)。

## 环境

| 项目 | 实际环境 |
|---|---|
| 设备 | Redmi 22041211AC / rubens，ARM64，Android 12 |
| 内核 | `5.10.66-android12-9-00001-gd262b276948e-ab8291727`，4KB 页 |
| 内核源码 | Android common `d262b276948e382dbaa38474845bac692bae4236`，与设备 /proc/config.gz 配置匹配 |
| KernelPatch | 设备报告 `c02`；仓库只读框架提交 `94e5be9cc3f8a6fbd9574155e3e9753200ab9bfb` |
| Dobby 基线 | `5dfc8546954ce3b3198132ab13fddb89ee92cdd7` |
| Dobby/设备程序 | Android NDK 25.1.8937393，android-24，Release |
| VM 对象 | Kbuild，Android Clang r498229b；ELF ARM64，nm -u 无输出 |
| KPM 其余对象 | aarch64-none-elf-gcc 14.2；模块版本 1.1.0 |
| 主机 | WSL Ubuntu 24.04，x86_64 |

准备内核树时仅修正了原构建机的 UNUSED_KSYMS_WHITELIST 路径，以及恢复 release 的 LOCALVERSION 构建参数；未替换 VM、锁、页、CFI 等布局配置。适配对象单独去除运行时编译插桩。

## 主机测试

`ctest --test-dir build/tests --output-on-failure`：4/4 通过。

| 测试 | 覆盖 |
|---|---|
| wxshadow_static / wxshadow_shared | 真 x86 native hook 回归，ABI、B 距离边界、严格失败、分配/发布失败、同页和页末、远 relay、安装回复丢失、查询失败保留恢复状态、撤销重试、fork 检查、实例改变 |
| wxshadow_vm_transactions | 真实适配层事务代码的归属、幂等、冲突、分配/walker 失败、精确撤销、读/执行切换、非阻塞失效重试、避免恢复旧 PTE 到新映射、fork 退役、QUIESCE 清理引用 |
| wxshadow_protocol | 真实协议层的 header/version/size/flags/reserved 校验、instance 不匹配、copy_from 失败、提交后的 copy_to 失败和查询恢复 |

## 设备测试

静态库和动态库均实际执行通过：

| 场景 | 结果 |
|---|---|
| 模块未加载时选 REQUIRED 并尝试 Hook | 失败，函数行为和源页 CRC 不变 |
| 普通 ARM64 后端 | replacement、original、Destroy、Destroy 后 original 调用通过 |
| QUERY | `abi=1 arch=1 page=4096 capabilities=5f required=1f` |
| 三个同源页函数，包括 offset=4092 的入口 | replacement/original 返回值正确，generation 一致 |
| 超过 ±128MiB 的 replacement | 经近 relay 调用正确 |
| 读取受保护源页 | 全 8192 字节 CRC 与安装前一致 |
| 同页删除 A，再删除 B/C/远 relay 入口 | 删除精确，剩余 hook 持续生效，最后恢复全部原行为 |
| 并发阶段 | 3 个执行线程各循环 200 次；1 个读取线程校验 50 次 CRC；无断言失败 |
| 受保护页 DobbyCodePatch / WxShadow DobbyInstrument | 分别返回 EBUSY / ENOTSUP |
| mprotect 修改权限 | hook 退役，DobbyDestroy 清理登记，恢复原函数 |
| fork | 父 hook 退役，子管理 API 返回 DOBBY_ERROR_FORK |
| 实际内核输入校验 | 未知 flags 返回 EINVAL，坏请求指针返回 EFAULT |
| PATCH_EX 后结果写入只读请求页失败 | syscall 返回 EFAULT；QUERY_PATCH 确认 ACTIVE，调用命中，CRC 不变 |
| RELEASE_EX 后结果写回失败 | syscall 返回 EFAULT；QUERY_PATCH 确认 RETIRED，函数恢复 |
| 辅助代码保留 | 每种库测试结束后 0 active / 0 recovery，12 个已发布映射、49152 字节保留 |
| 模块清理 | 所有测试进程退出 → root QUIESCE 成功 → kpm unload 成功 → 模块数量恢复 0 |
| 卸载后重新尝试 REQUIRED | 再次严格失败，源字节不变 |

设备程序的通过输出：

```text
PASS same-page hooks, original calls, CRC, page-end entry, far relay
PASS concurrent execution/read verification and precise removal
PASS mprotect/fork retirement and child API rejection
PASS protocol validation and committed requests with lost result copies
PASS all device tests; retained mappings=12 bytes=49152
```

本机 `build/device-static.log`、`build/device-shared.log` 和后续脚本运行日志保存执行输出；构建日志及产物均位于忽略的 build 目录，不将凭据写入仓库。

## 实机发现并修复的问题

- GKI LTO 存在名为 `_copy_from_user` 的局部特化函数，调用约定不等于公共原型；改用导出的 nofault 用户内存复制函数，并使用 __kmalloc。
- KPM 增加 `-mgeneral-regs-only -ffixed-x18`，避免干扰用户 SIMD 状态及内核 SCS 寄存器；提供模块内编译器所需内存复制例程。
- get_user_pages 补齐 mmap_read_lock；目标配置启用 DEBUG_LIST 时，内部私有链表不引入未解析的 Linux 调试符号。
- TLBI 无条件重复失效，包含需要 repeat-TLBI workaround 的设备路径。
- fork 的控制锁从 before 回调一直持有到 after 回调，关闭父映射恢复与实际页表复制之间的新安装间隙。

## 验证边界

上述结果来自一台设备和自有代码页。没有验证其他内核/页大小/厂商配置、BTI/PAC、原生 XOM、任意位置插桩、JIT、自读源页、跨进程 GUP 保持 hook、fork 子进程继续沿用 hook，或执行线程不停机时安装/撤销。这些不在本次已交付的支持范围。

KernelPatch 装载器忽略 exit 错误并释放模块，**不能强制阻止未 QUIESCE 的误卸载**。正常清理路径已经实机通过；违规卸载不构成受支持的恢复流程。
