#include "main_window.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("usb-linkbench"));
    QApplication::setOrganizationName(QStringLiteral("usb-linkbench"));

    QFontDatabase fonts;
    QFont font = app.font();
    if (fonts.hasFamily(QStringLiteral("Inter"))) {
        font = QFont(QStringLiteral("Inter"));
    } else if (fonts.hasFamily(QStringLiteral("Noto Sans"))) {
        font = QFont(QStringLiteral("Noto Sans"));
    }
    if (font.pointSize() > 0) font.setPointSize(font.pointSize() + 1);
    font.setStyleHint(QFont::SansSerif);
    app.setFont(font);

    MainWindow window;
    window.show();
    return app.exec();
}
