# OopzGuard

[![构建与验证](https://github.com/half-drop/oopz-guard/actions/workflows/build-release.yml/badge.svg)](https://github.com/half-drop/oopz-guard/actions/workflows/build-release.yml)
[![正式版本](https://img.shields.io/github/v/release/half-drop/oopz-guard)](https://github.com/half-drop/oopz-guard/releases/latest)

**OopzGuard** 是 Guard 系列的 Windows Oopz 去广告与 OBS 窗口过滤工具，提供安装、诊断与回滚。当前正式版：**v0.1.2**。

[下载正式版](https://github.com/half-drop/oopz-guard/releases/latest) · [使用说明](docs/使用说明.txt) · [验证范围](VALIDATION.md) · [同系列 KookGuard](https://github.com/half-drop/kook-guard)

## 兼容范围

| 项目 | 支持范围 |
| --- | --- |
| 客户端 | Oopz 0.88.41 的指定原始 `data/app.so` 快照 |
| 架构 | Windows x64 |
| 安装方式 | 将 `dwmapi.dll` 放到 `oopz.exe` 同目录 |
| 客户端更新 | 新版未适配；快照不匹配时保留原始行为 |

原始快照必须同时满足大小 **31,851,424 字节**及以下 SHA256；仅版本号相同不代表兼容：

```text
8384ee1a0a3c39194755034328c34ec9dc1f76842d38144cc78b43c3153cc8aa
```

版本不匹配或临时副本准备失败时，不启用广告修改与窗口过滤。正式版标记不扩大客户端兼容范围，当前官网新版及其他版本尚未适配、验证。

## 安装与卸载

1. 从 [正式版下载页](https://github.com/half-drop/oopz-guard/releases/latest) 下载 `OopzGuard-v0.1.2-windows.zip`，解压完整压缩包。
2. 从系统托盘完全退出 Oopz，将 `dwmapi.dll` 放到 `oopz.exe` 所在目录。常见路径为 `%LOCALAPPDATA%\oopz`；自定义安装以实际程序位置为准。
3. 用原来的 Oopz 快捷方式启动客户端，默认开启两项功能。

查看状态：在资源管理器打开 `%TEMP%\OopzGuard`，查看最新的 `oopz-进程号.log`。卸载：完全退出 Oopz，移除添加的 `dwmapi.dll`；如创建过 `oopz_guard.ini`，也可删除。原始程序与 AOT 文件保持不变。

如果目录里已经有 `dwmapi.dll`，先保留并确认其用途，避免覆盖其他补丁。DLL 应放在客户端目录，不要放入 Windows 系统目录。

## 功能与设置

| 功能 | 处理范围 |
| --- | --- |
| 广告请求 | 在临时 AOT 副本中等长改写两条已确认请求路径，使对应请求失败 |
| 发现页广告字段 | 等长改写 `advertisementBannerList`、`adBanner`、`adSection` 并更新 Dart 字符串哈希 |
| OBS 窗口过滤 | 覆盖 Oopz 经 Dart FFI 动态查询的 12 个 Win32 窗口 API |
| 系统 DWM 接口 | 命名接口按名称转发到 System32 原 DLL |

原始 `oopz.exe`、`flutter_windows.dll`、`data/app.so` 不会被替换或修改。请求失败处理、已有内存状态及其他推广接口可能影响结果，未宣称所有广告都已覆盖。

窗口过滤识别 `obs.exe`、`obs32.exe`、`obs64.exe`、`yyobs.exe` 及 `obs-studio` 目录路径。其他直播软件、改名软件和独立进程枚举途径未宣称覆盖；OBS 可能同时不出现在 Oopz 的屏幕共享窗口列表中。仅序号 DWM 导出保留调查时的基准映射，跨 Windows 版本尚未完整验证。

可选：将 `oopz_guard.ini.example` 复制为 DLL 同目录的 `oopz_guard.ini`，按需设置，完全退出并重启 Oopz 后生效：

```ini
[Guard]
BlockAds=1
HideOBSWindows=1
```

`1` 为开启，`0` 为关闭。默认无需配置文件。

## 客户端更新

本版仅适配上述快照，不自动适配新版。更新后请查看日志并核对兼容范围；不匹配会保留原快照和原窗口查询。官方更新若移除了同目录 DLL，需要在确认适配后重新放置。

## 验证与诊断

本次构建的验证结果与提交记录见发行附件 `build-info.json`；下载文件校验见 `SHA256SUMS.txt`，包内文件校验见 `FILE_HASHES.txt`。本地受控测试与自动化测试的边界见 [验证范围](VALIDATION.md)。

日志与匹配后的快照副本位于 `%TEMP%\OopzGuard`：

- `Exact AOT matched`：快照匹配，后续日志分别说明广告处理与窗口过滤状态。
- `UNSUPPORTED_OR_IO_ERROR`：版本或文件不匹配，或临时副本准备失败；修改未启用。
- 临时广告副本约 32 MB，加载前核验 SHA256。

匹配记录本身不能证明登录后的所有广告已消失。登录后广告界面、主播模式、语音与屏幕共享尚未完整人工验收。详细证据见 [调查报告](research/调查报告.md)。

## 从源码构建

需要 Windows x64 PowerShell、Python 3 及锁定的 Zig 0.16.0。Python 脚本仅使用标准库，编译器下载后校验 SHA256。

```powershell
./ci/install-zig.ps1
./build.ps1
./tests/run-ci.ps1 -ZigPath ./toolchain/zig-0.16.0/ziglang/zig.exe
python ./tools/verify_build.py
python ./tools/package_release.py --version 0.1.2
```

构建使用提交的版本配置与 DWM 导出表，无需安装 Oopz。`tests/run.ps1` 是需要本机原始 AOT 的额外受控验证，不在云端执行。用户安装时无需编译器或 Python。

## 自动构建与发布

推送 `main`、提交拉取请求或手动运行 Actions 时构建并验证。推送 `v*` 标签后，仅同次构建与校验成功才发布 Release；无后缀版本为正式版，带后缀版本为预发布版。

发行文件：`OopzGuard-v版本-windows.zip`、`OopzGuard-v版本-source.zip`、`build-info.json`、`SHA256SUMS.txt`，另提供单独的 `dwmapi.dll`。发布作业下载产物后再次核验 SHA256；官方 Actions 固定提交指纹，发布权限仅授予发布作业。原 Oopz 二进制、修改后的 AOT、编译器与本机缓存不随源码发布。

## 同系列项目

| 项目 | 用途 |
| --- | --- |
| [KookGuard](https://github.com/half-drop/kook-guard) | Windows KOOK 去广告与外部直播识别处理 |
| **OopzGuard** | Windows Oopz 去广告与 OBS 窗口过滤 |

两者使用一致的中文说明与发布格式，各自独立安装，兼容范围以对应仓库为准。
