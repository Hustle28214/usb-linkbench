#include "linkbench.hpp"

#include "stream_internal.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>

namespace linkbench {

struct Captured {
    std::uint64_t bytes = 0;
    std::int64_t timestamp_ns = 0;
    bool timestamp = false;
    bool dropped = false;
};

struct FrameQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Captured> frames;
    bool failed = false;
    std::string error;

    void push(Captured frame) {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
        cv.notify_one();
    }

    bool pop(Captured* out, std::uint64_t wait_ns) {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::nanoseconds(wait_ns), [&] { return !frames.empty() || failed; })) return false;
        if (frames.empty()) return false;
        *out = frames.front();
        frames.erase(frames.begin());
        return true;
    }
};

namespace {

std::string ns_string(NSString* text) {
    if (!text) return {};
    const char* utf8 = text.UTF8String;
    return utf8 ? utf8 : std::string{};
}

std::string fourcc_text(FourCharCode code) {
    char bytes[5] = {static_cast<char>((code >> 24) & 0xff), static_cast<char>((code >> 16) & 0xff),
                     static_cast<char>((code >> 8) & 0xff), static_cast<char>(code & 0xff), 0};
    std::string text(bytes);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

}  // namespace
}  // namespace linkbench

@interface LBSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) linkbench::FrameQueue* queue;
@end

@implementation LBSink
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (!self.queue || !sample) return;
    linkbench::Captured frame;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (block) frame.bytes = CMBlockBufferGetDataLength(block);
    if (frame.bytes == 0) {
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (image) frame.bytes = CVPixelBufferGetDataSize(image);
    }
    CMTime ts = CMSampleBufferGetPresentationTimeStamp(sample);
    if (CMTIME_IS_NUMERIC(ts)) {
        frame.timestamp = true;
        frame.timestamp_ns = static_cast<std::int64_t>(CMTimeGetSeconds(ts) * 1e9);
    }
    CFTypeRef dropped = CMGetAttachment(sample, kCMSampleBufferAttachmentKey_DroppedFrameReason, nullptr);
    frame.dropped = dropped != nullptr;
    self.queue->push(frame);
}

- (void)captureOutput:(AVCaptureOutput*)output
    didDropSampleBuffer:(CMSampleBufferRef)sample
         fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)sample;
    (void)connection;
    if (!self.queue) return;
    linkbench::Captured frame;
    frame.dropped = true;
    self.queue->push(frame);
}
@end

namespace linkbench {
namespace {

NSArray<AVCaptureDevice*>* video_devices() {
    NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray array];
    if (@available(macOS 14.0, *)) [types addObject:AVCaptureDeviceTypeExternal];
    else [types addObject:AVCaptureDeviceTypeExternalUnknown];
    [types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
    return session.devices ?: @[];
}

bool ids_of(AVCaptureDevice* device, std::uint16_t* vendor, std::uint16_t* product) {
    @try {
        id vid = [device valueForKey:@"USBVendorID"];
        id pid = [device valueForKey:@"USBProductID"];
        if (vid && pid) {
            *vendor = static_cast<std::uint16_t>([vid intValue]);
            *product = static_cast<std::uint16_t>([pid intValue]);
            return true;
        }
    } @catch (NSException*) {
    }
    const auto text = ns_string(device.uniqueID);
    auto lower = text;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto vid = lower.find("vid_");
    const auto pid = lower.find("pid_");
    if (vid == std::string::npos || pid == std::string::npos || vid + 8 > lower.size() || pid + 8 > lower.size()) return false;
    try {
        *vendor = static_cast<std::uint16_t>(std::stoul(lower.substr(vid + 4, 4), nullptr, 16));
        *product = static_cast<std::uint16_t>(std::stoul(lower.substr(pid + 4, 4), nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

AVCaptureDevice* find_device(const std::string& path, std::string* error) {
    NSArray<AVCaptureDevice*>* devices = video_devices();
    std::vector<AVCaptureDevice*> hits;
    for (AVCaptureDevice* device in devices) {
        const auto id = ns_string(device.uniqueID);
        const auto name = ns_string(device.localizedName);
        if (id == path || name == path || id.find(path) != std::string::npos || (!name.empty() && name.find(path) != std::string::npos)) {
            hits.push_back(device);
        }
    }
    if (hits.empty()) {
        if (error) *error = "没有这个视频设备: " + path;
        return nil;
    }
    if (hits.size() != 1) {
        if (error) *error = "视频节点匹配到 " + std::to_string(hits.size()) + " 个设备，请用 list 打出的 uniqueID";
        return nil;
    }
    return hits.front();
}

AVCaptureDeviceFormat* choose_format(AVCaptureDevice* device, const detail::ResolvedFormat& resolved) {
    if (!resolved.width && !resolved.height && !resolved.fourcc && !resolved.fps) return device.activeFormat;
    auto same = [](const std::string& have, const std::string& want) {
        auto lower = [](std::string text) {
            for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return text;
        };
        const auto a = lower(have);
        const auto b = lower(want);
        if (a == b) return true;
        return (a == "yuy2" && b == "yuyv") || (a == "yuyv" && b == "yuy2");
    };
    for (AVCaptureDeviceFormat* format in device.formats) {
        CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        const auto fourcc = fourcc_text(CMFormatDescriptionGetMediaSubType(format.formatDescription));
        if (resolved.width && *resolved.width != static_cast<std::uint32_t>(dim.width)) continue;
        if (resolved.height && *resolved.height != static_cast<std::uint32_t>(dim.height)) continue;
        if (resolved.fourcc && !same(fourcc, *resolved.fourcc)) continue;
        if (!resolved.fps) return format;
        for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
            if (range.minFrameRate - 0.51 <= *resolved.fps && *resolved.fps <= range.maxFrameRate + 0.51) return format;
        }
    }
    return nil;
}

struct Session {
    AVCaptureSession* session = nil;
    LBSink* sink = nil;
    FrameQueue queue;
    ~Session() {
        if (session) [session stopRunning];
    }
};

std::uint64_t sample_bytes_of_format(AVCaptureDeviceFormat* format) {
    (void)format;
    return 0;
}

FrameStream capture_one(const CaptureRequest& request, const std::string& path, detail::Gate& configured, detail::Gate& measuring) {
    std::string error;
    AVCaptureDevice* device = find_device(path, &error);
    if (!device) throw Error(error);
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
            throw Error("macOS 没有相机权限");
        default:
            break;
    }
    const auto resolved = detail::resolve_format(request, path);
    AVCaptureDeviceFormat* format = choose_format(device, resolved);
    if (!format) throw Error("没有匹配的采集格式");
    NSError* ns_error = nil;
    if (![device lockForConfiguration:&ns_error]) throw Error(ns_string(ns_error.localizedDescription));
    device.activeFormat = format;
    if (resolved.fps && *resolved.fps > 0) {
        device.activeVideoMinFrameDuration = CMTimeMake(1, static_cast<int32_t>(*resolved.fps));
        device.activeVideoMaxFrameDuration = CMTimeMake(1, static_cast<int32_t>(*resolved.fps));
    }
    [device unlockForConfiguration];

    Session opened;
    opened.session = [[AVCaptureSession alloc] init];
    opened.sink = [[LBSink alloc] init];
    opened.sink.queue = &opened.queue;
    [opened.session beginConfiguration];
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&ns_error];
    if (!input || ![opened.session canAddInput:input]) throw Error(ns_error ? ns_string(ns_error.localizedDescription) : "加不了视频输入");
    [opened.session addInput:input];
    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = NO;
    if (![opened.session canAddOutput:output]) throw Error("加不了视频输出");
    [opened.session addOutput:output];
    dispatch_queue_t queue = dispatch_queue_create("linkbench.capture", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:opened.sink queue:queue];
    [opened.session commitConfiguration];
    if (!configured.arrive()) throw Error("其他视频节点还没设好格式");
    [opened.session startRunning];
    for (std::uint32_t i = 0; i < request.warmup_frames; ++i) {
        Captured frame;
        if (!opened.queue.pop(&frame, 2'000'000'000ull)) throw Error("预热取帧超时");
    }
    if (!measuring.arrive()) throw Error("其他视频节点预热失败");

    const auto limit = static_cast<std::uint64_t>(request.seconds * 1e9);
    const auto started = now_ns();
    std::uint64_t frames = 0, error_frames = 0, empty_frames = 0, dropped = 0, bytes = 0, timestamped = 0;
    std::optional<std::int64_t> first_ts, last_ts;
    bool stalled = false;
    const std::uint32_t fps = resolved.fps.value_or(0);
    while (elapsed_ns(started) < limit) {
        Captured frame;
        if (!opened.queue.pop(&frame, 2'000'000'000ull)) {
            stalled = true;
            break;
        }
        ++frames;
        if (frame.dropped && frame.bytes == 0) {
            ++dropped;
            continue;
        }
        if (frame.bytes == 0) {
            ++empty_frames;
            continue;
        }
        if (frame.timestamp) {
            if (first_ts && last_ts && fps > 0 && frame.timestamp_ns > *last_ts) {
                const auto expected = 1000000000ll / static_cast<std::int64_t>(fps);
                if (expected > 0) {
                    const auto gap = (frame.timestamp_ns - *last_ts) / expected;
                    if (gap > 1 && gap < 1000) dropped += static_cast<std::uint64_t>(gap - 1);
                }
            }
            if (!first_ts) first_ts = frame.timestamp_ns;
            last_ts = frame.timestamp_ns;
            ++timestamped;
        }
        bytes += frame.bytes;
    }
    [opened.session stopRunning];
    const auto elapsed = std::max<std::uint64_t>(elapsed_ns(started), 1);
    const auto elapsed_s = static_cast<double>(elapsed) / 1e9;
    const auto payload_frames = frames - std::min(frames, empty_frames);
    const double mean = payload_frames == 0 ? 0 : static_cast<double>(bytes) / static_cast<double>(payload_frames);
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    FrameStream stream;
    stream.node = path;
    stream.driver = "AVFoundation";
    stream.card = ns_string(device.localizedName);
    stream.width = static_cast<std::uint32_t>(dim.width);
    stream.height = static_cast<std::uint32_t>(dim.height);
    stream.fourcc = fourcc_text(CMFormatDescriptionGetMediaSubType(format.formatDescription));
    stream.buffer_count = request.buffers;
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
    stream.interval_source = timestamped > 0 ? "sample_time" : "userspace_read";
    if (first_ts && last_ts && timestamped >= 2 && *last_ts > *first_ts) {
        stream.driver_fps = static_cast<double>(timestamped - 1) * 1e9 / static_cast<double>(*last_ts - *first_ts);
    }
    stream.stalled = stalled;
    stream.role = resolved.role;
    stream.verdict = detail::build_verdict(stream.window_fps, mean, resolved.min_fps, resolved.expect_frame_bytes);
    (void)sample_bytes_of_format;
    return stream;
}

}  // namespace

bool is_v4l2_capture_node(const std::string& path) {
    if (path.empty()) return false;
    std::string error;
    return find_device(path, &error) != nil;
}

StreamReport capture(const CaptureRequest& request) {
    auto report = detail::capture_parallel(request, [](const CaptureRequest& req, const std::string& path, detail::Gate& configured,
                                                       detail::Gate& measuring) { return capture_one(req, path, configured, measuring); });
    report.notes.push_back("macOS 用 AVFoundation 读样本字节。队列深度参数不作用到采集缓冲。");
    return report;
}

std::vector<NodeModes> describe_capture_nodes(const std::vector<std::string>& paths) {
    std::vector<NodeModes> nodes;
    for (const auto& path : paths) {
        NodeModes node;
        node.path = path;
        std::string error;
        AVCaptureDevice* device = find_device(path, &error);
        if (!device) {
            node.error = error.empty() ? "不是视频采集设备" : error;
            nodes.push_back(std::move(node));
            continue;
        }
        node.card = ns_string(device.localizedName);
        bool depth = false, color = false, infrared = false;
        for (AVCaptureDeviceFormat* format in device.formats) {
            CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            const auto fourcc = fourcc_text(CMFormatDescriptionGetMediaSubType(format.formatDescription));
            const auto kind = kind_of_fourcc(fourcc);
            depth = depth || kind == StreamKind::Depth;
            color = color || kind == StreamKind::Color;
            infrared = infrared || kind == StreamKind::Infrared;
            for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
                auto add = [&](double fps) {
                    const auto rounded = static_cast<std::uint32_t>(fps + 0.5);
                    if (dim.width <= 0 || dim.height <= 0 || rounded == 0) return;
                    VideoMode mode;
                    mode.width = static_cast<std::uint32_t>(dim.width);
                    mode.height = static_cast<std::uint32_t>(dim.height);
                    mode.fourcc = fourcc;
                    mode.fps = rounded;
                    mode.bytes_per_pixel = bytes_per_pixel_of(fourcc);
                    node.modes.push_back(mode);
                };
                add(range.minFrameRate);
                add(range.maxFrameRate);
            }
        }
        if (depth) node.kind = StreamKind::Depth;
        else if (color) node.kind = StreamKind::Color;
        else if (infrared) node.kind = StreamKind::Infrared;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

std::vector<std::string> capture_nodes_for_usb(std::uint16_t vendor, std::uint16_t product) {
    std::vector<std::string> nodes;
    @autoreleasepool {
        for (AVCaptureDevice* device in video_devices()) {
            std::uint16_t vid = 0;
            std::uint16_t pid = 0;
            if (!ids_of(device, &vid, &pid) || vid != vendor || pid != product) continue;
            const auto id = ns_string(device.uniqueID);
            if (!id.empty()) nodes.push_back(id);
        }
    }
    return nodes;
}

}  // namespace linkbench
