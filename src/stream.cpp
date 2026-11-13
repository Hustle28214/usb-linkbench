#include "linkbench.hpp"

#if defined(__linux__)
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

namespace linkbench {
#if defined(__linux__)
namespace {

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

struct Mapped {
    void* start = nullptr;
    std::size_t length = 0;
};

class Capture {
   public:
    explicit Capture(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) throw Error("打开 " + path + " 失败: " + std::strerror(errno));
    }
    ~Capture() {
        if (fd_ >= 0) {
            if (streaming_) {
                int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                ioctl(fd_, VIDIOC_STREAMOFF, &type);
            }
            for (auto& buffer : buffers_) {
                if (buffer.start && buffer.start != MAP_FAILED) munmap(buffer.start, buffer.length);
            }
            ::close(fd_);
        }
    }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    v4l2_capability caps() {
        v4l2_capability cap{};
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) throw Error(std::string("QUERYCAP 失败: ") + std::strerror(errno));
        return cap;
    }

    void set_format(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc) {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = fourcc;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) throw Error(std::string("设置格式失败: ") + std::strerror(errno));
    }

    v4l2_format format() {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_FMT, &fmt) < 0) throw Error(std::string("读取格式失败: ") + std::strerror(errno));
        return fmt;
    }

    std::string set_fps(std::uint32_t fps) {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) throw Error(std::string("设置帧率失败: ") + std::strerror(errno));
        return std::to_string(parm.parm.capture.timeperframe.numerator) + "/" +
               std::to_string(parm.parm.capture.timeperframe.denominator);
    }

    std::string interval() {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_PARM, &parm) < 0) return "unavailable";
        return std::to_string(parm.parm.capture.timeperframe.numerator) + "/" +
               std::to_string(parm.parm.capture.timeperframe.denominator);
    }

    std::uint32_t start(std::uint32_t count) {
        if (count < 2) throw Error("mmap 缓冲至少 2 个");
        v4l2_requestbuffers req{};
        req.count = count;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            throw Error(std::string("申请 mmap 缓冲失败: ") + std::strerror(errno));
        }
        buffers_.resize(req.count);
        for (std::uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) throw Error(std::string("QUERYBUF 失败: ") + std::strerror(errno));
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) throw Error(std::string("mmap 失败: ") + std::strerror(errno));
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) throw Error(std::string("QBUF 失败: ") + std::strerror(errno));
        }
        return req.count;
    }

    void stream_on() {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) throw Error(std::string("STREAMON 失败: ") + std::strerror(errno));
        streaming_ = true;
    }

    int fd() const { return fd_; }

    bool dequeue(v4l2_buffer& buf, bool warmup) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;// 值是 0x001
        const int rc = poll(&pfd, 1, 1000);
        if (rc == 0) {
            if (warmup) throw Error("预热取帧失败: 超时");
            return false;
        }
        if (rc < 0) {
            if (warmup) throw Error(std::string("预热取帧失败: ") + std::strerror(errno));
            return false;
        }
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (warmup) throw Error(std::string("预热取帧失败: ") + std::strerror(errno));
            return false;
        }
        const auto saved = buf;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0 && warmup) throw Error(std::string("预热取帧失败: ") + std::strerror(errno));
        buf = saved;
        return true;
    }

   private:
    int fd_ = -1;
    bool streaming_ = false;
    std::vector<Mapped> buffers_;
};

std::uint32_t fourcc_code(const std::string& text) {
    unsigned char bytes[4] = {' ', ' ', ' ', ' '};
    const auto n = std::min<std::size_t>(text.size(), 4);
    std::memcpy(bytes, text.data(), n);
    return v4l2_fourcc(bytes[0], bytes[1], bytes[2], bytes[3]);
}

std::string fourcc_text(std::uint32_t code) {
    char bytes[5] = {static_cast<char>(code & 0xff), static_cast<char>((code >> 8) & 0xff),
                     static_cast<char>((code >> 16) & 0xff), static_cast<char>((code >> 24) & 0xff), 0};
    std::string text(bytes);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

struct ResolvedFormat {
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::string> fourcc;
    std::optional<std::uint32_t> fps;
    std::optional<double> min_fps;
    std::optional<std::uint64_t> expect_frame_bytes;
    std::string role;
};

ResolvedFormat resolve_format(const CaptureRequest& request, const std::string& path) {
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

std::uint32_t fps_of(std::uint32_t numerator, std::uint32_t denominator) {
    if (numerator == 0) return 0;
    return denominator / numerator;
}

void add_mode(std::vector<VideoMode>& modes, std::uint32_t width, std::uint32_t height, const std::string& fourcc,
              std::uint32_t fps) {
    if (width == 0 || height == 0 || fps == 0) return;
    VideoMode mode;
    mode.width = width;
    mode.height = height;
    mode.fourcc = fourcc;
    mode.fps = fps;
    mode.bytes_per_pixel = bytes_per_pixel_of(fourcc);
    modes.push_back(mode);
}

void add_intervals(std::vector<VideoMode>& modes, int fd, std::uint32_t pixel_format, std::uint32_t width,
                   std::uint32_t height, const std::string& fourcc) {
    v4l2_frmivalenum ival{};
    ival.pixel_format = pixel_format;
    ival.width = width;
    ival.height = height;
    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival) == 0 && ival.index < 64) {
        if (ival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            add_mode(modes, width, height, fourcc, fps_of(ival.discrete.numerator, ival.discrete.denominator));
        } else {
            const auto fast = fps_of(ival.stepwise.min.numerator, ival.stepwise.min.denominator);
            const auto slow = fps_of(ival.stepwise.max.numerator, ival.stepwise.max.denominator);
            add_mode(modes, width, height, fourcc, fast);
            if (slow <= 30 && 30 <= fast) add_mode(modes, width, height, fourcc, 30);
        }
        ++ival.index;
    }
}

std::string c_string(const std::uint8_t* data, std::size_t n) {
    std::string text(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), n));
    return text;
}

NodeModes probe_node(const std::string& path) {
    Capture dev(path);
    NodeModes node;
    node.path = path;
    const auto caps = dev.caps();
    node.card = c_string(caps.card, sizeof(caps.card));
    v4l2_fmtdesc fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool depth = false;
    bool color = false;
    bool infrared = false;
    while (ioctl(dev.fd(), VIDIOC_ENUM_FMT, &fmt) == 0 && fmt.index < 64) {
        const auto fourcc = fourcc_text(fmt.pixelformat);
        const auto kind = kind_of_fourcc(fourcc);
        depth = depth || kind == StreamKind::Depth;
        color = color || kind == StreamKind::Color;
        infrared = infrared || kind == StreamKind::Infrared;
        v4l2_frmsizeenum size{};
        size.pixel_format = fmt.pixelformat;
        while (ioctl(dev.fd(), VIDIOC_ENUM_FRAMESIZES, &size) == 0 && size.index < 64) {
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                add_intervals(node.modes, dev.fd(), fmt.pixelformat, size.discrete.width, size.discrete.height, fourcc);
                ++size.index;
                continue;
            }
            add_intervals(node.modes, dev.fd(), fmt.pixelformat, size.stepwise.min_width, size.stepwise.min_height, fourcc);
            add_intervals(node.modes, dev.fd(), fmt.pixelformat, size.stepwise.max_width, size.stepwise.max_height, fourcc);
            break;
        }
        ++fmt.index;
    }
    if (depth) node.kind = StreamKind::Depth;
    else if (color) node.kind = StreamKind::Color;
    else if (infrared) node.kind = StreamKind::Infrared;
    return node;
}

std::optional<StreamVerdict> build_verdict(double achieved, double mean_bytes, std::optional<double> min_fps,
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

FrameStream measure(Capture& dev, const CaptureRequest& request, const ResolvedFormat& resolved, const std::string& node,
                    const std::string& driver, const std::string& card, const v4l2_format& format, const std::string& interval) {
    const auto limit = static_cast<std::uint64_t>(request.seconds * 1e9);
    const auto started = now_ns();
    std::uint64_t frames = 0, error_frames = 0, empty_frames = 0, dropped = 0, bytes = 0;
    std::uint64_t monotonic_frames = 0, other_frames = 0;
    std::optional<std::uint32_t> last_seq;
    std::optional<std::uint64_t> last_dequeue, last_driver, first_driver, last_seen;
    std::vector<std::uint64_t> dequeue_intervals, driver_intervals;
    bool stalled = false;
    while (elapsed_ns(started) < limit) {
        v4l2_buffer buf{};
        if (!dev.dequeue(buf, false)) {
            stalled = true;
            break;
        }
        const auto dequeued = now_ns();
        if (last_dequeue) dequeue_intervals.push_back(dequeued - *last_dequeue);
        last_dequeue = dequeued;
        if (last_seq) {
            const auto gap = buf.sequence - *last_seq;
            if (gap > 1) dropped += gap > 1000 ? 1 : gap - 1;
        }
        last_seq = buf.sequence;
        const bool monotonic = (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) && buf.timestamp.tv_sec >= 0 &&
                               buf.timestamp.tv_usec >= 0;
        if (monotonic) {
            ++monotonic_frames;
            const auto ts = static_cast<std::uint64_t>(buf.timestamp.tv_sec) * 1000000000ull +
                            static_cast<std::uint64_t>(buf.timestamp.tv_usec) * 1000ull;
            if (!first_driver) first_driver = ts;
            if (last_driver && ts >= *last_driver) driver_intervals.push_back(ts - *last_driver);
            last_driver = ts;
            last_seen = ts;
        } else {
            ++other_frames;
        }
        ++frames;
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            ++error_frames;
            continue;
        }
        if (buf.bytesused == 0) {
            ++empty_frames;
            continue;
        }
        bytes += buf.bytesused;
    }
    const auto elapsed = std::max<std::uint64_t>(elapsed_ns(started), 1);
    const auto elapsed_s = static_cast<double>(elapsed) / 1e9;
    const auto payload_frames = frames - std::min(frames, error_frames + empty_frames);
    const double mean = payload_frames == 0 ? 0 : static_cast<double>(bytes) / static_cast<double>(payload_frames);
    const bool timestamp_monotonic = monotonic_frames > 0 && other_frames == 0;
    FrameStream stream;
    stream.node = node;
    stream.driver = driver;
    stream.card = card;
    stream.width = format.fmt.pix.width;
    stream.height = format.fmt.pix.height;
    stream.fourcc = fourcc_text(format.fmt.pix.pixelformat);
    stream.sizeimage = format.fmt.pix.sizeimage;
    stream.warmup_frames = request.warmup_frames;
    stream.frames = frames;
    stream.error_frames = error_frames;
    stream.empty_frames = empty_frames;
    stream.dropped_frames = dropped;
    stream.bytes = bytes;
    stream.elapsed_ns = elapsed;
    stream.window_fps = static_cast<double>(frames) / elapsed_s;
    stream.window_bytes_per_sec = static_cast<double>(bytes) / elapsed_s;
    stream.mean_frame_bytes = mean;
    stream.interval_source = timestamp_monotonic ? "driver_monotonic" : "userspace_dequeue";
    if (timestamp_monotonic && first_driver && last_seen && monotonic_frames >= 2 && *last_seen > *first_driver) {
        stream.driver_fps = static_cast<double>(monotonic_frames - 1) * 1e9 / static_cast<double>(*last_seen - *first_driver);
    }
    stream.stalled = stalled;
    stream.role = resolved.role;
    stream.verdict = build_verdict(stream.window_fps, mean, resolved.min_fps, resolved.expect_frame_bytes);
    (void)interval;
    (void)dequeue_intervals;
    (void)driver_intervals;
    return stream;
}

FrameStream capture_one(const CaptureRequest& request, const std::string& path, Gate& configured, Gate& measuring) {
    Capture dev(path);
    const auto resolved = resolve_format(request, path);
    if (resolved.width && resolved.height && resolved.fourcc) {
        dev.set_format(*resolved.width, *resolved.height, fourcc_code(*resolved.fourcc));
    }
    std::string interval = "unavailable";
    if (resolved.fps) interval = dev.set_fps(*resolved.fps);
    else interval = dev.interval();
    auto caps = dev.caps();
    auto format = dev.format();
    const auto granted = dev.start(request.buffers);
    if (!configured.arrive()) throw Error("其他视频节点还没设好格式");
    dev.stream_on();
    for (std::uint32_t i = 0; i < request.warmup_frames; ++i) {
        v4l2_buffer buf{};
        dev.dequeue(buf, true);
    }
    if (!measuring.arrive()) throw Error("其他视频节点预热失败");
    auto stream = measure(dev, request, resolved, path, c_string(caps.driver, sizeof(caps.driver)),
                          c_string(caps.card, sizeof(caps.card)), format, interval);
    stream.buffer_count = granted;
    return stream;
}

}  // namespace
#endif

bool verdict_failed(const StreamReport& report) {
    for (const auto& stream : report.streams) {
        if (!stream.verdict) continue;
        if (stream.verdict->fps_ok == false || stream.verdict->frame_bytes_ok == false) return true;
    }
    return false;
}

#if defined(__linux__)
bool is_v4l2_capture_node(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    v4l2_capability cap{};
    const int rc = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    ::close(fd);
    if (rc < 0) return false;
    const auto caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) != 0 || (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
}

StreamReport capture(const CaptureRequest& request) {
    if (request.paths.empty()) throw Error("没有视频节点");
    std::vector<std::string> capture_paths;
    std::vector<SkippedNode> skipped;
    for (const auto& path : request.paths) {
        if (is_v4l2_capture_node(path)) capture_paths.push_back(path);
        else skipped.push_back(SkippedNode{path, "非视频采集节点（多为 UVC metadata）"});
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
            if (item.reason.find("非视频采集") != std::string::npos) continue;
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
#endif

namespace {

std::string percent_text(double ratio) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << (ratio * 100.0) << "%";
    return os.str();
}

std::string number_text(double value) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << value;
    return os.str();
}

StreamPassSample sample_of(const StreamReport& report) {
    const FrameStream* depth = nullptr;
    for (const auto& stream : report.streams) {
        if (stream.role == "depth") depth = &stream;
    }
    StreamPassSample sample;
    if (depth) {
        sample.bytes_per_sec = depth->window_bytes_per_sec;
        sample.fps = depth->window_fps;
        sample.frames = depth->frames;
        sample.dropped_frames = depth->dropped_frames;
        sample.error_frames = depth->error_frames;
        sample.empty_frames = depth->empty_frames;
        sample.stalled = depth->stalled;
        return sample;
    }
    sample.bytes_per_sec = report.aggregate_bytes_per_sec;
    for (const auto& stream : report.streams) {
        sample.fps += stream.window_fps;
        sample.frames += stream.frames;
        sample.dropped_frames += stream.dropped_frames;
        sample.error_frames += stream.error_frames;
        sample.empty_frames += stream.empty_frames;
        sample.stalled = sample.stalled || stream.stalled;
    }
    return sample;
}

void check_plan(const StreamBenchPlan& plan) {
    if (plan.passes == 0) throw Error("测速次数要大于 0");
    if (!(plan.seconds > 0)) throw Error("测速时长要大于 0");
    if (plan.interval_seconds < 0) throw Error("测速间隔不能为负");
    if (plan.buffers < 2) throw Error("mmap 缓冲至少 2 个");
    if (plan.max_drop_ratio < 0 || plan.max_drop_ratio > 1) throw Error("丢帧比例阈值要在 0 到 1 之间");
}

}  // namespace

StreamBenchPlan stream_bench_plan(const std::string& name) {
    StreamBenchPlan plan;
    if (name == "quick") {
        plan.id = "quick";
        plan.title = "快速性能";
        plan.seconds = 5;
        plan.passes = 3;
        plan.interval_seconds = 0;
        plan.buffers = 8;
        plan.keep_maximum = true;
        plan.fail_on_stream_errors = false;
        plan.max_drop_ratio = 1;
    } else if (name == "stress") {
        plan.id = "stress";
        plan.title = "压力测试";
        plan.seconds = 10;
        plan.passes = 3;
        plan.interval_seconds = 0;
        plan.buffers = 2;
        plan.keep_maximum = false;
        plan.fail_on_stream_errors = true;
        plan.max_drop_ratio = 0.01;
    } else {
        throw Error("未知测速档 " + name + "，可用 quick 或 stress");
    }
    return plan;
}

StreamBenchScore score_stream_passes(const StreamBenchPlan& plan, const std::vector<StreamPassSample>& passes) {
    if (passes.empty()) throw Error("没有测速结果");
    std::size_t chosen = 0;
    for (std::size_t i = 1; i < passes.size(); ++i) {
        const bool replace = plan.keep_maximum ? passes[i].bytes_per_sec > passes[chosen].bytes_per_sec
                                                : passes[i].bytes_per_sec < passes[chosen].bytes_per_sec;
        if (replace) chosen = i;
    }
    StreamBenchScore score;
    score.pass_index = static_cast<std::uint32_t>(chosen + 1);
    score.bytes_per_sec = passes[chosen].bytes_per_sec;
    score.fps = passes[chosen].fps;
    for (const auto& pass : passes) {
        score.frames += pass.frames;
        score.dropped_frames += pass.dropped_frames;
        score.error_frames += pass.error_frames;
        score.empty_frames += pass.empty_frames;
        if (pass.stalled) ++score.stalled_passes;
    }
    const auto denom = score.frames + score.dropped_frames;
    score.drop_ratio = denom == 0 ? 0 : static_cast<double>(score.dropped_frames) / static_cast<double>(denom);

    std::vector<std::string> reasons;
    if (score.frames == 0) reasons.push_back("没有采到帧");
    if (plan.fail_on_stream_errors) {
        if (score.stalled_passes > 0) reasons.push_back("停滞 " + std::to_string(score.stalled_passes) + " 次");
        if (score.error_frames > 0) reasons.push_back("错误帧 " + std::to_string(score.error_frames));
        if (score.empty_frames > 0) reasons.push_back("空帧 " + std::to_string(score.empty_frames));
        if (score.drop_ratio > plan.max_drop_ratio + 1e-12) {
            reasons.push_back("丢帧比例 " + percent_text(score.drop_ratio) + " 超过 " + percent_text(plan.max_drop_ratio));
        }
    }
    if (plan.min_fps && score.fps + std::numeric_limits<double>::epsilon() < *plan.min_fps) {
        reasons.push_back("帧率 " + number_text(score.fps) + " 低于 " + number_text(*plan.min_fps));
    }
    for (const auto& reason : reasons) {
        if (!score.reason.empty()) score.reason += "；";
        score.reason += reason;
    }
    score.failed = !reasons.empty();
    return score;
}

StreamBenchReport benchmark_stream(const CaptureRequest& request, const StreamBenchPlan& plan_in) {
    StreamBenchPlan plan = plan_in;
    if (request.min_fps) plan.min_fps = request.min_fps;
    check_plan(plan);

    CaptureRequest pass_request = request;
    pass_request.seconds = plan.seconds;
    pass_request.buffers = plan.buffers;

    StreamBenchReport report;
    report.plan = plan;
    std::vector<StreamPassSample> samples;
    samples.reserve(plan.passes);
    for (std::uint32_t i = 0; i < plan.passes; ++i) {
        if (i > 0 && plan.interval_seconds > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(plan.interval_seconds));
        }
        auto pass = capture(pass_request);
        samples.push_back(sample_of(pass));
        report.passes.push_back(std::move(pass));
    }
    report.score = score_stream_passes(plan, samples);
    report.notes.push_back(plan.title + "：每次 " + number_text(plan.seconds) + " s，共 " + std::to_string(plan.passes) +
                           " 次，" + (plan.keep_maximum ? "取最高" : "取最低") + "。预热帧不计入。");
#if defined(__linux__)
    report.notes.push_back("队列深度 " + std::to_string(plan.buffers) +
                           "。这是 V4L2 mmap 缓冲个数，对应 CrystalDiskMark 的队列深度。");
#elif defined(_WIN32)
    report.notes.push_back("队列深度 " + std::to_string(plan.buffers) +
                           "。Windows 的 Media Foundation 不用这个队列长度，数字只用来和 Linux 的测速档对齐。");
#elif defined(__APPLE__)
    report.notes.push_back("队列深度 " + std::to_string(plan.buffers) +
                           "。macOS 的 AVFoundation 不用这个队列长度，数字只用来和 Linux 的测速档对齐。");
#else
    report.notes.push_back("队列深度 " + std::to_string(plan.buffers) + "。");
#endif
    report.notes.push_back(plan.keep_maximum ? "快速性能按 CrystalDiskMark 记分：同一项重复测量，保留最高载荷速率。"
                                             : "压力测试保留最低载荷速率。丢帧比例按 丢帧 / (交出帧 + 丢帧) 计算，超过 " +
                                                   percent_text(plan.max_drop_ratio) + " 为未过。停滞、错误帧、空帧直接未过。");
#if defined(__linux__)
    report.notes.push_back("得分是驱动交出的 bytesused 除以实测墙钟，不是链路标称速率。");
#else
    report.notes.push_back("得分是样本字节数除以实测墙钟，不是链路标称速率。");
#endif
    if (!request.notes.empty()) {
        bool depth = false;
        for (const auto& node : request.node_formats) depth = depth || node.role == "depth";
        if (depth) report.notes.push_back("有深度路时，得分和丢帧按深度那一路计算，不含彩色。");
    }
    report.notes.insert(report.notes.end(), request.notes.begin(), request.notes.end());
    return report;
}

std::uint64_t VideoMode::payload_bytes_per_sec() const {
    if (bytes_per_pixel == 0 || fps == 0) return 0;
    return static_cast<std::uint64_t>(width) * height * bytes_per_pixel * fps;
}

namespace {

std::string normalize_fourcc(std::string fourcc) {
    while (!fourcc.empty() && (fourcc.back() == ' ' || fourcc.back() == '\0')) fourcc.pop_back();
    for (auto& ch : fourcc) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (fourcc == "YUYV") return "YUY2";
    return fourcc;
}

}  // namespace

StreamKind kind_of_fourcc(const std::string& fourcc) {
    const auto code = normalize_fourcc(fourcc);
    if (code == "Z16" || code == "Y16" || code == "W16" || code == "INZI" || code == "RW16") {
        return StreamKind::Depth;
    }
    if (code == "GREY" || code == "Y8" || code == "Y8I" || code == "Y12I" || code == "BA81") return StreamKind::Infrared;
    if (code == "MJPG" || code == "JPEG" || code == "YUY2" || code == "UYVY" || code == "NV12" || code == "NV21" ||
        code == "RGB3" || code == "BGR3") {
        return StreamKind::Color;
    }
    return StreamKind::Unknown;
}

std::uint32_t bytes_per_pixel_of(const std::string& fourcc) {
    const auto code = normalize_fourcc(fourcc);
    if (code == "Z16" || code == "Y16" || code == "W16" || code == "RW16" || code == "YUY2" || code == "UYVY") return 2;
    if (code == "GREY" || code == "Y8" || code == "Y8I" || code == "BA81") return 1;
    return 0;
}

namespace {

bool compressed_color(const std::string& fourcc) { return fourcc == "MJPG" || fourcc == "JPEG"; }

std::uint32_t fps_distance(std::uint32_t fps, std::uint32_t want) {
    if (want == 0) return fps;
    return fps > want ? fps - want : want - fps;
}

}  // namespace

VideoMode pick_depth_mode(const std::vector<VideoMode>& modes, std::uint32_t fps) {
    const VideoMode* best = nullptr;
    std::uint64_t payload = 0;
    for (const auto& mode : modes) {
        if (kind_of_fourcc(mode.fourcc) != StreamKind::Depth || mode.bytes_per_pixel < 2 || mode.fps == 0) continue;
        if (fps != 0 && mode.fps != fps) continue;
        const auto rate = mode.payload_bytes_per_sec();
        if (!best || rate > payload) {
            best = &mode;
            payload = rate;
        }
    }
    if (!best) throw Error(fps == 0 ? "没有未压缩深度模式" : "没有 " + std::to_string(fps) + " fps 的未压缩深度模式");
    return *best;
}

VideoMode pick_color_mode(const std::vector<VideoMode>& modes, std::uint32_t depth_fps, std::uint32_t depth_width,
                           std::uint32_t depth_height) {
    const VideoMode* best = nullptr;
    for (const auto& mode : modes) {
        if (kind_of_fourcc(mode.fourcc) != StreamKind::Color || mode.fps == 0) continue;
        if (!best) {
            best = &mode;
            continue;
        }
        const bool mode_mjpg = compressed_color(mode.fourcc);
        const bool best_mjpg = compressed_color(best->fourcc);
        if (mode_mjpg != best_mjpg) {
            if (mode_mjpg) best = &mode;
            continue;
        }
        const auto mode_distance = fps_distance(mode.fps, depth_fps);
        const auto best_distance = fps_distance(best->fps, depth_fps);
        if (mode_distance != best_distance) {
            if (mode_distance < best_distance) best = &mode;
            continue;
        }
        const bool raw = !mode_mjpg && depth_width > 0 && depth_height > 0;
        const bool mode_match = mode.width == depth_width && mode.height == depth_height;
        const bool best_match = best->width == depth_width && best->height == depth_height;
        if (raw && mode_match != best_match) {
            if (mode_match) best = &mode;
            continue;
        }
        const bool mode_vga = mode.width == 640 && mode.height == 480;
        const bool best_vga = best->width == 640 && best->height == 480;
        if (mode_vga != best_vga) {
            if (mode_vga) best = &mode;
            continue;
        }
        const auto mode_pixels = static_cast<std::uint64_t>(mode.width) * mode.height;
        const auto best_pixels = static_cast<std::uint64_t>(best->width) * best->height;
        if (mode_pixels < best_pixels) best = &mode;
    }
    if (!best) throw Error("没有彩色模式");
    return *best;
}

#if defined(__linux__)
std::vector<NodeModes> describe_capture_nodes(const std::vector<std::string>& paths) {
    std::vector<NodeModes> nodes;
    for (const auto& path : paths) {
        NodeModes node;
        node.path = path;
        if (!is_v4l2_capture_node(path)) {
            node.error = "非视频采集节点";
            nodes.push_back(std::move(node));
            continue;
        }
        try {
            nodes.push_back(probe_node(path));
        } catch (const std::exception& err) {
            node.error = err.what();
            nodes.push_back(std::move(node));
        }
    }
    return nodes;
}
#endif

namespace {

NodeFormat node_format_of(const std::string& path, const VideoMode& mode, const std::string& role, bool expect_bytes) {
    NodeFormat format;
    format.path = path;
    format.width = mode.width;
    format.height = mode.height;
    format.fourcc = mode.fourcc;
    format.fps = mode.fps;
    format.role = role;
    format.min_fps = static_cast<double>(mode.fps) * 0.95;
    if (expect_bytes && mode.bytes_per_pixel > 0) {
        format.expect_frame_bytes = static_cast<std::uint64_t>(mode.width) * mode.height * mode.bytes_per_pixel;
    }
    return format;
}

std::string mode_text(const std::string& node, const VideoMode& mode) {
    return node + " " + std::to_string(mode.width) + "x" + std::to_string(mode.height) + " " + mode.fourcc + " " +
           std::to_string(mode.fps) + " fps";
}

}  // namespace

ColorDepthPlan plan_color_depth(const std::vector<NodeModes>& nodes, std::uint32_t fps) {
    const NodeModes* depth_node = nullptr;
    VideoMode depth_mode;
    std::uint64_t best_payload = 0;
    for (const auto& node : nodes) {
        if (node.kind != StreamKind::Depth || !node.error.empty()) continue;
        try {
            const auto mode = pick_depth_mode(node.modes, fps);
            const auto payload = mode.payload_bytes_per_sec();
            if (!depth_node || payload > best_payload) {
                depth_node = &node;
                depth_mode = mode;
                best_payload = payload;
            }
        } catch (const Error&) {
        }
    }
    if (!depth_node) {
        std::string message = fps == 0 ? "没有可用的深度节点（需要 Z16 或 Y16）"
                                       : "没有 " + std::to_string(fps) + " fps 的未压缩深度模式";
        for (const auto& node : nodes) {
            if (node.error.empty()) continue;
            message += "；" + node.path + ": " + node.error;
        }
        throw Error(message);
    }

    const NodeModes* color_node = nullptr;
    VideoMode color_mode;
    auto color_rank = [](const std::string& fourcc) {
        const auto code = normalize_fourcc(fourcc);
        if (code == "MJPG" || code == "JPEG") return 400;
        if (code == "YUY2") return 300;
        if (code == "NV12" || code == "NV21") return 200;
        if (code == "UYVY") return 100;
        return 50;
    };
    for (const auto& node : nodes) {
        if (&node == depth_node || node.kind != StreamKind::Color || !node.error.empty()) continue;
        try {
            const auto mode = pick_color_mode(node.modes, depth_mode.fps, depth_mode.width, depth_mode.height);
            if (!color_node || color_rank(mode.fourcc) > color_rank(color_mode.fourcc)) {
                color_node = &node;
                color_mode = mode;
            }
        } catch (const Error&) {
        }
    }
    if (!color_node) throw Error("没有可与深度同时打开的彩色节点。保留深度时彩色也要开流。");

    ColorDepthPlan plan;
    plan.color_node = color_node->path;
    plan.depth_node = depth_node->path;
    plan.color_mode = color_mode;
    plan.depth_mode = depth_mode;
    plan.depth_budget_bytes_per_sec = best_payload;
    plan.request.paths = {color_node->path, depth_node->path};
    plan.request.node_formats = {node_format_of(color_node->path, color_mode, "color", false),
                                 node_format_of(depth_node->path, depth_mode, "depth", true)};
    plan.request.strict_nodes = true;
    plan.request.notes.push_back("彩色 " + mode_text(color_node->path, color_mode) + " 与深度 " +
                                 mode_text(depth_node->path, depth_mode) + " 同时开流。");
    plan.request.notes.push_back("深度预算 " + fmt_bytes_per_sec_u(best_payload) +
#if defined(__linux__)
                                 "。这是未压缩帧字节乘请求帧率，实测看深度那一路的 bytesused。");
#else
                                 "。这是未压缩帧字节乘请求帧率，实测看深度那一路的样本字节。");
#endif
    if (best_payload < 12'500'000) plan.request.notes.push_back("深度预算不到 100 Mbit/s。");
    if (const auto hs = default_one_isoc_endpoint_bytes_per_sec(LinkSpeed::High)) {
        if (best_payload > *hs) {
            plan.request.notes.push_back("深度预算超过 USB 2.0 单路等时上限 " + fmt_bytes_per_sec_u(*hs) +
                                         "。挂在 USB 2.0 上时测不到这个预算，需要 USB 3.x。");
        }
    }
    return plan;
}

}  // namespace linkbench
