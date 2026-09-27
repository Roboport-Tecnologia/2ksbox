// The launcher window: the machine library grid, and the four windows
// off it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

ApplicationWindow {
    id: root

    // Wide enough for a row's five buttons with room to spare: at 900
    // "Clone…" sat flush against the edge, and at 980 the Fluent style's
    // wider buttons clipped it.
    width: 1060
    height: 560
    visible: true
    title: qsTr("2ksbox")

    // --- state ------------------------------------------------------

    MachineModel {
        id: machines
        Component.onCompleted: refresh()
    }

    // A child process cannot push the news that it exited, so this polls
    // for it.
    Timer {
        interval: 500
        running: true
        repeat: true
        onTriggered: machines.poll()
    }

    Diag { id: diag }

    // The first-run shader offer (`src/qt/firstrun.rs`): a question over
    // the grid on the first start of a launcher with no preset
    // collection, and never again once it has been answered. The model's
    // `step` drives the dialog both ways, the way the wizard's and the
    // shader editor's `open` flags drive their windows.
    FirstRun {
        id: offer
        // Each step that has something to answer opens its own dialog;
        // neither is ever closed from here (`FirstRunDialog.qml` says
        // why: a MessageDialog closed programmatically emits
        // `rejected()` and would answer its own question).
        onStepChanged: {
            if (step === "asking") {
                firstRunDialog.open()
            } else if (step === "failed" || step === "done") {
                firstRunResultDialog.open()
            } else if (step === "") {
                // Answered and finished with. A collection may have
                // landed since these two were built, and with it the
                // starter profiles: the editor cached "there is none" at
                // construction, and the grid's Shader column lists what
                // the profile library holds.
                editor.rescanPresets()
                profiles.refresh()
                machines.refresh()
            }
        }
        // The first step is set before this file's bindings exist, so it
        // never arrives as a change.
        Component.onCompleted: if (step === "asking") firstRunDialog.open()
    }

    FirstRunDialog {
        id: firstRunDialog
        offer: offer
    }

    FirstRunResultDialog {
        id: firstRunResultDialog
        offer: offer
    }

    // The download runs on its own thread; nothing else would move the
    // megabytes in the header. It stops when the download does.
    Timer {
        interval: 300
        running: offer.busy
        repeat: true
        onTriggered: offer.poll()
    }

    // --- the grid ---------------------------------------------------

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Label {
                text: qsTr("Machines")
                font.pixelSize: 18
                font.bold: true
                Layout.fillWidth: true
            }
            // The preset download, while it runs. Here and not in a
            // dialog: it needs no answer, and a modal window with a
            // spinner would lock the launcher for a minute over a job the
            // user already agreed to. Both strings are the model's
            // (`firstrun::Message`).
            RowLayout {
                spacing: 6
                visible: offer.busy
                BusyIndicator { implicitWidth: 16; implicitHeight: 16; running: visible }
                Label {
                    text: offer.headline + " " + offer.detail
                    opacity: 0.7
                }
            }
            Label {
                // A failure to start a player is a whole sentence with
                // a path in it, and this label is 320px wide. Without
                // the tooltip the elided text reads as nothing having
                // happened.
                text: machines.status
                opacity: 0.7
                elide: Text.ElideRight
                Layout.maximumWidth: 320
                ToolTip.visible: statusHover.hovered && machines.status !== ""
                ToolTip.text: machines.status
                HoverHandler { id: statusHover }
            }
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // Column widths shared by the header and every row, so the two
        // cannot drift. Each fixed column is pinned (minimum = preferred
        // = maximum): a preferred width alone lets the RowLayout size a
        // column by its text, which put every row's buttons somewhere
        // else. The shader column takes whatever width is left, so the
        // buttons sit against the right edge at any window width, and
        // `shader` is its minimum.
        QtObject {
            id: cols
            readonly property int name: 190
            readonly property int family: 80
            readonly property int shader: 170
        }

        // A stock list with nothing drawn by hand (user decision: no list
        // box, no zebra rows, no colours of ours, only the style's own
        // look). A `ListView` of `ItemDelegate`s under a header row of
        // labels whose margins are a delegate's own padding, read off an
        // invisible one, so the columns line up under every style.
        ItemDelegate { id: rowMetrics; visible: false }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: rowMetrics.leftPadding
            Layout.rightMargin: rowMetrics.rightPadding
            spacing: 10

            Label {
                text: qsTr("Name"); font.bold: true
                Layout.minimumWidth: cols.name; Layout.preferredWidth: cols.name; Layout.maximumWidth: cols.name
            }
            Label {
                text: qsTr("Family"); font.bold: true
                Layout.minimumWidth: cols.family; Layout.preferredWidth: cols.family; Layout.maximumWidth: cols.family
            }
            Label {
                text: qsTr("Shader"); font.bold: true
                Layout.minimumWidth: cols.shader; Layout.fillWidth: true
            }
        }

        MenuSeparator { Layout.fillWidth: true }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: machines
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: machineRow

                // Declared `required`, so the roles arrive as
                // real properties of this item rather than out of
                // a context object nothing can see.
                required property int index
                required property string name
                required property string family
                required property string shader
                required property bool running

                width: list.width

                contentItem: RowLayout {
                    spacing: 10

                    Label {
                        text: machineRow.name
                        elide: Text.ElideRight
                        Layout.minimumWidth: cols.name; Layout.preferredWidth: cols.name; Layout.maximumWidth: cols.name
                    }
                    Label {
                        text: machineRow.family
                        elide: Text.ElideRight
                        Layout.minimumWidth: cols.family; Layout.preferredWidth: cols.family; Layout.maximumWidth: cols.family
                    }
                    Label {
                        text: machineRow.shader
                        elide: Text.ElideRight
                        Layout.minimumWidth: cols.shader; Layout.fillWidth: true
                    }

                    RowLayout {
                        spacing: 6

                        Label {
                            text: qsTr("Running")
                            visible: machineRow.running
                            Layout.preferredWidth: 60
                        }
                        Button {
                            text: qsTr("Play")
                            visible: !machineRow.running
                            Layout.preferredWidth: 60
                            onClicked: machines.play(machineRow.index)
                        }
                        Button {
                            text: qsTr("Edit…")
                            onClicked: {
                                profiles.refresh()
                                wizard.openEdit(machines.bundlePath(machineRow.index))
                            }
                        }
                        Button {
                            text: qsTr("Discs…")
                            onClicked: {
                                discs.openFor(machines.bundlePath(machineRow.index),
                                              machines.discLibraryPath(),
                                              machines.isRunning(machineRow.index))
                                discShelfWindow.show()
                            }
                        }
                        Button {
                            text: qsTr("Snapshots…")
                            onClicked: {
                                snapshots.openFor(machines.bundlePath(machineRow.index),
                                                  machines.isRunning(machineRow.index))
                                snapshotsWindow.show()
                            }
                        }
                        Button {
                            text: qsTr("Clone…")
                            // One copy at a time: the window
                            // shows the one that is running.
                            enabled: !cloner.busy
                            onClicked: cloner.openFor(machines.bundlePath(machineRow.index),
                                                      machines.isRunning(machineRow.index))
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: machines.count === 0
                horizontalAlignment: Text.AlignHCenter
                opacity: 0.7
                text: qsTr("No machines yet.\n%1").arg(machines.libraryDir)
            }
        }

        RowLayout {
            spacing: 8
            Button {
                text: qsTr("New machine…")
                onClicked: {
                    wizard.openFresh()
                    profiles.refresh()
                }
            }
            Button {
                text: qsTr("Disc shelf…")
                onClicked: {
                    discs.openLibrary(machines.discLibraryPath())
                    discShelfWindow.show()
                }
            }
            Button {
                text: qsTr("Shader profiles…")
                onClicked: {
                    profiles.refresh()
                    shaderWindow.show()
                }
            }
            Item { Layout.fillWidth: true }
        }
    }

    // --- the secondary windows --------------------------------------

    Wizard {
        id: wizard
        // The form's own `open` flag drives the window in both
        // directions, so a `submit()` that succeeds (which clears it)
        // puts the window away wherever it was called from.
        onOpenChanged: open ? wizardWindow.show() : closeIfShown(wizardWindow)
    }
    CloneModel {
        id: cloner
        // Same shape as the wizard: a clone that lands clears the flag
        // and the window goes away wherever it was.
        onOpenChanged: open ? cloneWindow.show() : closeIfShown(cloneWindow)
    }
    // A copy runs on its own thread; this polls it only while there is
    // one. When it lands the grid rescans, which shows the new machine.
    Timer {
        interval: 300
        repeat: true
        running: cloner.busy
        onTriggered: {
            if (cloner.poll()) {
                machines.refresh()
                if (cloner.status !== "")
                    machines.status = cloner.status
            }
        }
    }
    DiscModel { id: discs }
    SnapshotModel { id: snapshots }
    ProfileModel { id: profiles }
    ShaderEditor {
        id: editor
        // Same shape as the wizard: the editor's own `open` flag drives
        // its window both ways, so a save (which clears it) puts the
        // window away wherever it was called from.
        onOpenChanged: open ? shaderEditorWindow.show() : closeIfShown(shaderEditorWindow)
    }

    /// Close a flag-driven window, unless it is already on its way out.
    ///
    /// The flag is cleared from two directions: by a button (Cancel, a
    /// successful save), and by the window's own `onVisibleChanged` when
    /// the title bar's close button hid it. The second is already inside
    /// Qt's close: `destroy()` flips `visible` and emits the signal
    /// before it unregisters the modal window and hides the platform
    /// window. Calling `close()` back from there delivers a second close
    /// event to a window that is half gone. It deletes the platform
    /// window from inside the first event, and the first one, finding it
    /// null, skips the platform `setVisible(false)`. On macOS that
    /// skipped call is `endModalSession`, so the dialog was gone and the
    /// main window stayed locked behind it. `visible` is already false at
    /// that moment, which is the tell. The `closebox` probe below
    /// counts the close events the
    /// window receives; the `qt-close` check in `scripts/test.sh` wants
    /// exactly one.
    function closeIfShown(w) {
        if (w.visible)
            w.close()
    }

    WizardWindow {
        id: wizardWindow
        wizard: wizard
        onSaved: machines.refresh()
    }

    CloneWindow {
        id: cloneWindow
        cloner: cloner
    }

    DiscShelfWindow {
        id: discShelfWindow
        discs: discs
        onChanged: {
            machines.refresh()
            // A disc added or renamed should show up in the guest's own
            // CDSHELF listing without restarting the machine, so every
            // running drive gets the new shelf file, here, where the
            // running set lives.
            if (discs.takeSaved())
                machines.republishShelf()
        }
    }

    SnapshotsWindow {
        id: snapshotsWindow
        snapshots: snapshots
    }

    ShaderProfilesWindow {
        id: shaderWindow
        profiles: profiles
        editor: editor
        // The form's own picker lists the library too (a machine being
        // edited while a profile is deleted must not go on offering it).
        onChanged: { wizard.refreshProfiles(); machines.refresh() }
    }

    ShaderEditorWindow {
        id: shaderEditorWindow
        editor: editor
        profilesDir: machines.profileDir()
        onChanged: {
            profiles.refresh()
            wizard.refreshProfiles()
            machines.refresh()
        }
    }

    // --- headless screenshots (see src/qt/diag.rs) -------------------

    /// The open secondary window's own QML-declared body, or null when
    /// the grid is what should be captured.
    function openWindowItem() {
        // The first-run offer is not in this list: it is a
        // `MessageDialog`, a window the platform builds, and
        // `grabToImage` only works on an item the QML engine created
        // (see the note on `grabTimer` below). Its `firstrun` probe
        // screen prints what it holds instead of photographing it.
        const windows = [wizardWindow, cloneWindow, discShelfWindow, snapshotsWindow,
                         shaderWindow, shaderEditorWindow]
        for (const d of windows)
            if (d.visible && d.grabItem)
                return d.grabItem
        return null
    }

    /// Close events the wizard window has received, the `closebox`
    /// probe's count.
    property int closeEvents: 0
    Connections {
        target: wizardWindow
        function onClosing(close) { closeEvents++ }
    }

    Timer {
        // A screen with no shot path is a run that only drives the
        // window and prints what it shows, which needs no GPU and works
        // on a busy one.
        running: diag.shotPath !== "" || diag.screen !== ""
        interval: diag.delayMs
        onTriggered: {
            diag.note("arming screen=" + diag.screen + " shot=" + diag.shotPath)
            switch (diag.screen) {
            case "wizard":
                wizard.openFresh(); profiles.refresh(); wizardWindow.show()
                // `LAUNCHER_QT_ARG=<win98|xp|dos|other>[:<page>]` exercises the one piece
                // of form behaviour a screenshot can prove:
                // switching family moves the memory, processor,
                // acceleration and networking defaults with it, but only
                // while nobody has chosen them. The order is the shared
                // form's own (`bundle::Family::ALL`), which is also the
                // order `familyLabels()` hands the combo box, so the two
                // cannot get out of step.
                const families = ["win98", "xp", "dos", "other"]
                // The page the shot is of, as a section index after a
                // colon (`xp:2` is the Display page), for looking at one
                // row of the form rather than always its first page.
                const wizardArg = diag.arg.split(":")
                diag.arg = wizardArg[0]
                // Typed before the family moves, because the order is
                // the bug: a text field writes the model property and
                // nothing else, so a verb that republishes the form
                // without catching it up first writes the form's stale,
                // empty name back over what was typed.
                wizardWindow.typeName("Typed name")
                wizardWindow.typeExtraQemuArgs('-name "typed args"')
                // A page away and back first, the way the sidebar does
                // it: the page switch is a verb too, and it once
                // republished the form without catching it up (the name
                // vanished on a click on "System" and back). It comes
                // before the family, whose own verb catches the form up
                // and would hide the bug.
                wizard.chooseSection(1); wizard.chooseSection(0)
                if (families.indexOf(diag.arg) >= 0)
                    wizard.chooseFamily(families.indexOf(diag.arg))
                // What the memory field ended up showing, beside what the
                // model says it should: a spin box bounds the value it is
                // handed against the range it has at that moment, so the
                // two can disagree unseen by the model (a fresh Win98
                // machine once showed 32 MB).
                diag.note("wizard memory: shown " + wizardWindow.shownRamMb
                          + ", model " + wizard.ramMb
                          + ", range " + wizard.ramMin + ".." + wizard.ramMax)
                diag.note("wizard name: shown [" + wizardWindow.shownName
                          + "] model [" + wizard.name + "]")
                diag.note("wizard extra args: shown [" + wizardWindow.shownExtraQemuArgs
                          + "] model [" + wizard.extraQemuArgs + "]")
                // The Direct3D row, for the reason WizardWindow.qml gives
                // beside `shownD3d9`: a binding to a property that does
                // not exist leaves the combo box empty and says nothing.
                diag.note("wizard direct3d: shown [" + wizardWindow.shownD3d9
                          + "] of " + wizardWindow.shownD3d9Count
                          + " model " + wizard.d3d9 + " applies " + wizard.d3d9Applies)
                // ...and the sentence under it, which carries the host's
                // own answer.
                diag.note("wizard direct3d note: [" + wizard.d3d9Note.replace(/\n/g, " | ")
                          + "] warning " + wizard.d3d9Warning)
                // The shader profile combo, whose rows come from the model
                // the same way: the app default and then the library, one
                // of them showing.
                diag.note("wizard shader: shown [" + wizardWindow.shownShaderProfile
                          + "] of " + wizardWindow.shownShaderProfileCount
                          + " model " + wizard.shaderProfileIndex
                          + " default " + wizard.shaderProfileIsDefault)
                // What the window's height has to hold: each page's content
                // against the room a page gets, since the default was once
                // too tall even for the longest page.
                diag.note("wizard pages: " + wizardWindow.pageReport())
                if (wizardArg.length > 1)
                    wizard.chooseSection(parseInt(wizardArg[1]))
                else
                    Qt.callLater(wizardWindow.revealExtraQemuArgs)
                break
            case "optall":
                // The optimization shortcuts beside boxes somebody already
                // clicked ("Turn all on / off does nothing" was reported on
                // a machine with three boxes unticked by hand). The model
                // moves every time; the question is whether the boxes
                // still follow it once a click has been through them, so
                // every step prints what they show beside the model's
                // mask.
                wizard.openFresh(); profiles.refresh(); wizardWindow.show()
                const optReport = step => diag.note("optall " + step + ": shown "
                    + wizardWindow.shownOptimizationsMask() + " model " + wizard.optimizationsMask)
                diag.note("optall boxes " + wizardWindow.optimizationBoxes)
                // tb-invalidate-fast, tlb-floor, tls-hot-paths: the
                // reported three, by their place in `Optimization::ALL`
                for (const i of [7, 8, 9])
                    wizardWindow.clickOptimization(i)
                optReport("clicked")
                wizardWindow.clickOptimizationShortcut("on"); optReport("on")
                wizardWindow.clickOptimizationShortcut("off"); optReport("off")
                wizardWindow.clickOptimizationShortcut("defaults"); optReport("defaults")
                break
            case "closebox":
                // The title bar's close button on the wizard, the way the
                // window system delivers it: a close event, not
                // `close()`, which is the path a button takes and which
                // Qt guards against re-entry. Counts the close events the
                // window sees (`closeIfShown` says why two is the bug)
                // and asks whether a modal window is still registered.
                wizard.openFresh(); profiles.refresh(); wizardWindow.show()
                closeEvents = 0
                const modalLeft = diag.closeModalFromWindowSystem()
                diag.note("closebox: " + closeEvents + " close events, open="
                          + wizard.open + ", visible=" + wizardWindow.visible
                          + ", modal left=" + modalLeft)
                break
            case "wizardscroll":
                // Where the form opens: its first page at the top for a
                // new machine and for a different one than last time,
                // where it was left (page and scroll) for the same one
                // again (`WizardWindow.onVisibleChanged`).
                // A machine to edit comes first, through the create path
                // below; `LAUNCHER_QT_ARG=<disk>` names an existing disk
                // where `/dev/null` is not one. The steps are
                // `wizardScroll`: each open has to be shown and laid out
                // before its scroll position means anything.
                wizard.openFresh()
                wizard.chooseFamily(1)
                wizard.name = "scroll probe"
                wizard.existingDisk = true
                wizard.diskPath = diag.arg !== "" ? diag.arg : "/dev/null"
                diag.note("wizardscroll submit -> " + wizard.submit() + " " + wizard.savedPath())
                machines.refresh()
                wizardScroll.bundle = wizard.savedPath()
                wizardScroll.start()
                return   // `wizardScroll` ends the run
            case "create":
                // `LAUNCHER_QT_ARG=[<family>:]<name>`: the whole create
                // path, ending on the refreshed grid, so the run passes
                // only if the bundle landed in the library. The family is
                // worth naming: a bundle written through this window has
                // to match one written by `--wizard-new`, and DOS once
                // didn't.
                wizard.openFresh()
                const spec = diag.arg.split(":")
                const named = spec.length > 1
                const family = named ? ["win98", "xp", "dos", "other"].indexOf(spec[0]) : 1
                wizard.chooseFamily(family < 0 ? 1 : family)
                wizard.name = named ? spec[1] : diag.arg
                wizard.existingDisk = true
                wizard.diskPath = "/dev/null"
                diag.note("submit -> " + wizard.submit() + " " + wizard.savedPath())
                machines.refresh()
                break
            case "clone":
                // `LAUNCHER_QT_ARG=<machine.toml>`: a row's Clone… the
                // way a person does it. The window comes up with the name
                // the model offers, a new name is typed over it, Clone is
                // pressed, and the copy is waited out (`cloneSettle`),
                // ending on the rescanned grid, so it passes only if the
                // new machine landed in the library. `<path>;show` stops
                // at the open window, for a picture of it; `<path>;same`
                // ticks "same hard disk" first.
                const cloneSpec = diag.arg.split(";")
                cloner.openFor(cloneSpec[0], false)
                diag.note("clone offered [" + cloneWindow.shownName + "] model [" + cloner.name
                          + "], window " + cloneWindow.visible + ", can clone " + cloner.canSubmit
                          + ", error [" + cloner.error + "]")
                if (cloneSpec[1] === "show")
                    break
                if (cloneSpec[1] === "same") {
                    cloneWindow.clickSameDisk()
                    diag.note("clone same disk " + cloner.sameDisk + ", warning [" + cloner.warning + "]")
                }
                cloneWindow.retypeName("Typed twin")
                cloneWindow.clickClone()
                diag.note("clone started: busy=" + cloner.busy + ", error [" + cloner.error + "]")
                cloneSettle.start()
                return   // `cloneSettle` grabs when it is done
            case "adddisc":
                // `LAUNCHER_QT_ARG=<path>` onto the shared shelf.
                discs.openLibrary(machines.discLibraryPath())
                discs.add(diag.arg)
                diag.note("shelf now " + discs.count + " discs, status: " + discs.status)
                discShelfWindow.show()
                break
            case "pickdisc":
                // `LAUNCHER_QT_ARG=<path>` through the "Add disc"
                // field's dialog, not the model: a picked disc goes on
                // the shelf on its own, and the field it came through is
                // left empty. Nothing that asks the model can see either
                // half.
                discs.openLibrary(machines.discLibraryPath())
                discShelfWindow.show()
                const pickedUrl = discShelfWindow.pickDisc(diag.arg)
                diag.note("pickdisc: shelf " + discs.count + ", field ["
                          + discShelfWindow.shownAdd + "], status: " + discs.status
                          + ", filters [" + discShelfWindow.addFilters.join(" | ") + "]"
                          + ", url " + pickedUrl)
                break
            case "discs":
                if (diag.arg === "")
                    discs.openLibrary(machines.discLibraryPath())
                else
                    discs.openFor(diag.arg, machines.discLibraryPath(), false)
                discShelfWindow.show(); break
            case "snapshots":
                // `LAUNCHER_QT_SIZE=<w>x<h>` opens it at that size, for
                // the columns at a width other than the one it starts at.
                snapshots.openFor(diag.arg, false)
                if (diag.size !== "") {
                    const wh = diag.size.split("x")
                    snapshotsWindow.width = Number(wh[0])
                    snapshotsWindow.height = Number(wh[1])
                }
                snapshotsWindow.show(); break
            case "profiles":
                profiles.refresh(); shaderWindow.show(); break
            case "saveprofile":
                // `LAUNCHER_QT_ARG=<preset path>`: the flow a person
                // does. The profile list open, New profile…, a name, a
                // preset typed into the real field, Save, and then New
                // profile… again. Three things only a probe that drives
                // the windows can see, because the model is right in all
                // of them (doc 07): the list behind the editor has to
                // gain the profile just saved, the fresh editor's preset
                // field has to come up empty rather than showing the
                // last one, and the name typed before the preset has to
                // survive picking it (`save()` failing on `a name is
                // required` is that one).
                profiles.refresh()
                shaderWindow.show()
                const before = profiles.count
                editor.newProfile()
                editor.name = "Probe profile"
                shaderEditorWindow.typePreset(diag.arg)
                shaderEditorWindow.clickSave()
                diag.note("saveprofile: list " + before + " -> " + profiles.count
                          + ", editor open=" + editor.open
                          + ", dir=" + shaderEditorWindow.profilesDir
                          + ", error='" + editor.error + "'")
                editor.newProfile()
                diag.note("saveprofile: fresh preset field '"
                          + shaderEditorWindow.shownPreset
                          + "', model '" + editor.presetPath + "'")
                break
            case "firstrun":
                // The offer, driven through the dialog itself rather
                // than through the model behind it: that the real
                // `MessageDialog` is up, that it is showing the model's
                // words, and that its reject button (No) both closes it
                // and is remembered (the marker the check looks for).
                // `LAUNCHER_QT_ARG=decline` presses it; anything else
                // leaves the question standing.
                diag.note("firstrun: open=" + offer.open + ", dialog=" + firstRunDialog.visible
                          + ", step=" + offer.step + ", modality=" + firstRunDialog.modality
                          + ", buttons=" + firstRunDialog.buttons)
                diag.note("firstrun text: " + firstRunDialog.text + " | "
                          + firstRunDialog.informativeText.replace(/\n/g, " "))
                // The answers go in through the dialog's own signals,
                // what its standard buttons deliver, so the wiring from a
                // button to a verb is checked, not bypassed. Not
                // `reject()` / `accept()`: those both emit `rejected()`,
                // which is why these dialogs are never driven
                // programmatically.
                if (diag.arg === "decline") {
                    firstRunDialog.rejected()
                    // Not the dialog's own visibility: only a real press
                    // of No hides it, and this emits the signal that
                    // press delivers.
                    diag.note("firstrun declined: open=" + offer.open + ", step=" + offer.step)
                }
                if (diag.arg === "accept") {
                    // Yes, and then wait for the download to end. Point
                    // `LAUNCHER_SHADERS_DIR` somewhere unwritable and it
                    // ends at once, the cheap way to reach the step after
                    // it without fetching 50 MB.
                    firstRunDialog.accepted()
                    diag.note("firstrun accepted: dialog=" + firstRunDialog.visible
                              + ", step=" + offer.step + ", busy=" + offer.busy)
                    firstRunSettle.start()
                    return   // `firstRunSettle` grabs when it is done
                }
                break
            case "profilesclose":
                // The wizard opened once and cancelled, then the profile
                // list opened and closed: the list's close rescans the
                // wizard's profiles, a republish, and a wizard put away by
                // writing its `open` *property* (its form's own flag still
                // up) came back with it (user report, 2026-09-23). Every
                // step here is synchronous, so the answer is immediate.
                wizard.openFresh(); profiles.refresh()
                wizardWindow.close()
                diag.note("profilesclose after cancel: open=" + wizard.open + ", visible=" + wizardWindow.visible)
                profiles.refresh(); shaderWindow.show()
                shaderWindow.close()
                diag.note("profilesclose after list: open=" + wizard.open + ", visible=" + wizardWindow.visible)
                break
            case "escfocus":
                // New profile… from the profile list, and whether Esc can
                // close the editor it opens: the editor has to have the
                // keyboard, its Esc armed, and be the only armed Esc that
                // matches: two is an ambiguous shortcut, which Qt fires in
                // neither window. The steps are `escFocusSettle`, because
                // activation is asynchronous.
                profiles.refresh()
                shaderWindow.show()
                escFocusSettle.step = 0
                escFocusSettle.start()
                return   // `escFocusSettle` grabs when it is done
            case "editor":
                // `<preset.slangp>;<preview image>`
                const parts = diag.arg.split(";")
                profiles.refresh()
                shaderEditorWindow.editPreset(parts[0], parts[1] || "")
                break
            }
            grabTimer.restart()
        }
    }

    // The `firstrun accept` probe's wait: poll the download until it is
    // no longer running, then report what came up in the question's
    // place. It proves what a still picture cannot: that the question
    // closes on Yes, that the header (not a modal) shows the download,
    // and that the result dialog then arrives with its own words and
    // buttons.
    Timer {
        id: firstRunSettle
        interval: 200
        repeat: true
        onTriggered: {
            offer.poll()
            if (offer.busy)
                return
            stop()
            // Not whether the question is still up: this probe emits the
            // dialog's `accepted` rather than pressing its Yes, and only
            // a real press hides it. What it can say is what came after.
            diag.note("firstrun settled: result=" + firstRunResultDialog.visible
                      + ", step=" + offer.step
                      + ", buttons=" + firstRunResultDialog.buttons
                      + ", text=" + firstRunResultDialog.text
                      + " | " + firstRunResultDialog.informativeText.replace(/\n/g, " "))
            grabTimer.restart()
        }
    }

    // The `wizardscroll` probe's steps, one per beat: open, then read or
    // move the scroll position once the window has been laid out, then
    // close through the same path the title bar's button takes.
    Timer {
        id: wizardScroll
        property string bundle
        property int step: 0
        interval: diag.delayMs
        repeat: true
        onTriggered: {
            const at = (what) => diag.note("wizardscroll " + what + ": section=" + wizard.section
                                            + " y=" + wizardWindow.scrollY())
            // The System page with the optimizations open is the one page
            // taller than the window, so it is where a scroll can be seen.
            const scrollDown = () => {
                wizard.chooseSection(1); wizardWindow.expandOptimizations(); wizardWindow.scrollTo(240)
            }
            switch (step++) {
            case 0: wizard.openEdit(bundle); break
            case 1: scrollDown(); at("edit scrolled"); wizardWindow.close(); break
            case 2: wizard.openEdit(bundle); break
            case 3: at("same again"); wizardWindow.close(); break
            case 4: wizard.openFresh(); break
            case 5: at("fresh after edit"); scrollDown(); wizardWindow.close(); break
            case 6: wizard.openFresh(); break
            case 7: at("fresh again"); wizardWindow.close(); break
            case 8: wizard.openEdit(bundle); break
            case 9: at("edit after fresh"); wizardWindow.close(); stop(); grabTimer.restart(); break
            }
        }
    }

    // The `clone` probe's wait: the copy is polled by the timer beside
    // `cloner` above, exactly as for a person; this only waits for it to
    // end and reports what it left: the window, the model and the grid.
    Timer {
        id: cloneSettle
        interval: 200
        repeat: true
        onTriggered: {
            if (cloner.busy)
                return
            stop()
            diag.note("clone settled: open=" + cloner.open + ", window " + cloneWindow.visible
                      + ", error [" + cloner.error + "], status [" + machines.status
                      + "], saved " + cloner.savedPath() + ", grid " + machines.count)
            grabTimer.restart()
        }
    }

    // The `escfocus` probe's steps: the profile list has had a beat to
    // come up, then New profile… the way its button does it, then the
    // editor has had a beat. Each reports which window has the focus.
    Timer {
        id: escFocusSettle
        property int step: 0
        interval: diag.delayMs
        repeat: true
        onTriggered: {
            if (step === 0) {
                diag.note("escfocus list: focus=[" + diag.focusWindow() + "]")
                editor.newProfile()
                step = 1
                return
            }
            stop()
            // How many Esc shortcuts would claim the key, counted the way
            // Quick Controls' matcher decides it: armed, in a window that
            // is `active`, which a transient window reports whenever its
            // parent is, so both windows here count. Two is ambiguous, and
            // Qt fires neither.
            const matches = [shaderWindow, shaderEditorWindow]
                .filter(w => w.visible && w.active && w.escArmed).length
            diag.note("escfocus editor: visible=" + shaderEditorWindow.visible
                      + ", focus=[" + diag.focusWindow() + "]"
                      + ", esc armed=" + shaderEditorWindow.escArmed
                      + ", esc matches=" + matches)
            grabTimer.restart()
        }
    }

    // A second beat so the window just opened has been laid out and
    // rendered before the grab.
    Timer {
        id: grabTimer
        interval: diag.delayMs
        onTriggered: {
            if (snapshotsWindow.visible)
                diag.note("snapshots layout: " + snapshotsWindow.layoutReport())
            if (cloneWindow.visible)
                diag.note("clone layout: " + cloneWindow.layoutReport())
            if (diag.shotPath === "") {   // driven, not photographed
                Qt.quit()
                return
            }
            const cb = function (result) {
                diag.report(result.saveToFile(diag.shotPath))
                Qt.quit()
            }
            // `grabToImage` only works on an item the QML engine created:
            // it starts with `qmlEngine(this)` and a window's own
            // `contentItem` (and `Overlay.overlay`) are made in C++, so
            // both refuse with no warning. So the grab targets below are
            // always items declared in QML, and a whole-window headless
            // shot, dialog frame and all, would need a small C++ shim
            // calling `QQuickWindow::grabWindow()` (doc 07).
            const target = openWindowItem() || body
            diag.note("grabbing " + target.width + "x" + target.height
                      + " -> " + target.grabToImage(cb))
        }
    }
}
