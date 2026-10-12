# OopzGuard

实验性的 Windows x64 `dwmapi.dll`：放在 `oopz.exe` 同目录，由客户端正常导入自动加载。

**目前仅支持 Oopz 0.88.41 的指定 `data/app.so`。其他版本正常放行，不会套用补丁。** 已完成本机静态分析、模拟加载和窗口测试；登录后的广告界面、语音、屏幕共享及新版客户端尚未验证。

支持的原始 AOT SHA256：

```text
8384ee1a0a3c39194755034328c34ec9dc1f76842d38144cc78b43c3153cc8aa
```

- 广告部分修改临时 AOT 副本中的五个指定字符串对象及其 Dart 哈希，并让引擎读取副本；原程序文件保持原样。
- 直播软件部分过滤 Oopz 经 Dart FFI 查询的 OBS 窗口，覆盖 12 个 Win32 窗口 API。未覆盖所有进程枚举或直播软件检测途径，也可能使 OBS 不出现在 Oopz 的屏幕共享窗口列表中。
- 系统 DWM 接口转发到 Windows System32 中的原 DLL；命名接口按名称解析，避免系统版本之间的序号变化影响加载。仅序号导出沿用调查时的基准映射，未完成跨系统版本验证。

## 下载和使用

从 [Releases](https://github.com/half-drop/oopz-guard/releases) 下载实验版压缩包，完整退出 Oopz，将其中的 `dwmapi.dll` 放到 `oopz.exe` 旁边，然后重启客户端。可选配置见 [`oopz_guard.ini.example`](oopz_guard.ini.example) 和 [使用说明](docs/使用说明.txt)。卸载时退出 Oopz 并移除该 DLL 及自己复制的配置。

如果该目录已经有同名 DLL，先保留原文件并查明用途。不要把文件放入 Windows 系统目录。

## 从源码构建

在 Windows x64 PowerShell 中执行：

```powershell
./ci/install-zig.ps1
./build.ps1
./tests/run-ci.ps1 -ZigPath ./toolchain/zig-0.16.0/ziglang/zig.exe
python ./tools/verify_build.py
python ./tools/package_release.py --version v0.1.1-experimental
```

编译器固定为 Zig 0.16.0，下载后验证锁定的 SHA256。Python 脚本仅使用标准库。构建采用提交的版本配置和 DWM 导出表，不需要安装 Oopz。`tests/run.ps1` 是需要本机原始 AOT 文件的额外验证，不在云端执行。

## 自动构建和发布

推送 `main`、提交 PR 或手动运行 Actions 时构建并测试。推送 `v*` 标签时，在同一次成功构建之后创建预发布 Release，提供 DLL、ZIP、SHA256 校验文件及构建记录。ZIP 和 DLL 均由 Actions 本次编译生成。

云端测试验证实际目录加载、DWM 转发、版本不匹配时放行和自行创建的 OBS/普通窗口；不会运行真实 Oopz 或证明登录后的去广告效果。原 Oopz 二进制、修改后的 AOT、编译器下载和本机缓存均不提交。

完整调查、原始证据和本机验证边界见 [调查报告](research/调查报告.md)。
