#include "speed_gauge.hpp"

#include <QPainter>
#include <QTime>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

SpeedGauge::SpeedGauge(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 140);
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &SpeedGauge::onTick);
}

void SpeedGauge::setRange(double min_value, double max_value) {
    min_value_ = min_value;
    max_value_ = std::max(max_value, min_value + 1e-9);
    update();
}

void SpeedGauge::setValue(double value, bool animate) {
    target_value_ = std::clamp(value, min_value_, max_value_);
    if (!animate) {
        display_value_ = target_value_;
        update();
        return;
    }
    if (!timer_->isActive()) timer_->start();
}

void SpeedGauge::setUnit(const QString& unit) {
    unit_ = unit;
    update();
}

void SpeedGauge::setCaption(const QString& caption) {
    caption_ = caption;
    update();
}

void SpeedGauge::setBusy(bool busy) {
    busy_ = busy;
    if (busy_ && !timer_->isActive()) timer_->start();
    update();
}

void SpeedGauge::setAccent(const QColor& color) {
    accent_ = color;
    update();
}

QSize SpeedGauge::sizeHint() const { return {220, 190}; }

QSize SpeedGauge::minimumSizeHint() const { return {140, 130}; }

void SpeedGauge::onTick() {
    bool moved = false;
    const double delta = target_value_ - display_value_;
    if (std::abs(delta) > 1e-6) {
        display_value_ += delta * 0.2;
        if (std::abs(target_value_ - display_value_) < (max_value_ - min_value_) * 0.001) {
            display_value_ = target_value_;
        }
        moved = true;
    }
    if (busy_) moved = true;
    if (moved) update();
    if (!busy_ && std::abs(target_value_ - display_value_) < 1e-9) timer_->stop();
}

void SpeedGauge::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF area = QRectF(rect()).adjusted(10, 6, -10, -6);
    const qreal side = std::min(area.width(), area.height() * 1.2);
    const QPointF center(area.center().x(), area.top() + side * 0.56);
    const qreal radius = side * 0.40;
    const qreal stroke = std::max(8.0, radius * 0.12);

    constexpr qreal start_angle = 210.0;
    constexpr qreal span_angle = -240.0;

    QPen track(QColor(0xE8, 0xEC, 0xF0), stroke, Qt::SolidLine, Qt::RoundCap);
    p.setPen(track);
    p.drawArc(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2),
              static_cast<int>(start_angle * 16), static_cast<int>(span_angle * 16));

    double ratio = (display_value_ - min_value_) / (max_value_ - min_value_);
    ratio = std::clamp(ratio, 0.0, 1.0);
    if (busy_) {
        const qreal phase = (QTime::currentTime().msec() / 1000.0) * 360.0;
        ratio = 0.18 + 0.18 * (0.5 + 0.5 * std::sin(qDegreesToRadians(phase)));
    }

    QLinearGradient grad(center.x() - radius, center.y(), center.x() + radius, center.y());
    grad.setColorAt(0.0, accent_.lighter(118));
    grad.setColorAt(1.0, accent_);
    QPen progress(QBrush(grad), stroke, Qt::SolidLine, Qt::RoundCap);
    p.setPen(progress);
    p.drawArc(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2),
              static_cast<int>(start_angle * 16), static_cast<int>(span_angle * ratio * 16));

    QFont value_font = font();
    value_font.setFamilies({QStringLiteral("Inter"), QStringLiteral("Noto Sans"), QStringLiteral("Segoe UI"),
                            QStringLiteral("Sans Serif")});
    value_font.setPointSizeF(std::max(22.0, side * 0.145));
    value_font.setWeight(QFont::Bold);
    p.setFont(value_font);
    p.setPen(QColor(0x0F, 0x17, 0x23));

    QString value_text;
    if (busy_) {
        value_text = QStringLiteral("—");
    } else if (display_value_ >= 100) {
        value_text = QString::number(display_value_, 'f', 0);
    } else if (display_value_ >= 10) {
        value_text = QString::number(display_value_, 'f', 1);
    } else {
        value_text = QString::number(display_value_, 'f', 2);
    }

    const QRectF value_rect(area.left(), center.y() - side * 0.08, area.width(), side * 0.22);
    p.drawText(value_rect, Qt::AlignHCenter | Qt::AlignVCenter, value_text);

    QFont meta = font();
    meta.setFamilies({QStringLiteral("Inter"), QStringLiteral("Noto Sans"), QStringLiteral("Sans Serif")});
    meta.setPointSizeF(std::max(9.0, side * 0.048));
    meta.setWeight(QFont::Medium);
    p.setFont(meta);
    p.setPen(QColor(0x6B, 0x78, 0x86));
    const QRectF unit_rect(area.left(), value_rect.bottom() - 4, area.width(), side * 0.1);
    p.drawText(unit_rect, Qt::AlignHCenter | Qt::AlignTop, unit_);

    if (!caption_.isEmpty()) {
        QFont cap = font();
        cap.setFamilies({QStringLiteral("Inter"), QStringLiteral("Noto Sans"), QStringLiteral("Sans Serif")});
        cap.setPointSizeF(std::max(10.0, side * 0.048));
        cap.setWeight(QFont::DemiBold);
        p.setFont(cap);
        p.setPen(QColor(0x3A, 0x47, 0x54));
        p.drawText(QRectF(area.left(), area.top(), area.width(), side * 0.12), Qt::AlignHCenter | Qt::AlignTop, caption_);
    }
}
