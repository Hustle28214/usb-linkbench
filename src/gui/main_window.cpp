#include "main_window.hpp"

#include "linkbench.hpp"
#include "measure_worker.hpp"
#include "sparkline.hpp"
#include "speed_gauge.hpp"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

class BadgeIcon : public QWidget {
public:
    enum Kind { Device, Node };
    explicit BadgeIcon(Kind kind, QWidget* parent = nullptr) : QWidget(parent), kind_(kind) { setFixedSize(40, 40); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(1.2, 1.2, -1.2, -1.2);
        p.setPen(QPen(QColor(0x3A, 0x4C, 0x64), 1.2));
        p.setBrush(QColor(0x22, 0x2C, 0x40));
        p.drawEllipse(box);

        p.setPen(QPen(QColor(0xC9, 0xD5, 0xE4), 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const QPointF c = box.center();
        if (kind_ == Device) {
            p.drawRoundedRect(QRectF(c.x() - 8, c.y() - 5.5, 16, 11), 2.2, 2.2);
            p.drawEllipse(QRectF(c.x() - 3.1, c.y() - 2.6, 6.2, 6.2));
            p.drawLine(QPointF(c.x() - 5, c.y() - 5.5), QPointF(c.x() - 3, c.y() - 7.4));
            p.drawLine(QPointF(c.x() + 5, c.y() - 5.5), QPointF(c.x() + 3, c.y() - 7.4));
        } else {
            p.drawRoundedRect(QRectF(c.x() - 6.5, c.y() - 8, 13, 16), 1.6, 1.6);
            p.drawLine(QPointF(c.x() - 3.2, c.y() - 3.4), QPointF(c.x() + 3.2, c.y() - 3.4));
            p.drawLine(QPointF(c.x() - 3.2, c.y()), QPointF(c.x() + 3.2, c.y()));
            p.drawLine(QPointF(c.x() - 3.2, c.y() + 3.4), QPointF(c.x() + 3.2, c.y() + 3.4));
        }
    }

private:
    Kind kind_;
};

class CyanDot : public QWidget {
public:
    explicit CyanDot(QWidget* parent = nullptr) : QWidget(parent) { setFixedSize(12, 12); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2E, 0xE6, 0xC7));
        p.drawEllipse(QRectF(2.5, 2.5, 7, 7));
    }
};

QPushButton* makeGhostButton(const QString& text) {
    auto* btn = new QPushButton(text);
    btn->setObjectName(QStringLiteral("ghostBtn"));
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

QPushButton* makePrimaryButton(const QString& text) {
    auto* btn = new QPushButton(text);
    btn->setObjectName(QStringLiteral("primary"));
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

QLabel* makeMetricLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("metricLabel"));
    return label;
}

QLabel* makeMetricValue(const QString& object_name) {
    auto* label = new QLabel(QStringLiteral("0"));
    label->setObjectName(object_name);
    label->setAlignment(Qt::AlignCenter);
    return label;
}

QWidget* makeRule() {
    auto* rule = new QWidget;
    rule->setObjectName(QStringLiteral("rule"));
    rule->setFixedHeight(1);
    return rule;
}

double niceCeiling(double value) {
    static const double steps[] = {50, 100, 250, 500, 1000, 2000, 5000, 10000, 20000, 40000};
    for (double step : steps) {
        if (value <= step) return step;
    }
    return std::ceil(value / 10000.0) * 10000.0;
}

QString formatMbps(double value) {
    if (value >= 1000.0) return QString::number(value, 'f', 0);
    if (value >= 100.0) return QString::number(value, 'f', 1);
    return QString::number(value, 'f', 2);
}

QString formatUs(double value) {
    const double magnitude = std::abs(value);
    if (magnitude >= 100.0) return QString::number(value, 'f', 0);
    if (magnitude >= 10.0) return QString::number(value, 'f', 1);
    return QString::number(value, 'f', 2);
}

double mibToMbps(double mib_per_sec) { return mib_per_sec * (1024.0 * 1024.0 * 8.0) / 1.0e6; }

void rememberMbps(QVector<double>* series, Sparkline* spark, double mbps) {
    series->append(mbps);
    if (series->size() > 42) series->remove(0, series->size() - 42);
    double peak = 0;
    for (double value : *series) peak = std::max(peak, value);
    spark->setCeiling(niceCeiling(std::max(peak, 50.0)));
    spark->setSamples(*series);
    spark->setValueText(formatMbps(mbps));
    spark->setBusy(false);
}

QWidget* makeLinkBarRow(const QVariantMap& bar) {
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("linkRow"));
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(8);
    auto* title = new QHBoxLayout;
    title->setContentsMargins(0, 0, 0, 0);
    title->setSpacing(6);
    auto* label = new QLabel(bar.value(QStringLiteral("label")).toString());
    label->setObjectName(QStringLiteral("barLabel"));
    title->addWidget(label);
    const auto attr = bar.value(QStringLiteral("attr")).toString();
    if (!attr.isEmpty()) {
        auto* attr_label = new QLabel(attr);
        attr_label->setObjectName(QStringLiteral("barAttr"));
        title->addWidget(attr_label);
    }
    title->addStretch(1);
    auto* suffix = new QLabel(bar.value(QStringLiteral("suffix")).toString());
    suffix->setObjectName(QStringLiteral("barSuffix"));
    suffix->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    head->addLayout(title, 1);
    head->addWidget(suffix);
    layout->addLayout(head);

    auto* progress = new QProgressBar;
    progress->setTextVisible(false);
    progress->setMinimum(0);
    progress->setMaximum(1000);
    const double max_value = std::max(bar.value(QStringLiteral("max")).toDouble(), 1.0);
    const double value = std::clamp(bar.value(QStringLiteral("value")).toDouble(), 0.0, max_value);
    progress->setValue(static_cast<int>((value / max_value) * 1000.0));
    const QString color = bar.value(QStringLiteral("color")).toString();
    progress->setStyleSheet(QStringLiteral(
                                "QProgressBar {"
                                " background: #2A364C;"
                                " border: none;"
                                " border-radius: 4px;"
                                " min-height: 7px;"
                                " max-height: 7px;"
                                "}"
                                "QProgressBar::chunk {"
                                " background: %1;"
                                " border-radius: 4px;"
                                "}")
                                .arg(color));
    layout->addWidget(progress);
    return row;
}

void applyStandardBox(QFrame* box, QLabel* title, QLabel* badge, QLabel* detail, const QString& title_text,
                      const QString& badge_text, const QString& detail_text, const QString& meets) {
    title->setText(title_text);
    badge->setText(badge_text);
    badge->setVisible(!badge_text.isEmpty());
    detail->setText(detail_text);
    box->setProperty("meets", meets);
    box->style()->unpolish(box);
    box->style()->polish(box);
}

struct StandardReadout {
    QString title;
    QString badge;
    QString meets;
};

QString formatMbit(double mbit) {
    if (mbit >= 1000.0) return QString::number(mbit, 'f', 0);
    if (mbit >= 100.0) return QString::number(mbit, 'f', 1);
    return QString::number(mbit, 'f', 2);
}

StandardReadout readoutFromMeasured(double mbit) {
    const auto standard = linkbench::standard_from_measured_mbit(mbit);
    if (!standard.meets_usb3 && std::string(standard.name) == "未知") {
        return {QStringLiteral("未知"), QString(), QStringLiteral("unknown")};
    }
    const QString title = QString::fromUtf8(standard.name);
    if (!standard.meets_usb3) return {title, QStringLiteral("未到 3.0"), QStringLiteral("no")};
    const QString badge = title == QStringLiteral("USB 3.0") ? QStringLiteral("符合") : QStringLiteral("符合 3.0");
    return {title, badge, QStringLiteral("yes")};
}

QString measuredStandardLine(double mbit) {
    const auto shown = formatMbit(mbit);
    if (mbit > 480.0) return QStringLiteral("实测 %1 Mbit/s · 高于 480 即符合").arg(shown);
    return QStringLiteral("实测 %1 Mbit/s · 未超过 480").arg(shown);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("usb-linkbench"));
    resize(1180, 760);
    setMinimumSize(980, 680);

    auto* root = new QWidget;
    root->setObjectName(QStringLiteral("root"));
    setCentralWidget(root);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(28, 26, 28, 26);
    root_layout->setSpacing(0);

    auto* dashboard = new QFrame;
    dashboard->setObjectName(QStringLiteral("dashboard"));
    dashboard->setFrameShape(QFrame::NoFrame);
    auto* shadow = new QGraphicsDropShadowEffect(dashboard);
    shadow->setBlurRadius(36);
    shadow->setOffset(0, 12);
    shadow->setColor(QColor(24, 58, 102, 78));
    dashboard->setGraphicsEffect(shadow);
    root_layout->addWidget(dashboard);

    auto* dash = new QVBoxLayout(dashboard);
    dash->setContentsMargins(26, 22, 26, 16);
    dash->setSpacing(12);

    auto* hero = new QHBoxLayout;
    hero->setSpacing(18);
    stream_gauge_ = new SpeedGauge;
    stream_gauge_->setCaption(QStringLiteral("视频"));
    stream_gauge_->setUnit(QStringLiteral("Mbps"));
    stream_gauge_->setRange(0, 500);
    stream_gauge_->setValue(0, false);

    stream_std_box_ = new QFrame;
    stream_std_box_->setObjectName(QStringLiteral("stdBox"));
    auto* stream_std_layout = new QHBoxLayout(stream_std_box_);
    stream_std_layout->setContentsMargins(10, 4, 10, 4);
    stream_std_layout->setSpacing(8);
    stream_std_title_ = new QLabel(QStringLiteral("未测"));
    stream_std_title_->setObjectName(QStringLiteral("stdTitle"));
    stream_std_badge_ = new QLabel;
    stream_std_badge_->setObjectName(QStringLiteral("stdBadge"));
    stream_std_badge_->hide();
    stream_std_detail_ = new QLabel(QStringLiteral("高于 480 Mbit/s 即符合"));
    stream_std_detail_->setObjectName(QStringLiteral("stdDetail"));
    stream_std_layout->addWidget(stream_std_title_);
    stream_std_layout->addWidget(stream_std_badge_);
    stream_std_layout->addWidget(stream_std_detail_, 1);
    stream_std_box_->setProperty("meets", QStringLiteral("unknown"));
    stream_std_box_->setToolTip(
        QStringLiteral("判据：实测高于 USB 2.0 线速 480 Mbit/s 即符合 USB 3.0。达到 10 Gbit/s 标 USB 3.1，20 Gbit/s 标 USB 3.2。480 及以下标 USB 2.0，未到 3.0。"));

    auto* gauge_col = new QVBoxLayout;
    gauge_col->setSpacing(8);
    gauge_col->setContentsMargins(0, 0, 0, 0);
    gauge_col->addWidget(stream_gauge_, 1);
    gauge_col->addWidget(stream_std_box_);
    hero->addLayout(gauge_col, 5);

    auto* right = new QVBoxLayout;
    right->setSpacing(6);
    stream_spark_ = new Sparkline;
    stream_spark_->setTitle(QStringLiteral("视频"));
    stream_spark_->setAccent(QColor(0x2E, 0xE6, 0xC7));
    stream_spark_->setCeiling(500);
    storage_spark_ = new Sparkline;
    storage_spark_->setTitle(QStringLiteral("直读"));
    storage_spark_->setAccent(QColor(0x5B, 0xB8, 0xFF));
    storage_spark_->setCeiling(500);
    right->addWidget(stream_spark_);
    right->addSpacing(8);
    right->addWidget(storage_spark_);
    right->addStretch(1);

    auto* metrics = new QHBoxLayout;
    metrics->setContentsMargins(0, 6, 0, 2);
    metrics->setSpacing(12);
    metrics->addWidget(makeMetricLabel(QStringLiteral("时延/µs")), 0, Qt::AlignVCenter);
    latency_p50_ = makeMetricValue(QStringLiteral("metricMain"));
    latency_p95_ = makeMetricValue(QStringLiteral("metricSub"));
    latency_max_ = makeMetricValue(QStringLiteral("metricSub"));
    metrics->addWidget(latency_p50_, 0, Qt::AlignVCenter);
    metrics->addWidget(latency_p95_, 0, Qt::AlignVCenter);
    metrics->addWidget(latency_max_, 0, Qt::AlignVCenter);
    metrics->addWidget(new CyanDot, 0, Qt::AlignVCenter);
    metrics->addStretch(1);
    metrics->addWidget(makeMetricLabel(QStringLiteral("抖动/µs")), 0, Qt::AlignVCenter);
    jitter_value_ = makeMetricValue(QStringLiteral("metricMain"));
    metrics->addWidget(jitter_value_, 0, Qt::AlignVCenter);
    metrics->addSpacing(16);
    metrics->addWidget(makeMetricLabel(QStringLiteral("错误")), 0, Qt::AlignVCenter);
    error_value_ = makeMetricValue(QStringLiteral("metricMain"));
    metrics->addWidget(error_value_, 0, Qt::AlignVCenter);
    right->addLayout(metrics);
    hero->addLayout(right, 6);
    dash->addLayout(hero, 1);

    dash->addWidget(makeRule());

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(2, 2, 2, 2);
    footer->setSpacing(12);
    footer->addWidget(new BadgeIcon(BadgeIcon::Device));
    auto* device_text = new QVBoxLayout;
    device_text->setSpacing(1);
    device_text->setContentsMargins(0, 0, 0, 0);
    footer_device_title_ = new QLabel(QStringLiteral("未选择设备"));
    footer_device_title_->setObjectName(QStringLiteral("footerTitle"));
    footer_device_sub_ = new QLabel(QStringLiteral("刷新以枚举相机"));
    footer_device_sub_->setObjectName(QStringLiteral("footerSub"));
    device_text->addWidget(footer_device_title_);
    device_text->addWidget(footer_device_sub_);
    footer->addLayout(device_text);
    footer->addStretch(1);

    auto* node_text = new QVBoxLayout;
    node_text->setSpacing(1);
    node_text->setContentsMargins(0, 0, 0, 0);
    footer_node_title_ = new QLabel(QStringLiteral("节点"));
    footer_node_title_->setObjectName(QStringLiteral("footerTitle"));
    footer_node_sub_ = new QLabel(QStringLiteral("尚未读取链路"));
    footer_node_sub_->setObjectName(QStringLiteral("footerSub"));
    node_text->addWidget(footer_node_title_);
    node_text->addWidget(footer_node_sub_);
    footer->addWidget(new BadgeIcon(BadgeIcon::Node));
    footer->addLayout(node_text);
    dash->addLayout(footer);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("statusOk"));
    status_->setWordWrap(true);
    status_->setMaximumHeight(48);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->hide();
    dash->addWidget(status_);

    auto* dock = new QFrame;
    dock->setObjectName(QStringLiteral("dock"));
    dock->setFrameShape(QFrame::NoFrame);
    auto* dock_layout = new QVBoxLayout(dock);
    dock_layout->setContentsMargins(14, 12, 14, 12);
    dock_layout->setSpacing(8);

    device_box_ = new QComboBox;
    device_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    device_box_->setMinimumWidth(220);
    node_box_ = new QComboBox;
    node_box_->setMinimumWidth(150);
    seconds_box_ = new QDoubleSpinBox;
    seconds_box_->setRange(1.0, 60.0);
    seconds_box_->setDecimals(1);
    seconds_box_->setValue(5.0);
    seconds_box_->setSuffix(QStringLiteral(" s"));
    seconds_box_->setMinimumWidth(88);
    rtt_count_box_ = new QSpinBox;
    rtt_count_box_->setRange(20, 5000);
    rtt_count_box_->setValue(100);
    rtt_count_box_->setMinimumWidth(88);

    auto* selectors = new QHBoxLayout;
    selectors->setSpacing(8);
    auto* device_label = new QLabel(QStringLiteral("设备"));
    device_label->setObjectName(QStringLiteral("fieldLabel"));
    auto* node_label = new QLabel(QStringLiteral("节点"));
    node_label->setObjectName(QStringLiteral("fieldLabel"));
    auto* sec_label = new QLabel(QStringLiteral("时长"));
    sec_label->setObjectName(QStringLiteral("fieldLabel"));
    auto* rtt_label = new QLabel(QStringLiteral("RTT"));
    rtt_label->setObjectName(QStringLiteral("fieldLabel"));
    selectors->addWidget(device_label);
    selectors->addWidget(device_box_, 1);
    selectors->addWidget(node_label);
    selectors->addWidget(node_box_);
    selectors->addWidget(sec_label);
    selectors->addWidget(seconds_box_);
    selectors->addWidget(rtt_label);
    selectors->addWidget(rtt_count_box_);
    dock_layout->addLayout(selectors);

    refresh_devices_btn_ = makeGhostButton(QStringLiteral("刷新"));
    refresh_links_btn_ = makeGhostButton(QStringLiteral("链路"));
    stream_btn_ = makePrimaryButton(QStringLiteral("测速"));
    stream_quick_btn_ = makeGhostButton(QStringLiteral("快速"));
    stream_stress_btn_ = makeGhostButton(QStringLiteral("压力"));
    rtt_btn_ = makePrimaryButton(QStringLiteral("测 RTT"));
    refresh_devices_btn_->setToolTip(QStringLiteral("重新枚举相机"));
    refresh_links_btn_->setToolTip(QStringLiteral("读取 USB、Thunderbolt 和 PCIe 协商速率"));
    stream_btn_->setToolTip(QStringLiteral("按当前时长采集视频载荷"));
    stream_quick_btn_->setToolTip(QStringLiteral("3 次 × 5 秒，取得分最高的一次"));
    stream_stress_btn_->setToolTip(QStringLiteral("3 次 × 10 秒，浅队列，取得分最低的一次"));
    rtt_btn_->setToolTip(
        QStringLiteral("GET_STATUS 控制传输回传。Windows 上需要 WinUSB；D405 等相机请保留 UVC 以便测视频，RTT 请换测试板或临时改驱动。"));

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(refresh_devices_btn_);
    buttons->addWidget(refresh_links_btn_);
    buttons->addSpacing(8);
    buttons->addWidget(stream_btn_);
    buttons->addWidget(stream_quick_btn_);
    buttons->addWidget(stream_stress_btn_);
    buttons->addWidget(rtt_btn_);
    buttons->addStretch(1);
    dock_layout->addLayout(buttons);

    storage_path_ = new QLineEdit;
    storage_path_->setPlaceholderText(QStringLiteral("/media/…"));
    browse_btn_ = makeGhostButton(QStringLiteral("浏览"));
    storage_btn_ = makePrimaryButton(QStringLiteral("测直读"));
    browse_btn_->setToolTip(QStringLiteral("选择 U 盘上的文件"));
    storage_btn_->setToolTip(QStringLiteral("绕过页缓存的顺序读"));

    storage_std_box_ = new QFrame;
    storage_std_box_->setObjectName(QStringLiteral("stdBox"));
    auto* std_layout = new QHBoxLayout(storage_std_box_);
    std_layout->setContentsMargins(10, 4, 10, 4);
    std_layout->setSpacing(8);
    storage_std_title_ = new QLabel(QStringLiteral("未识别"));
    storage_std_title_->setObjectName(QStringLiteral("stdTitle"));
    storage_std_badge_ = new QLabel;
    storage_std_badge_->setObjectName(QStringLiteral("stdBadge"));
    storage_std_badge_->hide();
    storage_std_detail_ = new QLabel(QStringLiteral("选择 U 盘上的文件"));
    storage_std_detail_->setObjectName(QStringLiteral("stdDetail"));
    std_layout->addWidget(storage_std_title_);
    std_layout->addWidget(storage_std_badge_);
    std_layout->addWidget(storage_std_detail_, 1);
    storage_std_box_->setProperty("meets", QStringLiteral("unknown"));

    auto* storage_row = new QHBoxLayout;
    storage_row->setSpacing(8);
    auto* path_label = new QLabel(QStringLiteral("U 盘"));
    path_label->setObjectName(QStringLiteral("fieldLabel"));
    storage_row->addWidget(path_label);
    storage_row->addWidget(storage_path_, 1);
    storage_row->addWidget(browse_btn_);
    storage_row->addWidget(storage_btn_);
    storage_row->addWidget(storage_std_box_);
    dock_layout->addLayout(storage_row);

    auto* links_label = new QLabel(QStringLiteral("链路"));
    links_label->setObjectName(QStringLiteral("fieldLabel"));
    auto* links_scroll = new QScrollArea;
    links_scroll->setWidgetResizable(true);
    links_scroll->setFrameShape(QFrame::NoFrame);
    links_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    links_scroll->setObjectName(QStringLiteral("linksScroll"));
    links_scroll->setMinimumHeight(36);
    links_scroll->setMaximumHeight(108);
    links_bars_ = new QWidget;
    links_bars_->setObjectName(QStringLiteral("linkBars"));
    links_bars_layout_ = new QVBoxLayout(links_bars_);
    links_bars_layout_->setContentsMargins(0, 0, 0, 0);
    links_bars_layout_->setSpacing(8);
    links_bars_layout_->addStretch(1);
    links_scroll->setWidget(links_bars_);
    links_scroll->viewport()->setAutoFillBackground(false);
    dock_layout->addWidget(links_label);
    dock_layout->addWidget(links_scroll);
    dash->addWidget(dock);

    setStyleSheet(QStringLiteral(
        "QMainWindow, #root {"
        " background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #A9D4F5, stop:1 #6EACE6);"
        "}"
        "QLabel { background: transparent; }"
        "#dashboard {"
        " background: #171E2E;"
        " border: 1px solid #243044;"
        " border-radius: 22px;"
        "}"
        "#dock {"
        " background: #1C2536;"
        " border: 1px solid #2A364C;"
        " border-radius: 14px;"
        "}"
        "#rule { background: #2C3A50; border: none; }"
        "#fieldLabel { color: #8B9BB0; font-size: 12px; font-weight: 600; }"
        "#footerTitle { color: #F4F7FB; font-size: 15px; font-weight: 600; }"
        "#footerSub { color: #8B9BB0; font-size: 12px; }"
        "#metricLabel { color: #8B9BB0; font-size: 12px; font-weight: 600; }"
        "#metricMain { color: #F4F8FC; font-size: 22px; font-weight: 700; }"
        "#metricSub { color: #C5D2E2; font-size: 18px; font-weight: 600; }"
        "#statusOk {"
        " padding: 8px 12px; border-radius: 10px;"
        " background: rgba(46, 230, 199, 0.12); color: #B8FFF4;"
        " border: 1px solid #1E6F68; font-size: 12px; font-weight: 600;"
        "}"
        "#statusErr {"
        " padding: 8px 12px; border-radius: 10px;"
        " background: rgba(244, 63, 94, 0.12); color: #FECDD3;"
        " border: 1px solid #7F1D32; font-size: 12px; font-weight: 600;"
        "}"
        "QPushButton#primary {"
        " background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #5CFFE8, stop:1 #14C8D4);"
        " color: #062028; border: none; border-radius: 9px; padding: 7px 16px; font-weight: 700; font-size: 13px;"
        "}"
        "QPushButton#primary:hover {"
        " background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8CFFF0, stop:1 #3ADBE6);"
        "}"
        "QPushButton#primary:pressed { background: #0FB8B0; }"
        "QPushButton#primary:disabled { background: #1A3A40; color: #5C7A80; }"
        "QPushButton#ghostBtn {"
        " background: #222C40; color: #D5DEEA; border: 1px solid #33445C;"
        " border-radius: 9px; padding: 7px 12px; font-weight: 600; font-size: 13px;"
        "}"
        "QPushButton#ghostBtn:hover { background: #2A3650; border-color: #3E5270; }"
        "QPushButton#ghostBtn:pressed { background: #1A2333; }"
        "QPushButton#ghostBtn:disabled { color: #5C6B80; border-color: #2A3548; background: #1A2333; }"
        "QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {"
        " background: #222C40; color: #E7EEF7; border: 1px solid #33445C;"
        " border-radius: 9px; padding: 6px 10px; min-height: 18px;"
        " selection-background-color: #14C8BE; selection-color: #062028;"
        "}"
        "QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #3E5270; }"
        "QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #2EE6C7; }"
        "QComboBox::drop-down { border: none; width: 22px; }"
        "QComboBox QAbstractItemView {"
        " background: #1B2436; color: #E7EEF7; border: 1px solid #33445C;"
        " selection-background-color: #145E58; selection-color: #F4FFFC; outline: 0; padding: 4px;"
        "}"
        "#barLabel { font-size: 12px; font-weight: 600; color: #D5DEEA; }"
        "#barAttr { font-size: 10px; font-weight: 600; color: #7E8DA3; }"
        "#barSuffix { font-size: 11px; font-weight: 600; color: #A8B6C8; }"
        "#linkRow, #linkBars { background: transparent; }"
        "#stdBox { border-radius: 9px; }"
        "#stdBox[meets=\"yes\"] { background: rgba(46, 230, 199, 0.12); border: 1px solid #1F6F68; }"
        "#stdBox[meets=\"no\"] { background: rgba(240, 180, 41, 0.12); border: 1px solid #7A5A12; }"
        "#stdBox[meets=\"unknown\"] { background: #222C40; border: 1px solid #33445C; }"
        "#stdBox QLabel { background: transparent; }"
        "#stdTitle { font-size: 13px; font-weight: 700; }"
        "#stdBox[meets=\"yes\"] #stdTitle, #stdBox[meets=\"yes\"] #stdBadge { color: #8FFFEA; }"
        "#stdBox[meets=\"no\"] #stdTitle, #stdBox[meets=\"no\"] #stdBadge { color: #F6C15B; }"
        "#stdBox[meets=\"unknown\"] #stdTitle { color: #8B9BB0; }"
        "#stdBadge { font-size: 12px; font-weight: 700; }"
        "#stdDetail { font-size: 12px; color: #8B9BB0; }"
        "QScrollArea#linksScroll, QScrollArea#linksScroll > QWidget { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: #3A4C63; border-radius: 4px; min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "SpeedGauge, Sparkline { background: transparent; }"));

    worker_thread_ = new QThread(this);
    worker_ = new MeasureWorker;
    worker_->moveToThread(worker_thread_);
    connect(worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(refresh_devices_btn_, &QPushButton::clicked, this, &MainWindow::onRefreshDevices);
    connect(refresh_links_btn_, &QPushButton::clicked, this, &MainWindow::onRefreshLinks);
    connect(stream_btn_, &QPushButton::clicked, this, &MainWindow::onMeasureStream);
    connect(stream_quick_btn_, &QPushButton::clicked, this, &MainWindow::onMeasureStreamQuick);
    connect(stream_stress_btn_, &QPushButton::clicked, this, &MainWindow::onMeasureStreamStress);
    connect(rtt_btn_, &QPushButton::clicked, this, &MainWindow::onMeasureRtt);
    connect(storage_btn_, &QPushButton::clicked, this, &MainWindow::onMeasureStorage);
    connect(browse_btn_, &QPushButton::clicked, this, &MainWindow::onBrowseStorage);
    connect(device_box_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDeviceChanged);
    connect(node_box_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        syncNodeCaption();
        clearStreamHistory();
    });

    connect(worker_, &MeasureWorker::failed, this, &MainWindow::onFailed);
    connect(worker_, &MeasureWorker::listReady, this, &MainWindow::onListReady);
    connect(worker_, &MeasureWorker::linksReady, this, &MainWindow::onLinksReady);
    connect(worker_, &MeasureWorker::rttReady, this, &MainWindow::onRttReady);
    connect(worker_, &MeasureWorker::streamReady, this, &MainWindow::onStreamReady);
    connect(worker_, &MeasureWorker::streamBenchReady, this, &MainWindow::onStreamBenchReady);
    connect(worker_, &MeasureWorker::storageLinkReady, this, &MainWindow::onStorageLink);
    connect(worker_, &MeasureWorker::storageReady, this, &MainWindow::onStorageReady);
    connect(storage_path_, &QLineEdit::editingFinished, this, &MainWindow::inspectStoragePath);

    worker_thread_->start();
    onRefreshDevices();
    onRefreshLinks();
}

MainWindow::~MainWindow() {
    worker_thread_->quit();
    worker_thread_->wait(3000);
}

void MainWindow::setBusy(bool busy) {
    busy_count_ += busy ? 1 : -1;
    if (busy_count_ < 0) busy_count_ = 0;
    const bool any = busy_count_ > 0;
    refresh_devices_btn_->setEnabled(!any);
    refresh_links_btn_->setEnabled(!any);
    stream_btn_->setEnabled(!any);
    stream_quick_btn_->setEnabled(!any);
    stream_stress_btn_->setEnabled(!any);
    rtt_btn_->setEnabled(!any);
    storage_btn_->setEnabled(!any);
    browse_btn_->setEnabled(!any);
    device_box_->setEnabled(!any);
    node_box_->setEnabled(!any);
    seconds_box_->setEnabled(!any);
    rtt_count_box_->setEnabled(!any);
    storage_path_->setEnabled(!any);
}

void MainWindow::showStatus(const QString& message, bool error) {
    if (message.isEmpty() && !error) {
        status_->hide();
        status_->clear();
        status_->setToolTip(QString());
        return;
    }
    QString shown = message;
    constexpr int kMax = 180;
    if (shown.size() > kMax) shown = shown.left(kMax - 1) + QStringLiteral("…");
    status_->setText(shown);
    status_->setToolTip(message);
    status_->setObjectName(error ? QStringLiteral("statusErr") : QStringLiteral("statusOk"));
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
    status_->show();
}

bool MainWindow::selectedDevice(QVariantMap* out) const {
    const auto data = device_box_->currentData().toMap();
    if (data.isEmpty()) return false;
    *out = data;
    return true;
}

void MainWindow::onRefreshDevices() {
    setBusy(true);
    showStatus(QString(), false);
    QMetaObject::invokeMethod(worker_, "listCameras", Qt::QueuedConnection);
}

void MainWindow::onRefreshLinks() {
    setBusy(true);
    showStatus(QString(), false);
    QMetaObject::invokeMethod(worker_, "refreshLinks", Qt::QueuedConnection);
}

QStringList MainWindow::captureNodes() const {
    QStringList nodes;
    for (int i = 0; i < node_box_->count(); ++i) {
        const auto data = node_box_->itemData(i).toString();
        if (data.isEmpty() || data == QStringLiteral("color-depth")) continue;
        nodes << data;
    }
    return nodes;
}

void MainWindow::syncNodeCaption() {
    const bool depth = node_box_->currentData().toString() == QStringLiteral("color-depth");
    const QString caption = depth ? QStringLiteral("深度") : QStringLiteral("视频");
    stream_gauge_->setCaption(caption);
    stream_spark_->setTitle(depth ? QStringLiteral("深度") : QStringLiteral("视频"));
    const QString shown = node_box_->currentText();
    footer_node_title_->setText(shown.isEmpty() ? QStringLiteral("节点") : shown);
    footer_node_title_->setToolTip(node_box_->currentData().toString());
}

void MainWindow::clearStreamHistory() {
    stream_samples_.clear();
    stream_spark_->setBusy(false);
    stream_spark_->setSamples({});
    stream_spark_->setValueText(QString());
    stream_spark_->setCeiling(500);
    stream_gauge_->setBusy(false);
    stream_gauge_->setRange(0, 500);
    stream_gauge_->setValue(0, false);
}

void MainWindow::noteStoragePath() {
    const auto path = storage_path_->text().trimmed();
    if (path == storage_path_key_) return;
    storage_path_key_ = path;
    storage_samples_.clear();
    storage_spark_->setBusy(false);
    storage_spark_->setSamples({});
    storage_spark_->setValueText(QString());
    storage_spark_->setCeiling(500);
}

void MainWindow::showStream(double mib_per_sec, double mbit_per_sec, double fps, const QString& tip) {
    setBusy(false);
    stream_gauge_->setBusy(false);
    stream_gauge_->setRange(0, niceCeiling(std::max(mbit_per_sec, 500.0)));
    stream_gauge_->setValue(mbit_per_sec);
    stream_gauge_->setCaption(QStringLiteral("%1  ·  %2 fps")
                                  .arg(depth_gauge_ ? QStringLiteral("深度") : QStringLiteral("视频"))
                                  .arg(fps, 0, 'f', 1));
    stream_gauge_->setToolTip(QStringLiteral("%1 MiB/s\n%2").arg(mib_per_sec, 0, 'f', 2).arg(tip));
    rememberMbps(&stream_samples_, stream_spark_, mbit_per_sec);
    showCameraStandard(mbit_per_sec);
}

void MainWindow::startStream(const QString& profile) {
    QVariantMap device;
    if (!selectedDevice(&device)) {
        showStatus(QStringLiteral("未选设备"), true);
        return;
    }
    const auto node = node_box_->currentData().toString();
    if (node.isEmpty()) {
        showStatus(QStringLiteral("无采集节点"), true);
        return;
    }
    const bool color_depth = node == QStringLiteral("color-depth");
    depth_gauge_ = color_depth;
    syncNodeCaption();
    clearCameraStandard();
    setBusy(true);
    stream_gauge_->setBusy(true);
    stream_spark_->setBusy(true);
    showStatus(QString(), false);
    if (color_depth) {
        const auto nodes = captureNodes();
        if (profile.isEmpty()) {
            QMetaObject::invokeMethod(worker_, "measureColorDepth", Qt::QueuedConnection, Q_ARG(QStringList, nodes),
                                      Q_ARG(double, seconds_box_->value()));
        } else {
            QMetaObject::invokeMethod(worker_, "measureColorDepthBench", Qt::QueuedConnection, Q_ARG(QStringList, nodes),
                                      Q_ARG(QString, profile));
        }
        return;
    }
    if (profile.isEmpty()) {
        QMetaObject::invokeMethod(worker_, "measureStream", Qt::QueuedConnection, Q_ARG(QStringList, QStringList{node}),
                                  Q_ARG(double, seconds_box_->value()));
        return;
    }
    QMetaObject::invokeMethod(worker_, "measureStreamBench", Qt::QueuedConnection, Q_ARG(QStringList, QStringList{node}),
                              Q_ARG(QString, profile));
}

void MainWindow::onMeasureStream() { startStream(QString()); }

void MainWindow::onMeasureStreamQuick() { startStream(QStringLiteral("quick")); }

void MainWindow::onMeasureStreamStress() { startStream(QStringLiteral("stress")); }

void MainWindow::onMeasureRtt() {
    QVariantMap device;
    if (!selectedDevice(&device)) {
        showStatus(QStringLiteral("未选设备"), true);
        return;
    }
    setBusy(true);
    showStatus(QString(), false);
    // Windows 上 hub 枚举的 bus/addr 可能和刷新瞬间不一致，RTT 只按 VID/PID 找设备。
    QMetaObject::invokeMethod(worker_, "measureRtt", Qt::QueuedConnection,
                              Q_ARG(quint16, static_cast<quint16>(device.value(QStringLiteral("vendor_id")).toUInt())),
                              Q_ARG(quint16, static_cast<quint16>(device.value(QStringLiteral("product_id")).toUInt())),
                              Q_ARG(QString, QString()), Q_ARG(quint8, static_cast<quint8>(0)),
                              Q_ARG(quint32, static_cast<quint32>(rtt_count_box_->value())));
}

void MainWindow::onMeasureStorage() {
    const auto path = storage_path_->text().trimmed();
    if (path.isEmpty()) {
        showStatus(QStringLiteral("未选文件"), true);
        return;
    }
    noteStoragePath();
    setBusy(true);
    storage_spark_->setBusy(true);
    showStatus(QString(), false);
    QMetaObject::invokeMethod(worker_, "measureStorage", Qt::QueuedConnection, Q_ARG(QString, path),
                              Q_ARG(double, seconds_box_->value()));
}

void MainWindow::onBrowseStorage() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择文件"), QDir::homePath(),
                                                   QStringLiteral("所有文件 (*)"));
    if (!path.isEmpty()) {
        storage_path_->setText(path);
        inspectStoragePath();
    }
}

void MainWindow::inspectStoragePath() {
    noteStoragePath();
    QMetaObject::invokeMethod(worker_, "inspectStorage", Qt::QueuedConnection,
                              Q_ARG(QString, storage_path_->text().trimmed()));
}

void MainWindow::clearCameraStandard() {
    applyStandardBox(stream_std_box_, stream_std_title_, stream_std_badge_, stream_std_detail_, QStringLiteral("未测"),
                     QString(), QStringLiteral("高于 480 Mbit/s 即符合"), QStringLiteral("unknown"));
}

void MainWindow::refreshNegotiatedSpeed() {
    QVariantMap device;
    if (!selectedDevice(&device)) return;
    const auto speed = linkbench::live_link_speed(static_cast<std::uint16_t>(device.value(QStringLiteral("vendor_id")).toUInt()),
                                                  static_cast<std::uint16_t>(device.value(QStringLiteral("product_id")).toUInt()),
                                                  device.value(QStringLiteral("bus_id")).toString().toStdString(),
                                                  static_cast<std::uint8_t>(device.value(QStringLiteral("address")).toUInt()));
    if (!speed) return;
    const auto name = QString::fromUtf8(linkbench::speed_name(*speed));
    if (name == device.value(QStringLiteral("speed")).toString()) return;
    device.insert(QStringLiteral("speed"), name);
    device.insert(QStringLiteral("speed_code"), static_cast<int>(*speed));
    const auto label = device.value(QStringLiteral("vid_pid")).toString() + QStringLiteral("  ") + name +
                       QStringLiteral("  ") + device.value(QStringLiteral("product")).toString();
    device.insert(QStringLiteral("label"), label);
    const int index = device_box_->currentIndex();
    device_box_->setItemText(index, label);
    device_box_->setItemData(index, device);
    const auto nodes = device.value(QStringLiteral("video_nodes")).toStringList();
    int capture = 0;
    for (const auto& node : nodes) {
        if (linkbench::is_v4l2_capture_node(node.toStdString())) ++capture;
    }
    footer_device_sub_->setText(QStringLiteral("%1 · %2 · 节点 %3/%4")
                                    .arg(device.value(QStringLiteral("family")).toString())
                                    .arg(name)
                                    .arg(capture)
                                    .arg(nodes.size()));
}

void MainWindow::showCameraStandard(double mbit_per_sec) {
    refreshNegotiatedSpeed();
    const auto readout = readoutFromMeasured(mbit_per_sec);
    applyStandardBox(stream_std_box_, stream_std_title_, stream_std_badge_, stream_std_detail_, readout.title, readout.badge,
                     measuredStandardLine(mbit_per_sec), readout.meets);
}

void MainWindow::onStorageLink(bool usb, int speed, const QString& product, const QString& detail) {
    storage_link_known_ = true;
    storage_link_usb_ = usb;
    storage_link_speed_ = speed;
    storage_link_product_ = product;
    storage_link_detail_ = detail;
    if (!usb) {
        applyStandardBox(storage_std_box_, storage_std_title_, storage_std_badge_, storage_std_detail_,
                         QStringLiteral("未识别"), QString(), detail, QStringLiteral("unknown"));
        return;
    }
    const auto link_speed = static_cast<linkbench::LinkSpeed>(speed);
    const QString line = link_speed == linkbench::LinkSpeed::Unknown
                             ? QStringLiteral("没有读到协商速率")
                             : QStringLiteral("协商 %1").arg(QString::fromUtf8(linkbench::speed_name(link_speed)));
    applyStandardBox(storage_std_box_, storage_std_title_, storage_std_badge_, storage_std_detail_,
                     QStringLiteral("未测"), QString(), product.isEmpty() ? line : line + QStringLiteral(" · ") + product,
                     QStringLiteral("unknown"));
}

void MainWindow::onDeviceChanged(int) {
    node_box_->blockSignals(true);
    node_box_->clear();
    QVariantMap device;
    if (!selectedDevice(&device)) {
        node_box_->blockSignals(false);
        footer_device_title_->setText(QStringLiteral("未选择设备"));
        footer_device_title_->setToolTip(QString());
        footer_device_sub_->setText(QStringLiteral("刷新以枚举相机"));
        clearCameraStandard();
        footer_node_title_->setText(QStringLiteral("节点"));
        footer_node_title_->setToolTip(QString());
        clearStreamHistory();
        stream_gauge_->setCaption(QStringLiteral("视频"));
        stream_spark_->setTitle(QStringLiteral("视频"));
        latency_p50_->setText(QStringLiteral("0"));
        latency_p95_->setText(QStringLiteral("0"));
        latency_max_->setText(QStringLiteral("0"));
        jitter_value_->setText(QStringLiteral("0"));
        error_value_->setText(QStringLiteral("0"));
        return;
    }
    const auto nodes = device.value(QStringLiteral("video_nodes")).toStringList();
    QStringList capture_nodes;
    for (const auto& node : nodes) {
        if (linkbench::is_v4l2_capture_node(node.toStdString())) capture_nodes << node;
    }
    if (capture_nodes.size() >= 2) node_box_->addItem(QStringLiteral("彩色+深度"), QStringLiteral("color-depth"));
    for (const auto& node : capture_nodes) {
        QString label = node;
        const auto marker = QStringLiteral("::mfstream=");
        const int at = node.lastIndexOf(marker);
        if (at >= 0) label = QStringLiteral("流%1").arg(node.mid(at + marker.size()));
        node_box_->addItem(label, node);
        node_box_->setItemData(node_box_->count() - 1, node, Qt::ToolTipRole);
    }
    if (node_box_->count() == 0) node_box_->addItem(QStringLiteral("无采集节点"));
    if (capture_nodes.size() >= 2) node_box_->setCurrentIndex(0);
    node_box_->blockSignals(false);

    const auto product = device.value(QStringLiteral("product")).toString();
    const auto vidpid = device.value(QStringLiteral("vid_pid")).toString();
    footer_device_title_->setText(product.isEmpty() ? vidpid : product);
    footer_device_title_->setToolTip(vidpid + QStringLiteral("  ") + product);
    footer_device_sub_->setText(QStringLiteral("%1 · %2 · 节点 %3/%4")
                                    .arg(device.value(QStringLiteral("family")).toString())
                                    .arg(device.value(QStringLiteral("speed")).toString())
                                    .arg(capture_nodes.size())
                                    .arg(nodes.size()));
    clearCameraStandard();
    syncNodeCaption();
    clearStreamHistory();
    latency_p50_->setText(QStringLiteral("0"));
    latency_p95_->setText(QStringLiteral("0"));
    latency_max_->setText(QStringLiteral("0"));
    jitter_value_->setText(QStringLiteral("0"));
    error_value_->setText(QStringLiteral("0"));
}

void MainWindow::onFailed(const QString& message) {
    setBusy(false);
    stream_gauge_->setBusy(false);
    stream_spark_->setBusy(false);
    storage_spark_->setBusy(false);
    showStatus(message, true);
}

void MainWindow::onListReady(const QVariantList& devices, const QStringList& notes) {
    setBusy(false);
    device_box_->blockSignals(true);
    device_box_->clear();
    for (const auto& item : devices) {
        const auto map = item.toMap();
        device_box_->addItem(map.value(QStringLiteral("label")).toString(), map);
    }
    device_box_->blockSignals(false);
    onDeviceChanged(device_box_->currentIndex());
    if (devices.isEmpty()) {
        footer_device_title_->setText(QStringLiteral("无相机"));
        footer_device_sub_->setText(notes.isEmpty() ? QStringLiteral("未发现奥比中光或 RealSense")
                                                    : notes.join(QStringLiteral(" · ")));
        footer_device_sub_->setToolTip(notes.join(QStringLiteral("\n")));
        showStatus(notes.isEmpty() ? QStringLiteral("无相机") : notes.join(QStringLiteral(" · ")), true);
    } else {
        showStatus(QString(), false);
    }
}

void MainWindow::clearLinkBars() {
    while (QLayoutItem* item = links_bars_layout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
}

void MainWindow::onLinksReady(const QVariantList& bars) {
    setBusy(false);
    clearLinkBars();
    QStringList bits;
    for (const auto& item : bars) {
        const auto map = item.toMap();
        links_bars_layout_->addWidget(makeLinkBarRow(map));
        if (bits.size() < 2) {
            bits << map.value(QStringLiteral("label")).toString() + QLatin1Char(' ') +
                         map.value(QStringLiteral("suffix")).toString();
        }
    }
    if (bars.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("无线路"));
        empty->setObjectName(QStringLiteral("footerSub"));
        links_bars_layout_->addWidget(empty);
    }
    links_bars_layout_->addStretch(1);
    const QString summary = bits.isEmpty() ? QStringLiteral("无线路") : bits.join(QStringLiteral("    "));
    footer_node_sub_->setText(summary);
    footer_node_sub_->setToolTip(summary);
    showStatus(QString(), false);
}

void MainWindow::onRttReady(double p50_us, double p95_us, double max_us, quint32 errors, const QString& detail) {
    setBusy(false);
    latency_p50_->setText(formatUs(p50_us));
    latency_p95_->setText(formatUs(p95_us));
    latency_max_->setText(formatUs(max_us));
    jitter_value_->setText(formatUs(std::max(0.0, p95_us - p50_us)));
    error_value_->setText(QString::number(errors));
    latency_p50_->setToolTip(detail);
    showStatus(QString(), false);
}

void MainWindow::onStreamReady(double mib_per_sec, double mbit_per_sec, double fps, double, const QString& detail) {
    showStream(mib_per_sec, mbit_per_sec, fps, detail);
    showStatus(QString(), false);
}

void MainWindow::onStreamBenchReady(double mib_per_sec, double mbit_per_sec, double fps, bool failed, const QString& headline,
                                    const QString& detail) {
    showStream(mib_per_sec, mbit_per_sec, fps, headline + QLatin1Char('\n') + detail);
    if (failed) showStatus(headline.isEmpty() ? QStringLiteral("未过") : headline, true);
    else showStatus(QString(), false);
}

void MainWindow::onStorageReady(double mib_per_sec) {
    setBusy(false);
    const double mbps = mibToMbps(mib_per_sec);
    rememberMbps(&storage_samples_, storage_spark_, mbps);
    storage_spark_->setToolTip(QStringLiteral("%1 MiB/s").arg(mib_per_sec, 0, 'f', 2));
    if (!storage_link_known_ || !storage_link_usb_) {
        applyStandardBox(storage_std_box_, storage_std_title_, storage_std_badge_, storage_std_detail_,
                         QStringLiteral("未识别"), QString(),
                         storage_link_detail_.isEmpty() ? QStringLiteral("路径对不上 USB 设备") : storage_link_detail_,
                         QStringLiteral("unknown"));
    } else {
        const auto readout = readoutFromMeasured(mbps);
        QString line = measuredStandardLine(mbps);
        if (!storage_link_product_.isEmpty()) line += QStringLiteral(" · ") + storage_link_product_;
        applyStandardBox(storage_std_box_, storage_std_title_, storage_std_badge_, storage_std_detail_, readout.title,
                         readout.badge, line, readout.meets);
    }
    showStatus(QString(), false);
}
