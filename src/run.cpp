#include "linkbench.hpp"

namespace linkbench {
namespace {

DeviceFilter pin(const DeviceRecord& device) {
    DeviceFilter filter;
    filter.vendor_id = device.vendor_id;
    filter.product_id = device.product_id;
    filter.bus_id = device.bus_id;
    filter.address = device.address;
    return filter;
}

std::string step(const std::string& label, const std::string& status, const std::string& reason) {
    std::string line = "  " + label + ": " + status;
    if (!reason.empty()) line += " (" + reason + ")";
    return line + "\n";
}

}  // namespace

RunOutcome run_camera(const RunRequest& request) {
    auto listed = list_devices(request.filter);
    if (listed.devices.empty()) throw Error("没有找到符合条件的 USB 设备");
    if (!request.all_matches && listed.devices.size() != 1) {
        throw Error("匹配到 " + std::to_string(listed.devices.size()) + " 个设备，请用 --vid、--pid、--bus 或 --addr 收窄");
    }
    RunOutcome outcome;
    outcome.text = "相机测试  " + std::to_string(listed.devices.size()) + " 台\n";
    for (const auto& device : listed.devices) {
        const auto filter = pin(device);
        std::string audit_status = "ok";
        std::string audit_reason;
        std::optional<std::uint64_t> ceiling;
        try {
            auto audit = audit_one(filter);
            ceiling = audit.max_periodic_ceiling_bytes_per_sec;
        } catch (const std::exception& err) {
            audit_status = "failed";
            audit_reason = err.what();
        }
        std::string rtt_status = "ok";
        std::string rtt_reason;
        std::optional<Summary> rtt;
        try {
            RttOptions options;
            options.count = request.rtt_count;
            options.warmup = request.rtt_warmup;
            options.timeout_ms = 200;
            rtt = control_rtt(filter, options).samples_ns;
        } catch (const std::exception& err) {
            rtt_status = "failed";
            rtt_reason = err.what();
        }
        std::vector<std::string> labels;
        std::vector<std::uint64_t> payloads;
        for (const auto* mode : request.presets) {
            labels.push_back(std::string(mode->id) + " (" + mode->summary + ")");
            payloads.push_back(mode->payload_bytes_per_sec());
        }
        auto planning = budget_report(labels, payloads, device.speed);
        const auto preset_vs_descriptor =
            payloads.empty() ? Fit::Unknown : fit_payload(planning.aggregate_bytes_per_sec, ceiling);

        std::string stream_status = "ok";
        std::string stream_reason;
        std::optional<StreamReport> stream;
        if (device.video_nodes.empty()) {
            stream_status = "skipped";
            stream_reason = "没有匹配的 V4L2 节点。";
        } else {
            CaptureRequest capture_request;
            capture_request.paths = device.video_nodes;
            capture_request.seconds = request.seconds;
            capture_request.warmup_frames = request.warmup_frames;
            if (device.video_nodes.size() == 1 && request.presets.size() == 1) {
                capture_request.min_fps = static_cast<double>(request.presets[0]->fps) * 0.95;
                capture_request.expect_frame_bytes = request.presets[0]->payload_bytes_per_frame();
            }
            try {
                stream = capture(capture_request);
            } catch (const std::exception& err) {
                stream_status = "failed";
                stream_reason = err.what();
            }
        }
        bool failed = stream && verdict_failed(*stream);
        if (request.strict) {
            if (audit_status != "ok" || rtt_status != "ok" || stream_status == "failed") failed = true;
            if ((planning.highlighted && planning.highlighted->aggregate_vs_bus == Fit::Exceeds) ||
                (planning.highlighted && planning.highlighted->largest_vs_endpoint == Fit::Exceeds) ||
                preset_vs_descriptor == Fit::Exceeds) {
                failed = true;
            }
        }
        outcome.checks_failed = outcome.checks_failed || failed;
        outcome.text += text_list(ListReport{{device}, {}}).substr(std::string("协商链路  1 台设备\n").size());
        outcome.text += step("描述符", audit_status, audit_reason);
        if (ceiling) outcome.text += "  周期端点上限 " + fmt_bytes_per_sec_u(*ceiling) + "\n";
        outcome.text += step("回传", rtt_status, rtt_reason);
        outcome.text += step("码流", stream_status, stream_reason);
        if (stream) outcome.text += text_stream(*stream);
        outcome.text += std::string("  预设对描述符上限: ") + fit_name(preset_vs_descriptor) + "\n";
        if (planning.highlighted) {
            outcome.text += "  " + text_budget(BudgetReport{{}, planning.aggregate_bytes_per_sec, {}, planning.highlighted, {}} );
        }
    }
    outcome.text += "注: audit 和 rtt 在打开控制通道后会关掉，再开始码流，避免占住视频接口。\n";
    outcome.text += "注: 码流失败或本机没有 V4L2 时，该项为 skipped/failed，不改写其他实测。\n";
    if (outcome.checks_failed) outcome.text += "检查未通过\n";
    return outcome;
}

}  // namespace linkbench
