#include "linkbench.hpp"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace linkbench {
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

    void arrive() {
        std::unique_lock lock(mutex);
        if (failed) return;
        ++arrived;
        if (arrived >= total) cv.notify_all();
        else cv.wait(lock, [&] { return arrived >= total || failed; });
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
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
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

    void start() {
        v4l2_requestbuffers req{};
        req.count = 4;
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
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) throw Error(std::string("STREAMON 失败: ") + std::strerror(errno));
    }

    bool dequeue(v4l2_buffer& buf, bool warmup) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
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

std::string c_string(const std::uint8_t* data, std::size_t n) {
    std::string text(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), n));
    return text;
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

FrameStream measure(Capture& dev, const CaptureRequest& request, const std::string& node, const std::string& driver,
                    const std::string& card, const v4l2_format& format, const std::string& interval) {
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
    stream.verdict = build_verdict(stream.window_fps, mean, request.min_fps, request.expect_frame_bytes);
    (void)interval;
    (void)dequeue_intervals;
    (void)driver_intervals;
    return stream;
}

FrameStream capture_one(const CaptureRequest& request, const std::string& path, Gate& gate) {
    Capture dev(path);
    if (request.width && request.height && request.fourcc) {
        dev.set_format(*request.width, *request.height, fourcc_code(*request.fourcc));
    }
    std::string interval = "unavailable";
    if (request.fps) interval = dev.set_fps(*request.fps);
    else interval = dev.interval();
    auto caps = dev.caps();
    auto format = dev.format();
    dev.start();
    for (std::uint32_t i = 0; i < request.warmup_frames; ++i) {
        v4l2_buffer buf{};
        dev.dequeue(buf, true);
    }
    gate.arrive();
    return measure(dev, request, path, c_string(caps.driver, sizeof(caps.driver)), c_string(caps.card, sizeof(caps.card)),
                   format, interval);
}

}  // namespace

bool verdict_failed(const StreamReport& report) {
    for (const auto& stream : report.streams) {
        if (!stream.verdict) continue;
        if (stream.verdict->fps_ok == false || stream.verdict->frame_bytes_ok == false) return true;
    }
    return false;
}

StreamReport capture(const CaptureRequest& request) {
    if (request.paths.empty()) throw Error("没有视频节点");
    auto gate = std::make_shared<Gate>();
    gate->total = static_cast<int>(request.paths.size());
    std::mutex result_mutex;
    std::vector<FrameStream> streams;
    std::vector<SkippedNode> skipped;
    std::vector<std::thread> threads;
    for (const auto& path : request.paths) {
        threads.emplace_back([&, path] {
            try {
                auto stream = capture_one(request, path, *gate);
                std::lock_guard lock(result_mutex);
                streams.push_back(std::move(stream));
            } catch (const std::exception& err) {
                gate->fail();
                std::lock_guard lock(result_mutex);
                skipped.push_back(SkippedNode{path, err.what()});
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (request.strict_nodes && !skipped.empty()) {
        std::string message;
        for (const auto& item : skipped) {
            if (!message.empty()) message += "; ";
            message += item.node + ": " + item.reason;
        }
        throw Error(message);
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
    report.notes = {"window_bytes_per_sec 使用驱动交出的 bytesused 和实测墙钟。预热帧不计入。",
                    "分母是实际经过的时间，不是请求的秒数。", "这是视频载荷，不含 UVC 包头和总线编码开销。",
                    "多路节点在预热后同时进入计时窗；合计速率是各路窗内速率之和。"};
    return report;
}

}  // namespace linkbench
