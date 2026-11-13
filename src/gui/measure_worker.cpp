#include "measure_worker.hpp"

#include <QVariant>

#include <algorithm>
#include <vector>

namespace {

QString hex4(std::uint16_t value) {
    return QString("%1").arg(value, 4, 16, QChar('0')).toLower();
}

QVariantMap deviceMap(const linkbench::DeviceRecord& device) {
    QVariantMap map;
    map.insert(QStringLiteral("vendor_id"), device.vendor_id);
    map.insert(QStringLiteral("product_id"), device.product_id);
    map.insert(QStringLiteral("vid_pid"), hex4(device.vendor_id) + ":" + hex4(device.product_id));
    map.insert(QStringLiteral("bus_id"), QString::fromStdString(device.bus_id));
    map.insert(QStringLiteral("address"), static_cast<int>(device.address));
    map.insert(QStringLiteral("speed"), QString::fromUtf8(linkbench::speed_name(device.speed)));
    map.insert(QStringLiteral("speed_code"), static_cast<int>(device.speed));
    map.insert(QStringLiteral("product"), QString::fromStdString(device.product.value_or(device.catalog_name.value_or(""))));
    map.insert(QStringLiteral("family"), QString::fromStdString(device.family.value_or("-")));
    QStringList nodes;
    for (const auto& node : device.video_nodes) nodes.push_back(QString::fromStdString(node));
    map.insert(QStringLiteral("video_nodes"), nodes);
    map.insert(QStringLiteral("label"),
               map.value(QStringLiteral("vid_pid")).toString() + "  " + map.value(QStringLiteral("speed")).toString() + "  " +
                   map.value(QStringLiteral("product")).toString());
    return map;
}

QString joinNotes(const std::vector<std::string>& notes) {
    QStringList lines;
    for (const auto& note : notes) lines << QString::fromStdString(note);
    return lines.join(QStringLiteral("\n"));
}

}  // namespace

MeasureWorker::MeasureWorker(QObject* parent) : QObject(parent) {}

void MeasureWorker::listCameras() {
    try {
        linkbench::DeviceFilter filter;
        filter.cameras_only = true;
        auto report = linkbench::list_devices(filter);
        QVariantList devices;
        for (const auto& device : report.devices) devices.push_back(deviceMap(device));
        QStringList notes;
        for (const auto& note : report.notes) notes << QString::fromStdString(note);
        emit listReady(devices, notes);
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

void MeasureWorker::refreshLinks() {
    try {
        auto report = linkbench::links(false);
        QVariantList bars;

        auto addBar = [&](const QString& label, const QString& attr, const QString& suffix, double value, double max_value,
                          const QString& color) {
            QVariantMap bar;
            bar.insert(QStringLiteral("label"), label);
            bar.insert(QStringLiteral("attr"), attr);
            bar.insert(QStringLiteral("suffix"), suffix);
            bar.insert(QStringLiteral("value"), value);
            bar.insert(QStringLiteral("max"), std::max(max_value, 1.0));
            bar.insert(QStringLiteral("color"), color);
            bars.push_back(bar);
        };

        auto speedColor = [](double gts) -> QString {
            if (gts >= 16.0) return QStringLiteral("#2EE6C7");
            if (gts >= 8.0) return QStringLiteral("#5BB8FF");
            if (gts >= 5.0) return QStringLiteral("#3DDCFF");
            if (gts >= 2.5) return QStringLiteral("#F0B429");
            return QStringLiteral("#6E8098");
        };

        auto parseGts = [](const std::string& text) -> double {
            try {
                return std::stod(text);
            } catch (...) {
                return 0.0;
            }
        };

        for (const auto& port : report.thunderbolt) {
            const auto name = port.name ? QString::fromStdString(*port.name)
                                        : QString::fromStdString(port.path).section(QLatin1Char('/'), -1);
            const double tx = port.tx_speed_gbps.value_or(0.0);
            const double rx = port.rx_speed_gbps.value_or(0.0);
            const double ceiling = std::max({tx, rx, 40.0});
            if (port.tx_speed_gbps) {
                addBar(name + QStringLiteral(" TX"), QStringLiteral("Thunderbolt"),
                       QString("%1 Gbit/s").arg(tx, 0, 'f', 1), tx, ceiling, QStringLiteral("#5BB8FF"));
            }
            if (port.rx_speed_gbps) {
                addBar(name + QStringLiteral(" RX"), QStringLiteral("Thunderbolt"),
                       QString("%1 Gbit/s").arg(rx, 0, 'f', 1), rx, ceiling, QStringLiteral("#2EE6C7"));
            }
        }

        if (report.pcie_functions_with_link > 0) {
            const double total = static_cast<double>(report.pcie_functions_with_link);
            const double full = total - static_cast<double>(report.pcie_degraded.size());
            addBar(QStringLiteral("满速"), QStringLiteral("PCIe"),
                   QString("%1/%2").arg(static_cast<int>(full)).arg(static_cast<int>(total)), full, total,
                   full >= total ? QStringLiteral("#2EE6C7") : QStringLiteral("#F0B429"));

            std::vector<std::pair<double, std::pair<std::string, std::size_t>>> ranked;
            ranked.reserve(report.pcie_speed_counts.size());
            for (const auto& pair : report.pcie_speed_counts) {
                ranked.push_back({parseGts(pair.first), pair});
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& item : ranked) {
                const double gts = item.first;
                const auto& speed = item.second.first;
                const auto count = item.second.second;
                QString label = QString::fromStdString(speed);
                label.remove(QStringLiteral(" PCIe"));
                addBar(label, QStringLiteral("PCIe"), QString("×%1").arg(count), static_cast<double>(count), total,
                       speedColor(gts));
            }
        }

        emit linksReady(bars);
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

void MeasureWorker::measureRtt(quint16 vendor_id, quint16 product_id, const QString& bus_id, quint8 address, quint32 count) {
    try {
        linkbench::DeviceFilter filter;
        filter.vendor_id = vendor_id;
        filter.product_id = product_id;
        if (!bus_id.isEmpty()) filter.bus_id = bus_id.toStdString();
        if (address != 0) filter.address = address;
        linkbench::RttOptions options;
        options.count = count;
        options.warmup = 20;
        auto report = linkbench::control_rtt(filter, options);
        const double p50_us = report.samples_ns.p50 / 1000.0;
        const double p95_us = report.samples_ns.p95 / 1000.0;
        const double max_us = report.samples_ns.max / 1000.0;
        QString detail = QStringLiteral("n=%1  mean %2 µs  stdev %3 µs")
                             .arg(report.samples_ns.count)
                             .arg(report.samples_ns.mean / 1000.0, 0, 'f', 1)
                             .arg(report.samples_ns.stdev / 1000.0, 0, 'f', 1);
        const auto notes = joinNotes(report.notes);
        if (!notes.isEmpty()) detail += "\n" + notes;
        emit rttReady(p50_us, p95_us, max_us, report.errors, detail);
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

linkbench::CaptureRequest colorDepthRequest(const QStringList& nodes, double seconds, std::uint32_t buffers) {
    if (nodes.isEmpty()) throw linkbench::Error("没有可选的视频节点");
    std::vector<std::string> paths;
    for (const auto& node : nodes) paths.push_back(node.toStdString());
    auto planned = linkbench::plan_color_depth(linkbench::describe_capture_nodes(paths));
    planned.request.seconds = seconds;
    planned.request.warmup_frames = 30;
    planned.request.buffers = buffers;
    return planned.request;
}

const linkbench::FrameStream* depthStream(const std::vector<linkbench::FrameStream>& streams) {
    for (const auto& stream : streams) {
        if (stream.role == "depth") return &stream;
    }
    return nullptr;
}

QString streamDetail(const linkbench::StreamReport& report) {
    QStringList lines;
    for (const auto& stream : report.streams) {
        QString role;
        if (stream.role == "depth") role = QStringLiteral("深度 ");
        else if (stream.role == "color") role = QStringLiteral("彩色 ");
        lines << role + QString("%1  %2 fps  %3")
                         .arg(QString::fromStdString(stream.node))
                         .arg(stream.window_fps, 0, 'f', 1)
                         .arg(QString::fromStdString(linkbench::fmt_bytes_per_sec(stream.window_bytes_per_sec)));
    }
    for (const auto& skipped : report.skipped) {
        lines << QString("跳过 %1: %2").arg(QString::fromStdString(skipped.node), QString::fromStdString(skipped.reason));
    }
    const auto notes = joinNotes(report.notes);
    if (!notes.isEmpty()) lines << notes;
    return lines.join(QStringLiteral("\n"));
}

void MeasureWorker::measureColorDepth(const QStringList& nodes, double seconds) {
    try {
        auto report = linkbench::capture(colorDepthRequest(nodes, seconds, 4));
        const auto* depth = depthStream(report.streams);
        if (!depth) throw linkbench::Error("没有采到深度");
        const double mib_per_sec = depth->window_bytes_per_sec / (1024.0 * 1024.0);
        const double mbit_per_sec = depth->window_bytes_per_sec * 8.0 / 1e6;
        emit streamReady(mib_per_sec, mbit_per_sec, depth->window_fps, depth->mean_frame_bytes, streamDetail(report));
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

void MeasureWorker::measureColorDepthBench(const QStringList& nodes, const QString& profile) {
    try {
        auto plan = linkbench::stream_bench_plan(profile.toStdString());
        auto request = colorDepthRequest(nodes, plan.seconds, plan.buffers);
        auto report = linkbench::benchmark_stream(request, plan);
        if (report.score.pass_index == 0 || report.score.pass_index > report.passes.size()) {
            throw linkbench::Error("没有测速结果");
        }
        const auto& chosen = report.passes[report.score.pass_index - 1];
        const auto* depth = depthStream(chosen.streams);
        if (!depth) throw linkbench::Error("没有采到深度");
        const double mib_per_sec = depth->window_bytes_per_sec / (1024.0 * 1024.0);
        const double mbit_per_sec = depth->window_bytes_per_sec * 8.0 / 1e6;
        const auto headline = QString("%1 · 深度 · 第 %2/%3 次%4")
                                  .arg(QString::fromStdString(report.plan.title))
                                  .arg(report.score.pass_index)
                                  .arg(report.plan.passes)
                                  .arg(report.plan.keep_maximum ? QStringLiteral("取最高") : QStringLiteral("取最低"));
        emit streamBenchReady(mib_per_sec, mbit_per_sec, depth->window_fps, report.score.failed, headline,
                              QString::fromStdString(linkbench::text_stream_bench(report)));
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

void MeasureWorker::measureStream(const QStringList& nodes, double seconds) {
    try {
        if (nodes.isEmpty()) throw linkbench::Error("没有可选的视频节点");
        linkbench::CaptureRequest request;
        request.seconds = seconds;
        request.warmup_frames = 30;
        for (const auto& node : nodes) request.paths.push_back(node.toStdString());
        auto report = linkbench::capture(request);
        const double mib_per_sec = report.aggregate_bytes_per_sec / (1024.0 * 1024.0);
        const double mbit_per_sec = report.aggregate_bytes_per_sec * 8.0 / 1e6;
        double fps = 0;
        double mean_frame_bytes = 0;
        QStringList lines;
        for (const auto& stream : report.streams) {
            fps += stream.window_fps;
            mean_frame_bytes += stream.mean_frame_bytes;
            lines << QString("%1  %2 fps  %3")
                         .arg(QString::fromStdString(stream.node))
                         .arg(stream.window_fps, 0, 'f', 1)
                         .arg(QString::fromStdString(linkbench::fmt_bytes_per_sec(stream.window_bytes_per_sec)));
        }
        if (!report.streams.empty()) mean_frame_bytes /= static_cast<double>(report.streams.size());
        for (const auto& skipped : report.skipped) {
            lines << QString("跳过 %1: %2")
                         .arg(QString::fromStdString(skipped.node))
                         .arg(QString::fromStdString(skipped.reason));
        }
        const auto notes = joinNotes(report.notes);
        if (!notes.isEmpty()) lines << notes;
        emit streamReady(mib_per_sec, mbit_per_sec, fps, mean_frame_bytes, lines.join(QStringLiteral("\n")));
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

void MeasureWorker::measureStreamBench(const QStringList& nodes, const QString& profile) {
    try {
        if (nodes.isEmpty()) throw linkbench::Error("没有可选的视频节点");
        auto plan = linkbench::stream_bench_plan(profile.toStdString());
        linkbench::CaptureRequest request;
        request.seconds = plan.seconds;
        request.buffers = plan.buffers;
        request.warmup_frames = 30;
        for (const auto& node : nodes) request.paths.push_back(node.toStdString());
        auto report = linkbench::benchmark_stream(request, plan);
        const double mib_per_sec = report.score.bytes_per_sec / (1024.0 * 1024.0);
        const double mbit_per_sec = report.score.bytes_per_sec * 8.0 / 1e6;
        const auto headline = QString("%1 · 第 %2/%3 次%4")
                                  .arg(QString::fromStdString(report.plan.title))
                                  .arg(report.score.pass_index)
                                  .arg(report.plan.passes)
                                  .arg(report.plan.keep_maximum ? QStringLiteral("取最高") : QStringLiteral("取最低"));
        emit streamBenchReady(mib_per_sec, mbit_per_sec, report.score.fps, report.score.failed, headline,
                              QString::fromStdString(linkbench::text_stream_bench(report)));
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}

void MeasureWorker::inspectStorage(const QString& path) {
    const auto link = linkbench::storage_usb(path.toStdString());
    emit storageLinkReady(link.usb, static_cast<int>(link.speed), QString::fromStdString(link.product),
                          QString::fromStdString(link.detail));
}

void MeasureWorker::measureStorage(const QString& path, double seconds) {
    try {
        if (path.isEmpty()) throw linkbench::Error("请选择 U 盘上的文件路径");
        inspectStorage(path);
        linkbench::StorageOptions options;
        options.path = path.toStdString();
        options.seconds = seconds;
        options.block_bytes = 1024 * 1024;
        options.warmup_bytes = 4ull << 20;
        auto report = linkbench::read_direct(options);
        const double mib_per_sec = report.bytes_per_sec / (1024.0 * 1024.0);
        emit storageReady(mib_per_sec);
    } catch (const std::exception& ex) {
        emit failed(QString::fromUtf8(ex.what()));
    }
}
