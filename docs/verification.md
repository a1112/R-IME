# Linux 验证记录

日期：2026-09-28。此记录区分已经运行的自动化检查与尚待实机完成的桌面验收。

## 环境

- Windows 主机上的独立 Debian 13 WSL 2 构建环境，非 KDE 图形会话。
- Intel Core i9-14900K；WSL 可见 32 个逻辑 CPU、31 GiB 内存。
- Linux 5.15.153.1-microsoft-standard-WSL2；GCC 14.2；CMake 3.31.6。
- librime 1.13.1、Fcitx 5.1.12、Qt 6.8.2。
- Cairo 1.18.4、Pango 1.56.3、Wayland client 1.23.1。
- `rime-prelude`、`rime-essay`、Luna Pinyin 和 Double Pinyin 使用 Debian 13 软件包。

## 自动化范围

`ctest --test-dir build --output-on-failure` 包含以下检查：

最终执行结果：**9 / 9 项通过，0 项失败，总耗时 79.45 秒**。全量 CMake 构建通过。
引擎集成套件内含 11 个场景，全部通过；真实 Fcitx 宿主测试通过，包括空格提交不透传、
过期候选点击不重复提交，以及失焦只取消组合。预热后 200 次引擎 IPC 往返的 P95 为
**0.85ms**，测试环境与测量边界见下文。Linux CI 工作流已提供，尚未在远端执行。

本次实际命令：

```sh
cmake --build build -j4
TMPDIR=/dev/shm RIME_TEST_EXEC_TMPDIR=/dev/shm ctest --test-dir build --output-on-failure
```

| 检查 | 实际验证内容 |
|---|---|
| config | 配置校验与原子写入、短语校验、二进制备份、路径穿越与重复条目拒绝 |
| panel-layout | 候选几何、点击命中、屏幕边缘及负坐标屏幕定位 |
| panel-renderer | Cairo/Pango 实际渲染、长文本高度、明暗及横竖排、受限高度滚动 |
| ipc | 私有 socket、请求标识校验、过期回复断开 |
| engine-integration | 真实全拼/小鹤/自然码、简繁、选词、翻页、删除、取消、短语、学习开关、会话隔离、部署回滚、备份恢复、重启恢复、配置修复 |
| settings-backend | 暂存、校验、短语管理与导入导出、失败保留草稿 |
| settings-qml-smoke | 真实 Qt Quick 组件加载，离屏软件渲染 |
| settings-engine | 设置应用通过真实 worker 完成应用、备份、修改、恢复 |
| fcitx-host-integration | 真实插件加载及模拟前端上下文，预编辑/提交/焦点生命周期；不代表真实应用兼容性 |

测试创建独立临时 XDG 目录，不使用个人词库。关闭学习的验证比较
`rime_dict_manager` 导出的词条，而不是 LevelDB 每次打开都会变化的日志文件。
性能测试测量 200 次预热后的 worker Unix socket 往返；它不包含桌面事件分发与显示帧延迟，
不能替代“按键到屏幕候选更新 P95 ≤ 30ms”的端到端验收。

本机 WSL 的持久化磁盘与 Windows 挂载目录出现长时间 I/O 停顿，最终自动化运行将
`TMPDIR` 和 `RIME_TEST_EXEC_TMPDIR` 设为 `/dev/shm`，让临时用户目录与宿主测试的可执行文件
使用内存文件系统。配置原子替换、损坏恢复和回滚逻辑仍被执行，但这不能验证断电后的
实际磁盘持久性，也不能代表安装到真实 Linux 磁盘后的延迟。

截图来自真实 Qt Quick/Cairo 渲染，见 [浅色设置](images/settings-light.png)、
[深色设置](images/settings-dark.png)、[候选窗](images/candidate-light.png)。

安装及卸载已在 `DESTDIR` 暂存目录验证：清单中的 12 个文件均成功安装、卸载后均已移除；
测试用户目录中的哨兵文件保持原内容。此段仅记录 Linux 暂存安装，
Windows 本机安装与验证见 [Windows 指南](windows.md)。

## 本地控制接口回归（2026-10-08）

在同一独立 Debian 13 WSL 2 环境中，从冻结源码完成 Linux 全量构建（66 个构建步骤），
包括 Fcitx 插件、Qt 设置程序、引擎、C++ 控制 SDK 与 `r-ime-ctl`。
工具链为 GCC 14.2、CMake 3.31.6、Ninja 1.12.1；librime 1.13.1、Fcitx 5.1.12、
Qt 6.8.2、nlohmann/json 3.11.3，未新增或修改系统软件包。

本轮 **11 个自动化项目均实际通过**，覆盖上表 9 项及新增 `control-api`、`control-cli`。
首次运行 6 项通过；5 项失败后定位为验证环境问题，并仅重跑这 5 项，全部通过，
重跑耗时 **105.28 秒**。该时间不代表单次 11 项全套耗时。

首次运行将临时 XDG 用户目录放在过长路径下，导致 `engine.sock` 超过 Unix socket
地址长度上限；Qt smoke 另在平台插件枚举阶段出现冷启动读取停顿，未进入 QML。
纠正为短的、权限 0700 的私有 `/dev/shm/ri-4c9a7a61` 临时目录，
并将 `RIME_TEST_EXEC_TMPDIR` 指向同一内存文件系统后，通过原有测试。
没有修改生产代码、测试断言或时限；Qt 离屏 smoke 实际耗时 56.40 秒（上限 60 秒）。
此轮与历史记录相同，不能据此验证持久化磁盘断电恢复或真实桌面启动性能。

新增检查验证发送前输入/帧大小校验、JSON Lines 连接保持和错误后继续请求、
连接间会话隔离、全拼候选与中文提交、配置/短语快照冲突拒绝、
Unicode 文件和短语、备份恢复及断开后不重放操作。
Linux CMake 安装已在私有临时前缀完成，包含控制工具、公共头文件、
三个静态库、导出 CMake package 和 C++/Python 示例。
外部 C++ 项目仅通过已安装前缀的 `find_package(RIME)` 配置、编译和链接成功，
并向真实私有 worker 查询状态；安装后的 Python JSON Lines 示例成功读取候选并提交“你好”。
这两个示例均使用内存文件系统中的独立 XDG 用户数据，验证结束后停止了测试 worker。

## 必须在真实 KDE / Wayland 完成的发布检查

以下项目尚未在当前 Windows/WSL 工作区验收，不能标记通过：

- Kate、Konsole、Firefox、Chromium、VS Code、LibreOffice 的预编辑、提交和快捷键。
- KDE Wayland 原生输入面板位置、鼠标点击不抢焦点、无重复提交。
- 100% / 150% / 200% 缩放、多屏、屏幕边缘及明暗切换。
- 与 fcitx5-rime、其他输入法共存，切换后无残留窗口且原主题不变。
- Wayland/XWayland 混用及对应桌面版本；记录 Qt/GTK/Electron 使用的输入协议。
- 端到端 P95 延迟、恢复部署时的最大停顿及引擎崩溃后的按键透传。

首要验收环境必须实际使用 R-IME 专属候选窗。标准候选窗回退属于恢复行为，
不算专属候选窗验收通过。Wayland 桥接含来自 Fcitx 5.1.12 的内部封装；
升级 Fcitx 后必须重新编译并在图形会话复验。

## 人工复验步骤

1. 按 README 构建、安装，在 KDE 输入法入口添加 R-IME。
2. 每个应用输入 `nihao`，分别用空格、数字、鼠标选择；测试原始 Enter、Esc、Shift。
3. 切换至两种双拼输入“你好”；切换繁体后输入“汉语”；测试页码和方向键。
4. 添加长短语，设置 9 项竖排与 40px 字号，验证滚动和键盘选中项始终可见。
5. 有组合时切换应用/输入法，检查组合取消及专属窗口消失。
6. 导出备份、修改并恢复；关闭学习后输入新词并比较导出的用户词条。
7. 使用全新测试账户验证缺少词库、损坏配置、进程退出、部署失败的恢复行为。

用户文件始终保留在独立 `r-ime` 命名空间。配置修复会保存 `.invalid` 原件；
中断交换保留的 `*.interrupted-*` 目录可用于人工恢复，不应直接删除个人数据。
