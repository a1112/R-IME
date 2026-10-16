pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.FluentWinUI3 as Fluent
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 860; height: 620
    minimumWidth: 760; minimumHeight: 520
    visible: false
    title: "R-IME 设置"
    property var pages: ["general", "schemes", "phrases", "shortcuts", "appearance", "backup"]
    property var titles: ["常规", "输入方案", "词库和自学习", "按键", "外观", "备份和恢复"]
    property var subtitles: ["设置默认输入模式、字符集和标点。", "选择全拼或双拼方案。", "管理自定义短语和本机用户词库。", "设置模式切换和候选操作按键。", "设置候选窗口的显示方式。", "备份设置和用户词库，或恢复默认设置。"]
    property int page: Math.max(0, pages.indexOf(initialPage))
    readonly property string requestedTheme: previewTheme !== "system" ? previewTheme : settings.config.appearance.theme
    readonly property bool dark: Application.styleHints.colorScheme === Qt.Dark
    readonly property bool highContrast: Application.styleHints.accessibility.contrastPreference === Qt.HighContrast
    SystemPalette { id: systemPalette; colorGroup: SystemPalette.Active }
    readonly property color ink: highContrast ? systemPalette.windowText : dark ? "#ffffff" : "#1b1b1b"
    readonly property color muted: highContrast ? systemPalette.windowText : dark ? "#c5c5c5" : "#606060"
    readonly property color surface: highContrast ? systemPalette.base : dark ? "#2d2d2d" : "#ffffff"
    readonly property color borderColor: highContrast ? systemPalette.windowText : dark ? "#404040" : "#e5e5e5"
    readonly property color accent: highContrast ? systemPalette.highlight : dark ? "#60cdff" : "#0067c0"
    readonly property color selectionColor: highContrast ? systemPalette.highlight : dark ? "#3a3a3a" : "#ebebeb"
    readonly property color selectionInk: highContrast ? systemPalette.highlightedText : ink
    property string fileAction: ""
    property url pendingBackup
    property int editingPhrase: -1
    property bool allowClose: false
    color: highContrast ? systemPalette.window : dark ? "#202020" : "#f3f3f3"
    font.pixelSize: 14
    palette.window: color
    palette.windowText: ink
    palette.text: ink
    palette.base: surface
    palette.button: surface
    palette.buttonText: ink
    palette.highlight: accent
    palette.highlightedText: highContrast ? systemPalette.highlightedText : dark ? "black" : "white"
    palette.placeholderText: muted
    palette.dark: muted
    palette.mid: borderColor
    palette.light: surface
    onRequestedThemeChanged: applyTheme()
    onPageChanged: Qt.callLater(function() { scroll.contentItem.contentY = 0 })
    Component.onCompleted: applyTheme()
    function applyTheme() {
        // Unknown resets Qt's override so the Fluent controls follow Windows again.
        Application.styleHints.colorScheme = requestedTheme === "dark" ? Qt.Dark : requestedTheme === "light" ? Qt.Light : Qt.Unknown
    }
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
        implicitHeight: 36
    }
    component ChoiceField: ComboBox {
        implicitHeight: 36
    }
    component Card: Rectangle {
        default property alias content: contentColumn.data
        implicitHeight: contentColumn.implicitHeight + 32
        Layout.fillWidth: true
        radius: 8; color: window.surface; border.color: window.borderColor
        ColumnLayout { id: contentColumn; anchors { left: parent.left; right: parent.right; top: parent.top; margins: 16 } spacing: 16 }
    }
    component SectionTitle: Label { color: window.ink; font.pixelSize: 14; font.weight: Font.DemiBold; topPadding: 8 }
    component Note: Label { Layout.fillWidth: true; color: window.muted; font.pixelSize: 12; wrapMode: Text.WordWrap; lineHeight: 1.4; textFormat: Text.PlainText }
    component Line: Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: window.borderColor }
    component ToggleRow: RowLayout {
        id: toggleRow
        property string label
        property string description
        property string configKey
        Layout.fillWidth: true; spacing: 18
        ColumnLayout {
            Layout.fillWidth: true; spacing: 4
            Label { text: toggleRow.label; color: window.ink; font.weight: Font.Medium }
            Note { text: toggleRow.description }
        }
        Fluent.Switch {
            checked: window.option("general", toggleRow.configKey)
            onToggled: settings.setValue("general", toggleRow.configKey, checked)
            Accessible.name: toggleRow.label
        }
    }
    component ActionButton: Fluent.Button {
        property bool primary: false
        implicitHeight: 34; leftPadding: 16; rightPadding: 16
        highlighted: primary
    }

    RowLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 224
            color: window.color
            ColumnLayout {
                anchors { fill: parent; margins: 12 } spacing: 4
                RowLayout {
                    Layout.topMargin: 18; Layout.leftMargin: 12; Layout.bottomMargin: 26; spacing: 12
                    Image { source: "qrc:/branding/r-ime.svg"; sourceSize.width: 32; sourceSize.height: 32; Layout.preferredWidth: 32; Layout.preferredHeight: 32 }
                    Label { text: "R-IME 设置"; color: window.ink; font.pixelSize: 18; font.weight: Font.DemiBold }
                }
                Repeater {
                    model: window.titles
                    delegate: ItemDelegate {
                        id: navigationItem
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true; implicitHeight: 40
                        text: modelData
                        onClicked: window.page = index
                        background: Rectangle {
                            radius: 4; color: window.page === navigationItem.index ? window.selectionColor : navigationItem.hovered ? (window.dark ? "#303030" : "#e9e9e9") : "transparent"
                            Rectangle { visible: window.page === navigationItem.index; width: 3; height: 16; radius: 1.5; color: window.accent; anchors { left: parent.left; verticalCenter: parent.verticalCenter } }
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Label { text: ["\uE713", "\uE765", "\uE8F1", "\uE92E", "\uE771", "\uE777"][index]; font.family: "Segoe Fluent Icons"; color: window.page === index ? window.selectionInk : window.ink; font.pixelSize: 16; Layout.preferredWidth: 24; horizontalAlignment: Text.AlignHCenter }
                            Label { text: modelData; color: window.page === index ? window.selectionInk : window.ink; Layout.fillWidth: true }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: window.borderColor; Layout.bottomMargin: 10 }
                Label { text: "R-IME  0.2.0"; color: window.muted; font.pixelSize: 11 }
                Label { text: "离线输入法"; color: window.muted; font.pixelSize: 12; Layout.bottomMargin: 6 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
            ColumnLayout {
                Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; Layout.topMargin: 24; Layout.bottomMargin: 20; spacing: 8
                RowLayout {
                    Label { text: "R-IME"; color: window.muted; font.pixelSize: 14 }
                    Label { text: "\uE76C"; font.family: "Segoe Fluent Icons"; color: window.muted; font.pixelSize: 12 }
                    Label { text: window.titles[window.page]; color: window.ink; font.pixelSize: 14 }
                }
                Label { text: window.titles[window.page]; color: window.ink; font.pixelSize: 28; font.weight: Font.DemiBold }
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
                            ToggleRow { label: "使用英文标点"; description: "中文模式下使用英文标点。按 Ctrl + . 切换标点模式。"; configKey: "ascii_punctuation" }
                            Line {}
                            ToggleRow { label: "使用全角字符"; description: "将字母、数字和空格转换为全角。按 Shift + 空格切换。"; configKey: "full_shape" }
                        }
                        Card {
                            Label { text: "默认输入状态"; color: window.ink; font.weight: Font.DemiBold }
                            RowLayout {
                                spacing: 24
                                Label { text: window.option("general", "ascii") ? "A  英文" : "中  中文"; color: window.ink }
                                Label { text: window.option("general", "full_shape") ? "全角" : "半角"; color: window.ink }
                                Label { text: window.option("general", "ascii_punctuation") ? "英文标点" : "中文标点"; color: window.ink }
                            }
                        }
                        SectionTitle { text: "词频与隐私" }
                        Card { ToggleRow { label: "学习我的用词习惯"; description: "在本机调整候选顺序。密码等敏感输入不会学习。"; configKey: "learning" } }
                        Note { text: "按 Win + 空格选择 R-IME。单独按 Shift 切换中文和英文；组合输入中的 Shift 切换会先提交原始拼音。" }
                    }
                    ColumnLayout {
                        visible: window.page === 1
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "拼音方案" }
                        Repeater {
                            model: [{name:"全拼",id:"rime_ime_pinyin",detail:"输入完整拼音",sample:"你好  →  ni hao"},{name:"小鹤双拼",id:"rime_ime_flypy",detail:"每个音节使用两个按键",sample:"你好  →  ni hc"},{name:"自然码双拼",id:"rime_ime_ziranma",detail:"每个音节使用两个按键",sample:"你好  →  ni hk"}]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true; implicitHeight: 81; radius: 8
                                color: window.surface
                                border.color: window.borderColor
                                RowLayout {
                                    anchors { fill: parent; margins: 16 } spacing: 12
                                    RadioButton { checked: window.option("input","schema") === modelData.id; onClicked: settings.setValue("input","schema",modelData.id); Accessible.name: modelData.name }
                                    ColumnLayout { Layout.fillWidth: true; spacing: 5; Label { text: modelData.name; color: window.ink; font.weight: Font.DemiBold } Note { text: modelData.detail } }
                                    Label { text: modelData.sample; color: window.muted; font.pixelSize: 12 }
                                }
                            }
                        }
                        Card {
                            visible: window.option("input", "schema") !== "rime_ime_pinyin"
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
                            Label { visible: settings.phrases.length === 0; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; topPadding: 28; bottomPadding: 28; text: "没有自定义短语。\n选择“添加”来新建短语。"; color: window.muted; lineHeight: 1.8 }
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
                            Repeater { model: ["Shift + 空格    切换全角和半角字符", "Ctrl + .    切换中文和英文标点（中文模式）", "空格    选择当前候选", "数字键    选择本页对应候选", "↑ / ↓    移动候选选中项", "← / →    移动输入串光标", "Enter    提交原始输入串"]; delegate: Label { required property string modelData; text: modelData; color: window.muted; font.pixelSize: 13 } }
                        }
                        Note { text: "Win + 空格由 Windows 管理。全角和标点切换使用以上固定组合；它们不改变其他输入法的快捷键。" }
                    }
                    ColumnLayout {
                        visible: window.page === 4
                        Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; spacing: 12
                        SectionTitle { text: "实时预览" }
                        Rectangle {
                            Layout.fillWidth: true; implicitHeight: Math.max(132,candidatePreview.implicitHeight+48)
                            radius: 8; color: window.highContrast ? systemPalette.window : window.dark ? "#191919" : "#e9e9e9"
                            Rectangle {
                                id: candidatePreview
                                anchors.centerIn: parent; width: Math.min(parent.width-32,candidates.implicitWidth+20); implicitHeight: candidates.implicitHeight+16
                                radius: 8; color: window.highContrast ? systemPalette.base : window.dark ? "#2b2b2b" : "#fbfbfb"; border.color: window.highContrast ? window.borderColor : window.dark ? "#454545" : "#d8d8d8"
                                GridLayout {
                                    id: candidates; anchors.centerIn: parent; columns: window.option("appearance","orientation") === "vertical" ? 1 : Math.max(1,Math.min(window.option("appearance","page_size"),Math.floor((scroll.availableWidth-116)/(window.option("appearance","font_size")*3.8+20)))); columnSpacing: 2; rowSpacing: 2
                                    Repeater {
                                        model: window.option("appearance","page_size")
                                        delegate: Rectangle {
                                            required property int index
                                            implicitWidth: candidateText.implicitWidth+20; implicitHeight: candidateText.implicitHeight+16; radius: 4
                                            color: index === 0 ? window.selectionColor : "transparent"
                                            Label { id: candidateText; anchors.centerIn: parent; text: (index+1)+"  "+["你好","您好","拟好","你","呢","泥","妮","倪","霓"][index]; color: index === 0 ? window.selectionInk : window.ink; font.pixelSize: window.option("appearance","font_size"); font.family: window.option("appearance","font") || "Microsoft YaHei UI" }
                                            Rectangle {
                                                visible: index === 0
                                                color: window.accent; radius: 1.5
                                                width: window.option("appearance","orientation") === "vertical" ? 3 : 20
                                                height: window.option("appearance","orientation") === "vertical" ? 20 : 3
                                                x: window.option("appearance","orientation") === "vertical" ? 1 : (parent.width-width)/2
                                                y: window.option("appearance","orientation") === "vertical" ? (parent.height-height)/2 : parent.height-3
                                            }
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
                            Label { text: "备份设置和用户词库"; color: window.ink; font.weight: Font.DemiBold; font.pixelSize: 16 }
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
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: window.borderColor }
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
