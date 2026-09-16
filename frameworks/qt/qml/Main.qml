import QtQuick

Rectangle {
    color: "#10131a"

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        height: 190
        radius: 24
        color: mouse.pressed ? "#31435f" : "#202938"
        border.color: "#668bc4"
        border.width: 1

        Column {
            anchors.centerIn: parent
            spacing: 14

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Qt Quick on LCL"
                color: "white"
                font.pixelSize: 30
                font.weight: Font.DemiBold
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Native frame " + counter.value
                color: "#a9bad3"
                font.pixelSize: 17
            }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            onClicked: {
                counter.value += 1
                lclSurface.requestFrame()
            }
        }
    }

    QtObject {
        id: counter
        property int value: 1
    }
}
