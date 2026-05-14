#include <QGuiApplication>
#include <QQmlApplicationEngine>
int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.loadData("import QtQuick; Window { visible: true; width: 200; height: 100; title: 'Test' }");
    if (engine.rootObjects().isEmpty()) return -1;
    return app.exec();
}
