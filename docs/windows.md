# Windows 使用与开发

R-IME 使用 Windows Text Services Framework（TSF）接入系统输入。
64 位和 32 位应用各自加载对应的 COM DLL；独立的 64 位 worker 承载 librime，
设置程序使用 Qt 6 Quick。输入、候选显示及设置共用 Linux 版本的配置模型。

## 构建与安装

需要 Windows 11 x64、Visual Studio 2022 的 C++ 桌面工具链、Windows SDK、
CMake 3.22+、PowerShell 7，以及 Qt 6.8+ 的 `msvc2022_64` 版本。
首次构建会下载固定版本的 librime、nlohmann/json 和 yaml-cpp；输入运行时无需联网。

```powershell
./packaging/windows/build.ps1 -QtRoot 'C:\Qt\6.11.1\msvc2022_64' -CMake 'cmake'
./build-windows/package/install.ps1
```

`build-windows/package` 是可复制的完整程序目录，包含 Qt、VC 运行库、Rime 词库和许可证。
安装命令从普通用户 PowerShell 执行，系统阶段会请求 UAC 提升权限：

- 将程序复制到 `C:\Program Files\R-IME\0.2.0-<时间戳>`。
- 注册两个位数的 COM 组件和独立的中文 TSF 配置项。
- 为当前用户添加输入法、开机启动项以及开始菜单的“R-IME 设置”。
- 从普通用户进程启动引擎，保留原有默认输入法。

通过 **Win + 空格** 选择 **R-IME**。首次启动需要部署词库。
单独按 Shift 切换中英文；空格和数字选词，Enter 提交原始输入，Esc 取消。
候选窗使用系统字体、无焦点窗口，支持明暗、横竖排和字号调整。
输入法语言栏菜单也提供设置入口。窗口标题栏遵循 Windows 行为。

## 数据与卸载

用户文件位于 `%LOCALAPPDATA%\R-IME`：

| 子目录或文件 | 用途 |
|---|---|
| `config/config.json` | 输入与外观配置 |
| `data/rime` | 独立的 Rime 用户词库 |
| `cache`、`runtime` | 部署缓存与运行状态 |
| `installation.json` | 已安装版本、路径及安装状态 |
| `installation-attempt.json` | 未完成或失败的安装记录，不覆盖已有成功安装 |

命名管道仅允许当前用户 SID 访问，拒绝远程连接。每个输入上下文使用独立会话，
断开后不重放按键，失焦时取消组合；密码及敏感输入范围绕过候选和学习。
测试可通过 `RIME_USER_ROOT` 指定隔离目录，普通使用无需设置此变量。

卸载使用已安装目录中的脚本：

```powershell
$installation = Get-Content "$env:LOCALAPPDATA\R-IME\installation.json" -Raw | ConvertFrom-Json
& (Join-Path $installation.directory 'uninstall.ps1')
```

卸载会提升权限以移除系统注册，移除本用户入口与启动项，并保留用户词库及配置。
其他应用可能仍加载 TSF DLL，关闭这些应用或注销后才能删除剩余程序文件。
安装失败会保留带具体错误的安装清单，便于重试或卸载。
首次安装未完成时，可直接运行打包目录的 `uninstall.ps1` 清理已完成的部分。

## 图标更新与本机验证（2026-09-29）

Windows 输入法列表使用蓝底白色 R 标识。语言栏中文模式使用蓝色 R，英文模式使用灰色 R；
设置程序也使用同一品牌图标。图标包含 16～256px 多种分辨率，嵌入 64 / 32 位 DLL，
不依赖外部图标文件。按 `Win + 空格` 选择 **R-IME** 后，在输入区域键入 `nihao`。

当前安装版本为 `0.2.0-20260929014714`。两种位数的实际安装组件均通过系统 COM 和
Windows 按键分发验证，包含品牌图标解码、候选窗在屏幕内显示、提交后隐藏和中文提交。
结果保存在 `.cache/installed-validation-991ce30c31df4047b2027c05009623cc`。
本轮普通 CTest 的临时注册 TSF 宿主未激活测试服务，尚未解决；不能宣称本轮全套通过。
Windows 引擎回归重试通过。下面的 8/8 结果是上一版本的历史记录。

## 前一版本验证（2026-09-28）

环境：Windows 11 x64 build 26300、Intel Core i9-14900K、MSVC 19.44、
Windows SDK 10.0.26100、Qt 6.11.1、librime 1.17.0。

此前安装版本为 `0.2.0-20260928235332`，位于 `C:\Program Files\R-IME`。
安装清单状态为 `installed`；启动项、开始菜单和正在运行的引擎均指向此版本。
以下 64 / 32 位系统分发验证已在这份最终安装上复验通过。

```powershell
ctest --test-dir build-windows -C Release --output-on-failure
# 安装之后，通过系统 COM 注册和 TSF 按键分发验证实际接入：
./tests/verify_windows_install.ps1
```

完整 Windows CTest **8 / 8 项通过，耗时 9.53 秒**。覆盖全拼、两种双拼、简繁、
会话隔离、敏感会话、短语部署、备份恢复、非法配置及 Unicode 路径。
恢复测试包含“现有短语文件损坏时，仍能恢复有效备份”。
安装器另有失败恢复测试：模拟连续两次系统安装失败，验证保留最后成功版本、
独立记录本次错误，并保持卸载与下一次重试所需的信息：

```powershell
./tests/windows_install_test.ps1 -InstallScript "$PWD/packaging/windows/install.ps1" -PackageDirectory "$PWD/build-windows/package"
```

64 位与 32 位已安装组件均通过真实系统 COM 和 Windows 按键分发验证：
`nihao` 产生预编辑，空格提交“你好”，Esc 取消新组合且不重复提交。
独立 TSF 回归还验证点击选词、不抢焦点、失焦取消、密码/只读上下文、Shift、
快捷键热更新，以及应用不能提供同步编辑锁时按键透传。

已安装组件测试使用自建 `ITextStoreACP` 桌面宿主。后台自动化进程受 Windows 前台焦点规则限制，
因此脚本使用 Windows SDK Debugging Tools 的 x64/x86 CDB 启动测试宿主；
安装与日常输入均不依赖调试器。每次测试创建独立词库并写出明确的 JSON 成败结果，
不把调试器自身的退出码当成测试通过。依据为 Windows 的
[前台窗口规则](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)。

兼容性边界：这些自动化测试不能代替 Word、浏览器、Electron 等实际应用矩阵，
UWP / AppContainer、多显示器和完整缩放矩阵仍需复验。
尚未测得 Windows 按键到候选显示的端到端 P95，不能据此宣称达到 30ms 目标。

TSF 官方接口参考：[Text Services Framework](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework)。
