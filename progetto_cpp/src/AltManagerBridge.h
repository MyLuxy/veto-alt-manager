#pragma once
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QList>

class AltManagerBridge : public QObject {
    Q_OBJECT

    // ── Properties exposed to QML ───────────────────────────────────────────
    Q_PROPERTY(QStringList processes  READ processes  NOTIFY processesChanged)
    Q_PROPERTY(QString     status     READ status     NOTIFY statusChanged)
    Q_PROPERTY(int         selected   READ selected   WRITE  setSelected  NOTIFY selectedChanged)
    Q_PROPERTY(bool        injected   READ injected   NOTIFY injectedChanged)
    Q_PROPERTY(bool        busy       READ busy       NOTIFY busyChanged)
    Q_PROPERTY(QStringList recentAlts READ recentAlts NOTIFY recentAltsChanged)
    Q_PROPERTY(QString     currentAlt READ currentAlt NOTIFY currentAltChanged)

public:
    explicit AltManagerBridge(QObject* parent = nullptr, const QString& dllPath = {});

    QStringList processes() const { return m_processes; }
    QString     status()    const { return m_status;    }
    int         selected()  const { return m_selected;  }
    bool        injected()  const { return m_injected;  }
    bool        busy()      const { return m_busy;      }
    QStringList recentAlts() const { return m_recentAlts; }
    QString     currentAlt() const { return m_currentAlt; }

    void setSelected(int i);

    // ── QML-callable actions ────────────────────────────────────────────────
    Q_INVOKABLE void injectDLL();
    Q_INVOKABLE void changeAlt(const QString& username);
    Q_INVOKABLE void restoreOriginal();

signals:
    void processesChanged();
    void statusChanged();
    void selectedChanged();
    void injectedChanged();
    void busyChanged();
    void recentAltsChanged();
    void currentAltChanged();

private:
    void        refresh();
    void        setStatus(const QString& s);
    void        setBusy(bool b);
    bool        sendPipe(const char* cmd, char* resp, int sz);
    bool        inject(quint32 pid);
    QString     clientLabel(quint32 pid);
    void        loadRecentAlts();
    void        saveRecentAlts();
    void        addRecentAlt(const QString& username);
    void        setCurrentAlt(const QString& username);

    QStringList    m_processes;
    QList<quint32> m_pids;
    QString        m_status  { "Ready." };
    int            m_selected{ -1 };
    bool           m_injected{ false };
    bool           m_busy    { false };
    QTimer*        m_timer;
    QStringList    m_recentAlts;
    QString        m_currentAlt;
    QString        m_dllPath;
};
