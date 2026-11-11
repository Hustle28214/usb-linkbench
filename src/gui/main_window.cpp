#include "main_window.hpp"

#include "linkbench.hpp"
#include "measure_worker.hpp"
#include "speed_gauge.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QFrame* makePanel(const QString& title, QWidget* body) {
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("panel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);
    auto* heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(heading);
    layout->addWidget(body, 1);
    return panel;
}

QPushButton* makeGhostButton(const QString& text) {
    auto* btn = new QPushButton(text);
    btn->setObjectName(QStringLiteral("ghostBtn"));
    return btn;
}

QHBoxLayout* makeButtonRow(std::initializer_list<QPushButton*> buttons) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->setContentsMargins(0, 0, 0, 0);
    for (auto* btn : buttons) row->addWidget(btn, 1);
    return row;
}

QWidget* makeLinkBarRow(const QVariantMap& bar) {
    auto* row = new QWidget;
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
                                " background: #E8EEF2;"
                                " border: none;"
                                " border-radius: 5px;"
                                " min-height: 10px;"
                                " max-height: 10px;"
                                "}"
                                "QProgressBar::chunk {"
                                " background: %1;"
                                " border-radius: 5px;"
                                "}")
                                .arg(color));
    layout->addWidget(progress);
    return row;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("usb-linkbench"));
    resize(1200, 720);

    auto* root = new QWidget;
    setCentralWidget(root);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(20, 18, 20, 18);
    root_layout->setSpacing(14);

    auto* top = new QHBoxLayout;
    top->setSpacing(16);
    auto* brand = new QLabel(QStringLiteral("usb-linkbench"));
    brand->setObjectName(QStringLiteral("brand"));
    top->addWidget(brand, 1);

    seconds_box_ = new QDoubleSpinBox;
    seconds_box_->setRange(1.0, 60.0);
    seconds_box_->setDecimals(1);
    seconds_box_->setValue(5.0);
    seconds_box_->setSuffix(QStringLiteral(" s"));
    rtt_count_box_ = new QSpinBox;
    rtt_count_box_->setRange(20, 5000);
    rtt_count_box_->setValue(100);
    auto* params = new QHBoxLayout;
    params->setSpacing(10);
    auto* sec_label = new QLabel(QStringLiteral("时长"));
    sec_label->setObjectName(QStringLiteral("fieldLabel"));
    auto* rtt_label = new QLabel(QStringLiteral("RTT"));
    rtt_label->setObjectName(QStringLiteral("fieldLabel"));
    params->addWidget(sec_label);
    params->addWidget(seconds_box_);
    params->addWidget(rtt_label);
    params->addWidget(rtt_count_box_);
    top->addLayout(params);
    root_layout->addLayout(top);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("statusOk"));
    status_->setWordWrap(true);
    status_->setMaximumHeight(40);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->hide();
    root_layout->addWidget(status_);

    auto* splitter = new QSplitter;
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->setObjectName(QStringLiteral("mainSplit"));

    auto* side = new QWidget;
    auto* side_layout = new QVBoxLayout(side);
    side_layout->setContentsMargins(0, 0, 0, 0);
    side_layout->setSpacing(12);

    auto* device_body = new QWidget;
    auto* device_layout = new QVBoxLayout(device_body);
    device_layout->setContentsMargins(0, 0, 0, 0);
    device_layout->setSpacing(8);
    device_box_ = new QComboBox;
    node_box_ = new QComboBox;
    device_meta_ = new QLabel(QStringLiteral("—"));
    device_meta_->setObjectName(QStringLiteral("meta"));
    device_meta_->setWordWrap(true);
    refresh_devices_btn_ = makeGhostButton(QStringLiteral("刷新"));
    auto* node_label = new QLabel(QStringLiteral("节点"));
    node_label->setObjectName(QStringLiteral("fieldLabel"));
    device_layout->addWidget(device_box_);
    device_layout->addWidget(node_label);
    device_layout->addWidget(node_box_);
    device_layout->addWidget(device_meta_);
    device_layout->addWidget(refresh_devices_btn_);
    side_layout->addWidget(makePanel(QStringLiteral("设备"), device_body));

    auto* storage_body = new QWidget;
    auto* storage_layout = new QVBoxLayout(storage_body);
    storage_layout->setContentsMargins(0, 0, 0, 0);
    storage_layout->setSpacing(8);
    storage_path_ = new QLineEdit;
    storage_path_->setPlaceholderText(QStringLiteral("/media/…"));
    browse_btn_ = makeGhostButton(QStringLiteral("浏览"));
    storage_layout->addWidget(storage_path_);
    storage_layout->addWidget(browse_btn_);
    side_layout->addWidget(makePanel(QStringLiteral("U 盘"), storage_body));

    auto* links_scroll = new QScrollArea;
    links_scroll->setWidgetResizable(true);
    links_scroll->setFrameShape(QFrame::NoFrame);
    links_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    links_scroll->setObjectName(QStringLiteral("linksScroll"));
    links_bars_ = new QWidget;
    links_bars_layout_ = new QVBoxLayout(links_bars_);
    links_bars_layout_->setContentsMargins(0, 0, 0, 0);
    links_bars_layout_->setSpacing(10);
    links_bars_layout_->addStretch(1);
    links_scroll->setWidget(links_bars_);
    refresh_links_btn_ = makeGhostButton(QStringLiteral("刷新"));
    auto* links_body = new QWidget;
    auto* links_layout = new QVBoxLayout(links_body);
    links_layout->setContentsMargins(0, 0, 0, 0);
    links_layout->setSpacing(8);
    links_layout->addWidget(links_scroll, 1);
    links_layout->addWidget(refresh_links_btn_);
    side_layout->addWidget(makePanel(QStringLiteral("链路"), links_body), 1);

    splitter->addWidget(side);

    auto* main = new QWidget;
    auto* main_layout = new QHBoxLayout(main);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(12);

    auto* stream_body = new QWidget;
    auto* stream_layout = new QVBoxLayout(stream_body);
    stream_layout->setContentsMargins(0, 0, 0, 0);
    stream_layout->setSpacing(10);
    stream_gauge_ = new SpeedGauge;
    stream_gauge_->setCaption(QStringLiteral("视频"));
    stream_gauge_->setUnit(QStringLiteral("MiB/s"));
    stream_gauge_->setRange(0, 800);
    stream_btn_ = new QPushButton(QStringLiteral("测速"));
    stream_quick_btn_ = makeGhostButton(QStringLiteral("快速"));
    stream_stress_btn_ = makeGhostButton(QStringLiteral("压力"));
    stream_layout->addWidget(stream_gauge_, 1);
    stream_layout->addLayout(makeButtonRow({stream_btn_, stream_quick_btn_, stream_stress_btn_}));
    main_layout->addWidget(makePanel(QStringLiteral("吞吐"), stream_body), 1);

    auto* rtt_body = new QWidget;
    auto* rtt_layout = new QVBoxLayout(rtt_body);
    rtt_layout->setContentsMargins(0, 0, 0, 0);
    rtt_layout->setSpacing(10);
    rtt_gauge_ = new SpeedGauge;
    rtt_gauge_->setCaption(QStringLiteral("p50"));
    rtt_gauge_->setUnit(QStringLiteral("µs"));
    rtt_gauge_->setRange(0, 1000);
    rtt_gauge_->setAccent(QColor(0x0D, 0x94, 0x88));
    rtt_btn_ = new QPushButton(QStringLiteral("测 RTT"));
    rtt_layout->addWidget(rtt_gauge_, 1);
    rtt_layout->addWidget(rtt_btn_);
    main_layout->addWidget(makePanel(QStringLiteral("回传"), rtt_body), 1);

    auto* storage_panel_body = new QWidget;
    auto* storage_panel_layout = new QVBoxLayout(storage_panel_body);
    storage_panel_layout->setContentsMargins(0, 0, 0, 0);
    storage_panel_layout->setSpacing(10);
    storage_gauge_ = new SpeedGauge;
    storage_gauge_->setCaption(QStringLiteral("直读"));
    storage_gauge_->setUnit(QStringLiteral("MiB/s"));
    storage_gauge_->setRange(0, 600);
    storage_gauge_->setAccent(QColor(0x2F, 0x6F, 0xAD));
    storage_std_box_ = new QFrame;
    storage_std_box_->setObjectName(QStringLiteral("stdBox"));
    auto* std_layout = new QVBoxLayout(storage_std_box_);
    std_layout->setContentsMargins(12, 10, 12, 10);
    std_layout->setSpacing(2);
    auto* std_head = new QHBoxLayout;
    std_head->setContentsMargins(0, 0, 0, 0);
    std_head->setSpacing(8);
    storage_std_title_ = new QLabel(QStringLiteral("未识别"));
    storage_std_title_->setObjectName(QStringLiteral("stdTitle"));
    storage_std_badge_ = new QLabel;
    storage_std_badge_->setObjectName(QStringLiteral("stdBadge"));
    storage_std_badge_->hide();
    std_head->addWidget(storage_std_title_, 1);
    std_head->addWidget(storage_std_badge_);
    storage_std_detail_ = new QLabel(QStringLiteral("选择 U 盘上的文件"));
    storage_std_detail_->setObjectName(QStringLiteral("stdDetail"));
    storage_std_detail_->setWordWrap(true);
    std_layout->addLayout(std_head);
    std_layout->addWidget(storage_std_detail_);
    storage_std_box_->setProperty("meets", QStringLiteral("unknown"));
    storage_btn_ = new QPushButton(QStringLiteral("测直读"));
    storage_panel_layout->addWidget(storage_gauge_, 1);
    storage_panel_layout->addWidget(storage_std_box_);
    storage_panel_layout->addWidget(storage_btn_);
    main_layout->addWidget(makePanel(QStringLiteral("U 盘"), storage_panel_body), 1);

    splitter->addWidget(main);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 880});
    root_layout->addWidget(splitter, 1);

    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #F5F7F9; color: #0F1723; font-family: 'Inter','Noto Sans','Segoe UI',sans-serif; }"
        "#brand { font-size: 22px; font-weight: 700; letter-spacing: -0.3px; color: #0F1723; }"
        "#panel { background: #FFFFFF; border: 1px solid #E4E9EE; border-radius: 16px; }"
        "#panelTitle { font-size: 11px; font-weight: 700; letter-spacing: 0.08em; text-transform: uppercase; color: #7A8794; }"
        "#fieldLabel { font-size: 11px; font-weight: 600; color: #7A8794; }"
        "#meta { color: #5A6774; font-size: 12px; background: transparent; }"
        "#statusOk { padding: 8px 12px; border-radius: 10px; background: #E8F6F4; color: #0F766E; font-size: 12px; font-weight: 600; }"
        "#statusErr { padding: 8px 12px; border-radius: 10px; background: #FDECEC; color: #B42318; font-size: 12px; font-weight: 600; }"
        "QPushButton { background: #0F766E; color: white; border: none; border-radius: 10px; padding: 10px 14px; font-weight: 650; font-size: 13px; }"
        "QPushButton:hover { background: #0D9488; }"
        "QPushButton:pressed { background: #0F5F5A; }"
        "QPushButton:disabled { background: #B7CBC9; color: #F5F7F9; }"
        "QPushButton#ghostBtn { background: #EEF2F5; color: #334155; }"
        "QPushButton#ghostBtn:hover { background: #E2E8F0; }"
        "QPushButton#ghostBtn:disabled { background: #F1F4F7; color: #A0AAB4; }"
        "QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {"
        " background: #F8FAFC; border: 1px solid #E2E8F0; border-radius: 10px; padding: 7px 10px; min-height: 18px; }"
        "QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #CBD5E1; }"
        "QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #0D9488; background: #FFFFFF; }"
        "#barLabel { font-size: 12px; font-weight: 600; color: #334155; }"
        "#barAttr { font-size: 10px; font-weight: 600; color: #94A3B8; font-family: 'JetBrains Mono','Cascadia Mono','Menlo',monospace; }"
        "#barSuffix { font-size: 11px; font-weight: 600; color: #7A8794; }"
        "#stdBox { border-radius: 12px; }"
        "#stdBox[meets=\"yes\"] { background: #E8F6F4; border: 1px solid #99F6E4; }"
        "#stdBox[meets=\"no\"] { background: #FFF7ED; border: 1px solid #FED7AA; }"
        "#stdBox[meets=\"unknown\"] { background: #F8FAFC; border: 1px solid #E2E8F0; }"
        "#stdBox QLabel { background: transparent; }"
        "#stdTitle { font-size: 16px; font-weight: 700; }"
        "#stdBox[meets=\"yes\"] #stdTitle, #stdBox[meets=\"yes\"] #stdBadge { color: #0F766E; }"
        "#stdBox[meets=\"no\"] #stdTitle, #stdBox[meets=\"no\"] #stdBadge { color: #C2410C; }"
        "#stdBox[meets=\"unknown\"] #stdTitle { color: #64748B; }"
        "#stdBadge { font-size: 12px; font-weight: 700; }"
        "#stdDetail { font-size: 12px; color: #64748B; }"
        "QScrollArea#linksScroll { background: transparent; border: none; }"
        "QSplitter::handle { background: transparent; }"));

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
}

void MainWindow::showStatus(const QString& message, bool error) {
    if (message.isEmpty() && !error) {
        status_->hide();
        status_->clear();
        status_->setToolTip(QString());
        return;
    }
    QString shown = message;
    constexpr int kMax = 160;
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
    stream_gauge_->setCaption(color_depth ? QStringLiteral("深度") : QStringLiteral("视频"));
    setBusy(true);
    stream_gauge_->setBusy(true);
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
    rtt_gauge_->setBusy(true);
    showStatus(QString(), false);
    QMetaObject::invokeMethod(worker_, "measureRtt", Qt::QueuedConnection,
                              Q_ARG(quint16, static_cast<quint16>(device.value(QStringLiteral("vendor_id")).toUInt())),
                              Q_ARG(quint16, static_cast<quint16>(device.value(QStringLiteral("product_id")).toUInt())),
                              Q_ARG(QString, device.value(QStringLiteral("bus_id")).toString()),
                              Q_ARG(quint8, static_cast<quint8>(device.value(QStringLiteral("address")).toUInt())),
                              Q_ARG(quint32, static_cast<quint32>(rtt_count_box_->value())));
}

void MainWindow::onMeasureStorage() {
    const auto path = storage_path_->text().trimmed();
    if (path.isEmpty()) {
        showStatus(QStringLiteral("未选文件"), true);
        return;
    }
    setBusy(true);
    storage_gauge_->setBusy(true);
    showStatus(QString(), false);
    QMetaObject::invokeMethod(worker_, "measureStorage", Qt::QueuedConnection, Q_ARG(QString, path),
                              Q_ARG(double, seconds_box_->value()));
}

void MainWindow::onBrowseStorage() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择文件"), QStringLiteral("/media"),
                                                   QStringLiteral("所有文件 (*)"));
    if (!path.isEmpty()) {
        storage_path_->setText(path);
        inspectStoragePath();
    }
}

void MainWindow::inspectStoragePath() {
    QMetaObject::invokeMethod(worker_, "inspectStorage", Qt::QueuedConnection,
                              Q_ARG(QString, storage_path_->text().trimmed()));
}

void MainWindow::onStorageLink(bool usb, int speed, const QString& product, const QString& detail) {
    QString meets = QStringLiteral("unknown");
    QString title = QStringLiteral("未识别");
    QString badge;
    QString line = detail;
    if (usb) {
        const auto standard = linkbench::usb_standard(static_cast<linkbench::LinkSpeed>(speed));
        title = QString::fromUtf8(standard.name);
        meets = standard.meets_usb3 ? QStringLiteral("yes") : QStringLiteral("no");
        badge = standard.meets_usb3 ? (title == QStringLiteral("USB 3.0") ? QStringLiteral("符合") : QStringLiteral("符合 3.0"))
                                    : QStringLiteral("未到 3.0");
        line = QStringLiteral("协商 %1").arg(QString::fromUtf8(linkbench::speed_name(static_cast<linkbench::LinkSpeed>(speed))));
        if (!product.isEmpty()) line += QStringLiteral(" · ") + product;
    }
    storage_std_title_->setText(title);
    storage_std_badge_->setText(badge);
    storage_std_badge_->setVisible(!badge.isEmpty());
    storage_std_detail_->setText(line);
    storage_std_box_->setProperty("meets", meets);
    storage_std_box_->style()->unpolish(storage_std_box_);
    storage_std_box_->style()->polish(storage_std_box_);
}

void MainWindow::onDeviceChanged(int) {
    node_box_->clear();
    QVariantMap device;
    if (!selectedDevice(&device)) {
        device_meta_->setText(QStringLiteral("—"));
        return;
    }
    const auto nodes = device.value(QStringLiteral("video_nodes")).toStringList();
    QStringList capture_nodes;
    for (const auto& node : nodes) {
        if (linkbench::is_v4l2_capture_node(node.toStdString())) capture_nodes << node;
    }
    if (capture_nodes.size() >= 2) node_box_->addItem(QStringLiteral("彩色+深度"), QStringLiteral("color-depth"));
    for (const auto& node : capture_nodes) node_box_->addItem(node, node);
    device_meta_->setText(QStringLiteral("%1 · %2 · %3/%4")
                              .arg(device.value(QStringLiteral("family")).toString())
                              .arg(device.value(QStringLiteral("speed")).toString())
                              .arg(capture_nodes.size())
                              .arg(nodes.size()));
}

void MainWindow::onFailed(const QString& message) {
    setBusy(false);
    stream_gauge_->setBusy(false);
    rtt_gauge_->setBusy(false);
    storage_gauge_->setBusy(false);
    showStatus(message, true);
}

void MainWindow::onListReady(const QVariantList& devices, const QStringList& notes) {
    setBusy(false);
    device_box_->clear();
    for (const auto& item : devices) {
        const auto map = item.toMap();
        device_box_->addItem(map.value(QStringLiteral("label")).toString(), map);
    }
    if (devices.isEmpty()) {
        device_meta_->setText(QStringLiteral("—"));
        showStatus(notes.isEmpty() ? QStringLiteral("无相机") : notes.join(QStringLiteral(" · ")), true);
    } else {
        onDeviceChanged(device_box_->currentIndex());
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
    for (const auto& item : bars) {
        links_bars_layout_->addWidget(makeLinkBarRow(item.toMap()));
    }
    links_bars_layout_->addStretch(1);
    showStatus(QString(), false);
}

void MainWindow::onRttReady(double p50_us, double, double max_us, quint32, const QString&) {
    setBusy(false);
    rtt_gauge_->setBusy(false);
    const double ceiling = std::max(1000.0, max_us * 1.2);
    rtt_gauge_->setRange(0, ceiling);
    rtt_gauge_->setValue(p50_us);
    showStatus(QString(), false);
}

void MainWindow::onStreamReady(double mib_per_sec, double, double, double, const QString&) {
    setBusy(false);
    stream_gauge_->setBusy(false);
    const double floor = depth_gauge_ ? 25.0 : 100.0;
    const double ceiling = std::max(floor, mib_per_sec * 1.4);
    stream_gauge_->setRange(0, ceiling);
    stream_gauge_->setValue(mib_per_sec);
    showStatus(QString(), false);
}

void MainWindow::onStreamBenchReady(double mib_per_sec, double, double, bool failed, const QString&, const QString&) {
    setBusy(false);
    stream_gauge_->setBusy(false);
    const double floor = depth_gauge_ ? 25.0 : 100.0;
    const double ceiling = std::max(floor, mib_per_sec * 1.4);
    stream_gauge_->setRange(0, ceiling);
    stream_gauge_->setValue(mib_per_sec);
    if (failed) {
        showStatus(QStringLiteral("未过"), true);
    } else {
        showStatus(QString(), false);
    }
}

void MainWindow::onStorageReady(double mib_per_sec) {
    setBusy(false);
    storage_gauge_->setBusy(false);
    const double ceiling = std::max(50.0, mib_per_sec * 1.4);
    storage_gauge_->setRange(0, ceiling);
    storage_gauge_->setValue(mib_per_sec);
    showStatus(QString(), false);
}
