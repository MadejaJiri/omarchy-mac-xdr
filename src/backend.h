#pragma once

#include <QList>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

// The app's state is the `omarchy-xdr` command's state: this object reads
// `omarchy-xdr status --json` and runs its subcommands, one at a time, so the
// GUI, the top-bar widget and a terminal always agree.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool moduleLoaded READ moduleLoaded NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool autostart READ autostart NOTIFY changed)
    Q_PROPERTY(bool barIcon READ barIcon NOTIFY changed)
    Q_PROPERTY(int maxNits READ maxNits NOTIFY changed)
    Q_PROPERTY(int minNits READ minNits NOTIFY changed)
    Q_PROPERTY(int maxLimit READ maxLimit NOTIFY changed)
    Q_PROPERTY(int percent READ percent NOTIFY changed)
    Q_PROPERTY(int nits READ nits NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit Backend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool moduleLoaded() const { return m_loaded; }
    bool enabled() const { return m_enabled; }
    bool autostart() const { return m_autostart; }
    bool barIcon() const { return m_bar; }
    int maxNits() const { return m_max; }
    int minNits() const { return m_min; }
    int maxLimit() const { return m_maxLimit; }
    int percent() const { return m_percent; }
    int nits() const { return m_nits; }
    QString error() const { return m_error; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setEnabled(bool on);
    Q_INVOKABLE void setMaxNits(int nits);
    Q_INVOKABLE void setMinNits(int nits);
    Q_INVOKABLE void setAutostart(bool on);
    Q_INVOKABLE void setBarIcon(bool on);
    Q_INVOKABLE void setPercent(int percent);

    // Used by tests and by the offscreen screenshot harness.
    void applyStatus(const QByteArray &json);

signals:
    void changed();

private:
    void run(const QString &program, const QStringList &args);
    void pump();
    void fail(const QString &message);

    QString m_cli;
    QProcess m_process;
    QList<QStringList> m_queue;
    QTimer m_poll;
    bool m_statusRunning = false;

    bool m_ready = false, m_loaded = false, m_enabled = false, m_autostart = false, m_bar = false;
    int m_max = 1000, m_min = 0, m_maxLimit = 1600, m_percent = 0, m_nits = 0;
    QString m_error;
};
