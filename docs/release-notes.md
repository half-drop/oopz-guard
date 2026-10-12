Guard 系列正式版 **OopzGuard v0.2.0**：Windows Oopz 自适应去广告与 OBS 窗口过滤工具。

## 本版变化

- 删除客户端版本、固定文件大小和 SHA 白名单，删除固定补丁位置；启动时自动识别 Dart 字符串结构。
- 广告处理扩展到请求路径、数据字段和素材地址，不绑定 API 版本。当前官网客户端命中从 5 处扩大到 **25 处**。
- 自动适应字符串类编号、单字节/双字节编码与三种哈希布局变化，更新客户端后自动重新扫描。
- OBS 窗口过滤独立工作；未知快照保留原始行为，缓存按实际内容隔离。
- 保留 Guard 系列统一中文安装说明、源码包与构建记录，并互相链接 KookGuard。

## 安装与卸载

1. 下载 `OopzGuard-v0.2.0-windows.zip`，解压完整压缩包。
2. 从系统托盘完全退出 Oopz，将 `dwmapi.dll` 放到 `oopz.exe` 同目录。常见路径为 `%LOCALAPPDATA%\oopz`，自定义安装以实际程序位置为准。
3. 用原来的 Oopz 快捷方式启动客户端，默认开启两项功能。

查看状态：打开 `%TEMP%\OopzGuard`，查看最新的 `oopz-进程号.log`，`Adaptive snapshot` 显示命中数量。卸载：完全退出 Oopz，移除添加的 DLL 和可选 `oopz_guard.ini`。

面向 Windows x64 Oopz，更新后自动重新扫描。同目录已有 `dwmapi.dll` 时先保留并确认用途；不要将 DLL 放入 Windows 系统目录。原始程序与 AOT 文件保持不变。详细步骤见包内 `docs/使用说明.txt` 或 [使用说明](https://github.com/half-drop/oopz-guard/blob/v0.2.0/docs/使用说明.txt)。

## 下载文件

| 文件 | 用途 |
| --- | --- |
| `OopzGuard-v0.2.0-windows.zip` | 用户安装包 |
| `OopzGuard-v0.2.0-source.zip` | 对应版本源码 |
| `build-info.json` | 源码提交、Actions 运行地址与本次验证结果 |
| `SHA256SUMS.txt` | 发行附件校验；包内另含 `FILE_HASHES.txt`，不限制客户端版本 |
| `dwmapi.dll` | 单独 DLL，推荐首次使用时下载完整安装包 |

## 验证范围

13 组独立结构回归覆盖位置/大小变化、类编号变化、双字节编码、不同哈希布局、无广告、损坏输入和可执行区排除。实际 DLL 加载、缓存复用、两个独立开关、自建 OBS/普通窗口过滤、DWM 转发及导出表检查通过。

本地验证当前官网客户端快照，真实 Flutter 引擎可以读取修改副本。登录后的广告展示、语音和共享流程尚未完整人工验收；不能保证所有未来框架与新增广告名称。OBS 可能不出现在屏幕共享窗口列表中。完整范围见 [VALIDATION.md](https://github.com/half-drop/oopz-guard/blob/v0.2.0/VALIDATION.md)；本次构建结果以附件 `build-info.json` 为准。

## 同系列项目

[KookGuard](https://github.com/half-drop/kook-guard) 提供 Windows KOOK 去广告与外部直播识别处理：[下载正式版](https://github.com/half-drop/kook-guard/releases/latest)。两者独立安装。
