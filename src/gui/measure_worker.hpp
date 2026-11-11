#pragma once

#include "linkbench.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MeasureWorker : public QObject {
    Q_OBJECT
public:
    explicit MeasureWorker(QObject* parent = nullptr);

public slots:
    void listCameras();
    void refreshLinks();
    void measureRtt(quint16 vendor_id, quint16 product_id, const QString& bus_id, quint8 address, quint32 count);
    void measureStream(const QStringList& nodes, double seconds);
    void measureStreamBench(const QStringList& nodes, const QString& profile);
    void measureColorDepth(const QStringList& nodes, double seconds);
    void measureColorDepthBench(const QStringList& nodes, const QString& profile);
    void inspectStorage(const QString& path);
    void measureStorage(const QString& path, double seconds);

signals:
    void failed(const QString& message);
    void listReady(const QVariantList& devices, const QStringList& notes);
    void linksReady(const QVariantList& bars);
    void rttReady(double p50_us, double p95_us, double max_us, quint32 errors, const QString& detail);
    void streamReady(double mib_per_sec, double mbit_per_sec, double fps, double mean_frame_bytes, const QString& detail);
    void streamBenchReady(double mib_per_sec, double mbit_per_sec, double fps, bool failed, const QString& headline,
                          const QString& detail);
    void storageLinkReady(bool usb, int speed, const QString& product, const QString& detail);
    void storageReady(double mib_per_sec);
};
