#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class SpeedGauge : public QWidget {
    Q_OBJECT
public:
    explicit SpeedGauge(QWidget* parent = nullptr);

    void setRange(double min_value, double max_value);
    void setValue(double value, bool animate = true);
    void setUnit(const QString& unit);
    void setCaption(const QString& caption);
    void setBusy(bool busy);
    void setAccent(const QColor& color);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTick();

private:
    double min_value_ = 0;
    double max_value_ = 100;
    double display_value_ = 0;
    double target_value_ = 0;
    QString unit_;
    QString caption_;
    QColor accent_{0x1f, 0xa2, 0xa6};
    bool busy_ = false;
    class QTimer* timer_ = nullptr;
};
