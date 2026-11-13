#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

class Sparkline : public QWidget {
    Q_OBJECT
public:
    explicit Sparkline(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setValueText(const QString& text);
    void setSamples(const QVector<double>& samples);
    void setAccent(const QColor& color);
    void setCeiling(double ceiling);
    void setBusy(bool busy);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTick();

private:
    void retarget();

    QString title_;
    QString value_text_;
    QVector<double> samples_;
    QColor accent_{0x2e, 0xe6, 0xc7};
    double ceiling_ = 500;
    double display_ratio_ = 0;
    double target_ratio_ = 0;
    bool busy_ = false;
    class QTimer* timer_ = nullptr;
};
