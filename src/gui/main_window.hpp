#pragma once

#include <QMainWindow>
#include <QVariantMap>
#include <QVector>

class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QThread;
class QVBoxLayout;
class QWidget;
class MeasureWorker;
class Sparkline;
class SpeedGauge;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onRefreshDevices();
    void onRefreshLinks();
    void onMeasureStream();
    void onMeasureStreamQuick();
    void onMeasureStreamStress();
    void onMeasureRtt();
    void onMeasureStorage();
    void onBrowseStorage();
    void onDeviceChanged(int index);

    void onFailed(const QString& message);
    void onListReady(const QVariantList& devices, const QStringList& notes);
    void onLinksReady(const QVariantList& bars);
    void onRttReady(double p50_us, double p95_us, double max_us, quint32 errors, const QString& detail);
    void onStreamReady(double mib_per_sec, double mbit_per_sec, double fps, double mean_frame_bytes, const QString& detail);
    void onStreamBenchReady(double mib_per_sec, double mbit_per_sec, double fps, bool failed, const QString& headline,
                            const QString& detail);
    void onStorageLink(bool usb, int speed, const QString& product, const QString& detail);
    void onStorageReady(double mib_per_sec);

private:
    void setBusy(bool busy);
    void showStatus(const QString& message, bool error);
    bool selectedDevice(QVariantMap* out) const;
    void startStream(const QString& profile);
    void clearLinkBars();
    void inspectStoragePath();
    void noteStoragePath();
    void syncNodeCaption();
    void clearCameraStandard();
    void showCameraStandard(double mbit_per_sec);
    void refreshNegotiatedSpeed();
    void clearStreamHistory();
    void showStream(double mib_per_sec, double mbit_per_sec, double fps, const QString& tip);
    QStringList captureNodes() const;

    QThread* worker_thread_ = nullptr;
    MeasureWorker* worker_ = nullptr;

    QLabel* status_ = nullptr;
    QComboBox* device_box_ = nullptr;
    QComboBox* node_box_ = nullptr;
    QWidget* links_bars_ = nullptr;
    QVBoxLayout* links_bars_layout_ = nullptr;
    QLineEdit* storage_path_ = nullptr;
    QDoubleSpinBox* seconds_box_ = nullptr;
    QSpinBox* rtt_count_box_ = nullptr;

    SpeedGauge* stream_gauge_ = nullptr;
    Sparkline* stream_spark_ = nullptr;
    Sparkline* storage_spark_ = nullptr;
    QLabel* latency_p50_ = nullptr;
    QLabel* latency_p95_ = nullptr;
    QLabel* latency_max_ = nullptr;
    QLabel* jitter_value_ = nullptr;
    QLabel* error_value_ = nullptr;
    QLabel* footer_device_title_ = nullptr;
    QLabel* footer_device_sub_ = nullptr;
    QLabel* footer_node_title_ = nullptr;
    QLabel* footer_node_sub_ = nullptr;

    QFrame* stream_std_box_ = nullptr;
    QLabel* stream_std_title_ = nullptr;
    QLabel* stream_std_badge_ = nullptr;
    QLabel* stream_std_detail_ = nullptr;
    QFrame* storage_std_box_ = nullptr;
    QLabel* storage_std_title_ = nullptr;
    QLabel* storage_std_badge_ = nullptr;
    QLabel* storage_std_detail_ = nullptr;

    QPushButton* refresh_devices_btn_ = nullptr;
    QPushButton* refresh_links_btn_ = nullptr;
    QPushButton* stream_btn_ = nullptr;
    QPushButton* stream_quick_btn_ = nullptr;
    QPushButton* stream_stress_btn_ = nullptr;
    QPushButton* rtt_btn_ = nullptr;
    QPushButton* storage_btn_ = nullptr;
    QPushButton* browse_btn_ = nullptr;

    QVector<double> stream_samples_;
    QVector<double> storage_samples_;
    QString storage_path_key_;
    bool storage_link_known_ = false;
    bool storage_link_usb_ = false;
    int storage_link_speed_ = 0;
    QString storage_link_product_;
    QString storage_link_detail_;

    int busy_count_ = 0;
    bool depth_gauge_ = false;
};
