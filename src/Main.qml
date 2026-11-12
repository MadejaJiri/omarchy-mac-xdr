import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 480
    height: 700
    minimumWidth: 400
    minimumHeight: 560
    visible: true
    title: qsTr("XDR Screen Control")
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    readonly property bool usable: backend.ready && backend.moduleLoaded
    readonly property color dim: "#9a9aa2"

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: win.usable
        onActivated: backend.setEnabled(!backend.enabled)
    }

    // A labelled slider row: title and value on one line, the slider below.
    component SliderRow: ColumnLayout {
        id: row
        property string title
        property string valueText
        property alias slider: control
        property bool dimmed: false
        signal committed(int value)
        Layout.fillWidth: true
        spacing: 2
        opacity: dimmed ? 0.45 : 1

        RowLayout {
            Layout.fillWidth: true
            Label { text: row.title; font.pixelSize: 14; Layout.fillWidth: true }
            Label { text: row.valueText; color: win.dim; font.pixelSize: 13 }
        }
        Slider {
            id: control
            Layout.fillWidth: true
            snapMode: Slider.SnapAlways
            enabled: win.usable
            onPressedChanged: if (!pressed) row.committed(Math.round(value))
        }
    }

    // A labelled switch row with a one-line explanation.
    component SwitchRow: RowLayout {
        id: row
        property string title
        property string detail
        property alias checked: toggle.checked
        signal toggled(bool on)
        Layout.fillWidth: true
        spacing: 12

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Label { text: row.title; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Label { text: row.detail; color: win.dim; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
        Switch {
            id: toggle
            enabled: win.usable || row.title === qsTr("Show icon in the top bar")
            onToggled: row.toggled(checked)
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 18

            // Title, state, master switch.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Label { text: "󰔎"; font.family: "JetBrainsMono Nerd Font"; font.pixelSize: 34; color: theme.accent }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label { text: qsTr("XDR Screen Control"); font.pixelSize: 20; font.bold: true }
                    Label {
                        font.pixelSize: 12
                        color: win.dim
                        text: !backend.ready ? qsTr("Unavailable")
                            : !backend.moduleLoaded ? qsTr("Kernel module not loaded")
                            : !backend.enabled ? qsTr("Off: standard brightness range")
                            : backend.batteryLimit && backend.onBattery ? qsTr("On battery: standard maximum")
                            : backend.nits > 0 ? qsTr("%1% is about %2 nits").arg(backend.percent).arg(backend.nits)
                            : qsTr("Extended range on")
                    }
                }
                Switch {
                    id: master
                    enabled: win.usable
                    checked: backend.enabled
                    onToggled: backend.setEnabled(checked)
                }
            }

            Rectangle {
                visible: backend.error !== ""
                Layout.fillWidth: true
                implicitHeight: errorLabel.implicitHeight + 20
                radius: Math.min(4, theme.radius)
                color: "#3a1d1d"
                Label {
                    id: errorLabel
                    anchors.fill: parent
                    anchors.margins: 10
                    text: backend.error
                    color: "#ffb4b4"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#26262a" }

            SliderRow {
                id: maxRow
                title: qsTr("Maximum")
                valueText: qsTr("%1 nits").arg(Math.round(slider.value))
                dimmed: !backend.enabled
                slider.from: 500
                slider.to: backend.maxLimit
                slider.stepSize: 50
                Binding { target: maxRow.slider; property: "value"; value: backend.maxNits; when: !maxRow.slider.pressed }
                onCommitted: function(v) { backend.setMaxNits(v) }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("What 100% brightness means. The panel's own peak is 1600 nits; Apple rates it for 1000 nits sustained across the whole screen.")
                color: win.dim
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            SliderRow {
                id: minRow
                title: qsTr("Minimum")
                valueText: Math.round(slider.value) === 0 ? qsTr("Off") : qsTr("%1 nits").arg(Math.round(slider.value))
                dimmed: !backend.enabled
                slider.from: 0
                slider.to: 50
                slider.stepSize: 1
                Binding { target: minRow.slider; property: "value"; value: backend.minNits; when: !minRow.slider.pressed }
                onCommitted: function(v) { backend.setMinNits(v) }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("At Off, the lowest brightness step switches the backlight off, as on macOS. Brighten with the brightness keys to bring it back.")
                color: win.dim
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            SliderRow {
                id: nowRow
                title: qsTr("Brightness now")
                valueText: "%1%".arg(Math.round(slider.value))
                slider.from: 1
                slider.to: 100
                slider.stepSize: 1
                Binding { target: nowRow.slider; property: "value"; value: backend.percent; when: !nowRow.slider.pressed }
                onCommitted: function(v) { backend.setPercent(v) }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#26262a" }

            SwitchRow {
                title: qsTr("Show icon in the top bar")
                detail: qsTr("Quick access to these settings from the bar.")
                checked: backend.barIcon
                onToggled: function(on) { backend.setBarIcon(on) }
            }
            SwitchRow {
                title: qsTr("Standard maximum on battery")
                detail: qsTr("Cap brightness at the normal maximum while unplugged, to save power. The minimum still applies.")
                checked: backend.batteryLimit
                onToggled: function(on) { backend.setBatteryLimit(on) }
            }
            SwitchRow {
                title: qsTr("Apply at login")
                detail: qsTr("Restore this range every time you sign in.")
                checked: backend.autostart
                onToggled: function(on) { backend.setAutostart(on) }
            }

            Item { Layout.fillHeight: true }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("? for keyboard shortcuts")
                color: win.dim
                font.pixelSize: 11
            }
        }

        // Keyboard shortcut overlay.
        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: "#d0000000"
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 340)
                height: help.implicitHeight + 40
                radius: Math.min(8, theme.radius)
                color: "#18181b"
                border.color: "#2c2c31"
                ColumnLayout {
                    id: help
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 8
                    Label { text: qsTr("Keyboard shortcuts"); font.bold: true; font.pixelSize: 15 }
                    Repeater {
                        model: [
                            { key: "Space", what: qsTr("Turn the extended range on or off") },
                            { key: "?", what: qsTr("Show or hide this list") },
                            { key: "Q", what: qsTr("Quit") }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label { text: modelData.key; color: theme.accent; font.family: "monospace"; Layout.preferredWidth: 60 }
                            Label { text: modelData.what; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        }
                    }
                }
            }
        }
    }
}
