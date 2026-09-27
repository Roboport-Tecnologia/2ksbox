// "Clone…" (doc 07): a new machine that is a whole copy of this one,
// disk included unless "same hard disk" is ticked. The model decides the name offered, what is copied and
// whether the machine may be copied at all; this only draws it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

// A real top-level window, modal (see `WizardWindow.qml`).
Window {
    id: root

    // Typed, not `var` (see `ShaderProfilesWindow.qml`).
    required property CloneModel cloner

    /// The item the headless screenshot path grabs (see `Main.qml`).
    property Item grabItem: body

    /// What the name field is showing, a way to type a new one over it
    /// the way a person does (select all, type), and the Clone button's
    /// own click. The `qt-clone` probe drives the window through these,
    /// so it sees what a person would.
    readonly property alias shownName: nameField.text
    function retypeName(text) {
        nameField.selectAll()
        nameField.remove(nameField.selectionStart, nameField.selectionEnd)
        nameField.insert(0, text)
    }
    function clickClone() { cloneButton.click() }
    function clickSameDisk() { sameDisk.click() }

    /// Where the layout put things, for the `qt-clone` probe: the window
    /// is as tall as its content, so its height and the layout's must agree.
    function layoutReport() {
        return "window " + width + "x" + height + ", layout h=" + bodyLayout.height
            + " implicit=" + bodyLayout.implicitHeight + ", min=" + minimumHeight + " max=" + maximumHeight
    }

    title: cloner.title
    width: 560
    minimumWidth: 420
    // As tall as what it shows and no taller: a note, the name and two
    // buttons most of the time, the progress bar or a warning when there
    // is one. A fixed height left a band of nothing above the buttons.
    // The minimum and maximum are bound to the content, not to `height`:
    // the platform assigns a height at show, which breaks a binding on
    // it, and a min and max bound to that would then pin the window at
    // whatever it was before the layout had its size.
    readonly property int contentHeight: bodyLayout.height + 2 * bodyLayout.x
    height: contentHeight
    minimumHeight: contentHeight
    maximumHeight: contentHeight
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: palette.window

    // Closing the window is Cancel: the flag drives the window both ways
    // (`Main.qml`), so clearing it here keeps the two from disagreeing
    // after a close from the title bar. A copy that is running carries on
    // and lands in the grid by itself.
    onVisibleChanged: if (!visible && cloner.open) cloner.dismiss()

    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: !root.cloner.busy
        onActivated: root.close()
    }

    Rectangle {
        id: body
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: bodyLayout
            // Sized by its content, not anchored to fill: the window follows
            // the layout, so the layout cannot follow the window.
            x: 14
            y: 14
            width: parent.width - 2 * x
            spacing: 10

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.cloner.note
                wrapMode: Text.Wrap
                opacity: 0.75
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Name") }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    selectByMouse: true
                    enabled: !root.cloner.busy
                    text: root.cloner.name
                    onTextChanged: if (text !== root.cloner.name) root.cloner.rename(text)
                    onAccepted: if (cloneButton.enabled) cloneButton.click()
                }
            }

            CheckBox {
                id: sameDisk
                text: root.cloner.sameDiskLabel
                enabled: !root.cloner.busy
                checked: root.cloner.sameDisk
                onToggled: root.cloner.shareDisk(checked)
            }

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.cloner.warning
                color: "#c88200"
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.cloner.busy
                spacing: 4
                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: root.cloner.progress
                }
                Label {
                    text: root.cloner.progressLabel
                    opacity: 0.75
                }
            }

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.cloner.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    enabled: !root.cloner.busy
                    onClicked: root.cloner.dismiss()
                }
                Button {
                    id: cloneButton
                    text: qsTr("Clone")
                    enabled: root.cloner.canSubmit
                    onClicked: root.cloner.submit()
                }
            }
        }
    }
}
