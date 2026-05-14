#define WIN32_LEAN_AND_MEAN
#undef UNICODE
#undef _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include "AltManagerBridge.h"
#include <QTimer>
#include <QFileInfo>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>

#define PIPE_NAME "\\\\.\\pipe\\VetoAltMgr"

// ── Helpers (pure Windows, no Qt) ────────────────────────────────────────────
static std::string winLower(const char* s) {
    std::string r(s); for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

struct WinTitleCtx { DWORD pid; std::string title; };
static BOOL CALLBACK EnumWin(HWND hwnd, LPARAM lp) {
    auto* c = (WinTitleCtx*)lp;
    DWORD p=0; GetWindowThreadProcessId(hwnd,&p);
    if (p!=c->pid || !IsWindowVisible(hwnd)) return TRUE;
    char buf[512]; if (GetWindowTextA(hwnd,buf,512)>4){c->title=buf;return FALSE;}
    return TRUE;
}

// ── Bridge implementation ────────────────────────────────────────────────────
AltManagerBridge::AltManagerBridge(QObject* parent, const QString& dllPath) : QObject(parent), m_dllPath(dllPath) {
    m_timer = new QTimer(this);
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &AltManagerBridge::refresh);
    m_timer->start();
    loadRecentAlts();
    refresh(); // immediate first scan
}

static QString recentAltsPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/recent_alts.txt";
}

void AltManagerBridge::loadRecentAlts() {
    QString path = recentAltsPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (!line.isEmpty())
            m_recentAlts << line;
        if (m_recentAlts.size() >= 20) break;
    }
    // Don't restore currentAlt — we don't know which account is actually active
}

void AltManagerBridge::saveRecentAlts() {
    QString path = recentAltsPath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    for (const QString& alt : m_recentAlts)
        ts << alt << "\n";
}

void AltManagerBridge::addRecentAlt(const QString& username) {
    m_recentAlts.removeAll(username);
    m_recentAlts.prepend(username);
    if (m_recentAlts.size() > 15)
        m_recentAlts = m_recentAlts.mid(0, 15);
    emit recentAltsChanged();
    saveRecentAlts();
    setCurrentAlt(username);
}

void AltManagerBridge::setCurrentAlt(const QString& username) {
    if (m_currentAlt == username) return;
    m_currentAlt = username;
    emit currentAltChanged();
}

void AltManagerBridge::setSelected(int i) {
    if (m_selected == i) return;
    m_selected = i;
    emit selectedChanged();
}

void AltManagerBridge::setStatus(const QString& s) {
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

void AltManagerBridge::setBusy(bool b) {
    if (m_busy == b) return;
    m_busy = b;
    emit busyChanged();
}

// ── Process scanning ─────────────────────────────────────────────────────────
QString AltManagerBridge::clientLabel(quint32 pid) {
    // 1. EXE path
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
    if (hp) {
        char path[MAX_PATH]{}; DWORD sz=MAX_PATH;
        QueryFullProcessImageNameA(hp,0,path,&sz); CloseHandle(hp);
        std::string lo = winLower(path);
        if (lo.find("lunar")!=std::string::npos || lo.find("moonsworth")!=std::string::npos) return "Lunar Client";
        if (lo.find("badlion")!=std::string::npos || lo.find("bac-")!=std::string::npos)     return "Badlion";
    }
    // 2. Window title
    WinTitleCtx ctx{pid,""};
    EnumWindows(EnumWin,(LPARAM)&ctx);
    if (!ctx.title.empty()) {
        std::string t = winLower(ctx.title.c_str());
        if (t.find("lunar")    !=std::string::npos) return "Lunar Client";
        if (t.find("badlion")  !=std::string::npos) return "Badlion";
        if (t.find("optifine") !=std::string::npos) return "OptiFine";
        if (t.find("minecraft")!=std::string::npos) return "Vanilla";
    }
    // 3. Module scan
    HANDLE hp2 = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
    if (hp2) {
        HMODULE mods[512]; DWORD need=0;
        if (EnumProcessModules(hp2,mods,sizeof(mods),&need)) {
            DWORD n=(need/sizeof(HMODULE))<512?(need/sizeof(HMODULE)):512;
            for (DWORD m=0;m<n;m++) {
                char nm[MAX_PATH]{}; GetModuleBaseNameA(hp2,mods[m],nm,MAX_PATH);
                std::string lo=winLower(nm);
                if (lo.find("lwjgl")!=std::string::npos||lo.find("openal")!=std::string::npos)
                    { CloseHandle(hp2); return "Vanilla"; }
            }
        }
        CloseHandle(hp2);
    }
    return {};
}

void AltManagerBridge::refresh() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return;

    QStringList    newProcs;
    QList<quint32> newPids;

    PROCESSENTRY32 pe{sizeof(pe)};
    if (Process32First(snap,&pe)) do {
        char ex[MAX_PATH]; strncpy(ex,pe.szExeFile,MAX_PATH-1); _strlwr(ex);
        if (!strstr(ex,"java")) continue;
        QString lbl = clientLabel(pe.th32ProcessID);
        if (lbl.isEmpty()) continue;
        newProcs << QString("[%1]  %2  (PID %3)")
                        .arg(lbl)
                        .arg(QString::fromLocal8Bit(pe.szExeFile))
                        .arg(pe.th32ProcessID);
        newPids  << pe.th32ProcessID;
    } while(Process32Next(snap,&pe));
    CloseHandle(snap);

    if (newProcs != m_processes) {
        // Preserve selection by PID
        quint32 prevPid = (m_selected>=0 && m_selected<m_pids.size())
                          ? m_pids[m_selected] : 0;
        m_processes = newProcs;
        m_pids      = newPids;
        int newSel  = m_pids.indexOf(prevPid);
        m_selected  = (newSel>=0) ? newSel : (!m_pids.isEmpty() ? 0 : -1);
        emit processesChanged();
        emit selectedChanged();
    }
}

// ── Pipe communication ────────────────────────────────────────────────────────
bool AltManagerBridge::sendPipe(const char* cmd, char* resp, int sz) {
    HANDLE h = CreateFileA(PIPE_NAME,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if (h==INVALID_HANDLE_VALUE) return false;
    DWORD w=0;
    if (!WriteFile(h,cmd,(DWORD)strlen(cmd),&w,NULL)){CloseHandle(h);return false;}
    FlushFileBuffers(h);
    DWORD r=0;
    if (!ReadFile(h,resp,sz-1,&r,NULL)){CloseHandle(h);return false;}
    resp[r]=0; CloseHandle(h); return true;
}

// ── DLL injection ─────────────────────────────────────────────────────────────
static bool remoteLoadLib(HANDLE hp, const char* path, QString& errOut) {
    size_t sz = strlen(path) + 1;
    LPVOID mem = VirtualAllocEx(hp, NULL, sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { errOut = "VirtualAllocEx failed."; return false; }
    WriteProcessMemory(hp, mem, path, sz, NULL);
    LPVOID ll = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE ht = CreateRemoteThread(hp, NULL, 0, (LPTHREAD_START_ROUTINE)ll, mem, 0, NULL);
    if (!ht) {
        DWORD e = GetLastError();
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        errOut = e==5 ? "Access denied — run as Admin." : QString("CreateRemoteThread failed (%1).").arg(e);
        return false;
    }
    WaitForSingleObject(ht, 10000);
    DWORD ec = 0; GetExitCodeThread(ht, &ec);
    VirtualFreeEx(hp, mem, 0, MEM_RELEASE); CloseHandle(ht);
    if (!ec) { errOut = QString("LoadLibraryA returned NULL for: %1").arg(path); return false; }
    return true;
}

bool AltManagerBridge::inject(quint32 pid) {
    if (m_dllPath.isEmpty() || !QFileInfo::exists(m_dllPath)) {
        setStatus(QString("DLL not found: %1").arg(m_dllPath));
        return false;
    }

    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|
        PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ, FALSE, pid);
    if (!hp) { setStatus(QString("OpenProcess failed (err=%1). Run as Admin.").arg(GetLastError())); return false; }

    QString errMsg;

    QByteArray pathBytes = m_dllPath.toLocal8Bit();
    if (!remoteLoadLib(hp, pathBytes.constData(), errMsg)) {
        setStatus(errMsg);
        CloseHandle(hp);
        return false;
    }

    CloseHandle(hp);
    return true;
}

// ── Public Q_INVOKABLE actions ────────────────────────────────────────────────
void AltManagerBridge::injectDLL() {
    if (m_selected<0||m_selected>=m_pids.size()){ setStatus("Select a process first."); return; }
    setBusy(true);
    setStatus("Injecting DLL...");
    quint32 pid = m_pids[m_selected];
    if (inject(pid)) {
        m_injected = true;
        emit injectedChanged();
        setStatus(QString("DLL injected into PID %1").arg(pid));
    }
    setBusy(false);
}

void AltManagerBridge::changeAlt(const QString& username) {
    if (username.trimmed().isEmpty()){ setStatus("Enter a username first."); return; }
    setBusy(true);
    const QString uname = username.trimmed();
    std::string cmd = "CHANGE " + uname.toStdString();
    char resp[256]{};

    if (sendPipe(cmd.c_str(), resp, sizeof(resp))) {
        setStatus(QString("Alt changed: %1").arg(QString::fromLocal8Bit(resp)));
        addRecentAlt(uname);
        setBusy(false);
        return;
    }

    // Pipe not ready — need to inject first
    if (m_selected<0||m_selected>=m_pids.size()){ setStatus("Select a process first."); setBusy(false); return; }
    setStatus("Auto-injecting DLL...");
    if (!inject(m_pids[m_selected])){ setBusy(false); return; }
    m_injected = true; emit injectedChanged();

    // Poll until pipe is ready (max 30s), non-blocking via QTimer
    setStatus("Waiting for DLL pipe...");
    auto* poll = new QTimer(this);
    poll->setInterval(500);
    int* attempts = new int(0);
    std::string* cmdPtr = new std::string(cmd);
    connect(poll, &QTimer::timeout, this, [=]() mutable {
        char pong[32]{};
        if (sendPipe("PING",pong,32) && strcmp(pong,"PONG")==0) {
            poll->stop(); poll->deleteLater();
            char r[256]{};
            sendPipe(cmdPtr->c_str(), r, sizeof(r));
            setStatus(QString("Alt changed: %1").arg(QString::fromLocal8Bit(r)));
            addRecentAlt(uname);
            delete attempts; delete cmdPtr;
            setBusy(false);
        } else if (++(*attempts) > 60) {
            poll->stop(); poll->deleteLater();
            setStatus("Pipe not responding after injection. Try again.");
            delete attempts; delete cmdPtr;
            setBusy(false);
        }
    });
    poll->start();
}

void AltManagerBridge::restoreOriginal() {
    char resp[256]{};
    setBusy(true);
    setStatus("Restoring original session...");
    if (!sendPipe("RESTORE",resp,sizeof(resp))) {
        setStatus("Pipe not ready — inject DLL first.");
    } else {
        bool ok = strcmp(resp,"OK")==0;
        setStatus(ok ? "Session restored to original account."
                     : QString("Restore: %1").arg(QString::fromLocal8Bit(resp)));
        if (ok) setCurrentAlt("");
    }
    setBusy(false);
}
