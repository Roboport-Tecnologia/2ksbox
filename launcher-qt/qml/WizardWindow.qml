// The guided creation wizard (doc 07), over the shared form
// (`launcher_core::wizard::Form`). The same form doubles as "Edit
// machine" for an existing bundle. A sidebar of sections and a page
// each, like a virtual machine's settings window anywhere else.
//
// Every field with a consequence goes through an invokable
// (`chooseFamily`, `chooseRam`, …) rather than assigning the property:
// see the header of `src/qt/wizard.rs` for why.
//
// The combo boxes' labels and the file dialogs' name filters come from
// the model too (`familyLabels()`, `diskFilter()`, …) rather than being
// retyped here, so no front end can word a choice differently.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
// FolderDialog, for the one field that names a directory (the MT-32's
// ROMs) rather than a file.
import QtQuick.Dialogs
import com._2ksbox.launcher

// A real top-level window, not an in-window popup: the launcher's
// secondary screens are separate windows the user can move and resize,
// which the platform already handles. `Qt.Dialog` keeps it transient for
// the launcher window: the compositor stacks it above and gives it a
// dialog frame.
//
// **Modal**, like every other secondary window (user decision). Modeless
// windows let the wizard, the disc shelf, the snapshots list and the
// profile editor all be on screen at once with nothing saying which one
// you are answering. One at a time, closed by Esc, is what every other
// dialog on the desktop does.
Window {
    id: root

    // Typed, not `var` (see `ShaderProfilesWindow.qml`).
    required property Wizard wizard

    signal saved()

    /// The item the headless screenshot path grabs (see `Main.qml`).
    property Item grabItem: form

    /// What the memory spin box is showing, which is not always what the
    /// model says: a control that clamps holds the value it was given
    /// against the range it had at that moment, so a publish in the
    /// wrong order leaves the two disagreeing where only a screenshot
    /// would notice. The headless path prints it (`Main.qml`), and
    /// the `qt-wizard` check in `scripts/test.sh` compares it with the
    /// model's own number.
    readonly property int shownRamMb: ram.value

    /// What the name field is showing, and a way to type into it. A JS
    /// assignment would not do: writing the field's `text` from QML
    /// destroys the `text:` binding, and the binding surviving a
    /// keystroke is what lets the model wipe the name out from under the
    /// user (`src/qt/wizard.rs`'s `edit`). `insert`
    /// is what a key press does, so the `qt-wizard` check sees what the
    /// user sees.
    readonly property alias shownName: nameField.text
    function typeName(text) { nameField.insert(nameField.length, text) }
    /// What the Direct3D combo box is showing, and how many entries it
    /// has. A binding that names a property the object lacks is
    /// **silent** in QML: no warning, no error, the control takes no
    /// model. This row first shipped that way: the Rust side spelled the
    /// property `d3d9_labels`, cxx-qt's auto camel-case made it
    /// `d3D9Labels`, this file asked for `d3d9Labels`, and the combo box
    /// was empty. Both sides say `d3d9` now (`src/qt/wizard.rs` names
    /// each `cxx_name`), and the `qt-wizard` check reads the count below
    /// so the next such slip fails a check.
    readonly property alias shownD3d9: d3d9Combo.currentText
    readonly property alias shownD3d9Count: d3d9Combo.count
    /// The shader profile combo, the same way: its rows come from the
    /// model, so a misspelt property empties it too.
    readonly property alias shownShaderProfile: profileBox.currentText
    readonly property alias shownShaderProfileCount: profileBox.count

    /// The same for the extra QEMU arguments, which is bound the same way,
    /// plus a scroll to it so a screenshot shows it.
    readonly property alias shownExtraQemuArgs: extraQemuArgsField.text
    function typeExtraQemuArgs(text) { extraQemuArgsField.insert(extraQemuArgsField.length, text) }
    function revealExtraQemuArgs() {
        root.wizard.chooseSection(1)   // the System page
        const flick = systemPage.contentItem
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height,
            extraQemuArgsField.mapToItem(systemPage.column, 0, 0).y - flick.height / 2))
    }

    /// The page on show, where it is scrolled to and a way to scroll it,
    /// and a way to open the optimizations (the one thing that makes a
    /// page taller than the window), for the `wizardscroll` probe
    /// (`Main.qml`): whether opening another machine starts on the first
    /// page at the top and reopening the same one does not.
    function currentPage() { return pages.children[pages.currentIndex] }
    /// Every page's content height beside the room the window gives a
    /// page, so the default height can be sized to the tallest page
    /// rather than guessed (the `wizard` probe prints it).
    function pageReport() {
        const names = root.wizard.sectionLabels()
        let out = "room " + Math.round(pages.height) + " of window " + height
        for (let i = 0; i < pages.children.length; i++) {
            const p = pages.children[i]
            if (p.column)
                out += ", " + names[i].toLowerCase() + " " + Math.round(p.column.implicitHeight)
        }
        return out
    }
    function scrollY() { return currentPage().contentItem.contentY }
    function scrollTo(y) { currentPage().contentItem.contentY = y }
    function expandOptimizations() { optimizationsExpander.expanded = true }

    /// What the emulation-optimization boxes are showing, as a mask in
    /// the model's own bit order (`optimizationsMask`), and a way to click
    /// one box and each of the three shortcuts beside them. `click()` is
    /// what a mouse click does (the toggle, `toggled` and its handler),
    /// so the `qt-wizard` check sees what the user sees, as `typeName`
    /// does for the name field.
    readonly property int optimizationBoxes: optimizations.count
    function shownOptimizationsMask() {
        let mask = 0
        for (let i = 0; i < optimizations.count; i++)
            if (optimizations.itemAt(i).box.checked)
                mask |= 1 << i
        return mask
    }
    function clickOptimization(i) { optimizations.itemAt(i).box.click() }
    function clickOptimizationShortcut(which) {
        ({ off: optAllOff, on: optAllOn, defaults: optDefaults })[which].click()
    }

    title: wizard.title
    // Sized to the tallest page, not guessed: `pageReport()` measured the
    // pages at 293 (System with its optimizations closed, Display) with 70
    // of chrome around them on the Windows PC (600 was too tall even for
    // the longest page). The `qt-wizard` check keeps every page inside
    // the room this leaves. The System page with the optimizations open
    // scrolls, by design.
    width: 820
    height: 440
    minimumWidth: 640
    minimumHeight: 400
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: palette.window

    // Closing the window is cancelling the form: the flag drives the
    // window in both directions (`Main.qml`), so clearing it here keeps
    // the two from disagreeing after a close from the title bar.
    //
    // Opening it starts on the first page, at the top, unless it is the
    // same machine as last time: a ScrollView keeps its position across a
    // hide and show, so editing one machine after another would open the
    // second wherever the first was left. A new machine is never "the
    // same", so creating always starts at the top.
    // The form itself opens on the first page every time (the core
    // resets it); putting the same machine back on its page is this
    // window's own memory.
    property string lastOpened: ""
    property int lastSection: 0
    onVisibleChanged: {
        if (!visible) {
            lastSection = wizard.section
            // Through the model (`dismiss`), never `wizard.open = false`
            // on the property: that left the form's own flag up, and the
            // profile list's close (a profile rescan, so a republish)
            // raised the property again and this window with it.
            if (wizard.open) wizard.dismiss()
            return
        }
        const opened = wizard.editing ? wizard.bundlePath : ""
        if (opened === "" || opened !== lastOpened) {
            for (const p of pages.children)
                if (p.contentItem) p.contentItem.contentY = 0
        } else {
            wizard.chooseSection(lastSection)
        }
        lastOpened = opened
    }

    // Esc is Cancel, as in every other dialog on the desktop. It goes
    // through `close()` rather than hiding the window, because that runs
    // `onVisibleChanged` above, the one place a model's own `open` flag
    // is put back. Not while a file dialog is up: that Esc is the
    // dialog's (`PathField.browsing`).
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: !soundfontField.browsing && !diskField.browsing
            && !mediaField.browsing && !floppyField.browsing
            && !mt32RomsDialog.visible
        onActivated: root.close()
    }

    /// One page of the form: a stock ScrollView over a column, and the
    /// fields declared inside it land in that column.
    component Page: ScrollView {
        id: page
        default property alias content: column.data
        /// The column itself, for a probe that maps a field's position.
        readonly property Item column: column
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            id: column
            width: parent.width
            spacing: 10
        }
    }

    Rectangle {
        id: form
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: formLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            // The pages: a sidebar of sections and one page each, the way
            // VirtualBox's and UTM's settings are laid out (user request:
            // one long scrolling form had outgrown its window).
            // The sections, their names and their order are the shared
            // form's (`Form::section_labels`, `wizard.section`); which
            // field goes on which page is this file's. Every page scrolls
            // on its own and the buttons under them never move.
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                ListView {
                    id: sectionList
                    Layout.preferredWidth: 150
                    Layout.fillHeight: true
                    clip: true
                    model: root.wizard.sectionLabels()
                    currentIndex: root.wizard.section
                    delegate: ItemDelegate {
                        required property int index
                        required property string modelData
                        width: sectionList.width
                        text: modelData
                        highlighted: ListView.isCurrentItem
                        onClicked: root.wizard.chooseSection(index)
                    }
                }

                StackLayout {
                    id: pages
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.wizard.section

                    Page {
                        id: generalPage

                            GridLayout {
                                columns: 2
                                columnSpacing: 8
                                rowSpacing: 8
                                Layout.fillWidth: true

                                Label { text: qsTr("Family") }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: root.wizard.familyLabels()
                                    currentIndex: root.wizard.family
                                    onActivated: root.wizard.chooseFamily(currentIndex)
                                }

                                // Only "Other" has one (the model decides, not this
                                // file); it spans both columns so it reads as a
                                // sentence under the picker rather than a second value.
                                Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    visible: root.wizard.familyNote !== ""
                                    text: root.wizard.familyNote
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 11
                                    opacity: 0.75
                                }

                                Label { text: qsTr("Name") }
                                TextField {
                                    id: nameField
                                    Layout.fillWidth: true
                                    text: root.wizard.name
                                    selectByMouse: true
                                    onTextChanged: root.wizard.name = text
                                }
                            }
                    }

                    Page {
                        id: systemPage

                            // --- memory -------------------------------------------------
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: qsTr("Memory (MB)"); Layout.minimumWidth: 150 }
                                SpinBox {
                                    id: ram
                                    from: root.wizard.ramMin
                                    to: root.wizard.ramMax
                                    stepSize: 16
                                    editable: true
                                    value: root.wizard.ramMb
                                    onValueModified: root.wizard.chooseRam(value)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.ramIsDefault
                                    onClicked: root.wizard.resetRam()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.ramNote !== ""
                                text: root.wizard.ramNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }

                            // --- processor ----------------------------------------------
                            // Named machines rather than a number: nobody knows how many
                            // instructions per second their DOS game wants, while "it
                            // wants a 486" is written on the box. This field decides
                            // whether an era game is playable at all (doc 06).
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.cpuSpeedApplies
                                spacing: 8
                                Label { text: qsTr("Processor"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 200
                                    model: root.wizard.cpuSpeedLabels()
                                    currentIndex: root.wizard.cpuSpeed
                                    onActivated: root.wizard.chooseCpuSpeed(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.cpuIsDefault
                                    onClicked: root.wizard.resetCpuSpeed()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.cpuSpeedApplies && root.wizard.cpuNote !== ""
                                text: root.wizard.cpuNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }

                            // --- acceleration -------------------------------------------
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: qsTr("Acceleration"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 200
                                    model: root.wizard.accelLabels()
                                    currentIndex: root.wizard.accel
                                    onActivated: root.wizard.chooseAccel(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.accelIsDefault
                                    onClicked: root.wizard.resetAccel()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.wizard.accelNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                // The one case that is a warning rather than a note: KVM was
                                // demanded and this host hasn't got it, so the machine will
                                // refuse to start.
                                color: root.wizard.accelWarning ? "#c88200" : palette.windowText
                                opacity: root.wizard.accelWarning ? 1.0 : 0.75
                            }

                            // --- emulation optimizations ---------------------------------
                            // Our own QEMU fast paths (patches/qemu/README.md), one
                            // checkbox each, behind a disclosure: fourteen switches
                            // nobody needs to touch would push the fields that matter
                            // off the bottom of the window. The header carries the
                            // count, so a machine with one turned off says so while
                            // closed. The labels, the sentences and the count all come
                            // from the shared form.
                            //
                            // The header is a `Disclosure`, not a checkbox
                            // (`Disclosure.qml` says why).
                            Disclosure {
                                id: optimizationsExpander
                                Layout.fillWidth: true
                                text: qsTr("Emulation optimizations (%1)").arg(root.wizard.optimizationsSummary)
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 18
                                spacing: 4
                                visible: optimizationsExpander.expanded

                                Label {
                                    Layout.fillWidth: true
                                    text: root.wizard.optimizationsNote
                                    wrapMode: Text.Wrap
                                    font.pixelSize: 11
                                    opacity: 0.75
                                }
                                Repeater {
                                    id: optimizations
                                    // Read once: both lists are fixed for the life of the
                                    // build, and a binding would call across the bridge
                                    // for every row on every publish.
                                    readonly property var notes: root.wizard.optimizationNotes()
                                    model: root.wizard.optimizationLabels()

                                    ColumnLayout {
                                        id: row
                                        required property int index
                                        required property string modelData
                                        Layout.fillWidth: true
                                        spacing: 0
                                        readonly property alias box: optBox

                                        CheckBox {
                                            id: optBox
                                            text: row.modelData
                                            checked: (root.wizard.optimizationsMask & (1 << row.index)) !== 0
                                            onToggled: root.wizard.chooseOptimization(row.index, checked)
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 6
                                            text: optimizations.notes[row.index]
                                            wrapMode: Text.Wrap
                                            font.pixelSize: 11
                                            opacity: 0.75
                                        }
                                    }
                                }
                                // The two shortcuts sit beside "All defaults" rather
                                // than replacing it: fourteen switches is too many to
                                // walk through to build a control run, and the way
                                // back is not "all on" (x87-pc64-as-53 ships off) but
                                // the defaults.
                                RowLayout {
                                    spacing: 6
                                    Button {
                                        id: optAllOff
                                        text: qsTr("Turn all off")
                                        enabled: !root.wizard.optimizationsAllOff
                                        onClicked: root.wizard.disableAllOptimizations()
                                    }
                                    Button {
                                        id: optAllOn
                                        text: qsTr("Turn all on")
                                        enabled: !root.wizard.optimizationsAllOn
                                        onClicked: root.wizard.enableAllOptimizations()
                                    }
                                    Button {
                                        id: optDefaults
                                        text: qsTr("All defaults")
                                        enabled: !root.wizard.optimizationsAreDefault
                                        onClicked: root.wizard.resetOptimizations()
                                    }
                                }
                            }

                            // --- extra QEMU arguments -------------------------------------
                            // The escape hatch for what the form has no field for. Bound
                            // both ways like the name (the model's `pull` takes the text
                            // before a save), and committed when the field loses focus so
                            // the note under it catches a quote left open.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: qsTr("Extra QEMU arguments"); Layout.minimumWidth: 150 }
                                TextField {
                                    id: extraQemuArgsField
                                    Layout.fillWidth: true
                                    text: root.wizard.extraQemuArgs
                                    placeholderText: qsTr("-global d3dpt-vga.ddflags=32768")
                                    selectByMouse: true
                                    onTextChanged: root.wizard.extraQemuArgs = text
                                    onEditingFinished: root.wizard.commitExtraQemuArgs()
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.wizard.extraQemuArgsNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: root.wizard.extraQemuArgsWarning ? "#c88200" : palette.windowText
                                opacity: root.wizard.extraQemuArgsWarning ? 1.0 : 0.75
                            }
                    }

                    Page {
                        id: displayPage

                            // --- the display adapter ------------------------------------
                            // Both the list and whether there is a choice at all come from
                            // the model (`videoLabels` is a property, not an invokable,
                            // because the list changes with the family); DOS is the one
                            // family with none, its adapter being a fact of the era.
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.videoApplies
                                spacing: 8
                                Label { text: qsTr("Display adapter"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 260
                                    model: root.wizard.videoLabels
                                    currentIndex: root.wizard.video
                                    onActivated: root.wizard.chooseVideo(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.videoIsDefault
                                    onClicked: root.wizard.resetVideo()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            // About the machine rather than the entry selected, so it sits
                            // above the notes: changing this under an installed guest is a
                            // hardware change and the guest will say so.
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.videoWarning !== ""
                                text: root.wizard.videoWarning
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: "#c88200"
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.videoApplies
                                text: root.wizard.videoNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }

                            // --- which Direct3D 9 the host runs the executor on ----------
                            // A host question under the adapter that carries it: DXVK
                            // everywhere, and on Windows this PC's own Direct3D 9 for a
                            // host below DXVK's Vulkan 1.3 floor. `d3d9Applies` is the
                            // model's answer about the adapter, not this file's.
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.d3d9Applies
                                spacing: 8
                                Label { text: qsTr("Direct3D"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    id: d3d9Combo
                                    Layout.preferredWidth: 260
                                    model: root.wizard.d3d9Labels
                                    currentIndex: root.wizard.d3d9
                                    onActivated: root.wizard.chooseD3d9(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.d3d9IsDefault
                                    onClicked: root.wizard.resetD3d9()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            // The host's own answer is in this note (ADR-013): what
                            // this host runs the pass-through on, orange only for the
                            // software Vulkan driver, the case that runs but slowly.
                            // It sits under this picker, not the adapter (user
                            // decision).
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.d3d9Applies
                                text: root.wizard.d3d9Note
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: root.wizard.d3d9Warning ? "#c88200" : palette.windowText
                                opacity: root.wizard.d3d9Warning ? 1.0 : 0.75
                            }

                            // --- the Voodoo 2 -------------------------------------------
                            // Two checkboxes on one line: the card beside the display
                            // adapter (doc 21), and the "Voodoo3 undither filter", the card's
                            // own dither undone at scanout (doc 21 §12), a property of that card
                            // that belongs next to it rather than under a heading of its
                            // own. Whether the second can be answered at all is the
                            // form's to say, not this file's. The sentences are the
                            // shared form's too.
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.voodoo2Applies
                                spacing: 16

                                CheckBox {
                                    text: qsTr("3dfx Voodoo 2")
                                    checked: root.wizard.voodoo2
                                    onToggled: root.wizard.chooseVoodoo2(checked)
                                }
                                CheckBox {
                                    text: qsTr("Voodoo3 undither filter")
                                    enabled: root.wizard.voodoo2UnditherEnabled
                                    checked: root.wizard.voodoo2Undither
                                    onToggled: root.wizard.chooseVoodoo2Undither(checked)
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.voodoo2Applies
                                text: root.wizard.voodoo2Note
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.voodoo2Applies && root.wizard.voodoo2UnditherNote !== ""
                                text: root.wizard.voodoo2UnditherNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }

                            // --- the shader profile -------------------------------------
                            // The same shape as every picker above: the rows are the
                            // model's (the app default first, then the library by
                            // name), the choice is a row, and the style draws the
                            // list, so it looks like every other combo box in the
                            // form.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: qsTr("Shader profile"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    id: profileBox
                                    Layout.preferredWidth: 260
                                    model: root.wizard.shaderProfileLabels
                                    currentIndex: root.wizard.shaderProfileIndex
                                    onActivated: root.wizard.chooseShaderProfile(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.shaderProfileIsDefault
                                    onClicked: root.wizard.resetShaderProfile()
                                }
                                Item { Layout.fillWidth: true }
                            }
                    }

                    Page {
                        id: audioPage

                            // --- the sound card and the MIDI port (doc 20 §6) -----------
                            // Two pickers rather than one: the card is what the guest
                            // plays sound effects on and needs a driver for, the port
                            // is what plays its music, and a machine of the era had
                            // both. The FM chip is in neither list: it comes with the
                            // card that carried one, as on the hardware, and the
                            // card's note says so.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: qsTr("Sound card"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 260
                                    model: root.wizard.soundLabels
                                    currentIndex: root.wizard.sound
                                    onActivated: root.wizard.chooseSound(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.soundIsDefault
                                    onClicked: root.wizard.resetSound()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.soundWarning !== ""
                                text: root.wizard.soundWarning
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: "#c88200"
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.wizard.soundNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.musicApplies
                                spacing: 8
                                Label { text: qsTr("Music (MIDI)"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 260
                                    model: root.wizard.musicLabels
                                    currentIndex: root.wizard.music
                                    onActivated: root.wizard.chooseMusic(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.musicIsDefault
                                    onClicked: root.wizard.resetMusic()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.musicApplies
                                text: root.wizard.musicNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                            // The bank is optional (empty means the one we ship) and
                            // the ROMs are not: an MT-32 machine without them is refused
                            // when the form is saved, because nothing of Roland's can be
                            // shipped with this program.
                            PathField {
                                id: soundfontField
                                Layout.fillWidth: true
                                visible: root.wizard.soundfontApplies
                                label: qsTr("SoundFont (optional)")
                                nameFilter: root.wizard.soundfontFilter()
                                value: root.wizard.soundfont
                                onEdited: (path) => root.wizard.setSoundfontPath(path)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.mt32RomsApplies
                                spacing: 8
                                Label { text: qsTr("MT-32 ROMs"); Layout.minimumWidth: 150 }
                                TextField {
                                    id: mt32RomsField
                                    Layout.fillWidth: true
                                    text: root.wizard.mt32Roms
                                    placeholderText: qsTr("Folder with your CM-32L control and PCM ROMs")
                                    onEditingFinished: root.wizard.setMt32RomsPath(text)
                                }
                                Button {
                                    // A directory, so Qt's FolderDialog rather than the
                                    // PathField above: no name filter can express "a
                                    // folder" (the disc shelf's "Add folder…" has the
                                    // same problem).
                                    text: qsTr("Browse…")
                                    onClicked: {
                                        mt32RomsDialog.currentFolder = mt32Browse.startUrl(root.wizard.mt32Roms, "")
                                        mt32RomsDialog.open()
                                    }
                                }
                            }
                            Browse { id: mt32Browse }
                            FolderDialog {
                                id: mt32RomsDialog
                                title: qsTr("Choose the folder with your CM-32L ROMs")
                                onAccepted: {
                                    const path = mt32Browse.localPath(selectedFolder)
                                    mt32Browse.remember(path)
                                    root.wizard.setMt32RomsPath(path)
                                }
                            }
                    }

                    Page {
                        id: inputPage

                            // --- the gamepad (M13) --------------------------------------
                            // Same shape as the adapter above, and a property list for the
                            // same reason: DOS has no USB stack and is offered no USB
                            // controller, so the list changes with the family.
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.padApplies
                                spacing: 8
                                Label { text: qsTr("Gamepad"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 260
                                    model: root.wizard.padLabels
                                    currentIndex: root.wizard.pad
                                    onActivated: root.wizard.choosePad(currentIndex)
                                }
                                Button {
                                    text: qsTr("Default")
                                    enabled: !root.wizard.padIsDefault
                                    onClicked: root.wizard.resetPad()
                                }
                                Item { Layout.fillWidth: true }
                            }
                            // Adding or removing the USB controller is a hardware change,
                            // so it is about the machine and sits above the notes.
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.padWarning !== ""
                                text: root.wizard.padWarning
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: "#c88200"
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.padApplies
                                text: root.wizard.padNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }

                            // --- the pointer ---------------------------------------------
                            // One checkbox: does the host pointer walk into this machine
                            // (the USB tablet, an absolute device) or does the window
                            // take it (the PS/2 mouse, grabbed on a click). The
                            // sentences are the shared form's, hotkey included.
                            CheckBox {
                                text: qsTr("Seamless mouse")
                                checked: root.wizard.seamlessMouse
                                onToggled: root.wizard.chooseSeamlessMouse(checked)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.wizard.seamlessMouseNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                    }

                    Page {
                        id: networkPage

                            // --- networking ---------------------------------------------
                            CheckBox {
                                text: qsTr("Networking")
                                checked: root.wizard.network
                                onToggled: root.wizard.chooseNetwork(checked)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.wizard.networkNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                    }

                    Page {
                        id: storagePage

                            // --- disk ---------------------------------------------------
                            CheckBox {
                                visible: !root.wizard.editing
                                text: qsTr("Use an existing disk image")
                                checked: root.wizard.existingDisk
                                onToggled: root.wizard.existingDisk = checked
                            }
                            PathField {
                                id: diskField
                                Layout.fillWidth: true
                                visible: root.wizard.editing || root.wizard.existingDisk
                                label: qsTr("Disk path")
                                nameFilter: root.wizard.diskFilter()
                                value: root.wizard.diskPath
                                onEdited: (path) => root.wizard.diskPath = path
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: !root.wizard.editing && !root.wizard.existingDisk
                                spacing: 8
                                Label { text: qsTr("New disk size (GB)"); Layout.minimumWidth: 150 }
                                SpinBox {
                                    from: 1
                                    to: 128
                                    value: root.wizard.diskSizeGb
                                    editable: true
                                    onValueModified: root.wizard.diskSizeGb = value
                                }
                                Item { Layout.fillWidth: true }
                            }
                            PathField {
                                id: mediaField
                                Layout.fillWidth: true
                                label: qsTr("Install media (optional)")
                                nameFilter: root.wizard.mediaFilter()
                                value: root.wizard.installMedia
                                onEdited: (path) => root.wizard.installMedia = path
                            }
                            // A floppy in A:, and what the machine boots from. Doc 06
                            // lists a floppy on the Win98 machine and doc 07 lists floppy
                            // images among the media the launcher handles.
                            PathField {
                                id: floppyField
                                Layout.fillWidth: true
                                visible: root.wizard.floppyApplies
                                label: qsTr("Floppy (optional)")
                                nameFilter: root.wizard.floppyFilter()
                                value: root.wizard.floppy
                                // Through an invokable, not the property: the boot note
                                // below depends on whether there is an image at all.
                                onEdited: (path) => root.wizard.setFloppyPath(path)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.wizard.bootApplies
                                spacing: 8
                                Label { text: qsTr("Boot from"); Layout.minimumWidth: 150 }
                                ComboBox {
                                    Layout.preferredWidth: 200
                                    model: root.wizard.bootLabels()
                                    currentIndex: root.wizard.boot
                                    onActivated: root.wizard.chooseBoot(currentIndex)
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: root.wizard.bootApplies && root.wizard.bootNote !== ""
                                text: root.wizard.bootNote
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                    }
                }   // StackLayout: pages
            }       // RowLayout: sidebar + pages

            Label {
                Layout.fillWidth: true
                visible: root.wizard.error !== ""
                text: root.wizard.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.wizard.dismiss()
                }
                Button {
                    text: root.wizard.editing ? qsTr("Save") : qsTr("Create")
                    // A failed submit leaves the window open with the
                    // reason in the error line above.
                    onClicked: if (root.wizard.submit()) root.saved()
                }
            }
        }
    }
}
