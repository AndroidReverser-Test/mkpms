# android 无痕probe 无痕hook kpm demo
利用kpm模块实现kernel中动态切换内存分页，实现应用对内存的读和执行分离，读和执行发生在不同的物理页，使应用读到的是原始物理页，执行的是被hook过的物理页，**从而隐藏hook产生的内存修改**

缺陷：无法对自读代码区域设置断点，同一页无法同时读取和执行

## Linux uprobe / Kernel-Trace 接入

新增独立模块 **wxuprobe.kpm**：结合本项目的读执行分离与 Kernel-Trace 使用的 Linux uprobe，在函数原指令执行前修改 **x0–x30 和 SP**，由内核继续完成指令模拟、XOL 和单步。支持原 Kernel-Trace ARM64 mincore 接口，以及新增的寄存器更新、单独删除和 prctl 管理接口；运行时解密代码直接使用内存原指令。

- [构建、接入、兼容接口与卸载](kpms/wxuprobe/README.md)
- [两个项目的实现梳理、集成设计与实机验证](docs/kernel-trace-wxuprobe.md)
- [可重复执行的设备测试脚本](tests/wxuprobe/run-device-tests.ps1)

已在当前 Android 12 / ARM64 / Linux 5.10.66 / 4KB 页设备验证 2000 个同时活动 hook、全部通用寄存器和 SP、整页读取校验、生命周期及完整卸载。需要匹配目标内核完整配置的 Kbuild 对象。不要同时加载原 kernel_trace；卸载前必须先通过客户端 QUIESCE，详见模块文档。

## Dobby 直接接入（Android ARM64）

已增加可选的 WxShadow 后端，继续使用 `DobbyHook` / `DobbyDestroy`：

```cpp
int rc = dobby_set_hook_backend(DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED);
if (rc == 0) rc = DobbyHook(target, replacement, &original);
```

REQUIRED 严格失败，不回退普通入口写入。需要启用 `DOBBY_WITH_WXSHADOW`，并将按目标 Linux 5.10 完整配置 Kbuild 的 VM 适配对象链接进 KPM。已通过 Android 12 / 5.10.66 设备上的静态库和动态库测试。

- [构建、使用、错误恢复与卸载](docs/dobby-wxshadow-usage.md)
- [设计与源码实现](docs/dobby-wxshadow-design.md)
- [实际验证范围及结果](docs/dobby-wxshadow-validation.md)
- [Native hook 的 Java 参数解析：JNI 字符串、数组、对象字段及 ARM64 参数解码](docs/dobby-jni-usage.md)

安装/撤销须由调用方协调停止整个目标页访问。DobbyInstrument、BTI/PAC、JIT 和同页自读不在当前支持范围。卸载顺序：所有客户端撤销 hook → 管理员 `wxshadow_protocol_client quiesce` → `kpatch kpm unload`；框架无法阻止绕过 QUIESCE 的误卸载。

### 目前通过prctl提供接口给用户态进程调用，支持两种接口bp和patch
* wxshadow_client实现了基本的wxshadow接口调用，可以用来测试bp断点和patch能力
* bp接口能够直接对某个地址设置断点，可以用来进行简单的测试无痕效果，设置断点后通过dmesg查看日志
* patch接口可以配合用户态hook框架写入inline hook代码（跳板代码不能存在读取同一页内存的指令），需要用户态工具联调wxshadow提供的接口实现无痕（无法直接对frida等hook框架产生隐藏效果）
* 目前用户态实现hook工具为rustFrida项目，已基本的Java hook和native hook
  
git config submodule.recurse true 自动更新submodule

# 快速使用
* 使用KernelPatch或者Apatch加载kpm模块，wxshadow_client -p pid -a address -r x0=9 -r可选的修改寄存器值，dmesg查看日志  
* 打开测试app，复制value函数的地址，设置断点测试，观察crc校验结果保持不变，修改寄存器改变value函数返回值  
* 复制crc32函数地址设置断点，app卡死，无法同时读取和执行  

<img width="1116" height="173" alt="c62acc29b9c00edf4d3a301c0aacd05a" src="https://github.com/user-attachments/assets/de6280d8-5093-46d3-9fed-db38355ae6f1" />

<img width="585" height="789" alt="d3d3b72fbf1c8d493d2098417def85f2" src="https://github.com/user-attachments/assets/e7be1d78-5d66-4ba7-8dfc-41c8c1f568a3" />

# 更新
* 配合rustfrida实现java、native hook
* https://bbs.kanxue.com/thread-290304.htm#msg_header_h2_5
## 交流群

![wxshadow 微信群](image.png)

## License

This project is licensed under the [GPL v3](LICENSE). Any derivative work that uses, modifies, or distributes this project's code must be open-sourced under the same license.

## 免责声明

本项目仅供安全研究与学习交流使用，**严禁用于任何非法用途**。使用者应遵守所在地区的法律法规，因使用本项目产生的一切后果由使用者自行承担，与项目作者无关。
