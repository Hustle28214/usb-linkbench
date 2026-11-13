#include "linkbench.hpp"

#include "os.hpp"
#include "stream_internal.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace linkbench {
namespace {

struct VideoDevice {
    std::string link;
    std::string name;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
};

struct ComApartment {
    bool own = false;
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        own = hr == S_OK || hr == S_FALSE;
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw Error("COM 初始化失败");
    }
    ~ComApartment() {
        if (own) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

void ensure_media() {
    static std::once_flag once;
    static HRESULT status = S_OK;
    std::call_once(once, [] { status = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
    if (FAILED(status)) throw Error("Media Foundation 初始化失败。Windows N 版本需要安装 Media Feature Pack");
}

std::string hr_text(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
    return buf;
}

std::string canon_fourcc(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (text == "YUYV") return "YUY2";
    return text;
}

std::string fourcc_from_subtype(const GUID& guid) {
    const auto code = guid.Data1;
    std::string text;
    text.push_back(static_cast<char>(code & 0xff));
    text.push_back(static_cast<char>((code >> 8) & 0xff));
    text.push_back(static_cast<char>((code >> 16) & 0xff));
    text.push_back(static_cast<char>((code >> 24) & 0xff));
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
    return text;
}

bool parse_vid_pid(const std::string& text, std::uint16_t* vendor, std::uint16_t* product) {
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

std::string allocated_string(IMFActivate* activate, const GUID& key) {
    wchar_t* text = nullptr;
    UINT32 chars = 0;
    if (FAILED(activate->GetAllocatedString(key, &text, &chars)) || !text) return {};
    const auto out = wide_to_utf8(text, static_cast<int>(chars));
    CoTaskMemFree(text);
    return out;
}

std::vector<VideoDevice> enumerate_sources() {
    ComApartment apartment;
    ensure_media();
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) throw Error("创建 Media Foundation 属性失败");
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    attrs->Release();
    if (FAILED(hr)) throw Error("枚举视频设备失败: " + hr_text(hr));
    std::vector<VideoDevice> out;
    for (UINT32 i = 0; i < count; ++i) {
        VideoDevice device;
        device.link = allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        device.name = allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        parse_vid_pid(device.link, &device.vendor, &device.product);
        if (!device.link.empty()) out.push_back(std::move(device));
        devices[i]->Release();
    }
    if (devices) CoTaskMemFree(devices);
    std::sort(out.begin(), out.end(), [](const VideoDevice& a, const VideoDevice& b) { return a.link < b.link; });
    return out;
}

std::vector<VideoDevice> match_sources(const std::string& path) {
    const auto all = enumerate_sources();
    std::vector<VideoDevice> exact;
    std::vector<VideoDevice> partial;
    for (const auto& device : all) {
        if (device.link == path || device.name == path) exact.push_back(device);
        else if (device.link.find(path) != std::string::npos || (!device.name.empty() && device.name.find(path) != std::string::npos)) {
            partial.push_back(device);
        }
    }
    if (!exact.empty()) return exact;
    return partial;
}

struct Reader {
    IMFMediaSource* source = nullptr;
    IMFSourceReader* reader = nullptr;
    std::string name;
    ~Reader() {
        if (reader) reader->Release();
        if (source) {
            source->Shutdown();
            source->Release();
        }
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&& other) noexcept : source(other.source), reader(other.reader), name(std::move(other.name)) {
        other.source = nullptr;
        other.reader = nullptr;
    }
    Reader& operator=(Reader&&) = delete;
    Reader() = default;
};

Reader open_reader(const std::string& path) {
    auto matches = match_sources(path);
    if (matches.empty()) throw Error("没有这个视频设备: " + path);
    if (matches.size() != 1) throw Error("视频节点匹配到 " + std::to_string(matches.size()) + " 个设备，请用 list 打出的完整链接");
    ComApartment apartment;
    ensure_media();
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2)) || !attrs) throw Error("创建 Media Foundation 属性失败");
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    const auto wide = utf8_to_wide(matches.front().link);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wide.c_str());
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    attrs->Release();
    if (FAILED(hr) || count == 0 || !devices) throw Error("打开视频设备失败: " + path);
    Reader reader;
    reader.name = matches.front().name.empty() ? matches.front().link : matches.front().name;
    hr = devices[0]->ActivateObject(IID_IMFMediaSource, reinterpret_cast<void**>(&reader.source));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (FAILED(hr) || !reader.source) throw Error("激活视频设备失败: " + hr_text(hr));
    hr = MFCreateSourceReaderFromMediaSource(reader.source, nullptr, &reader.reader);
    if (FAILED(hr) || !reader.reader) throw Error("创建视频读取器失败: " + hr_text(hr));
    return reader;
}

struct TypeInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string fourcc;
    std::uint32_t fps = 0;
    std::uint32_t sizeimage = 0;
};

TypeInfo read_type(IMFMediaType* type) {
    TypeInfo info;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 num = 0;
    UINT32 den = 0;
    GUID subtype{};
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    UINT32 sample = 0;
    type->GetUINT32(MF_MT_SAMPLE_SIZE, &sample);
    info.width = width;
    info.height = height;
    info.fourcc = fourcc_from_subtype(subtype);
    info.fps = den == 0 ? 0 : num / den;
    info.sizeimage = sample;
    return info;
}

void apply_format(IMFSourceReader* reader, const detail::ResolvedFormat& resolved) {
    if (!resolved.width && !resolved.height && !resolved.fourcc && !resolved.fps) return;
    const auto want = canon_fourcc(resolved.fourcc.value_or(""));
    IMFMediaType* chosen = nullptr;
    for (DWORD index = 0;; ++index) {
        IMFMediaType* type = nullptr;
        const HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), index, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr) || !type) break;
        const auto info = read_type(type);
        bool ok = true;
        if (resolved.width && *resolved.width != info.width) ok = false;
        if (resolved.height && *resolved.height != info.height) ok = false;
        if (!want.empty() && canon_fourcc(info.fourcc) != want) ok = false;
        if (resolved.fps && info.fps != 0 && info.fps != *resolved.fps) {
            UINT32 min_n = 0, min_d = 1, max_n = 0, max_d = 1;
            MFGetAttributeRatio(type, MF_MT_FRAME_RATE_RANGE_MIN, &min_n, &min_d);
            MFGetAttributeRatio(type, MF_MT_FRAME_RATE_RANGE_MAX, &max_n, &max_d);
            const double slow = min_d ? static_cast<double>(min_n) / static_cast<double>(min_d) : 0;
            const double fast = max_d ? static_cast<double>(max_n) / static_cast<double>(max_d) : 0;
            const double low = std::min(slow, fast);
            const double high = std::max(slow, fast);
            if (*resolved.fps + 0.51 < low || *resolved.fps - 0.51 > high) ok = false;
        }
        if (ok && !chosen) chosen = type;
        else type->Release();
    }
    if (!chosen) throw Error("没有匹配的采集格式");
    IMFMediaType* copy = nullptr;
    if (FAILED(MFCreateMediaType(&copy)) || !copy) {
        chosen->Release();
        throw Error("复制媒体类型失败");
    }
    chosen->CopyAllItems(copy);
    chosen->Release();
    if (resolved.fps) MFSetAttributeRatio(copy, MF_MT_FRAME_RATE, *resolved.fps, 1);
    const HRESULT hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, copy);
    copy->Release();
    if (FAILED(hr)) throw Error(std::string("设置格式失败: ") + hr_text(hr));
}

struct Sample {
    std::uint64_t bytes = 0;
    std::optional<std::int64_t> timestamp;
    bool error = false;
    bool empty = false;
};

bool read_one(IMFSourceReader* reader, Sample* sample, bool warmup) {
    const auto started = now_ns();
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* raw = nullptr;
    const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &timestamp, &raw);
    if (now_ns() - started > 2'000'000'000ull) {
        if (raw) raw->Release();
        if (warmup) throw Error("预热取帧超时");
        return false;
    }
    if (FAILED(hr)) {
        if (raw) raw->Release();
        if (warmup) throw Error(std::string("预热取帧失败: ") + hr_text(hr));
        sample->error = true;
        return true;
    }
    if (flags & MF_SOURCE_READERF_ERROR) sample->error = true;
    if (flags & MF_SOURCE_READERF_STREAMTICK) {
        if (raw) raw->Release();
        return true;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (raw) raw->Release();
        if (warmup) throw Error("预热时视频流结束");
        return false;
    }
    if (!raw) {
        sample->empty = true;
        return true;
    }
    sample->timestamp = timestamp;
    IMFMediaBuffer* buffer = nullptr;
    if (SUCCEEDED(raw->ConvertToContiguousBuffer(&buffer)) && buffer) {
        DWORD length = 0;
        buffer->GetCurrentLength(&length);
        sample->bytes = length;
        buffer->Release();
    }
    raw->Release();
    if (sample->bytes == 0) sample->empty = true;
    return true;
}

FrameStream capture_one(const CaptureRequest& request, const std::string& path, detail::Gate& configured, detail::Gate& measuring) {
    ComApartment apartment;
    auto opened = open_reader(path);
    const auto resolved = detail::resolve_format(request, path);
    apply_format(opened.reader, resolved);
    IMFMediaType* current = nullptr;
    TypeInfo format;
    if (SUCCEEDED(opened.reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current)) && current) {
        format = read_type(current);
        current->Release();
    }
    if (!configured.arrive()) throw Error("其他视频节点还没设好格式");
    for (std::uint32_t i = 0; i < request.warmup_frames; ++i) {
        for (;;) {
            Sample sample;
            if (!read_one(opened.reader, &sample, true) || sample.error) throw Error("预热取帧失败");
            if (!sample.timestamp && sample.bytes == 0 && !sample.empty) continue;
            break;
        }
    }
    if (!measuring.arrive()) throw Error("其他视频节点预热失败");

    const auto limit = static_cast<std::uint64_t>(request.seconds * 1e9);
    const auto started = now_ns();
    std::uint64_t frames = 0, error_frames = 0, empty_frames = 0, dropped = 0, bytes = 0;
    std::optional<std::int64_t> last_ts;
    std::optional<std::int64_t> first_ts;
    std::uint64_t timestamped = 0;
    bool stalled = false;
    const std::uint32_t fps = resolved.fps.value_or(format.fps);
    while (elapsed_ns(started) < limit) {
        Sample sample;
        if (!read_one(opened.reader, &sample, false)) {
            stalled = true;
            break;
        }
        if (!sample.timestamp && sample.bytes == 0 && !sample.error && !sample.empty) continue;
        ++frames;
        if (sample.error) {
            ++error_frames;
            continue;
        }
        if (sample.empty || sample.bytes == 0) {
            ++empty_frames;
            continue;
        }
        if (sample.timestamp) {
            if (first_ts && last_ts && fps > 0 && *sample.timestamp > *last_ts) {
                const auto expected = 10'000'000ll / static_cast<std::int64_t>(fps);
                if (expected > 0) {
                    const auto gap = (*sample.timestamp - *last_ts) / expected;
                    if (gap > 1 && gap < 1000) dropped += static_cast<std::uint64_t>(gap - 1);
                }
            }
            if (!first_ts) first_ts = sample.timestamp;
            last_ts = sample.timestamp;
            ++timestamped;
        }
        bytes += sample.bytes;
    }
    const auto elapsed = std::max<std::uint64_t>(elapsed_ns(started), 1);
    const auto elapsed_s = static_cast<double>(elapsed) / 1e9;
    const auto payload_frames = frames - std::min(frames, error_frames + empty_frames);
    const double mean = payload_frames == 0 ? 0 : static_cast<double>(bytes) / static_cast<double>(payload_frames);
    FrameStream stream;
    stream.node = path;
    stream.driver = "Media Foundation";
    stream.card = opened.name;
    stream.width = format.width;
    stream.height = format.height;
    stream.fourcc = format.fourcc;
    stream.sizeimage = format.sizeimage;
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
        stream.driver_fps = static_cast<double>(timestamped - 1) * 1e7 / static_cast<double>(*last_ts - *first_ts);
    }
    stream.stalled = stalled;
    stream.role = resolved.role;
    stream.verdict = detail::build_verdict(stream.window_fps, mean, resolved.min_fps, resolved.expect_frame_bytes);
    return stream;
}

NodeModes probe_node(const std::string& path) {
    ComApartment apartment;
    auto opened = open_reader(path);
    NodeModes node;
    node.path = path;
    node.card = opened.name;
    bool depth = false;
    bool color = false;
    bool infrared = false;
    for (DWORD index = 0;; ++index) {
        IMFMediaType* type = nullptr;
        const HRESULT hr = opened.reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), index, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr) || !type) break;
        const auto info = read_type(type);
        UINT32 min_n = 0, min_d = 1, max_n = 0, max_d = 1;
        MFGetAttributeRatio(type, MF_MT_FRAME_RATE_RANGE_MIN, &min_n, &min_d);
        MFGetAttributeRatio(type, MF_MT_FRAME_RATE_RANGE_MAX, &max_n, &max_d);
        type->Release();
        const auto kind = kind_of_fourcc(info.fourcc);
        depth = depth || kind == StreamKind::Depth;
        color = color || kind == StreamKind::Color;
        infrared = infrared || kind == StreamKind::Infrared;
        auto add = [&](std::uint32_t fps) {
            if (info.width == 0 || info.height == 0 || fps == 0) return;
            VideoMode mode;
            mode.width = info.width;
            mode.height = info.height;
            mode.fourcc = info.fourcc;
            mode.fps = fps;
            mode.bytes_per_pixel = bytes_per_pixel_of(info.fourcc);
            node.modes.push_back(mode);
        };
        add(info.fps);
        const auto slow = min_d ? min_n / min_d : 0;
        const auto fast = max_d ? max_n / max_d : 0;
        if (slow && slow != info.fps) add(slow);
        if (fast && fast != info.fps) add(fast);
    }
    if (depth) node.kind = StreamKind::Depth;
    else if (color) node.kind = StreamKind::Color;
    else if (infrared) node.kind = StreamKind::Infrared;
    return node;
}

}  // namespace

bool is_v4l2_capture_node(const std::string& path) {
    if (path.empty()) return false;
    try {
        return !match_sources(path).empty();
    } catch (...) {
        return false;
    }
}

StreamReport capture(const CaptureRequest& request) {
    auto report = detail::capture_parallel(request, [](const CaptureRequest& req, const std::string& path, detail::Gate& configured,
                                                       detail::Gate& measuring) { return capture_one(req, path, configured, measuring); });
    report.notes.push_back("Windows 用 Media Foundation 读样本字节。队列深度参数不作用到采集缓冲。");
    return report;
}

std::vector<NodeModes> describe_capture_nodes(const std::vector<std::string>& paths) {
    std::vector<NodeModes> nodes;
    for (const auto& path : paths) {
        NodeModes node;
        node.path = path;
        if (!is_v4l2_capture_node(path)) {
            node.error = "不是视频采集设备";
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

std::vector<std::string> capture_nodes_for_usb(std::uint16_t vendor, std::uint16_t product) {
    std::vector<std::string> nodes;
    try {
        for (const auto& device : enumerate_sources()) {
            if (device.vendor == vendor && device.product == product) nodes.push_back(device.link);
        }
    } catch (...) {
    }
    return nodes;
}

}  // namespace linkbench
