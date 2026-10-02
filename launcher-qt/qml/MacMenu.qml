// The macOS menu bar's About item. `Main.qml` creates this only on
// macOS, so no other platform has to resolve the `Qt.labs.platform`
// import, and KDE's global menu does not grow a bar the other systems
// lack. Labs and not Quick Controls' own `MenuBar`: only labs lets an
// item carry `AboutRole`, which is what moves it into the application
// menu as "About 2ksbox", where a Mac user looks for it.
import QtQuick
import Qt.labs.platform as Platform

Platform.MenuBar {
    id: bar

    signal aboutRequested()

    Platform.Menu {
        title: qsTr("Help")
        Platform.MenuItem {
            text: qsTr("About 2ksbox")
            role: Platform.MenuItem.AboutRole
            onTriggered: bar.aboutRequested()
        }
    }
}
