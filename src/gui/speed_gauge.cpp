#include "speed_gauge.hpp"

#include <QPainter>
#include <QTime>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

double tickStep(double min_value, double max_value) {
    const double span = std::max(max_value - min_value, 1e-9);
    const double raw = span / 10.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double factor = 10.0;
    if (norm <= 1.0) factor = 1.0;
    else if (norm <= 2.0) factor = 2.0;
    else if (norm <= 2.5) factor = 2.5;
    else if (norm <= 5.0) factor = 5.0;
    return factor * mag;
}

QString formatTick(double value) {
    if (std::abs(value) < 5e-4) return QStringLiteral("0");
    if (std::abs(value - std::round(value)) < 1e-3) return QString::number(std::round(value), 'f', 0);
    return QString::number(value, 'f', 1);
}

QString formatReadout(double value) {
    if (value >= 1000.0) return QString::number(value, 'f', 0);
    if (value >= 100.0) return QString::number(value, 'f', 1);
    return QString::number(value, 'f', 2);
}

}  // namespace

SpeedGauge::SpeedGauge(QWidget* parent) : QWidget(parent) {
    setMinimumSize(260, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &SpeedGauge::onTick);
}

void SpeedGauge::setRange(double min_value, double max_value) {
    min_value_ = min_value;
    max_value_ = std::max(max_value, min_value + 1e-9);
    target_value_ = std::clamp(target_value_, min_value_, max_value_);
    display_value_ = std::clamp(display_value_, min_value_, max_value_);
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

QSize SpeedGauge::sizeHint() const { return {380, 360}; }

QSize SpeedGauge::minimumSizeHint() const { return {240, 220}; }

void SpeedGauge::onTick() {
    bool moved = false;
    const double delta = target_value_ - display_value_;
    if (std::abs(delta) > 1e-6) {
        display_value_ += delta * 0.18;
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
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF bounds = QRectF(rect()).adjusted(2, 2, -2, -2);
    const qreal avail = std::min(bounds.width(), bounds.height());
    const qreal stroke = std::clamp(avail * 0.058, 14.0, 22.0);
    const qreal radius = std::max(48.0, avail * 0.5 - 34.0 - stroke * 0.2);
    const QPointF center = bounds.center();

    constexpr qreal start_angle = 224.0;
    constexpr qreal span_angle = -272.0;
    const QRectF arc_rect(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    auto point_at = [&](qreal degrees, qreal r) {
        const qreal rad = qDegreesToRadians(degrees);
        return QPointF(center.x() + std::cos(rad) * r, center.y() - std::sin(rad) * r);
    };

    const QColor tick_major(0x9A, 0xAB, 0xC2);
    const QColor tick_minor(0x5A, 0x6C, 0x84);
    const double step = tickStep(min_value_, max_value_);
    const int major_n = std::max(1, static_cast<int>(std::round((max_value_ - min_value_) / step)));
    const qreal tick_inner = radius + stroke * 0.55 + 2.0;

    QFont tick_font = font();
    tick_font.setFamilies({QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans CJK SC")});
    tick_font.setPixelSize(std::clamp(static_cast<int>(radius * 0.072), 10, 12));
    tick_font.setWeight(QFont::Normal);
    p.setFont(tick_font);
    const QFontMetrics tick_fm(tick_font);

    for (int i = 0; i <= major_n; ++i) {
        const double value = min_value_ + step * static_cast<double>(i);
        if (value > max_value_ + step * 0.02) break;
        const qreal t = std::clamp((value - min_value_) / (max_value_ - min_value_), 0.0, 1.0);
        const qreal angle = start_angle + span_angle * t;
        p.setPen(QPen(tick_major, 1.4, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(point_at(angle, tick_inner), point_at(angle, tick_inner + 8.0));

        const QString label = formatTick(value);
        const QPointF anchor = point_at(angle, tick_inner + 8.0 + 12.0);
        const QSizeF text = tick_fm.size(Qt::TextSingleLine, label);
        p.setPen(tick_major);
        p.drawText(QRectF(anchor.x() - text.width() / 2.0, anchor.y() - text.height() / 2.0, text.width(), text.height()),
                   Qt::AlignCenter, label);

        if (i == major_n) continue;
        for (int m = 1; m < 5; ++m) {
            const qreal mt = t + (static_cast<qreal>(m) / 5.0) * (step / (max_value_ - min_value_));
            if (mt >= 1.0) break;
            const qreal minor_angle = start_angle + span_angle * mt;
            p.setPen(QPen(tick_minor, 1.1, Qt::SolidLine, Qt::FlatCap));
            p.drawLine(point_at(minor_angle, tick_inner + 1.0), point_at(minor_angle, tick_inner + 5.0));
        }
    }

    QPen track(QColor(0x31, 0x41, 0x58), stroke, Qt::SolidLine, Qt::RoundCap);
    p.setPen(track);
    p.drawArc(arc_rect, static_cast<int>(start_angle * 16), static_cast<int>(span_angle * 16));

    double ratio = (display_value_ - min_value_) / (max_value_ - min_value_);
    ratio = std::clamp(ratio, 0.0, 1.0);
    qreal draw_start = start_angle;
    qreal draw_span = span_angle * ratio;
    if (busy_) {
        const int ms = QTime::currentTime().msecsSinceStartOfDay();
        const qreal travel = std::fmod(ms / 1400.0, 1.0);
        constexpr qreal seg = 0.20;
        const qreal begin = travel * (1.0 - seg);
        draw_start = start_angle + span_angle * begin;
        draw_span = span_angle * seg;
    }

    if (std::abs(draw_span) > 0.4) {
        p.setPen(QPen(QColor(accent_.red(), accent_.green(), accent_.blue(), 36), stroke * 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(arc_rect, static_cast<int>(draw_start * 16), static_cast<int>(draw_span * 16));
        p.setPen(QPen(QColor(accent_.red(), accent_.green(), accent_.blue(), 70), stroke * 1.35, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(arc_rect, static_cast<int>(draw_start * 16), static_cast<int>(draw_span * 16));

        QLinearGradient grad(center.x() - radius, center.y(), center.x() + radius, center.y());
        const QColor bright = accent_.lighter(145);
        grad.setColorAt(0.0, bright);
        grad.setColorAt(0.55, accent_);
        grad.setColorAt(1.0, accent_.darker(115));
        p.setPen(QPen(QBrush(grad), stroke, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(arc_rect, static_cast<int>(draw_start * 16), static_cast<int>(draw_span * 16));

        const QPointF tip = point_at(draw_start + draw_span, radius);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(accent_.red(), accent_.green(), accent_.blue(), 70));
        p.drawEllipse(tip, stroke * 0.85, stroke * 0.85);
        p.setBrush(accent_);
        p.drawEllipse(tip, stroke * 0.34, stroke * 0.34);
        p.setBrush(QColor(0xF7, 0xFF, 0xFC));
        p.drawEllipse(tip, stroke * 0.14, stroke * 0.14);
    }

    QString value_text = busy_ ? QStringLiteral("—") : formatReadout(display_value_);
    QFont value_font = font();
    value_font.setFamilies({QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans CJK SC")});
    value_font.setWeight(QFont::Bold);
    value_font.setLetterSpacing(QFont::PercentageSpacing, 97);
    int value_px = static_cast<int>(std::clamp(radius * 0.44, 36.0, 74.0));
    value_font.setPixelSize(value_px);
    QFontMetrics value_fm(value_font);
    const qreal max_text = radius * 1.25;
    if (value_fm.horizontalAdvance(value_text) > max_text) {
        value_px = std::max(24, static_cast<int>(value_px * max_text / value_fm.horizontalAdvance(value_text)));
        value_font.setPixelSize(value_px);
        value_fm = QFontMetrics(value_font);
    }

    const qreal value_h = value_fm.height();
    const QRectF value_rect(center.x() - radius, center.y() - value_h * 0.58, radius * 2.0, value_h);
    p.setFont(value_font);
    p.setPen(QColor(0xF7, 0xFB, 0xFF));
    p.drawText(value_rect, Qt::AlignHCenter | Qt::AlignVCenter, value_text);

    if (!caption_.isEmpty()) {
        QFont cap = font();
        cap.setFamilies({QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans CJK SC")});
        cap.setPixelSize(std::clamp(static_cast<int>(radius * 0.105), 12, 15));
        cap.setWeight(QFont::Medium);
        p.setFont(cap);
        p.setPen(QColor(0x8B, 0x9B, 0xB0));
        p.drawText(QRectF(center.x() - radius, value_rect.top() - cap.pixelSize() - 6, radius * 2.0, cap.pixelSize() + 4),
                   Qt::AlignHCenter | Qt::AlignBottom, caption_);
    }

    if (!unit_.isEmpty()) {
        QFont unit_font = font();
        unit_font.setFamilies({QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans CJK SC")});
        unit_font.setPixelSize(std::clamp(static_cast<int>(value_px * 0.34), 13, 22));
        unit_font.setWeight(QFont::Medium);
        p.setFont(unit_font);
        p.setPen(QColor(0x9A, 0xAB, 0xC0));
        p.drawText(QRectF(center.x() - radius, value_rect.bottom() - 2, radius * 2.0, unit_font.pixelSize() + 8),
                   Qt::AlignHCenter | Qt::AlignTop, unit_);
    }
}
