#include "backend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {
// The brightness device of the built-in panel.
const char *const kBacklight = "apple-panel-bl";
}

Backend::Backend(QObject *parent) : QObject(parent) {
    m_cli = qEnvironmentVariable("XDR_CLI");
    if (m_cli.isEmpty())
        m_cli = QStandardPaths::findExecutable("omarchy-xdr");

    connect(&m_process, &QProcess::finished, this, [this] {
        if (m_statusRunning) {
            m_statusRunning = false;
            applyStatus(m_process.readAllStandardOutput());
        } else if (m_process.exitCode() != 0) {
            fail(QString::fromUtf8(m_process.readAllStandardError()).trimmed());
        }
        pump();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_statusRunning = false;
        fail(tr("Could not run %1.").arg(m_process.program()));
        pump();
    });

    // The brightness keys change the level behind the app's back.
    m_poll.setInterval(2000);
    connect(&m_poll, &QTimer::timeout, this, &Backend::refresh);
    m_poll.start();
    refresh();
}

void Backend::fail(const QString &message) {
    m_error = message;
    emit changed();
}

void Backend::refresh() {
    if (m_cli.isEmpty()) {
        m_ready = false;
        m_error = tr("The omarchy-xdr command is not installed.");
        emit changed();
        return;
    }
    if (m_process.state() != QProcess::NotRunning || !m_queue.isEmpty())
        return;
    m_statusRunning = true;
    m_process.start(m_cli, {"status", "--json"});
}

void Backend::applyStatus(const QByteArray &json) {
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        m_ready = false;
        m_error = tr("The omarchy-xdr command gave no status.");
        emit changed();
        return;
    }
    const auto o = doc.object();
    auto flag = [&](const char *key) { return o.value(key).toInt() == 1 || o.value(key).toBool(); };
    m_ready = true;
    m_loaded = flag("loaded");
    m_enabled = flag("enabled");
    m_autostart = flag("autostart");
    m_bar = flag("bar");
    m_max = qBound(500, o.value("max").toInt(1000), 1600);
    m_min = qBound(0, o.value("min").toInt(0), 50);
    m_maxLimit = qBound(500, o.value("maxLimit").toInt(1600), 1600);
    m_percent = qBound(0, o.value("percent").toInt(0), 100);
    m_nits = qMax(0, o.value("nits").toInt(0));
    m_error = m_loaded ? QString() : tr("The kernel module is not loaded. Run: sudo systemctl start omarchy-xdr-probe");
    emit changed();
}

void Backend::run(const QString &program, const QStringList &args) {
    m_queue.append(QStringList{program} + args);
    pump();
}

// Commands run one after another and the state is re-read when the queue
// drains, so a fast slider never overlaps two writes.
void Backend::pump() {
    if (m_process.state() != QProcess::NotRunning)
        return;
    if (m_queue.isEmpty()) {
        if (m_cli.isEmpty() || m_statusRunning)
            return;
        m_statusRunning = true;
        m_process.start(m_cli, {"status", "--json"});
        return;
    }
    const auto next = m_queue.takeFirst();
    m_process.start(next.first(), next.mid(1));
}

void Backend::setEnabled(bool on) { run(m_cli, {on ? "enable" : "disable"}); }
void Backend::setMaxNits(int nits) { run(m_cli, {"max", QString::number(nits)}); }
void Backend::setMinNits(int nits) { run(m_cli, {"min", QString::number(nits)}); }
void Backend::setAutostart(bool on) { run(m_cli, {"autostart", on ? "on" : "off"}); }
void Backend::setBarIcon(bool on) { run(m_cli, {"bar", on ? "on" : "off"}); }

// Lets the user try the range from here; the same call the brightness keys make.
void Backend::setPercent(int percent) {
    run(QStandardPaths::findExecutable("brightnessctl"),
        {"-q", "-d", kBacklight, "set", QString::number(qBound(1, percent, 100)) + "%"});
}
