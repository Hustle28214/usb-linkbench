#include "sparkline.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QTime>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {

QString formatCeiling(double ceiling) {
    if (std::abs(ceiling - std::round(ceiling)) < 1e-3) return QString::number(std::round(ceiling), 'f', 0);
    return QString::number(ceiling, 'f', 1);
}

void drawKnob(QPainter& painter, const QPointF& point, const QColor& accent) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 80));
    painter.drawEllipse(point, 7.0, 7.0);
    painter.setBrush(accent);
    painter.drawEllipse(point, 3.6, 3.6);
    painter.setBrush(QColor(0xF7, 0xFF, 0xFC));
    painter.drawEllipse(point, 1.35, 1.35);
}

}  // namespace

Sparkline::Sparkline(QWidget* parent) : QWidget(parent) {
    setFixedHeight(72);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &Sparkline::onTick);
}

void Sparkline::setTitle(const QString& title) {
    title_ = title;
    update();
}

void Sparkline::setValueText(const QString& text) {
    value_text_ = text;
    update();
}

void Sparkline::setSamples(const QVector<double>& samples) {
    samples_ = samples;
    retarget();
}

void Sparkline::setAccent(const QColor& color) {
    accent_ = color;
    update();
}

void Sparkline::setCeiling(double ceiling) {
    ceiling_ = std::max(ceiling, 1.0);
    retarget();
}

void Sparkline::setBusy(bool busy) {
    busy_ = busy;
    if (busy_ && !timer_->isActive()) timer_->start();
    update();
}

void Sparkline::retarget() {
    const double value = samples_.isEmpty() ? 0.0 : samples_.back();
    target_ratio_ = std::clamp(value / ceiling_, 0.0, 1.0);
    if (!timer_->isActive()) timer_->start();
    update();
}

QSize Sparkline::sizeHint() const { return {360, 72}; }

QSize Sparkline::minimumSizeHint() const { return {180, 72}; }

void Sparkline::onTick() {
    bool moved = false;
    const double delta = target_ratio_ - display_ratio_;
    if (std::abs(delta) > 0.0008) {
        display_ratio_ += delta * 0.18;
        if (std::abs(target_ratio_ - display_ratio_) < 0.004) display_ratio_ = target_ratio_;
        moved = true;
    }
    if (busy_) moved = true;
    if (moved) update();
    if (!busy_ && std::abs(target_ratio_ - display_ratio_) < 1e-4) timer_->stop();
}

void Sparkline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QColor muted(0x8B, 0x9B, 0xB0);
    const QColor track(0x2A, 0x36, 0x4C);
    QFont ui = font();
    ui.setFamilies({QStringLiteral("Ubuntu"), QStringLiteral("Noto Sans CJK SC")});

    QFont title_font = ui;
    title_font.setPixelSize(13);
    title_font.setWeight(QFont::DemiBold);
    p.setFont(title_font);
    p.setPen(Qt::NoPen);
    p.setBrush(accent_);
    p.drawEllipse(QRectF(1, 7, 8, 8));
    p.setPen(QColor(0xD5, 0xDE, 0xEA));
    p.drawText(QRectF(14, 0, width() * 0.46, 24), Qt::AlignLeft | Qt::AlignVCenter, title_);

    if (!value_text_.isEmpty()) {
        QFont value_font = ui;
        value_font.setPixelSize(20);
        value_font.setWeight(QFont::Bold);
        p.setFont(value_font);
        const QFontMetrics value_fm(value_font);
        QFont unit_font = ui;
        unit_font.setPixelSize(12);
        unit_font.setWeight(QFont::Medium);
        const QFontMetrics unit_fm(unit_font);
        const QString unit = QStringLiteral("Mbps");
        const qreal unit_w = unit_fm.horizontalAdvance(unit);
        const qreal value_w = value_fm.horizontalAdvance(value_text_);
        const qreal right = width() - 2;
        p.setFont(unit_font);
        p.setPen(muted);
        p.drawText(QRectF(right - unit_w, 2, unit_w, 22), Qt::AlignRight | Qt::AlignVCenter, unit);
        p.setFont(value_font);
        p.setPen(QColor(0xF4, 0xF8, 0xFC));
        p.drawText(QRectF(right - unit_w - value_w - 6, 0, value_w + 2, 24), Qt::AlignRight | Qt::AlignVCenter, value_text_);
    }

    constexpr qreal label_h = 16;
    constexpr qreal bar_h = 12;
    const QRectF bar(2, 34, std::max(12.0, width() - 4.0), bar_h);

    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(bar, bar_h / 2.0, bar_h / 2.0);
    p.setPen(QPen(QColor(255, 255, 255, 28), 1));
    for (int i = 1; i < 4; ++i) {
        const qreal x = bar.left() + bar.width() * i / 4.0;
        p.drawLine(QPointF(x, bar.top() + 3), QPointF(x, bar.bottom() - 3));
    }

    const qreal fill_w = bar.width() * std::clamp(display_ratio_, 0.0, 1.0);
    if (fill_w > 1.0) {
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(bar, bar_h / 2.0, bar_h / 2.0);
        p.setClipPath(clip);
        QLinearGradient grad(bar.left(), bar.center().y(), bar.left() + std::max(fill_w, 8.0), bar.center().y());
        grad.setColorAt(0.0, accent_.lighter(118));
        grad.setColorAt(1.0, accent_);
        p.fillRect(QRectF(bar.left(), bar.top(), fill_w, bar.height()), grad);
        p.restore();
        if (!busy_) {
            const qreal knob_x = std::clamp(bar.left() + fill_w, bar.left() + bar_h / 2.0, bar.right() - bar_h / 2.0);
            drawKnob(p, QPointF(knob_x, bar.center().y()), accent_);
        }
    }
    if (busy_) {
        const qreal travel = std::fmod(QTime::currentTime().msecsSinceStartOfDay() / 1100.0, 1.0);
        const qreal seg = std::max(36.0, bar.width() * 0.28);
        const qreal x = bar.left() + (bar.width() + seg) * travel - seg;
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(bar, bar_h / 2.0, bar_h / 2.0);
        p.setClipPath(clip);
        QLinearGradient glow(x, 0, x + seg, 0);
        glow.setColorAt(0.0, QColor(accent_.red(), accent_.green(), accent_.blue(), 0));
        glow.setColorAt(0.5, QColor(255, 255, 255, 150));
        glow.setColorAt(1.0, QColor(accent_.red(), accent_.green(), accent_.blue(), 0));
        p.fillRect(QRectF(x, bar.top(), seg, bar.height()), glow);
        p.restore();
    }

    QFont scale = ui;
    scale.setPixelSize(11);
    scale.setWeight(QFont::Medium);
    p.setFont(scale);
    p.setPen(muted);
    const QRectF labels(bar.left(), bar.bottom() + 1, bar.width(), label_h);
    p.drawText(labels, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));
    p.drawText(labels, Qt::AlignRight | Qt::AlignVCenter, formatCeiling(ceiling_));
}
