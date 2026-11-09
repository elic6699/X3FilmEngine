# 跨平台引擎与界面文档

这些文档由 `engine/` 根目录归档至此，保留原内容、验证范围和历史命名；命令仍从各文档指定的仓库根目录执行。

- [Linux / Windows 总览](LINUX_WINDOWS.md)
- [移植功能覆盖](PORT_COVERAGE.md)

当前对外产品名统一为 FrameMatter；内部工程目标、引擎命令和兼容存储标识保留原名。Windows/Linux 的功能覆盖仍以各文档为准，不能仅凭品牌统一或 macOS 测试认定已与当前 macOS 版本功能同步。

Windows 专属界面、构建脚本与四份 Windows 指南已按维护者要求移入 `pravite/`，不随当前源码分发。它们的本地入口保留原作者标识；公开 CI 不再构建或上传 Windows 成品。

移植过程记录和待办计划仅保存在本地：`pravite/开发记录/engine/docs/platforms/PORT_NOTES.md`、`pravite/开发记录/engine/docs/platforms/PORT_PLAN.md`。旧路径映射见本地 `pravite/开发记录/README.md`，这些记录不随公开源码分发。
