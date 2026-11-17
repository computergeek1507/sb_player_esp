#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/res/app.png")));
    MainWindow window;
    window.show();
    return app.exec();
}
