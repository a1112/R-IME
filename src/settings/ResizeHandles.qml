import QtQuick
import QtQuick.Window

Item {
    id: root
    property var targetWindow
    enabled: Qt.platform.os !== "osx" && targetWindow.visibility === Window.Windowed
    visible: enabled
    // Each handle starts the compositor's gesture; no manual drag state survives cancellation.
    Repeater {
        model: [Qt.TopEdge | Qt.LeftEdge, Qt.TopEdge, Qt.TopEdge | Qt.RightEdge,
                Qt.LeftEdge, Qt.RightEdge, Qt.BottomEdge | Qt.LeftEdge, Qt.BottomEdge, Qt.BottomEdge | Qt.RightEdge]
        MouseArea {
            required property int modelData
            readonly property bool leftEdge: (modelData & Qt.LeftEdge) !== 0
            readonly property bool rightEdge: (modelData & Qt.RightEdge) !== 0
            readonly property bool topEdge: (modelData & Qt.TopEdge) !== 0
            readonly property bool bottomEdge: (modelData & Qt.BottomEdge) !== 0
            x: rightEdge ? root.width - 8 : leftEdge ? 0 : 8
            y: bottomEdge ? root.height - 8 : topEdge ? 0 : 8
            width: leftEdge || rightEdge ? 8 : root.width - 16
            height: topEdge || bottomEdge ? 8 : root.height - 16
            acceptedButtons: Qt.LeftButton
            cursorShape: (leftEdge && topEdge) || (rightEdge && bottomEdge) ? Qt.SizeFDiagCursor
                : (rightEdge && topEdge) || (leftEdge && bottomEdge) ? Qt.SizeBDiagCursor
                : leftEdge || rightEdge ? Qt.SizeHorCursor : Qt.SizeVerCursor
            onPressed: targetWindow.startSystemResize(modelData)
        }
    }
}
