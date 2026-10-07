# 本地输入法控制接口

R-IME 提供 C++20 `ControlClient` 和 `r-ime-ctl` 命令行工具。其他语言可通过常驻的 JSON Lines 桥接使用同一接口。通信复用当前用户私有的 Windows 命名管道或 Linux Unix socket，离线运行，无需打开设置窗口。

## 命令行

Windows 安装目录包含 `r-ime-ctl.exe`；在 PowerShell 中执行：

```powershell
$installation = Get-Content "$env:LOCALAPPDATA\R-IME\installation.json" -Raw | ConvertFrom-Json
$ctl = Join-Path $installation.directory 'r-ime-ctl.exe'
& $ctl status
& $ctl config get
& $ctl mode chinese
& $ctl schema flypy
& $ctl config set /general/full_shape true
& $ctl phrases set ceshi '中文控制短语' 100
& $ctl phrases list
& $ctl backup "$env:USERPROFILE\Desktop\r-ime-backup.json"
```

Linux 安装后直接使用 `r-ime-ctl`。源码构建后的 Windows 工具位于 `build-windows/bin/r-ime-ctl.exe`。引擎需要先运行；工具不会自动启动引擎或重放失败的操作。

| 命令 | 行为 |
| --- | --- |
| `status` | 查询有效配置、引擎状态或恢复诊断 |
| `config get` | 读取一致的有效配置与短语 TSV 快照 |
| `config set POINTER JSON` | 修改一个已存在的配置字段；POINTER 使用 JSON Pointer，例如 `/appearance/font_size` |
| `config import FILE` | 应用完整配置 JSON 文件，保留现有短语 |
| `schema pinyin\|flypy\|ziranma` | 设置全拼、小鹤或自然码为默认方案 |
| `mode chinese\|ascii` | 设置默认中文或英文模式 |
| `phrases list` | 输出 `{code,text,weight}` 对象数组 |
| `phrases set CODE TEXT WEIGHT` | 新增或更新一条短语，以编码和文字共同作为标识 |
| `phrases remove CODE TEXT` | 删除对应短语；不存在则报错 |
| `phrases import FILE` | 用 UTF-8 TSV 替换全部自定义短语 |
| `phrases export FILE` | 原子导出 UTF-8 TSV |
| `backup FILE` | 导出配置、短语及用户词库的版本化 JSON 备份 |
| `restore FILE` | 校验、部署并恢复备份，失败保留上一份有效数据 |
| `rpc` | 从标准输入持续读取 JSON Lines，每行输出一个响应并立即刷新 |

`config set` 的值严格按照 JSON 解析：布尔值为 `true` / `false`，整数为 `16`，字符串需要 JSON 引号。PowerShell 的原生程序引号处理因版本而异；复杂配置可以通过 UTF-8 文件使用 `config import`，也可通过下方 Python 桥接传递 JSON。

参数必须放在命令前：`--socket PATH` 指定端点，`--timeout MS` 覆盖请求和部署超时（1～600000 毫秒）。默认普通请求为 2000 毫秒，配置部署、备份和恢复为 180000 毫秒。Windows 的 `--socket` 是用于计算管道名的逻辑 `runtime/engine.sock` 路径，该文件本身不存在。

成功响应包含 `version:1` 和 `ok:true`。失败响应包含 `ok:false` 与 `error:{code,message}`。退出码为：0 成功、1 操作失败、2 命令行参数错误。`--help` 输出说明文字；其他命令的标准输出仅包含 UTF-8 JSON。

## 会话、模式和配置边界

`mode` 和 `schema` 修改持久默认值，不能切换另一个应用当前选用的系统输入法，也不能读取它正在输入的内容。`rpc` 中的 `create` 为调用者建立独立会话，输入操作返回预编辑、候选和提交文本，由调用者决定如何使用文本。

每个输入会话只属于创建它的连接。关闭桥接或调用 `disconnect()` 会清理该连接的会话；另一连接不能继续使用相同会话 ID。连接失败时请求不会自动重试，新连接必须重新创建会话。
传输超时无法确认一项配置或恢复操作是否已经完成；重新连接后先读取最新状态和配置核对，不能直接重复提交同一次修改。

配置、方案、标点默认值、候选数量和短语变更可能触发部署；备份也会短暂关闭并重开引擎以一致地读取用户词库。这些操作可能取消所有当前组合并使会话失效。成功响应中的 `sessions_invalidated:true` 表示应重新创建会话；传输失败也使该连接的会话失效。纯颜色、字体、排列和快捷键调整通常保留会话。不要在其他应用正在组合文字时主动备份或部署。

配置和短语更新使用一次有效快照，发送 `expected_config` 和原始 `expected_phrases`。若其他客户端已修改数据，引擎拒绝本次写入且保留当前会话；调用者应重新读取并让用户决定如何处理冲突。接口不会自动覆盖或重试。文件路径读写使用 UTF-8 / Windows 宽字符路径，导出使用原子替换。

## Python：常驻 JSON Lines 桥接

完整可运行示例见 `examples/control_client.py`，参数为 CLI 的绝对路径。示例使用自己的会话输入 `nihao` 并读取 `你好`，不会向其他应用发送按键或提交文字。

```python
import json
import subprocess

with subprocess.Popen(
    [ctl_path, "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    text=True, encoding="utf-8", bufsize=1,
) as bridge:
    def call(op, **fields):
        bridge.stdin.write(json.dumps({"op": op, **fields}, ensure_ascii=False) + "\n")
        bridge.stdin.flush()
        line = bridge.stdout.readline()
        if not line:
            raise RuntimeError("R-IME bridge disconnected")
        reply = json.loads(line)
        if not reply.get("ok"):
            raise RuntimeError(reply["error"])
        return reply

    session = call("create")["session"]
    state = call("set_schema", session=session, schema="rime_ime_pinyin")
    if not state["simplified"]:
        call("toggle_simplified", session=session)
    call("set_ascii", session=session, value=False)
    for character in "nihao":
        state = call("key", session=session, keycode=ord(character), modifiers=0)
    print(state["candidates"])
    print(call("select", session=session, index=0)["commit"])
    call("destroy", session=session)
```

桥接负责分配 `id` 和 `version`，请求只需携带 `op`、可选 `session` 和该操作的参数。传入的 `id` 会被内部序号替换；显式传入的 `version` 必须为 1。每个输入行对应一个输出行，按请求顺序返回。空行、损坏 JSON、无效请求也产生一条错误响应，随后仍可继续请求。只要有一条请求失败，桥接在 EOF 后以退出码 1 结束。

## C++ API 与构建

```cpp
#include <rime_ime/control.hpp>
#include <iostream>

int main() {
    try {
        rime_ime::ControlClient control;
        std::cout << control.status().dump() << '\n';
        auto result = control.set_config("/appearance/font_size", 18);
        std::cout << result.dump() << '\n';
    } catch (const rime_ime::ControlError& error) {
        std::cerr << error.code() << ": " << error.what() << '\n';
        return 1;
    }
}
```

源码中链接 `rime-ime-control`；安装后使用 `find_package(RIME 0.2 CONFIG REQUIRED)` 与 `target_link_libraries(your_app PRIVATE RIME::control)`。SDK 安装包含控制库、依赖静态库、公共头文件和 CMake package。Linux 需要系统 `nlohmann_json` 开发包；Windows 源码构建所用的 JSON 依赖随 CMake SDK 安装。

`examples/CMakeLists.txt` 是可独立构建的只读 C++ 示例。将 SDK 安装到自己的目录后，使用 `cmake -S examples -B build-control-example -DCMAKE_PREFIX_PATH=SDK目录`，再执行 `cmake --build build-control-example --config Release`。

默认启用 `RIME_BUILD_CONTROL=ON`，可设为 `OFF`。只构建控制工具可关闭 `RIME_BUILD_ENGINE`、`RIME_BUILD_SETTINGS`、`RIME_BUILD_FCITX` 和 `RIME_BUILD_WINDOWS_TSF`，不需要 librime 或 Qt。接口不承诺跨编译器的二进制 ABI；SDK 与应用需要一致的编译器、架构、运行库和 nlohmann/json 版本。

`ControlClient` 持有单个连接，不可复制，非线程安全。每个线程分别使用自己的实例。构造参数可指定逻辑 socket 路径、普通超时和部署超时；`call(request, timeout_ms)` 可为一次请求指定超时。常用方法为 `status`、`configuration`、`configure`、`patch_config`、`set_config`、`phrases`、`replace_phrases`、`upsert_phrase`、`remove_phrase`、`backup`、`restore` 和 `disconnect`。

`ControlError::code()` 区分 `invalid_request`、`conflict`、`engine_error` 和 `transport_error`；其中 `conflict` 表示配置或短语快照已经过期。CLI 文件错误为 `io_error`，参数错误为 `invalid_arguments`。`sessions_invalidated()` 提供会话失效信息。恢复模式下可以读取诊断；配置修改不会自动请求损坏配置修复，需导入有效备份或在设置中显式恢复。

## 底层协议 v1

直接通信通常应使用 SDK 或桥接，以便检查序号和处理平台帧格式。

- Linux：当前用户拥有且仅该用户可读写的 Unix socket，UTF-8 JSON + 换行。检查双方用户身份。
- Windows：带当前用户 SID ACL 的命名管道，每帧为 4 字节小端无符号长度加 UTF-8 JSON，无行分隔符。
- 帧上限 136 MiB，配置 1 MiB、短语 8 MiB。请求为 `{version:1,id:递增正整数,session:0或已拥有的ID,op:操作,...}`；响应回显版本、序号和会话，`create` 返回新 ID。
- `configuration`：会话 0，返回有效 `config`、原始短语 TSV `phrases`、`recovery_required` 和 `diagnostic`。
- `configure`：会话 0，完整 `config`、可选 `phrases`，以及可选 `expected_config` / `expected_phrases`。没有条件字段的旧客户端保持既有行为。
- `backup` / `restore`：会话 0；备份返回 `backup` 对象，恢复请求携带该对象。
- `create(sensitive)` / `destroy` / `status` / `reset`：建立、销毁、查询或取消自己的组合。
- `key(keycode,modifiers)`：使用 Rime/X11 keysym 与 modifier mask；字母为字符码，Enter `0xff0d`、Backspace `0xff08`、Escape `0xff1b`、Space `32`；Shift 为 1、Control 为 4、Alt 为 8、Super 为 64、release 为 `1<<30`。
- `select(index)`：选择本页候选，索引从 0 开始；`page(backward)` / `navigate(backward)`：翻页或移动选中项。
- `set_schema(schema)`：仅自己的会话，值为 `rime_ime_pinyin`、`rime_ime_flypy` 或 `rime_ime_ziranma`。
- `set_ascii(value)` / `set_full_shape(value)` / `set_ascii_punctuation(value)`：布尔值，操作自己的会话；切到英文时提交原始组合一次。
- `toggle_ascii` / `toggle_full_shape` / `toggle_ascii_punctuation` / `toggle_simplified`：会话模式切换。

底层引擎的错误字段为字符串；SDK 将其封装为异常，CLI 再转成上述结构化错误对象。输入快照还返回 `handled`、`commit`、`preedit`、`cursor`、`candidates`、`highlighted`、`page`、`last_page`、`ascii`、`full_shape`、`ascii_punctuation`、`simplified` 和 `generation`。这些接口不会列出或读取其他连接的会话。

默认目录沿用 Windows `%LOCALAPPDATA%\R-IME` 和 Linux XDG 约定。开发测试中，Windows 可使用绝对路径 `RIME_USER_ROOT` 隔离数据，Linux 使用 `XDG_CONFIG_HOME`、`XDG_DATA_HOME`、`XDG_CACHE_HOME` 和 `XDG_RUNTIME_DIR`。不要将测试引擎指向正在使用的真实词库。
