## Dobby

本地版本增加 Android ARM64 的可选 WxShadow 后端，通过内核影子页发布 DobbyHook 入口。

- 编译：`DOBBY_WITH_WXSHADOW=ON`，`DOBBY_WXSHADOW_UAPI_DIR` 指向外层仓库 shared 目录。
- 使用：选择 `DOBBY_HOOK_BACKEND_WXSHADOW_REQUIRED`，继续调用 DobbyHook / DobbyDestroy。
- 依赖：与目标 Linux 5.10 源码和完整配置匹配的 VM 适配对象及 wxshadow KPM。
- 卸载：客户端撤销全部 hook，再运行管理员 QUIESCE，最后卸载 KPM。

详见外层 mkpms 仓库的 [构建和使用文档](../docs/dobby-wxshadow-usage.md) 与 [实机验证记录](../docs/dobby-wxshadow-validation.md)。普通后端仍为默认；严格模式没有普通写入回退。DobbyInstrument、BTI/PAC、自读页等不在本后端当前支持范围。
