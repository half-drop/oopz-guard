Guard 系列正式版 **OopzGuard v0.1.2**：Windows Oopz 去广告与 OBS 窗口过滤工具。

## 本版变化

- 与 KookGuard 统一中文简介、README、使用说明和 Release 栏目，并加入同系列项目链接。
- 统一正式版版本号、构建记录与发布规则；补充源码 ZIP、包内文件校验与验证范围文档。
- DLL 诊断版本统一为 0.1.2，保留现有精确快照匹配、广告字符串改写与 OBS 窗口过滤行为。

## 兼容范围

Windows **x64**，仅支持 Oopz **0.88.41** 的指定原始 `data/app.so`：大小 **31,851,424 字节**，SHA256 为：

```text
8384ee1a0a3c39194755034328c34ec9dc1f76842d38144cc78b43c3153cc8aa
```

仅版本号相同不代表兼容。文件不匹配或临时副本准备失败时保留原始行为；当前官网新版及其他版本尚未适配、验证。

## 安装与卸载

1. 下载 `OopzGuard-v0.1.2-windows.zip`，解压完整压缩包。
2. 从系统托盘完全退出 Oopz，将 `dwmapi.dll` 放到 `oopz.exe` 同目录。常见路径为 `%LOCALAPPDATA%\oopz`，自定义安装以实际程序位置为准。
3. 用原来的 Oopz 快捷方式启动客户端，默认开启两项功能。

查看状态：打开 `%TEMP%\OopzGuard`，查看最新的 `oopz-进程号.log`。卸载：完全退出 Oopz，移除添加的 DLL 和可选 `oopz_guard.ini`。

同目录已有 `dwmapi.dll` 时先保留并确认用途；不要将 DLL 放入 Windows 系统目录。原始程序与 AOT 文件保持不变。详细步骤见包内 `docs/使用说明.txt` 或 [使用说明](https://github.com/half-drop/oopz-guard/blob/v0.1.2/docs/使用说明.txt)。

## 下载文件

| 文件 | 用途 |
| --- | --- |
| `OopzGuard-v0.1.2-windows.zip` | 用户安装包 |
| `OopzGuard-v0.1.2-source.zip` | 对应版本源码 |
| `build-info.json` | 源码提交、Actions 运行地址与本次验证结果 |
| `SHA256SUMS.txt` | 发行附件 SHA256 校验值；包内另含 `FILE_HASHES.txt` |
| `dwmapi.dll` | 单独 DLL，推荐首次使用时下载完整安装包 |

## 验证范围

发布流水线重新编译 DLL，验证实际同目录加载、DWM 转发、不匹配快照放行、自建 OBS/普通窗口过滤、导出表、配置一致性、产物哈希与打包完整性。既有本地记录覆盖五个原始 AOT 受控用例及真实 Flutter 引擎读取修改快照。

登录后的广告界面、主播模式、语音与屏幕共享尚未完整人工验收；OBS 可能不出现在屏幕共享窗口列表中。其他检测途径、其他直播软件与仅序号 DWM 导出的跨 Windows 兼容性尚未全面验证。完整范围见 [VALIDATION.md](https://github.com/half-drop/oopz-guard/blob/v0.1.2/VALIDATION.md)；本次构建结果以附件 `build-info.json` 为准。

## 同系列项目

[KookGuard](https://github.com/half-drop/kook-guard) 提供 Windows KOOK 去广告与外部直播识别处理：[下载正式版](https://github.com/half-drop/kook-guard/releases/latest)。两者独立安装，各自遵循对应客户端兼容范围。
