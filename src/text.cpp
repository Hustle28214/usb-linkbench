#include "linkbench.hpp"

#include <iomanip>
#include <sstream>

namespace linkbench {
namespace {

std::string json_escape(const std::string& text) {
    std::string out;
    for (unsigned char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                if (ch < 0x20) {
                    std::ostringstream os;
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                    out += os.str();
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    return out;
}

std::string q(const std::string& text) { return "\"" + json_escape(text) + "\""; }

std::string device_line(const DeviceRecord& device) {
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(4) << device.vendor_id << ":" << std::setw(4) << device.product_id
       << std::dec << " bus " << device.bus_id << " addr " << static_cast<int>(device.address) << "  " << speed_name(device.speed)
       << "  " << device.family.value_or("-") << "  " << device.product.value_or(device.catalog_name.value_or("")) << "  视频节点 ";
    if (device.video_nodes.empty()) os << "-";
    else {
        for (std::size_t i = 0; i < device.video_nodes.size(); ++i) {
            if (i) os << ",";
            os << device.video_nodes[i];
        }
    }
    return os.str();
}

void push_notes(std::string& out, const std::vector<std::string>& notes) {
    for (const auto& note : notes) out += "注: " + note + "\n";
}

std::string summary_line(const std::string& label, const Summary& summary) {
    return label + " n=" + std::to_string(summary.count) + " min " + fmt_us(summary.min) + " p50 " + fmt_us(summary.p50) +
           " p95 " + fmt_us(summary.p95) + " p99 " + fmt_us(summary.p99) + " max " + fmt_us(summary.max) + " mean " +
           fmt_us_f(summary.mean) + " stdev " + fmt_us_f(summary.stdev);
}

std::string plan_line(const LinkPlan& plan) {
    auto detail = [](std::optional<std::uint64_t> value) { return value ? fmt_bytes_per_sec_u(*value) : "-"; };
    return std::string(speed_name(plan.speed)) + "  合计相对总线编码上限 " + fit_name(plan.aggregate_vs_bus) + "，上限 " +
           detail(plan.line_coding_payload) + "；最大一路相对单端点上限 " + fit_name(plan.largest_vs_endpoint) + "，上限 " +
           detail(plan.one_isoc) + "\n";
}

std::string summary_json(const Summary& summary) {
    std::ostringstream os;
    os << "{\"count\":" << summary.count << ",\"min\":" << summary.min << ",\"p50\":" << summary.p50
       << ",\"p95\":" << summary.p95 << ",\"p99\":" << summary.p99 << ",\"max\":" << summary.max << ",\"mean\":" << summary.mean
       << ",\"stdev\":" << summary.stdev << "}";
    return os.str();
}

std::string device_json(const DeviceRecord& device) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    os << "{\"vendor_id\":\"" << std::setw(4) << device.vendor_id << "\",\"product_id\":\"" << std::setw(4) << device.product_id
       << "\",\"bus\":" << q(device.bus_id) << ",\"address\":" << std::dec << static_cast<int>(device.address)
       << ",\"speed\":" << q(speed_name(device.speed)) << ",\"product\":";
    os << (device.product ? q(*device.product) : "null") << ",\"family\":" << (device.family ? q(*device.family) : "null")
       << ",\"video_nodes\":[";
    for (std::size_t i = 0; i < device.video_nodes.size(); ++i) {
        if (i) os << ",";
        os << q(device.video_nodes[i]);
    }
    os << "]}";
    return os.str();
}

}  // namespace

std::string text_list(const ListReport& report) {
    std::string out = "协商链路  " + std::to_string(report.devices.size()) + " 台设备\n";
    if (report.devices.empty()) out += "没有匹配的 USB 设备。\n";
    for (const auto& device : report.devices) out += device_line(device) + "\n";
    push_notes(out, report.notes);
    return out;
}

std::string text_audit(const AuditReport& report) {
    std::string out = "描述符上限  " + device_line(report.device) + "\n活动配置里的周期端点最大上限: " +
                      (report.max_periodic_ceiling_bytes_per_sec ? fmt_bytes_per_sec_u(*report.max_periodic_ceiling_bytes_per_sec)
                                                                : "无") +
                      "\n";
    for (const auto& config : report.configurations) {
        out += "配置 " + std::to_string(config.value) + (config.active ? " (活动)\n" : "\n");
        for (const auto& iface : config.interfaces) {
            for (const auto& alt : iface.alternates) {
                out += "  接口 " + std::to_string(iface.number) + " alt " + std::to_string(alt.alternate) + " " + alt.class_name +
                       "\n";
                for (const auto& ep : alt.endpoints) {
                    std::ostringstream addr;
                    addr << std::hex << std::setfill('0') << "    0x" << std::setw(2) << static_cast<int>(ep.address);
                    out += addr.str() + " " + ep.direction + " " + transfer_name(ep.transfer) + " 包 " +
                           std::to_string(ep.max_packet) + " 间隔 " + std::to_string(ep.interval) + " 上限 " +
                           (ep.periodic_ceiling_bytes_per_sec ? fmt_bytes_per_sec_u(*ep.periodic_ceiling_bytes_per_sec) : "-") +
                           "\n";
                }
            }
        }
    }
    push_notes(out, report.notes);
    return out;
}

std::string text_rtt(const RttReport& report) {
    std::string out = "控制回传  GET_STATUS  " + device_line(report.device) + "\n预热丢弃 " +
                      std::to_string(report.warmup_discarded) + "  超时 " + std::to_string(report.timeout_ms) + " ms  失败 " +
                      std::to_string(report.errors) + "\n" + summary_line("样本", report.samples_ns) + "\n计时地板 p50 " +
                      fmt_us(report.timer_overhead_ns.p50) + "\n";
    push_notes(out, report.notes);
    return out;
}

std::string text_bulk(const BulkReport& report) {
    std::ostringstream addr;
    addr << std::hex << "0x" << std::setw(2) << std::setfill('0') << static_cast<int>(report.endpoint);
    std::ostringstream elapsed;
    elapsed.setf(std::ios::fixed);
    elapsed.precision(3);
    elapsed << (static_cast<double>(report.elapsed_ns) / 1e9);
    std::string out = "Bulk IN  " + device_line(report.device) + "  接口 " + std::to_string(report.interface) + " 端点 " +
                      addr.str() + "\n" + fmt_bytes_per_sec(report.bytes_per_sec) + "  次数 " + std::to_string(report.reads) +
                      "  用时 " + elapsed.str() + " s  卸载驱动 " + (report.detached_kernel_driver ? "true" : "false") + "\n";
    push_notes(out, report.notes);
    return out;
}

std::string role_label(const std::string& role) {
    if (role == "depth") return "深度";
    if (role == "color") return "彩色";
    return role;
}

std::string text_stream(const StreamReport& report) {
    std::string out = "视频载荷  合计 " + fmt_bytes_per_sec(report.aggregate_bytes_per_sec) + "\n";
    for (const auto& item : report.streams) {
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(2);
        if (!item.role.empty()) line << role_label(item.role) << "  ";
        line << item.node << "  " << item.card << " " << item.width << "x" << item.height << " " << item.fourcc << "  "
             << item.window_fps << " fps  " << fmt_bytes_per_sec(item.window_bytes_per_sec) << "  帧 " << item.frames
             << "  丢帧 " << item.dropped_frames << "  用时 ";
        line.precision(3);
        line << (static_cast<double>(item.elapsed_ns) / 1e9) << " s\n";
        out += line.str();
        if (item.driver_fps) {
            std::ostringstream driver;
            driver.setf(std::ios::fixed);
            driver.precision(2);
            driver << "  驱动时间戳 " << *item.driver_fps << " fps  间隔来源 " << item.interval_source << "\n";
            out += driver.str();
        }
        if (item.verdict) {
            auto flag = [](std::optional<bool> value) {
                if (!value) return std::string("None");
                return *value ? std::string("true") : std::string("false");
            };
            out += "  判定 fps " + flag(item.verdict->fps_ok) + "  帧字节 " + flag(item.verdict->frame_bytes_ok) + "\n";
        }
    }
    for (const auto& skipped : report.skipped) out += "跳过 " + skipped.node + "  " + skipped.reason + "\n";
    push_notes(out, report.notes);
    return out;
}

std::string text_stream_bench(const StreamBenchReport& report) {
    std::ostringstream head;
    head.setf(std::ios::fixed);
    head.precision(2);
    bool depth_score = false;
    if (report.score.pass_index > 0 && report.score.pass_index <= report.passes.size()) {
        for (const auto& stream : report.passes[report.score.pass_index - 1].streams) {
            if (stream.role == "depth") depth_score = true;
        }
    }
    head << report.plan.title << "  " << (depth_score ? "深度 " : "") << (report.plan.keep_maximum ? "得分 " : "下限 ")
         << fmt_bytes_per_sec(report.score.bytes_per_sec) << "  第 " << report.score.pass_index << "/"
         << report.plan.passes << " 次（" << (report.plan.keep_maximum ? "取最高" : "取最低") << "）  " << report.score.fps
         << " fps\n";
    head.precision(1);
    head << "队列 " << report.plan.buffers << "  每次 " << report.plan.seconds << " s  次数 " << report.plan.passes
         << "  间隔 " << report.plan.interval_seconds << " s\n";
    std::string out = head.str();
    for (std::size_t i = 0; i < report.passes.size(); ++i) {
        const auto& pass = report.passes[i];
        double fps = 0;
        std::uint64_t frames = 0;
        std::uint64_t dropped = 0;
        bool stalled = false;
        for (const auto& stream : pass.streams) {
            fps += stream.window_fps;
            frames += stream.frames;
            dropped += stream.dropped_frames;
            stalled = stalled || stream.stalled;
        }
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(2);
        line << "  " << (i + 1) << "  " << fmt_bytes_per_sec(pass.aggregate_bytes_per_sec) << "  " << fps << " fps  帧 "
             << frames << "  丢帧 " << dropped << "  停滞 " << (stalled ? "true" : "false") << "\n";
        out += line.str();
        for (const auto& stream : pass.streams) {
            std::ostringstream detail;
            detail.setf(std::ios::fixed);
            detail.precision(2);
            detail << "    ";
            if (!stream.role.empty()) detail << role_label(stream.role) << "  ";
            detail << stream.node << "  " << stream.width << "x" << stream.height << " " << stream.fourcc << "  "
                   << stream.window_fps << " fps  " << fmt_bytes_per_sec(stream.window_bytes_per_sec) << "\n";
            out += detail.str();
        }
    }
    out += std::string("判定: ") + (report.score.failed ? "未过  " + report.score.reason : "通过") + "\n";
    push_notes(out, report.notes);
    return out;
}

std::string text_storage(const StorageReport& report) {
    std::ostringstream elapsed;
    elapsed.setf(std::ios::fixed);
    elapsed.precision(3);
    elapsed << (static_cast<double>(report.elapsed_ns) / 1e9);
    std::string out = "直接读  " + report.path + "\n" + fmt_bytes_per_sec(report.bytes_per_sec) + "  字节 " +
                      std::to_string(report.bytes) + "  用时 " + elapsed.str() + " s  圈数 " + std::to_string(report.wraps) + "\n";
    if (report.link.usb) {
        const auto standard = usb_standard(report.link.speed);
        out += std::string("标准  ") + standard.name + "  " + (standard.meets_usb3 ? "符合 USB 3.0" : "未到 USB 3.0") + "  协商 " +
               speed_name(report.link.speed);
        if (!report.link.product.empty()) out += "  " + report.link.product;
        out += "\n";
    } else if (!report.link.detail.empty()) {
        out += "标准  " + report.link.detail + "\n";
    }
    push_notes(out, report.notes);
    return out;
}

std::string text_links(const LinksReport& report) {
    std::string out = "协商链路  Thunderbolt / PCIe\n";
    if (report.thunderbolt_skip_reason) out += "Thunderbolt 跳过: " + *report.thunderbolt_skip_reason + "\n";
    for (const auto& port : report.thunderbolt) {
        out += port.path + "  " + port.name.value_or("-") + "  TX " + (port.tx_speed_gbps ? std::to_string(*port.tx_speed_gbps) : "-") +
               " Gbit/s  RX " + (port.rx_speed_gbps ? std::to_string(*port.rx_speed_gbps) : "-") + " Gbit/s  代 " +
               port.generation.value_or("-") + "\n";
    }
    out += "PCIe 有链路的功能号 " + std::to_string(report.pcie_functions_with_link) + "，降速 " +
           std::to_string(report.pcie_degraded.size()) + " 个\n";
    for (const auto& [speed, count] : report.pcie_speed_counts) out += "  " + speed + ": " + std::to_string(count) + "\n";
    for (const auto& function : report.pcie_degraded) {
        out += "  降速 " + function.address + " 当前 " + function.current_speed.value_or("-") + " x" +
               (function.current_width ? std::to_string(*function.current_width) : "-") + " 最大 " +
               function.max_speed.value_or("-") + " x" + (function.max_width ? std::to_string(*function.max_width) : "-") + "\n";
    }
    push_notes(out, report.notes);
    return out;
}

std::string text_budget(const BudgetReport& report) {
    std::string out = "载荷预算  合计 " + fmt_bytes_per_sec_u(report.aggregate_bytes_per_sec) + "\n";
    for (const auto& label : report.labels) out += label + "\n";
    for (const auto& plan : report.plans) out += plan_line(plan);
    if (report.highlighted) {
        out += "指定链路: ";
        out += plan_line(*report.highlighted);
    }
    push_notes(out, report.notes);
    return out;
}

std::string text_timing(const TimingReport& report) {
    std::string out = "计时检查  地板 p50 " + fmt_us(report.timer_overhead_ns.p50) + "  睡眠请求 " +
                      fmt_us(report.sleep_requested_ns) + " 实测 " + fmt_us(report.sleep_measured_ns) + "\n结果: " +
                      (report.sleep_ok ? "通过" : "异常") + "\n";
    push_notes(out, report.notes);
    return out;
}

std::string json_list(const ListReport& report) {
    std::string out = "{\"devices\":[";
    for (std::size_t i = 0; i < report.devices.size(); ++i) {
        if (i) out += ",";
        out += device_json(report.devices[i]);
    }
    out += "]}";
    return out;
}

std::string json_rtt(const RttReport& report) {
    return "{\"device\":" + device_json(report.device) + ",\"samples_ns\":" + summary_json(report.samples_ns) +
           ",\"errors\":" + std::to_string(report.errors) + ",\"timer_overhead_ns\":" + summary_json(report.timer_overhead_ns) + "}";
}

std::string json_stream(const StreamReport& report) {
    std::ostringstream os;
    os << "{\"aggregate_bytes_per_sec\":" << report.aggregate_bytes_per_sec << ",\"streams\":[";
    for (std::size_t i = 0; i < report.streams.size(); ++i) {
        const auto& item = report.streams[i];
        if (i) os << ",";
        os << "{\"node\":" << q(item.node) << ",\"role\":" << q(item.role) << ",\"fourcc\":" << q(item.fourcc)
           << ",\"width\":" << item.width
           << ",\"height\":" << item.height << ",\"frames\":" << item.frames << ",\"bytes\":" << item.bytes
           << ",\"elapsed_ns\":" << item.elapsed_ns << ",\"window_fps\":" << item.window_fps
           << ",\"window_bytes_per_sec\":" << item.window_bytes_per_sec << "}";
    }
    os << "]}";
    return os.str();
}

std::string json_stream_bench(const StreamBenchReport& report) {
    std::ostringstream os;
    os << "{\"profile\":" << q(report.plan.id) << ",\"keep_maximum\":" << (report.plan.keep_maximum ? "true" : "false")
       << ",\"passes\":" << report.plan.passes << ",\"seconds\":" << report.plan.seconds
       << ",\"buffers\":" << report.plan.buffers << ",\"pass_index\":" << report.score.pass_index
       << ",\"bytes_per_sec\":" << report.score.bytes_per_sec << ",\"fps\":" << report.score.fps
       << ",\"drop_ratio\":" << report.score.drop_ratio << ",\"failed\":" << (report.score.failed ? "true" : "false")
       << ",\"reason\":" << q(report.score.reason) << ",\"rounds\":[";
    for (std::size_t i = 0; i < report.passes.size(); ++i) {
        if (i) os << ",";
        os << "{\"bytes_per_sec\":" << report.passes[i].aggregate_bytes_per_sec << "}";
    }
    os << "]}";
    return os.str();
}

std::string json_bulk(const BulkReport& report) {
    std::ostringstream os;
    os << "{\"bytes\":" << report.bytes << ",\"elapsed_ns\":" << report.elapsed_ns << ",\"bytes_per_sec\":" << report.bytes_per_sec
       << ",\"reads\":" << report.reads << "}";
    return os.str();
}

std::string json_storage(const StorageReport& report) {
    std::ostringstream os;
    os << "{\"path\":" << q(report.path) << ",\"bytes\":" << report.bytes << ",\"elapsed_ns\":" << report.elapsed_ns
       << ",\"bytes_per_sec\":" << report.bytes_per_sec << ",\"wraps\":" << report.wraps << "}";
    return os.str();
}

std::string json_budget(const BudgetReport& report) {
    std::ostringstream os;
    os << "{\"aggregate_bytes_per_sec\":" << report.aggregate_bytes_per_sec << ",\"labels\":[";
    for (std::size_t i = 0; i < report.labels.size(); ++i) {
        if (i) os << ",";
        os << q(report.labels[i]);
    }
    os << "]}";
    return os.str();
}

std::string json_timing(const TimingReport& report) {
    std::ostringstream os;
    os << "{\"sleep_ok\":" << (report.sleep_ok ? "true" : "false") << ",\"sleep_measured_ns\":" << report.sleep_measured_ns
       << ",\"timer_overhead_ns\":" << summary_json(report.timer_overhead_ns) << "}";
    return os.str();
}

std::string json_audit(const AuditReport& report) {
    std::ostringstream os;
    os << "{\"device\":" << device_json(report.device) << ",\"max_periodic_ceiling_bytes_per_sec\":";
    if (report.max_periodic_ceiling_bytes_per_sec) os << *report.max_periodic_ceiling_bytes_per_sec;
    else os << "null";
    os << "}";
    return os.str();
}

std::string json_links(const LinksReport& report) {
    std::ostringstream os;
    os << "{\"pcie_functions_with_link\":" << report.pcie_functions_with_link
       << ",\"pcie_degraded\":" << report.pcie_degraded.size() << ",\"thunderbolt\":" << report.thunderbolt.size() << "}";
    return os.str();
}

}  // namespace linkbench
