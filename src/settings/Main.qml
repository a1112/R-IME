import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 860; height: 620
    minimumWidth: 760; minimumHeight: 560
    visible: false
    title: "R-IME 设置"
    property var pages: ["general", "schemes", "phrases", "shortcuts", "appearance", "backup"]
    property var titles: ["常规", "输入方案", "词库与短语", "快捷键", "外观", "备份与恢复"]
    property var subtitles: ["让每一次输入，都合乎习惯。", "选择适合你的拼音方式。", "把常用的表达，变成你的词库。", "用熟悉的按键，保持输入节奏。", "一个安静、清晰的候选窗口。", "妥善保留你的输入习惯。"]
    property int page: Math.max(0, pages.indexOf(initialPage))
    property bool dark: (previewTheme !== "system" ? previewTheme : settings.config.appearance.theme) === "dark"
                        || ((previewTheme === "system" && settings.config.appearance.theme === "system") && Application.styleHints.colorScheme === Qt.Dark)
    readonly property color ink: dark ? "#f1f2f5" : "#252936"
    readonly property color muted: dark ? "#a6acb9" : "#777e8e"
    readonly property color surface: dark ? "#292c33" : "#ffffff"
    readonly property color borderColor: dark ? "#3b3f49" : "#e6e8ed"
    readonly property color accent: "#3478f6"
    property string fileAction: ""
    property url pendingBackup
    property int editingPhrase: -1
    property bool allowClose: false
    color: dark ? "#22252c" : "#f7f8fa"
    font.family: Qt.platform.os === "windows" ? "Microsoft YaHei UI" : Qt.platform.os === "osx" ? "PingFang SC" : "Noto Sans CJK SC"
    font.pixelSize: 14
    palette.window: color
    palette.windowText: ink
    palette.text: ink
    palette.base: surface
    palette.button: surface
    palette.buttonText: ink
    palette.highlight: accent
    palette.highlightedText: "white"
    palette.placeholderText: muted
    palette.dark: muted
    palette.mid: borderColor
    palette.light: surface
    onClosing: function(close) {
        if (settings.busy) { close.accepted = false; return }
        if (settings.dirty && !allowClose) { close.accepted = false; closeDialog.open() }
    }
    function option(group, key) { return settings.config[group][key] }
    function chooseFile(action) {
        fileAction = action
        filePicker.title = action === "importPhrases" ? "导入自定义短语" : action === "exportPhrases" ? "导出自定义短语" : action === "exportBackup" ? "导出 R-IME 备份" : "选择要恢复的备份"
        filePicker.fileMode = action.startsWith("export") ? FileDialog.SaveFile : FileDialog.OpenFile
        filePicker.nameFilters = action.endsWith("Phrases") ? ["短语文件 (*.tsv)", "所有文件 (*)"] : ["R-IME 备份 (*.json)", "所有文件 (*)"]
        filePicker.defaultSuffix = action.endsWith("Phrases") ? "tsv" : "json"
        filePicker.open()
    }
    function editPhrase(index, phrase) {
        editingPhrase = index; phraseCode.text = phrase ? phrase.code : ""
        phraseText.text = phrase ? phrase.text : ""; phraseWeight.value = phrase ? phrase.weight : 100
        phraseDialog.open(); phraseCode.forceActiveFocus()
    }

    component FormField: TextField {
        implicitHeight: 36; leftPadding: 10; rightPadding: 10
        color: window.ink; placeholderTextColor: window.muted
        selectionColor: window.accent; selectedTextColor: "white"
        background: Rectangle { radius: 6; color: window.dark ? "#23262e" : "white"; border.width: parent.activeFocus ? 2 : 1; border.color: parent.activeFocus ? window.accent : window.borderColor }
    }
    component ChoiceField: ComboBox {
        implicitHeight: 36
        background: Rectangle { radius: 6; color: window.dark ? "#23262e" : "white"; border.width: parent.activeFocus ? 2 : 1; border.color: parent.activeFocus ? window.accent : window.borderColor }
    }
    component Card: Rectangle {
        default property alias content: contentColumn.data
        implicitHeight: contentColumn.implicitHeight + 28
        Layout.fillWidth: true
        radius: 10; color: window.surface; border.color: window.borderColor
        ColumnLayout { id: contentColumn; anchors { left: parent.left; right: parent.right; top: parent.top; margins: 14 } spacing: 12 }
    }
    component SectionTitle: Label { color: window.muted; font.pixelSize: 12; font.weight: Font.DemiBold; topPadding: 8 }
    component Note: Label { Layout.fillWidth: true; color: window.muted; font.pixelSize: 12; wrapMode: Text.WordWrap; lineHeight: 1.4; textFormat: Text.PlainText }
    component Line: Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: window.borderColor }
    component ToggleRow: RowLayout {
        property string label
        property string description
        property string configKey
        Layout.fillWidth: true; spacing: 18
        ColumnLayout {
            Layout.fillWidth: true; spacing: 4
            Label { text: label; color: window.ink; font.weight: Font.Medium }
            Note { text: description }
        }
        Switch {
            checked: window.option("general", configKey)
            onToggled: settings.setValue("general", configKey, checked)
            indicator: Rectangle {
                implicitWidth: 38; implicitHeight: 23
                x: parent.leftPadding; y: (parent.height-height)/2
                radius: 12; color: parent.checked ? window.accent : window.dark ? "#555a66" : "#ced2da"
                Rectangle { width: 19; height: 19; radius: 10; y: 2; x: parent.parent.checked ? 17 : 2; color: "white"; Behavior on x { NumberAnimation { duration: 100 } } }
            }
        }
    }
    component ActionButton: Button {
        property bool primary: false
        implicitHeight: 34; leftPadding: 16; rightPadding: 16
        contentItem: Text { text: parent.text; font: parent.font; color: !parent.enabled ? window.muted : parent.primary ? "white" : window.ink; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 7; border.color: parent.primary ? "transparent" : window.borderColor; color: !parent.enabled ? (window.dark ? "#363a43" : "#eceef2") : parent.primary ? (parent.down ? "#2464d7" : window.accent) : (parent.hovered ? (window.dark ? "#3a3e47" : "#f1f3f8") : window.surface) }
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 196
            color: window.dark ? "#1b1e25" : "#eceef3"
            Rectangle { width: 1; anchors { right: parent.right; top: parent.top; bottom: parent.bottom } color: window.borderColor }
            ColumnLayout {
                anchors { fill: parent; margins: 16 } spacing: 6
                RowLayout {
                    Layout.topMargin: 13; Layout.bottomMargin: 26; spacing: 10
                    Rectangle { width: 38; height: 38; radius: 11; color: window.accent; Label { anchors.centerIn: parent; text: "R"; font.pixelSize: 24; font.weight: Font.DemiBold; color: "white" } }
                    Column { spacing: 1; Label { text: "R-IME"; color: window.ink; font.pixelSize: 18; font.weight: Font.DemiBold } Label { text: "你的输入方式"; color: window.muted; font.pixelSize: 11 } }
                }
                Repeater {
                    model: window.titles
                    delegate: ItemDelegate {
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true; implicitHeight: 41
                        text: modelData
                        onClicked: window.page = index
                        background: Rectangle { radius: 7; color: window.page === index ? (window.dark ? "#314666" : "#dbe7fd") : parent.hovered ? (window.dark ? "#292e38" : "#e4e7ee") : "transparent" }
                        contentItem: RowLayout {
                            spacing: 11
                            Label { text: ["◉", "拼", "词", "⌘", "◐", "↺"][index]; color: window.page === index ? (window.dark ? "#82afff" : "#2568d8") : window.muted; font.pixelSize: 16; Layout.preferredWidth: 22; horizontalAlignment: Text.AlignHCenter }
                            Label { text: modelData; color: window.page === index ? (window.dark ? "#c7ddff" : "#225db9") : window.ink; font.weight: window.page === index ? Font.DemiBold : Font.Normal; Layout.fillWidth: true }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Rectangle { Layout.fillWidth: true; height: 1; color: window.borderColor; Layout.bottomMargin: 10 }
                Label { text: "R-IME  0.2.0"; color: window.muted; font.pixelSize: 11 }
                Label { text: "本地运行 · 专注输入"; color: window.muted; font.pixelSize: 11; Layout.bottomMargin: 6 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
            ColumnLayout {
                Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; Layout.topMargin: 28; Layout.bottomMargin: 20; spacing: 6
                Label { text: window.titles[window.page]; color: window.ink; font.pixelSize: 25; font.weight: Font.DemiBold }
                Label { text: window.subtitles[window.page]; color: window.muted; font.pixelSize: 13 }
            }
            ScrollView {
                id: scroll
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: scroll.availableWidth; spacing: 14
                    enabled: !settings.busy
                    ColumnLayout {
                        visible: window.page === 0
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "输入偏好" }
                        Card {
                            ToggleRow { label: "默认英文输入"; description: "启用后，新输入会话使用英文模式。"; configKey: "ascii" }
                            Line {}
                            ToggleRow { label: "使用简体中文"; description: "关闭后输出繁体中文。"; configKey: "simplified" }
                            Line {}
                            ToggleRow { label: "使用英文标点"; description: "在中文模式下，也使用半角英文标点。"; configKey: "ascii_punctuation" }
                        }
                        SectionTitle { text: "词频与隐私" }
                        Card { ToggleRow { label: "学习我的用词习惯"; description: "在本机调整候选顺序。密码等敏感输入不会学习。"; configKey: "learning" } }
                        Note { text: "使用桌面输入法菜单切换 R-IME；单独按 Shift 可切换中英文。" }
                    }
                    ColumnLayout {
                        visible: window.page === 1
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "拼音方案" }
                        Repeater {
                            model: [{name:"全拼",id:"rime_ime_pinyin",detail:"完整拼音 · 熟悉、自然",sample:"你好  →  ni hao"},{name:"小鹤双拼",id:"rime_ime_flypy",detail:"两键一音 · 高频韵母布局",sample:"你好  →  ni hc"},{name:"自然码双拼",id:"rime_ime_ziranma",detail:"经典布局 · 两键一音",sample:"你好  →  ni hk"}]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true; implicitHeight: 81; radius: 10
                                color: window.surface
                                border.width: window.option("input","schema") === modelData.id ? 2 : 1
                                border.color: window.option("input","schema") === modelData.id ? window.accent : window.borderColor
                                RowLayout {
                                    anchors { fill: parent; margins: 16 } spacing: 12
                                    RadioButton { checked: window.option("input","schema") === modelData.id; onClicked: settings.setValue("input","schema",modelData.id); Accessible.name: modelData.name }
                                    ColumnLayout { Layout.fillWidth: true; spacing: 5; Label { text: modelData.name; color: window.ink; font.weight: Font.DemiBold } Note { text: modelData.detail } }
                                    Label { text: modelData.sample; color: window.muted; font.pixelSize: 12 }
                                }
                            }
                        }
                        Card {
                            Label { text: "双拼键位速查"; color: window.ink; font.weight: Font.DemiBold }
                            Note { text: "声母：zh → v，ch → i，sh → u。输入每个音节的声母键，再输入韵母键。" }
                            Note { text: window.option("input","schema") === "rime_ime_ziranma" ? "自然码：ai → l　ei → z　ao → k　ou → b\nan → j　en → f　ang → h　eng → g\nian → m　iao → c　ing → y　iong / ong → s\n零声母：ai / an 可原样输入；也支持 ai → al、an → aj、ang → ah。" : "小鹤：ai → d　ei → w　ao → c　ou → z\nan → j　en → f　ang → h　eng → g\nian → m　iao → n　ing → k　iong / ong → s\n零声母：单字母双击；两字母原样，如 a → aa、ai → ai；ang → ah。" }
                        }
                    }
                    ColumnLayout {
                        visible: window.page === 2
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        RowLayout { Layout.fillWidth: true; FormField { id: phraseSearch; Layout.fillWidth: true; placeholderText: "搜索编码或短语"; selectByMouse: true } ActionButton { text: "+ 添加"; primary: true; onClicked: window.editPhrase(-1,null) } }
                        Card {
                            RowLayout { Layout.fillWidth: true; Label { text: "编码"; color: window.muted; Layout.preferredWidth: 110 } Label { text: "短语"; color: window.muted; Layout.fillWidth: true } Label { text: "权重"; color: window.muted; Layout.preferredWidth: 55 } Item { implicitWidth: 82 } }
                            Line {}
                            Label { visible: settings.phrases.length === 0; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; topPadding: 28; bottomPadding: 28; text: "还没有自定义短语\n添加常用表达，输入更轻松。"; color: window.muted; lineHeight: 1.8 }
                            ListView {
                                id: phraseList
                                Layout.fillWidth: true
                                implicitHeight: Math.min(240,count * 38)
                                clip: true
                                ScrollBar.vertical: ScrollBar {}
                                model: {
                                    const all = settings.phrases
                                    const result = []
                                    for(let i = 0; i < all.length; ++i) {
                                        if(!phraseSearch.text || all[i].code.includes(phraseSearch.text) || all[i].text.includes(phraseSearch.text))
                                            result.push({sourceIndex:i, code:all[i].code, text:all[i].text, weight:all[i].weight})
                                    }
                                    return result
                                }
                                delegate: RowLayout {
                                    required property var modelData
                                    width: ListView.view.width; height: 38; spacing: 10
                                    Label { text: modelData.code; color: window.muted; Layout.preferredWidth: 110; elide: Text.ElideRight }
                                    Label { text: modelData.text; color: window.ink; Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText }
                                    Label { text: modelData.weight; color: window.muted; Layout.preferredWidth: 55; font.pixelSize: 12 }
                                    ToolButton { text: "编辑"; onClicked: window.editPhrase(modelData.sourceIndex,modelData); Accessible.name: "编辑 " + modelData.text }
                                    ToolButton { text: "×"; onClicked: settings.removePhrase(modelData.sourceIndex); Accessible.name: "删除 " + modelData.text }
                                }
                            }
                            Note { visible: settings.phrases.length > 0 && phraseList.count === 0; text: "没有找到匹配的短语。" }
                        }
                        RowLayout { ActionButton { text: "导入 TSV…"; onClicked: window.chooseFile("importPhrases") } ActionButton { text: "导出 TSV…"; onClicked: window.chooseFile("exportPhrases") } Item { Layout.fillWidth: true } Label { text: settings.phrases.length + " 条短语"; color: window.muted; font.pixelSize: 12 } }
                        Note { text: "UTF-8 文本，每行包含编码、短语、权重，以 Tab 分隔。导入会合并到当前列表；所有短语变更在“应用”后生效。用户学习词库可通过完整备份导出。" }
                    }
                    ColumnLayout {
                        visible: window.page === 3
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "输入法内部快捷键" }
                        Card {
                            Repeater {
                                model: [{key:"toggle_ascii",label:"切换中英文"},{key:"next_page",label:"下一页候选"},{key:"previous_page",label:"上一页候选"},{key:"cancel",label:"取消当前组合"}]
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    Label { text: modelData.label; color: window.ink; Layout.fillWidth: true }
                                    FormField { Layout.preferredWidth: 210; text: window.option("shortcuts",modelData.key); selectByMouse: true; onEditingFinished: { if(!settings.setValue("shortcuts",modelData.key,text)) text = Qt.binding(function(){ return window.option("shortcuts",modelData.key) }) } Accessible.name: modelData.label }
                                }
                            }
                        }
                        Note { text: "键名示例：Shift、Page_Down、Page_Up、Escape、Control+j。使用 Control / Alt / Shift / Super 和 + 组合。重复绑定及常见桌面冲突会被拒绝。" }
                        SectionTitle { text: "常用操作" }
                        Card {
                            Repeater { model: ["空格  ·  选择当前候选", "数字键  ·  选择本页对应候选", "↑ / ↓  ·  移动候选选中项", "← / →  ·  移动输入串光标", "Enter  ·  提交原始输入串"]; delegate: Label { required property string modelData; text: modelData; color: window.muted; font.pixelSize: 13 } }
                        }
                        Note { text: Qt.platform.os === "windows" ? "使用 Windows 输入法菜单或 Win + 空格切换系统输入法。" : "系统输入法切换由 Fcitx 5 与桌面快捷键设置管理。" }
                    }
                    ColumnLayout {
                        visible: window.page === 4
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "实时预览" }
                        Rectangle {
                            Layout.fillWidth: true; implicitHeight: Math.max(132,candidatePreview.implicitHeight+44)
                            radius: 12; color: window.dark ? "#191c23" : "#eaeef5"
                            Rectangle {
                                id: candidatePreview
                                anchors.centerIn: parent; width: Math.min(parent.width-32,candidates.implicitWidth+24); implicitHeight: candidates.implicitHeight+20
                                radius: 10; color: window.dark ? "#292c33" : "#fcfcfd"; border.color: window.dark ? "#444955" : "#d3d8e2"
                                GridLayout {
                                    id: candidates; anchors.centerIn: parent; columns: window.option("appearance","orientation") === "vertical" ? 1 : Math.max(1,Math.min(window.option("appearance","page_size"),Math.floor((scroll.availableWidth-112)/(window.option("appearance","font_size")*3.8+20)))); columnSpacing: 3; rowSpacing: 3
                                    Repeater {
                                        model: window.option("appearance","page_size")
                                        delegate: Rectangle {
                                            required property int index
                                            implicitWidth: candidateText.implicitWidth+16; implicitHeight: candidateText.implicitHeight+12; radius: 6
                                            color: index === 0 ? window.accent : "transparent"
                                            Label { id: candidateText; anchors.centerIn: parent; text: (index+1)+"  "+["你好","您好","拟好","你","呢","泥","妮","倪","霓"][index]; color: index === 0 ? "white" : window.ink; font.pixelSize: window.option("appearance","font_size"); font.family: window.option("appearance","font") || window.font.family }
                                        }
                                    }
                                }
                            }
                        }
                        Card {
                            RowLayout { Layout.fillWidth: true; Label { text: "明暗模式"; color: window.ink; Layout.fillWidth: true } ChoiceField { model: ["跟随系统","浅色","深色"]; currentIndex: ["system","light","dark"].indexOf(window.option("appearance","theme")); onActivated: settings.setValue("appearance","theme",["system","light","dark"][currentIndex]); Layout.preferredWidth: 190 } }
                            Line {}
                            RowLayout { Layout.fillWidth: true; Label { text: "字体"; color: window.ink; Layout.fillWidth: true } FormField { placeholderText: "系统字体"; text: window.option("appearance","font"); onEditingFinished: settings.setValue("appearance","font",text); Layout.preferredWidth: 190 } }
                            RowLayout { Layout.fillWidth: true; Label { text: "文字大小"; color: window.ink; Layout.fillWidth: true } SpinBox { from: 10; to: 40; value: window.option("appearance","font_size"); onValueModified: settings.setValue("appearance","font_size",value); editable: true; Layout.preferredWidth: 130 } Label { text: "px"; color: window.muted } }
                            RowLayout { Layout.fillWidth: true; Label { text: "每页候选数"; color: window.ink; Layout.fillWidth: true } SpinBox { from: 3; to: 9; value: window.option("appearance","page_size"); onValueModified: settings.setValue("appearance","page_size",value); Layout.preferredWidth: 153 } }
                            RowLayout { Layout.fillWidth: true; Label { text: "排列方式"; color: window.ink; Layout.fillWidth: true } ChoiceField { model: ["横排","竖排"]; currentIndex: window.option("appearance","orientation") === "vertical" ? 1 : 0; onActivated: settings.setValue("appearance","orientation",currentIndex ? "vertical" : "horizontal"); Layout.preferredWidth: 190 } }
                        }
                        Note { text: "预览使用示例候选。实际候选窗随显示器缩放，并自动避让屏幕边缘。" }
                    }
                    ColumnLayout {
                        visible: window.page === 5
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "数据备份" }
                        Card {
                            Label { text: "带走你的输入习惯"; color: window.ink; font.weight: Font.DemiBold; font.pixelSize: 16 }
                            Note { text: "完整备份包含已应用的设置、自定义短语和用户学习词库。引擎会暂停词库写入，保证备份一致。备份文件仅保存在你选择的位置。" }
                            RowLayout { ActionButton { text: "导出备份…"; primary: true; enabled: !settings.dirty; onClicked: window.chooseFile("exportBackup") } ActionButton { text: "从备份恢复…"; onClicked: window.chooseFile("importBackup") } }
                            Note { visible: settings.dirty; text: "请先应用或放弃当前修改，再导出完整备份。" }
                        }
                        SectionTitle { text: "恢复默认设置" }
                        Card { Note { text: "将输入偏好、方案、快捷键和外观恢复为默认值。保留自定义短语及学习词库；点击“应用”后生效。" } ActionButton { text: "恢复默认设置"; onClicked: settings.restoreDefaults() } }
                        SectionTitle { text: "运行状态" }
                        Card { Note { text: settings.diagnostic || "候选窗的显示状态将在输入法首次激活后显示。" } ActionButton { text: "检查引擎连接"; onClicked: settings.refreshStatus() } }
                    }
                    Item { implicitHeight: 18 }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: window.borderColor }
            ColumnLayout {
                Layout.fillWidth: true; Layout.leftMargin: 24; Layout.rightMargin: 24; Layout.topMargin: 10; Layout.bottomMargin: 14; spacing: 7
                Label { visible: settings.error.length > 0; text: settings.error; color: window.dark ? "#ffad9f" : "#b64131"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12; maximumLineCount: 3; elide: Text.ElideRight }
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    BusyIndicator { running: settings.busy; visible: running; implicitWidth: 22; implicitHeight: 22 }
                    Label { text: settings.busy ? settings.status : settings.dirty ? "有未应用的更改" : settings.status || "设置已加载"; color: window.muted; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                    ActionButton { text: "放弃更改"; enabled: settings.dirty && !settings.busy; onClicked: settings.discard() }
                    ActionButton { text: settings.busy ? "处理中…" : "应用"; primary: true; enabled: settings.dirty && !settings.busy; onClicked: settings.apply() }
                }
            }
        }
    }

    FileDialog {
        id: filePicker
        onAccepted: {
            if(window.fileAction === "importPhrases") settings.importPhrases(selectedFile)
            else if(window.fileAction === "exportPhrases") settings.exportPhrases(selectedFile)
            else if(window.fileAction === "exportBackup") settings.exportBackup(selectedFile)
            else { window.pendingBackup = selectedFile; restoreDialog.open() }
        }
    }
    Dialog {
        id: phraseDialog; title: window.editingPhrase < 0 ? "添加自定义短语" : "编辑自定义短语"
        anchors.centerIn: parent; width: 430; modal: true
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: "编码"; color: window.ink }
            FormField { id: phraseCode; Layout.fillWidth: true; placeholderText: "小写字母，如 nihao"; selectByMouse: true }
            Label { text: "短语"; color: window.ink }
            FormField { id: phraseText; Layout.fillWidth: true; placeholderText: "输入要保存的文字"; selectByMouse: true }
            RowLayout { Label { text: "权重"; color: window.ink; Layout.fillWidth: true } SpinBox { id: phraseWeight; from: 0; to: 1000000; editable: true; Layout.preferredWidth: 190 } }
            Note { text: "权重越高，候选越靠前。保存后点击主窗口“应用”生效。" }
            Label { visible: settings.error.length > 0; text: settings.error; color: "#be4839"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12 }
        }
        footer: DialogButtonBox {
            Button { text: "取消"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: phraseDialog.close() }
            Button { text: "保存"; DialogButtonBox.buttonRole: DialogButtonBox.ActionRole; onClicked: if(settings.upsertPhrase(window.editingPhrase,phraseCode.text,phraseText.text,phraseWeight.value)) phraseDialog.close() }
        }
    }
    Dialog {
        id: restoreDialog; title: "从备份恢复？"; anchors.centerIn: parent; width: 430; modal: true
        contentItem: Label { text: "备份将替换当前设置、自定义短语和用户学习词库。未应用的更改也会被替换。建议先导出一份当前备份。"; color: window.ink; wrapMode: Text.WordWrap }
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: settings.importBackup(window.pendingBackup)
    }
    Dialog {
        id: closeDialog; title: "放弃未应用的更改？"; anchors.centerIn: parent; width: 360; modal: true
        contentItem: Label { text: "尚有更改未应用。关闭窗口会放弃这些更改。"; color: window.ink; wrapMode: Text.WordWrap }
        standardButtons: Dialog.Cancel | Dialog.Discard
        onDiscarded: { window.allowClose = true; window.close() }
    }
}
