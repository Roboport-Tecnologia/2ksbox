// About 2ksbox: the version, the licence and the projects it is built
// on (`launcher_core::about`), opened from the grid's "?" button and,
// on macOS, from the application menu (`MacMenu.qml`).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

Window {
    id: root

    /// The item the headless screenshot path grabs (see `Main.qml`).
    property Item grabItem: body

    /// What the `about` probe prints: the heading and the credit rows.
    function report() {
        return "heading '" + about.name + "  " + about.version + "', credits " + credits.count
    }

    title: qsTr("About 2ksbox")
    width: 620
    height: 600
    minimumWidth: 460
    minimumHeight: 360
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: palette.window

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: root.close()
    }

    AboutModel { id: about }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            spacing: 14
            Layout.fillWidth: true

            Image {
                source: "qrc:/qt/qml/com/_2ksbox/launcher/icon/2ksbox-128.png"
                sourceSize.width: 72
                sourceSize.height: 72
                Layout.alignment: Qt.AlignTop
            }
            ColumnLayout {
                spacing: 3
                Layout.fillWidth: true

                Label {
                    text: about.name + "  " + about.version
                    font.pixelSize: 22
                    font.bold: true
                }
                Label {
                    text: about.tagline
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    text: about.license
                    opacity: 0.7
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    text: "<a href=\"" + about.url + "\">" + about.url + "</a>"
                    textFormat: Text.StyledText
                    onLinkActivated: link => Qt.openUrlExternally(link)
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }
            }
        }

        MenuSeparator { Layout.fillWidth: true }

        Label {
            text: about.thanks
            font.bold: true
        }

        ListView {
            id: credits
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: about
            spacing: 2
            ScrollBar.vertical: ScrollBar {}

            section.property: "section"
            section.delegate: Label {
                required property string section
                text: section
                opacity: 0.7
                font.bold: true
                topPadding: 10
                bottomPadding: 4
            }

            delegate: RowLayout {
                id: credit
                required property string name
                required property string what
                required property string license
                required property string url

                width: credits.width - 16
                spacing: 10

                Label {
                    text: "<a href=\"" + credit.url + "\">" + credit.name + "</a>"
                    textFormat: Text.StyledText
                    elide: Text.ElideRight
                    onLinkActivated: link => Qt.openUrlExternally(link)
                    ToolTip.visible: linkHover.hovered
                    ToolTip.text: credit.url
                    ToolTip.delay: 500
                    HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
                    Layout.preferredWidth: 160
                    Layout.maximumWidth: 160
                }
                Label {
                    text: credit.what
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: credit.license
                    opacity: 0.7
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: 140
                    Layout.maximumWidth: 140
                }
            }
        }

        Button {
            text: qsTr("Close")
            Layout.alignment: Qt.AlignRight
            onClicked: root.close()
        }
    }
}
