import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons
import qs.Commons as Commons

// Bar widget and settings panel for omarchy-xdr: the range that the ordinary
// brightness control covers on the built-in mini-LED panel. All state lives
// in the omarchy-xdr command; this only shows it and calls it.
Panel {
  id: root
  moduleName: "xdr.brightness"
  ipcTarget: "xdr.brightness"

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  readonly property string cli: Quickshell.env("HOME") + "/.local/bin/omarchy-xdr"

  property bool xdrLoaded: false
  property bool xdrOn: false
  property bool xdrActive: false
  property bool autostart: false
  property int maxNits: 1000
  property int minNits: 2
  property int maxLimit: 1600
  property int percent: 0
  property int nits: 0
  property bool known: false

  property var queue: []

  function refresh() {
    if (statusProc.running) return
    statusProc.running = true
  }

  // Run omarchy-xdr commands one after another, then re-read the state.
  function run(args) {
    queue = queue.concat([args])
    pump()
  }

  function pump() {
    if (actionProc.running) return
    if (queue.length === 0) { refresh(); return }
    var next = queue[0]
    queue = queue.slice(1)
    actionProc.command = [root.cli].concat(next)
    actionProc.running = true
  }

  function applyStatus(text) {
    var s = null
    try { s = JSON.parse(text) } catch (e) { s = null }
    if (!s || typeof s !== "object") { root.known = false; return }
    root.known = true
    root.xdrLoaded = s.loaded === 1 || s.loaded === true
    root.xdrOn = s.enabled === 1 || s.enabled === true
    root.xdrActive = s.active === 1 || s.active === true
    root.autostart = s.autostart === 1 || s.autostart === true
    root.maxNits = Math.max(500, Math.min(1600, Number(s.max) || 1000))
    root.minNits = Math.max(2, Math.min(100, Number(s.min) || 2))
    root.maxLimit = Math.max(500, Math.min(1600, Number(s.maxLimit) || 1600))
    root.percent = Math.max(0, Math.min(100, Number(s.percent) || 0))
    root.nits = Math.max(0, Number(s.nits) || 0)
  }

  function statusLine() {
    if (!root.known) return "omarchy-xdr not installed"
    if (!root.xdrLoaded) return "Kernel module not loaded"
    if (!root.xdrOn) return "Off: standard range"
    if (root.nits > 0) return root.percent + "% is about " + root.nits + " nits"
    return root.minNits + " to " + root.maxNits + " nits"
  }

  Component.onCompleted: refresh()
  onOpenedChanged: if (opened) refresh()

  Process {
    id: statusProc
    command: [root.cli, "status", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.applyStatus(String(text || ""))
    }
  }

  Process {
    id: actionProc
    onExited: function(exitCode) { root.pump() }
  }

  // The brightness keys change the level behind our back; keep the readout
  // current while the panel is open.
  Timer {
    interval: 1500
    repeat: true
    running: root.opened
    onTriggered: root.refresh()
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: root.xdrOn && root.xdrLoaded ? "󰃠" : "󰃞"
    onPressed: function(b) { root.toggle() }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(360))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(480))

    content: PanelKeyCatcher {
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      onCloseRequested: root.close()

      Column {
        id: column
        width: parent.width
        spacing: Style.space(14)

        // ---------- Title, state, master switch ----------
        Item {
          width: parent.width
          implicitHeight: Math.max(titles.implicitHeight, masterSwitch.implicitHeight)

          Column {
            id: titles
            anchors.left: parent.left
            anchors.right: masterSwitch.left
            anchors.rightMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Text {
              textFormat: Text.PlainText
              text: "XDR Brightness"
              color: root.bar.foreground
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.title
              font.bold: true
              elide: Text.ElideRight
              width: parent.width
            }

            Text {
              textFormat: Text.PlainText
              text: root.statusLine().toUpperCase()
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
              elide: Text.ElideRight
              width: parent.width
            }
          }

          ToggleSwitch {
            id: masterSwitch
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            checked: root.xdrOn
            interactive: root.xdrLoaded
            foreground: root.bar.foreground
            onToggled: root.run([root.xdrOn ? "disable" : "enable"])
          }
        }

        PanelSeparator { foreground: root.bar.foreground }

        // ---------- Maximum ----------
        Column {
          width: parent.width
          spacing: Style.space(6)
          opacity: root.xdrOn ? 1 : 0.5

          Item {
            width: parent.width
            implicitHeight: Math.max(maxHeader.implicitHeight, maxValue.implicitHeight)

            PanelSectionHeader {
              id: maxHeader
              text: "Maximum"
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
            }

            Text {
              id: maxValue
              textFormat: Text.PlainText
              text: Math.round((maxSlider.dragging ? maxSlider.liveValue : root.maxNits) / 50) * 50 + " nits"
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              anchors.right: parent.right
              anchors.rightMargin: Style.space(6)
              anchors.verticalCenter: parent.verticalCenter
            }
          }

          Item {
            width: parent.width
            height: maxSlider.implicitHeight + Style.spacing.controlGap

            PanelSlider {
              id: maxSlider
              bar: root.bar
              anchors.fill: parent
              anchors.leftMargin: Style.space(6)
              anchors.rightMargin: Style.space(6)
              minimum: 500
              maximum: root.maxLimit
              step: 50
              integer: true
              value: root.maxNits
              onReleased: function(v) { root.run(["max", String(Math.round(v / 50) * 50)]) }
            }
          }
        }

        // ---------- Minimum ----------
        Column {
          width: parent.width
          spacing: Style.space(6)
          opacity: root.xdrOn ? 1 : 0.5

          Item {
            width: parent.width
            implicitHeight: Math.max(minHeader.implicitHeight, minValue.implicitHeight)

            PanelSectionHeader {
              id: minHeader
              text: "Minimum"
              foreground: root.bar.foreground
              fontFamily: root.bar.fontFamily
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
            }

            Text {
              id: minValue
              textFormat: Text.PlainText
              text: Math.round(minSlider.dragging ? minSlider.liveValue : root.minNits) + " nits"
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              anchors.right: parent.right
              anchors.rightMargin: Style.space(6)
              anchors.verticalCenter: parent.verticalCenter
            }
          }

          Item {
            width: parent.width
            height: minSlider.implicitHeight + Style.spacing.controlGap

            PanelSlider {
              id: minSlider
              bar: root.bar
              anchors.fill: parent
              anchors.leftMargin: Style.space(6)
              anchors.rightMargin: Style.space(6)
              minimum: 2
              maximum: 50
              step: 1
              integer: true
              value: root.minNits
              onReleased: function(v) { root.run(["min", String(Math.round(v))]) }
            }
          }
        }

        PanelSeparator { foreground: root.bar.foreground }

        // ---------- Apply at login ----------
        Item {
          width: parent.width
          implicitHeight: Math.max(loginLabel.implicitHeight, loginSwitch.implicitHeight)

          Text {
            id: loginLabel
            textFormat: Text.PlainText
            text: "Apply at login"
            color: root.bar.foreground
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
          }

          ToggleSwitch {
            id: loginSwitch
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            checked: root.autostart
            foreground: root.bar.foreground
            onToggled: root.run(["autostart", root.autostart ? "off" : "on"])
          }
        }
      }
    }
  }
}
