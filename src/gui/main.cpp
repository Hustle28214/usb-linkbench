#include "main_window.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("usb-linkbench"));
    QApplication::setOrganizationName(QStringLiteral("usb-linkbench"));

    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) app.setStyle(fusion);

    QPalette pal = app.palette();
    const QColor card(0x17, 0x1E, 0x2E);
    const QColor base(0x22, 0x2C, 0x40);
    const QColor text(0xE7, 0xEE, 0xF7);
    const QColor muted(0x8B, 0x9B, 0xB0);
    pal.setColor(QPalette::Window, card);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, QColor(0x1B, 0x24, 0x36));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, base);
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::Highlight, QColor(0x14, 0xC8, 0xBE));
    pal.setColor(QPalette::HighlightedText, QColor(0x06, 0x20, 0x28));
    pal.setColor(QPalette::ToolTipBase, card);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Light, QColor(0x3A, 0x4C, 0x64));
    pal.setColor(QPalette::Midlight, QColor(0x2C, 0x3A, 0x50));
    pal.setColor(QPalette::Mid, QColor(0x24, 0x30, 0x44));
    pal.setColor(QPalette::Dark, QColor(0x0E, 0x14, 0x22));
    pal.setColor(QPalette::Shadow, QColor(0x00, 0x00, 0x00));
    pal.setColor(QPalette::PlaceholderText, muted);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x5C, 0x6B, 0x80));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(0x5C, 0x6B, 0x80));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5C, 0x6B, 0x80));
    app.setPalette(pal);

    QFont font;
#if defined(_WIN32)
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei")});
#elif defined(__APPLE__)
    font.setFamilies({QStringLiteral("PingFang SC"), QStringLiteral("Helvetica Neue")});
#else
    font.setFamilies({QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans CJK SC")});
#endif
    font.setPixelSize(13);
    font.setStyleHint(QFont::SansSerif);
    font.setHintingPreference(QFont::PreferFullHinting);
    app.setFont(font);

    MainWindow window;
    window.show();
    return app.exec();
}
