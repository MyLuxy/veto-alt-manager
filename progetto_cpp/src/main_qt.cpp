#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include "AltManagerBridge.h"

static FILE* g_log = nullptr;
static void log(const char* msg) {
    if (g_log) { fprintf(g_log, "%s\n", msg); fflush(g_log); }
}

// Extract embedded SessionChanger.dll (resource ID 101) to outPath.
// Returns true on success.
static bool extractEmbeddedDll(const char* outPath) {
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(101), (LPCSTR)RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hGlob = LoadResource(NULL, hRes);
    if (!hGlob) return false;
    void* data = LockResource(hGlob);
    DWORD size = SizeofResource(NULL, hRes);
    HANDLE f = CreateFileA(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, data, size, &written, NULL);
    CloseHandle(f);
    return written == size;
}

static void qtMessageHandler(QtMsgType, const QMessageLogContext&, const QString& msg) {
    log(msg.toLocal8Bit().constData());
}

int main(int argc, char* argv[]) {
    // ── Variable declarations (all at top — required with QT_NEEDS_QMAIN) ─────
    char exeDir[MAX_PATH]  = {};
    char logPath[MAX_PATH] = {};
    char qmlDir[MAX_PATH]  = {};
    char appdata[MAX_PATH] = {};
    char dllDir[MAX_PATH]  = {};
    char dllPath[MAX_PATH] = {};

    // ── Resolve absolute EXE directory ───────────────────────────────────────
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) *slash = '\0';

    // ── Open log file in %APPDATA%\VetoAlts\ ─────────────────────────────────
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    snprintf(dllDir,   MAX_PATH, "%s\\VetoAlts", appdata);
    CreateDirectoryA(dllDir, NULL);
    snprintf(logPath, MAX_PATH, "%s\\veto_log.txt", dllDir);
    g_log = fopen(logPath, "w");
    log("=== VetoAltManager startup ===");
    log(exeDir);

    // ── Redirect all Qt messages to log file ─────────────────────────────────
    qInstallMessageHandler(qtMessageHandler);

    // ── Set Qt environment BEFORE QGuiApplication initialises Qt ─────────────
    // QT_PLUGIN_PATH  → tells Qt where to find platforms\qwindows.dll etc.
    // QML2_IMPORT_PATH → tells QML engine where to find QtQuick, Controls etc.
    log("Setting QT_PLUGIN_PATH...");
    SetEnvironmentVariableA("QT_PLUGIN_PATH",   exeDir);
    snprintf(qmlDir, MAX_PATH, "%s\\qml", exeDir);
    SetEnvironmentVariableA("QML2_IMPORT_PATH", qmlDir);

    // ── Qt application ────────────────────────────────────────────────────────
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    log("Creating QGuiApplication...");
    QGuiApplication app(argc, argv);
    app.setOrganizationName("VetoAlts");
    app.setApplicationName("VetoAltManager");

    log("Setting software renderer...");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    // Extract embedded SessionChanger.dll to %APPDATA%\VetoAlts\ (dir already created above)
    snprintf(dllPath, MAX_PATH, "%s\\SessionChanger.dll", dllDir);
    if (extractEmbeddedDll(dllPath))
        log("SessionChanger.dll extracted OK");
    else
        log("WARNING: failed to extract SessionChanger.dll");

    AltManagerBridge bridge(nullptr, QString::fromLocal8Bit(dllPath));

    QQmlApplicationEngine engine;
    engine.addImportPath(QString::fromLocal8Bit(qmlDir));
    engine.rootContext()->setContextProperty("bridge", &bridge);
    engine.rootContext()->setContextProperty("appDir", QString::fromLocal8Bit(exeDir));
    log("Loading QML...");
    engine.load(QUrl(QStringLiteral("qrc:/Veto/qml/main.qml")));

    if (engine.rootObjects().isEmpty()) { log("ERROR: QML failed to load"); if(g_log)fclose(g_log); return -1; }

    // ── Center window on screen ───────────────────────────────────────────────
    log("QML loaded OK. Showing window...");
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (win) {
        HWND hwnd = (HWND)win->winId();

        // WS_MINIMIZEBOX lets Windows minimize the window when the taskbar
        // button is clicked. Qt.FramelessWindowHint omits it by default.
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX);

        // Rounded corners via Windows region
        HRGN hRgn = CreateRoundRectRgn(0, 0, win->width() + 1, win->height() + 1, 16, 16);
        SetWindowRgn(hwnd, hRgn, TRUE);

        QRect sg = QGuiApplication::primaryScreen()->availableGeometry();
        win->setX(sg.x() + (sg.width()  - win->width())  / 2);
        win->setY(sg.y() + (sg.height() - win->height()) / 2);
        win->show();
        win->raise();
        win->requestActivate();
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        ShowWindow(hwnd, SW_SHOW);
    }

    log("Entering event loop...");
    int ret = app.exec();
    log("Exiting.");
    if (g_log) fclose(g_log);
    return ret;
}
