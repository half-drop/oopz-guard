# OopzGuard

[![构建与验证](https://github.com/half-drop/oopz-guard/actions/workflows/build-release.yml/badge.svg)](https://github.com/half-drop/oopz-guard/actions/workflows/build-release.yml)
[![正式版本](https://img.shields.io/github/v/release/half-drop/oopz-guard)](https://github.com/half-drop/oopz-guard/releases/latest)

**OopzGuard** 是 Guard 系列的 Windows Oopz 自适应去广告与 OBS 窗口过滤工具，提供安装、诊断与回滚。当前正式版：**v0.2.0**。

[下载正式版](https://github.com/half-drop/oopz-guard/releases/latest) · [使用说明](docs/使用说明.txt) · [验证范围](VALIDATION.md) · [同系列 KookGuard](https://github.com/half-drop/kook-guard)

## 工作方式

面向 **Windows x64 Oopz**。启动时自动识别客户端快照中的 Dart 字符串结构，再按广告特征处理请求、数据字段和素材地址；不绑定客户端版本号、固定文件大小、文件指纹或补丁位置。客户端更新后会重新扫描，无需为普通版本更新重新编译。

修改只发生在临时副本中，原始 `oopz.exe`、`flutter_windows.dll`、`data/app.so` 保持不变。无法识别结构或无法生成副本时，使用原始快照；OBS 过滤独立启用。框架结构彻底变化或广告命名改变仍可能需要更新规则，已测范围见 [验证范围](VALIDATION.md)。

## 安装与卸载

1. 从 [正式版下载页](https://github.com/half-drop/oopz-guard/releases/latest) 下载 `OopzGuard-v0.2.0-windows.zip`，解压完整压缩包。
2. 从系统托盘完全退出 Oopz，将 `dwmapi.dll` 放到 `oopz.exe` 所在目录。常见路径为 `%LOCALAPPDATA%\oopz`；自定义安装以实际程序位置为准。
3. 用原来的 Oopz 快捷方式启动客户端，默认开启两项功能。

查看状态：在资源管理器打开 `%TEMP%\OopzGuard`，查看最新的 `oopz-进程号.log`。卸载：完全退出 Oopz，移除添加的 `dwmapi.dll`；如创建过 `oopz_guard.ini`，也可删除。

如果目录里已经有 `dwmapi.dll`，先保留并确认其用途，避免覆盖其他补丁。DLL 应放在客户端目录，不要放入 Windows 系统目录。

## 功能与设置

| 功能 | 处理范围 |
| --- | --- |
| 广告请求 | 自动定位广告路径，覆盖不同 API 版本及 `advertisement`、`advert`、`advertising`、`ads`、`ad` 路径段 |
| 广告数据 | 处理广告列表、横幅、分区及推荐广告字段，包括 `advs`、`advertisement*`、`adBanner*`、`adSection*`、`adList*` 等 |
| 广告素材 | 处理明确广告目录中的网络素材与本地素材地址 |
| OBS 窗口过滤 | 覆盖 Oopz 经 Dart FFI 动态查询的 12 个 Win32 窗口 API，与广告处理独立开关 |
| 系统 DWM 接口 | 命名接口按名称转发到 System32 原 DLL |

在临时副本中等长改写广告字符串并更新 Dart 字符串哈希，保持对象布局；自动识别类编号、单字节/双字节编码及三种哈希布局。普通 `banner`、通用推荐字段、下载和语音地址保留，避免影响正常业务。

窗口过滤识别 `obs.exe`、`obs32.exe`、`obs64.exe`、`yyobs.exe` 及 `obs-studio` 目录路径。OBS 可能同时不出现在 Oopz 的屏幕共享窗口列表中。其他直播软件、独立进程枚举和仅序号 DWM 导出的跨 Windows 兼容性见 [验证范围](VALIDATION.md)。

可选：将 `oopz_guard.ini.example` 复制为 DLL 同目录的 `oopz_guard.ini`，按需设置，完全退出并重启 Oopz 后生效：

```ini
[Guard]
BlockAds=1
HideOBSWindows=1
```

`1` 为开启，`0` 为关闭。默认无需配置文件。

## 客户端更新

更新后的下次启动自动扫描新快照，按新内容生成独立缓存。官方更新若移除了同目录 DLL，重新放置即可。发现残留广告时可根据日志中的命中数量调查新增入口，规则不受旧版文件指纹限制。

## 验证与诊断

日志与临时副本位于 `%TEMP%\OopzGuard`：

- `Adaptive snapshot`：显示识别到的字符串数、结构及广告请求/字段/素材命中数量；`patched=0` 表示未发现符合规则的内容。
- `SNAPSHOT_UNRECOGNIZED_OR_IO_ERROR`：无法识别或准备副本，使用原始快照；OBS 过滤状态会单独记录。
- `BlockAds=0`：按设置使用原始快照。

当前官网客户端静态命中 **25 处**：2 条广告请求、11 个广告字段、12 条素材地址。真实 Flutter 引擎读取、跨结构回归和 DLL 加载测试通过；登录后的广告展示、语音和共享流程仍需人工验收，命中数量不能证明所有广告都已消失。

本次构建结果见发行附件 `build-info.json`；下载文件校验见 `SHA256SUMS.txt`，包内校验见 `FILE_HASHES.txt`。这些用于核验发行文件，不限制客户端版本。实现与调查见 [自适应广告过滤](research/自适应广告过滤.md)。

## 从源码构建

需要 Windows x64 PowerShell、Python 3 及锁定的 Zig 0.16.0。Python 脚本仅使用标准库。

```powershell
./ci/install-zig.ps1
./build.ps1
./tests/run-ci.ps1 -ZigPath ./toolchain/zig-0.16.0/ziglang/zig.exe
python ./tools/verify_build.py
python ./tools/package_release.py --version 0.2.0
```

构建和自动化测试无需安装 Oopz。`tests/run.ps1` 可额外验证本机 AOT；传入 `-ClientRoot` 可检查真实引擎读取，不调用 Oopz 主入口。用户安装时无需编译器或 Python。

## 自动构建与发布

推送 `main`、提交拉取请求或手动运行 Actions 时构建并验证。推送 `v*` 标签后，仅同次构建与校验成功才发布 Release；无后缀版本为正式版，带后缀版本为预发布版。

发行文件：`OopzGuard-v版本-windows.zip`、`OopzGuard-v版本-source.zip`、`build-info.json`、`SHA256SUMS.txt`，另提供单独的 `dwmapi.dll`。原 Oopz 二进制、修改后的 AOT、编译器与本机缓存不随源码发布。

## 同系列项目

| 项目 | 用途 |
| --- | --- |
| [KookGuard](https://github.com/half-drop/kook-guard) | Windows KOOK 去广告与外部直播识别处理 |
| **OopzGuard** | Windows Oopz 自适应去广告与 OBS 窗口过滤 |

两者使用一致的中文说明与发布格式，各自独立安装。
