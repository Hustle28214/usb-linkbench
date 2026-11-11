#pragma once

#include <QMainWindow>
#include <QVariantMap>

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
    QStringList captureNodes() const;

    QThread* worker_thread_ = nullptr;
    MeasureWorker* worker_ = nullptr;

    QLabel* status_ = nullptr;
    QComboBox* device_box_ = nullptr;
    QComboBox* node_box_ = nullptr;
    QLabel* device_meta_ = nullptr;
    QWidget* links_bars_ = nullptr;
    QVBoxLayout* links_bars_layout_ = nullptr;
    QLineEdit* storage_path_ = nullptr;
    QDoubleSpinBox* seconds_box_ = nullptr;
    QSpinBox* rtt_count_box_ = nullptr;

    SpeedGauge* stream_gauge_ = nullptr;
    SpeedGauge* rtt_gauge_ = nullptr;
    SpeedGauge* storage_gauge_ = nullptr;
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

    int busy_count_ = 0;
    bool depth_gauge_ = false;
};
