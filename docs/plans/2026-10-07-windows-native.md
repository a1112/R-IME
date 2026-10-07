# Windows 11 输入法改造与审查

**目标：** 按 Windows 11 微软拼音的系统入口、候选操作和设置习惯改造 R-IME 的 Windows 实现。

**架构：** 保留隔离的 Rime 引擎、独立词库和 TSF DLL。Windows 专用界面采用 Fluent 设置控件和紧凑候选窗；Linux 的现有界面和配置兼容性继续保留。

**技术：** C++20、Windows TSF/COM、Win32 GDI/DWM、Qt Quick FluentWinUI3、librime。

## 审查依据

- [微软拼音](https://support.microsoft.com/windows/microsoft-simplified-chinese-ime)
- [Windows 自定义输入法要求](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)
- [TSF UI-less 候选接管](https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview)
- [Qt FluentWinUI3](https://doc.qt.io/qt-6/qtquickcontrols-fluentwinui3.html)

## 实施与验收

1. 审查 `service.cpp` / `registration.cpp`。先在 `windows_tsf_test.cpp` 增加公开 COM 接口契约失败测试，再实现 UIElement 生命周期、系统输入模式 compartment 同步、`GUID_LBI_INPUTMODE` 的中/A图标和布局变化处理。品牌图标仍用于 R-IME 的独立输入法入口。
2. 在核心行为测试中先覆盖上下逐项导航、当前项空格提交、全半角和中英标点切换、旧配置兼容，再修改 `engine.cpp` 和 `config.cpp`。常用 Windows 按键包含 Shift、Shift+Space、Ctrl+句号、数字/空格、PageUp/PageDown、Enter/Esc。
3. 改造 `panel.cpp`：紧凑候选列表、分离的灰色序号、浅色选中底板与系统强调色指示条、上一页/下一页；保留无激活点击和旧帧防重复提交。跟随 Windows 明暗、高对比度和缩放，限制在显示器工作区内；溢出候选可滚动。
4. 为 Windows 新增独立 QML 页面，使用真实 FluentWinUI3 控件与 Windows 设置式导航/卡片。六页现有功能全部可用，外观预览与候选窗一致。Linux 继续加载原页面。
5. 构建 x64/x86 DLL、共享引擎和设置；运行核心/配置/面板/QML/COM 契约测试与真实 TSF 系统分发验证。截图核验浅深色设置和候选窗。更新本机安装并确认注册与运行进程指向本轮版本。

## 边界

“类似 Windows 原输入法”作为外观、常用键盘操作和公开系统接口的目标。微软私有的云词库、预测算法、表情搜索、语音及手写服务没有公开的等价复用接口；本项目不宣称与微软拼音的所有功能、词频或每个像素完全等价。系统设置内的微软拼音页面属于 Windows 本身，R-IME 使用自己的 Fluent 设置窗口。

Windows 图形验收应分别记录真实应用和自建 TSF 宿主。契约测试不能代替应用输入测试；遇到测试宿主或首次部署失败须保留具体失败结果，不将调试器退出码视为成功。
