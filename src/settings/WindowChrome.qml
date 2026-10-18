import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: chrome
    required property var targetWindow
    required property var effects
    property color foreground
    property bool closeEnabled: true
    implicitHeight: 48
    function maximizeOrRestore() {
        if (targetWindow.visibility === Window.Maximized || targetWindow.visibility === Window.FullScreen)
            targetWindow.showNormal()
        else targetWindow.showMaximized()
    }
    function toggleFullscreen() {
        if (targetWindow.visibility === Window.FullScreen) targetWindow.showNormal()
        else targetWindow.showFullScreen()
    }
    function titleDoubleClick() {
        if (!enabled || targetWindow.visibility === Window.FullScreen) return
        if (Qt.platform.os === "osx") effects.titlebarDoubleClick()
        else maximizeOrRestore()
    }
    function startTitleDrag() {
        if (!enabled || targetWindow.visibility === Window.FullScreen) return
        targetWindow.startSystemMove()
    }
    Item {
        objectName: "windowDragRegion"
        anchors { left: parent.left; right: controls.left; top: parent.top; bottom: parent.bottom; margins: 8 }
        Label {
            anchors.fill: parent
            anchors.leftMargin: 8
            text: chrome.targetWindow.title
            color: chrome.foreground
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onDoubleTapped: chrome.titleDoubleClick()
        }
        DragHandler {
            target: null
            acceptedButtons: Qt.LeftButton
            onActiveChanged: if (active) chrome.startTitleDrag()
        }
    }
    component ChromeButton: Button {
        width: 34; height: 28
        focusPolicy: Qt.StrongFocus
        Accessible.name: ToolTip.text
        ToolTip.visible: hovered
    }
    Row {
        id: controls
        anchors { top: parent.top; right: parent.right; topMargin: 10; rightMargin: 10 }
        spacing: 4
        // These controls are clear of all eight 8px edge/corner resize regions.
        ChromeButton {
            objectName: "windowEffectsButton"
            text: "◈"
            checkable: true
            checked: chrome.effects.enabled
            ToolTip.text: checked ? "关闭背景材质" : "启用背景材质"
            onClicked: chrome.effects.setEnabled(checked)
        }
        ChromeButton {
            objectName: "windowFullscreenButton"
            text: chrome.targetWindow.visibility === Window.FullScreen ? "↙" : "⛶"
            ToolTip.text: chrome.targetWindow.visibility === Window.FullScreen ? "退出全屏" : "全屏"
            onClicked: chrome.toggleFullscreen()
        }
        ChromeButton {
            objectName: "windowMinimizeButton"
            text: "−"; ToolTip.text: "最小化"
            onClicked: chrome.targetWindow.showMinimized()
        }
        ChromeButton {
            objectName: "windowMaximizeButton"
            text: chrome.targetWindow.visibility === Window.Windowed ? "□" : "↙"
            ToolTip.text: chrome.targetWindow.visibility === Window.Windowed ? "最大化" : "还原"
            onClicked: chrome.maximizeOrRestore()
        }
        ChromeButton {
            objectName: "windowCloseButton"
            text: "×"; ToolTip.text: "关闭设置"
            enabled: chrome.closeEnabled
            // Never quit() or hide(): busy/dirty confirmation lives in onClosing.
            onClicked: chrome.targetWindow.close()
        }
    }
}
