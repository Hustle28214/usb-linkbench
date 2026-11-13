#pragma once

#include "linkbench.hpp"

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace linkbench {
namespace detail {

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    int total = 0;
    bool failed = false;

    void fail() {
        std::lock_guard lock(mutex);
        failed = true;
        cv.notify_all();
    }

    bool arrive() {
        std::unique_lock lock(mutex);
        if (failed) return false;
        ++arrived;
        if (arrived >= total) cv.notify_all();
        else cv.wait(lock, [&] { return arrived >= total || failed; });
        return !failed;
    }
};

struct ResolvedFormat {
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::string> fourcc;
    std::optional<std::uint32_t> fps;
    std::optional<double> min_fps;
    std::optional<std::uint64_t> expect_frame_bytes;
    std::string role;
};

inline ResolvedFormat resolve_format(const CaptureRequest& request, const std::string& path) {
    ResolvedFormat resolved;
    resolved.width = request.width;
    resolved.height = request.height;
    resolved.fourcc = request.fourcc;
    resolved.fps = request.fps;
    resolved.min_fps = request.min_fps;
    resolved.expect_frame_bytes = request.expect_frame_bytes;
    for (const auto& node : request.node_formats) {
        if (node.path != path) continue;
        if (node.width) resolved.width = node.width;
        if (node.height) resolved.height = node.height;
        if (node.fourcc) resolved.fourcc = node.fourcc;
        if (node.fps) resolved.fps = node.fps;
        if (node.min_fps) resolved.min_fps = node.min_fps;
        if (node.expect_frame_bytes) resolved.expect_frame_bytes = node.expect_frame_bytes;
        if (!node.role.empty()) resolved.role = node.role;
    }
    return resolved;
}

inline std::optional<StreamVerdict> build_verdict(double achieved, double mean_bytes, std::optional<double> min_fps,
                                                  std::optional<std::uint64_t> expect) {
    if (!min_fps && !expect) return std::nullopt;
    StreamVerdict verdict;
    verdict.min_fps = min_fps;
    verdict.achieved_fps = achieved;
    if (min_fps) verdict.fps_ok = achieved + std::numeric_limits<double>::epsilon() >= *min_fps;
    verdict.expected_frame_bytes = expect;
    verdict.mean_frame_bytes = mean_bytes;
    if (expect) {
        const auto low = static_cast<double>(*expect) * 0.95;
        const auto high = static_cast<double>(*expect) * 1.25;
        verdict.frame_bytes_ok = mean_bytes >= low && mean_bytes <= high;
    }
    return verdict;
}

template <typename Fn>
StreamReport capture_parallel(const CaptureRequest& request, Fn&& capture_one) {
    if (request.paths.empty()) throw Error("没有视频节点");
    std::vector<std::string> capture_paths;
    std::vector<SkippedNode> skipped;
    for (const auto& path : request.paths) {
        if (is_v4l2_capture_node(path)) capture_paths.push_back(path);
        else skipped.push_back(SkippedNode{path, "不是视频采集设备"});
    }
    if (capture_paths.empty()) {
        std::string message;
        for (const auto& item : skipped) {
            if (!message.empty()) message += "; ";
            message += item.node + ": " + item.reason;
        }
        throw Error(message.empty() ? "没有可采集的视频节点" : message);
    }

    auto configured = std::make_shared<Gate>();
    auto measuring = std::make_shared<Gate>();
    configured->total = static_cast<int>(capture_paths.size());
    measuring->total = static_cast<int>(capture_paths.size());
    std::mutex result_mutex;
    std::vector<FrameStream> streams;
    std::vector<std::thread> threads;
    for (const auto& path : capture_paths) {
        threads.emplace_back([&, path] {
            try {
                auto stream = capture_one(request, path, *configured, *measuring);
                std::lock_guard lock(result_mutex);
                streams.push_back(std::move(stream));
            } catch (const std::exception& err) {
                configured->fail();
                measuring->fail();
                std::lock_guard lock(result_mutex);
                skipped.push_back(SkippedNode{path, err.what()});
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (request.strict_nodes && streams.size() != capture_paths.size()) {
        std::string message;
        for (const auto& item : skipped) {
            if (item.reason.find("不是视频采集") != std::string::npos) continue;
            if (!message.empty()) message += "; ";
            message += item.node + ": " + item.reason;
        }
        throw Error(message.empty() ? "采集节点未全部成功" : message);
    }
    if (streams.empty()) {
        std::string message;
        for (const auto& item : skipped) {
            if (!message.empty()) message += "; ";
            message += item.node + ": " + item.reason;
        }
        throw Error(message.empty() ? "没有采到视频" : message);
    }
    StreamReport report;
    for (const auto& stream : streams) report.aggregate_bytes_per_sec += stream.window_bytes_per_sec;
    report.streams = std::move(streams);
    report.skipped = std::move(skipped);
    report.timer_overhead_ns = *summarize(overhead_samples(200));
    report.notes.insert(report.notes.end(), request.notes.begin(), request.notes.end());
    return report;
}

}  // namespace detail
}  // namespace linkbench
