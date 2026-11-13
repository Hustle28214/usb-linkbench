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
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace linkbench {
namespace {

constexpr const char kStreamMarker[] = "::mfstream=";

struct VideoDevice {
    std::string link;
    std::string name;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
};

struct NodeRef {
    std::string link;
    std::optional<DWORD> stream;
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

NodeRef parse_node(const std::string& path) {
    NodeRef ref;
    const auto at = path.rfind(kStreamMarker);
    if (at == std::string::npos) {
        ref.link = path;
        return ref;
    }
    ref.link = path.substr(0, at);
    try {
        ref.stream = static_cast<DWORD>(std::stoul(path.substr(at + sizeof(kStreamMarker) - 1)));
    } catch (...) {
        ref.link = path;
        ref.stream.reset();
    }
    return ref;
}

std::string make_node(const std::string& link, DWORD stream) {
    return link + kStreamMarker + std::to_string(stream);
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
    const auto ref = parse_node(path);
    const auto all = enumerate_sources();
    std::vector<VideoDevice> exact;
    std::vector<VideoDevice> partial;
    for (const auto& device : all) {
        if (device.link == ref.link || device.name == ref.link) exact.push_back(device);
        else if (device.link.find(ref.link) != std::string::npos ||
                 (!device.name.empty() && device.name.find(ref.link) != std::string::npos)) {
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

Reader open_reader(const std::string& link_or_path) {
    const auto ref = parse_node(link_or_path);
    auto matches = match_sources(ref.link);
    if (matches.empty()) throw Error("没有这个视频设备: " + ref.link);
    if (matches.size() != 1) throw Error("视频节点匹配到 " + std::to_string(matches.size()) + " 个设备，请用 list 打出的完整链接");
    ComApartment apartment;
    ensure_media();
    // 不要用 SYMBOLIC_LINK 过滤 MFEnumDeviceSources：那样常会落到 Frame Server，
    // RealSense 只剩一路 NV12，Z16/多流原生类型消失。改为全量枚举再按链接匹配。
    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) throw Error("创建 Media Foundation 属性失败");
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    attrs->Release();
    if (FAILED(hr) || count == 0 || !devices) throw Error("打开视频设备失败: " + ref.link);

    IMFActivate* chosen = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        const auto link = allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        if (link == matches.front().link) {
            chosen = devices[i];
            break;
        }
    }
    if (!chosen) {
        for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);
        throw Error("打开视频设备失败: " + ref.link);
    }

    Reader reader;
    reader.name = matches.front().name.empty() ? matches.front().link : matches.front().name;
    hr = chosen->ActivateObject(IID_IMFMediaSource, reinterpret_cast<void**>(&reader.source));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (FAILED(hr) || !reader.source) throw Error("激活视频设备失败: " + hr_text(hr));

    IMFAttributes* reader_attrs = nullptr;
    if (FAILED(MFCreateAttributes(&reader_attrs, 2)) || !reader_attrs) throw Error("创建读取器属性失败");
    reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    reader_attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(reader.source, reader_attrs, &reader.reader);
    reader_attrs->Release();
    if (FAILED(hr) || !reader.reader) throw Error("创建视频读取器失败: " + hr_text(hr));
    return reader;
}

std::vector<DWORD> list_video_streams(IMFMediaSource* source, IMFSourceReader* reader) {
    std::vector<DWORD> out;
    // RealSense 等设备默认只选中第一路；未选中时 GetNativeMediaType 可能直接失败，必须先全选。
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), TRUE);
    for (DWORD index = 0; index < 32; ++index) {
        IMFMediaType* type = nullptr;
        const HRESULT hr = reader->GetNativeMediaType(index, 0, &type);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (hr == MF_E_NO_MORE_TYPES) continue;
        if (FAILED(hr)) continue;
        GUID major{};
        const bool video = SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) && major == MFMediaType_Video;
        type->Release();
        if (video) out.push_back(index);
    }
    if (!out.empty()) return out;
    if (!source) return out;
    IMFPresentationDescriptor* pd = nullptr;
    if (FAILED(source->CreatePresentationDescriptor(&pd)) || !pd) return out;
    DWORD count = 0;
    pd->GetStreamDescriptorCount(&count);
    for (DWORD index = 0; index < count; ++index) {
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = nullptr;
        if (FAILED(pd->GetStreamDescriptorByIndex(index, &selected, &sd)) || !sd) continue;
        IMFMediaTypeHandler* handler = nullptr;
        GUID major{};
        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)) && handler) {
            if (SUCCEEDED(handler->GetMajorType(&major)) && major == MFMediaType_Video) out.push_back(index);
            handler->Release();
        }
        sd->Release();
    }
    pd->Release();
    return out;
}

DWORD resolve_stream_index(IMFMediaSource* source, IMFSourceReader* reader, const NodeRef& ref) {
    const auto streams = list_video_streams(source, reader);
    if (streams.empty()) throw Error("设备没有视频流");
    if (!ref.stream) return streams.front();
    for (const auto index : streams) {
        if (index == *ref.stream) return index;
    }
    throw Error("没有视频流 " + std::to_string(*ref.stream));
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

void apply_format(IMFSourceReader* reader, DWORD stream, const detail::ResolvedFormat& resolved) {
    if (!resolved.width && !resolved.height && !resolved.fourcc && !resolved.fps) return;
    const auto want = canon_fourcc(resolved.fourcc.value_or(""));
    IMFMediaType* chosen = nullptr;
    for (DWORD index = 0;; ++index) {
        IMFMediaType* type = nullptr;
        const HRESULT hr = reader->GetNativeMediaType(stream, index, &type);
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
    const HRESULT hr = reader->SetCurrentMediaType(stream, nullptr, copy);
    copy->Release();
    if (FAILED(hr)) throw Error(std::string("设置格式失败: ") + hr_text(hr));
}

struct Sample {
    std::uint64_t bytes = 0;
    std::optional<std::int64_t> timestamp;
    bool error = false;
    bool empty = false;
};

struct StreamSlot {
    std::string path;
    DWORD index = 0;
    detail::ResolvedFormat resolved;
    TypeInfo format;
    std::uint64_t frames = 0;
    std::uint64_t error_frames = 0;
    std::uint64_t empty_frames = 0;
    std::uint64_t dropped = 0;
    std::uint64_t bytes = 0;
    std::uint64_t warmup_left = 0;
    std::optional<std::int64_t> last_ts;
    std::optional<std::int64_t> first_ts;
    std::uint64_t timestamped = 0;
    bool stalled = false;
};

void select_only(IMFSourceReader* reader, const std::vector<DWORD>& indices) {
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    for (const auto index : indices) {
        const HRESULT hr = reader->SetStreamSelection(index, TRUE);
        if (FAILED(hr)) throw Error("选择视频流失败: " + hr_text(hr));
    }
}

FrameStream finish_slot(const StreamSlot& slot, const CaptureRequest& request, std::uint64_t elapsed, const std::string& card) {
    const auto elapsed_s = static_cast<double>(std::max<std::uint64_t>(elapsed, 1)) / 1e9;
    const auto payload_frames = slot.frames - std::min(slot.frames, slot.error_frames + slot.empty_frames);
    const double mean = payload_frames == 0 ? 0 : static_cast<double>(slot.bytes) / static_cast<double>(payload_frames);
    FrameStream stream;
    stream.node = slot.path;
    stream.driver = "Media Foundation";
    stream.card = card;
    stream.width = slot.format.width;
    stream.height = slot.format.height;
    stream.fourcc = slot.format.fourcc;
    stream.sizeimage = slot.format.sizeimage;
    stream.buffer_count = request.buffers;
    stream.warmup_frames = request.warmup_frames;
    stream.frames = slot.frames;
    stream.error_frames = slot.error_frames;
    stream.empty_frames = slot.empty_frames;
    stream.dropped_frames = slot.dropped;
    stream.bytes = slot.bytes;
    stream.elapsed_ns = elapsed;
    stream.window_fps = static_cast<double>(slot.frames) / elapsed_s;
    stream.window_bytes_per_sec = static_cast<double>(slot.bytes) / elapsed_s;
    stream.mean_frame_bytes = mean;
    stream.interval_source = slot.timestamped > 0 ? "sample_time" : "userspace_read";
    if (slot.first_ts && slot.last_ts && slot.timestamped >= 2 && *slot.last_ts > *slot.first_ts) {
        stream.driver_fps = static_cast<double>(slot.timestamped - 1) * 1e7 / static_cast<double>(*slot.last_ts - *slot.first_ts);
    }
    stream.stalled = slot.stalled;
    stream.role = slot.resolved.role;
    stream.verdict = detail::build_verdict(stream.window_fps, mean, slot.resolved.min_fps, slot.resolved.expect_frame_bytes);
    return stream;
}

void account_sample(StreamSlot* slot, const Sample& sample) {
    if (!sample.timestamp && sample.bytes == 0 && !sample.error && !sample.empty) return;
    ++slot->frames;
    if (sample.error) {
        ++slot->error_frames;
        return;
    }
    if (sample.empty || sample.bytes == 0) {
        ++slot->empty_frames;
        return;
    }
    const std::uint32_t fps = slot->resolved.fps.value_or(slot->format.fps);
    if (sample.timestamp) {
        if (slot->first_ts && slot->last_ts && fps > 0 && *sample.timestamp > *slot->last_ts) {
            const auto expected = 10'000'000ll / static_cast<std::int64_t>(fps);
            if (expected > 0) {
                const auto gap = (*sample.timestamp - *slot->last_ts) / expected;
                if (gap > 1 && gap < 1000) slot->dropped += static_cast<std::uint64_t>(gap - 1);
            }
        }
        if (!slot->first_ts) slot->first_ts = sample.timestamp;
        slot->last_ts = sample.timestamp;
        ++slot->timestamped;
    }
    slot->bytes += sample.bytes;
}

std::vector<FrameStream> capture_device(const CaptureRequest& request, const std::string& link, const std::vector<std::string>& paths,
                                        detail::Gate& configured, detail::Gate& measuring) {
    ComApartment apartment;
    auto opened = open_reader(link);
    std::vector<StreamSlot> slots;
    slots.reserve(paths.size());
    for (const auto& path : paths) {
        StreamSlot slot;
        slot.path = path;
        slot.index = resolve_stream_index(opened.source, opened.reader, parse_node(path));
        slot.resolved = detail::resolve_format(request, path);
        slot.warmup_left = request.warmup_frames;
        slots.push_back(std::move(slot));
    }

    std::vector<DWORD> indices;
    for (const auto& slot : slots) indices.push_back(slot.index);
    select_only(opened.reader, indices);
    for (auto& slot : slots) {
        apply_format(opened.reader, slot.index, slot.resolved);
        IMFMediaType* current = nullptr;
        if (SUCCEEDED(opened.reader->GetCurrentMediaType(slot.index, &current)) && current) {
            slot.format = read_type(current);
            current->Release();
        }
    }
    if (!configured.arrive()) throw Error("其他视频节点还没设好格式");

    auto find_slot = [&](DWORD index) -> StreamSlot* {
        for (auto& slot : slots) {
            if (slot.index == index) return &slot;
        }
        return nullptr;
    };

    while (true) {
        bool need = false;
        for (const auto& slot : slots) need = need || slot.warmup_left > 0;
        if (!need) break;
        DWORD actual = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* raw = nullptr;
        const auto started = now_ns();
        const HRESULT hr = opened.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &actual, &flags, &timestamp, &raw);
        if (now_ns() - started > 2'000'000'000ull) {
            if (raw) raw->Release();
            throw Error("预热取帧超时");
        }
        if (FAILED(hr)) {
            if (raw) raw->Release();
            throw Error(std::string("预热取帧失败: ") + hr_text(hr));
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (raw) raw->Release();
            throw Error("预热时视频流结束");
        }
        auto* slot = find_slot(actual);
        if (!slot || (flags & MF_SOURCE_READERF_STREAMTICK) || !raw) {
            if (raw) raw->Release();
            continue;
        }
        if (slot->warmup_left > 0) --slot->warmup_left;
        raw->Release();
    }
    if (!measuring.arrive()) throw Error("其他视频节点预热失败");

    const auto limit = static_cast<std::uint64_t>(request.seconds * 1e9);
    const auto started = now_ns();
    while (elapsed_ns(started) < limit) {
        DWORD actual = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* raw = nullptr;
        const auto read_started = now_ns();
        const HRESULT hr =
            opened.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &actual, &flags, &timestamp, &raw);
        if (now_ns() - read_started > 2'000'000'000ull) {
            if (raw) raw->Release();
            for (auto& slot : slots) slot.stalled = true;
            break;
        }
        if (FAILED(hr)) {
            if (raw) raw->Release();
            auto* slot = find_slot(actual);
            if (slot) {
                Sample sample;
                sample.error = true;
                account_sample(slot, sample);
            }
            continue;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (raw) raw->Release();
            for (auto& slot : slots) slot.stalled = true;
            break;
        }
        auto* slot = find_slot(actual);
        if (!slot) {
            if (raw) raw->Release();
            continue;
        }
        Sample sample;
        if (flags & MF_SOURCE_READERF_ERROR) sample.error = true;
        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            if (raw) raw->Release();
            continue;
        }
        if (!raw) {
            sample.empty = true;
            account_sample(slot, sample);
            continue;
        }
        sample.timestamp = timestamp;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(raw->ConvertToContiguousBuffer(&buffer)) && buffer) {
            DWORD length = 0;
            buffer->GetCurrentLength(&length);
            sample.bytes = length;
            buffer->Release();
        }
        raw->Release();
        if (sample.bytes == 0) sample.empty = true;
        account_sample(slot, sample);
    }

    const auto elapsed = std::max<std::uint64_t>(elapsed_ns(started), 1);
    std::vector<FrameStream> streams;
    streams.reserve(slots.size());
    const auto card_base = opened.name;
    for (const auto& slot : slots) {
        auto card = card_base;
        if (slots.size() > 1) card += " 流" + std::to_string(slot.index);
        streams.push_back(finish_slot(slot, request, elapsed, card));
    }
    return streams;
}

NodeModes probe_node(const std::string& path) {
    ComApartment apartment;
    auto opened = open_reader(path);
    const auto ref = parse_node(path);
    const auto stream = resolve_stream_index(opened.source, opened.reader, ref);
    select_only(opened.reader, {stream});
    NodeModes node;
    node.path = path;
    node.card = opened.name;
    if (ref.stream) node.card += " 流" + std::to_string(stream);
    bool depth = false;
    bool color = false;
    bool infrared = false;
    for (DWORD index = 0;; ++index) {
        IMFMediaType* type = nullptr;
        const HRESULT hr = opened.reader->GetNativeMediaType(stream, index, &type);
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

std::vector<std::string> expand_device_nodes(const VideoDevice& device) {
    try {
        auto opened = open_reader(device.link);
        const auto streams = list_video_streams(opened.source, opened.reader);
        if (streams.empty()) return {device.link};
        if (streams.size() == 1) return {make_node(device.link, streams.front())};
        std::vector<std::string> nodes;
        for (const auto index : streams) nodes.push_back(make_node(device.link, index));
        return nodes;
    } catch (...) {
        return {device.link};
    }
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
    if (request.paths.empty()) throw Error("没有视频节点");
    std::map<std::string, std::vector<std::string>> groups;
    std::vector<SkippedNode> skipped;
    for (const auto& path : request.paths) {
        if (!is_v4l2_capture_node(path)) {
            skipped.push_back(SkippedNode{path, "不是视频采集设备"});
            continue;
        }
        groups[parse_node(path).link].push_back(path);
    }
    if (groups.empty()) {
        std::string message;
        for (const auto& item : skipped) {
            if (!message.empty()) message += "; ";
            message += item.node + ": " + item.reason;
        }
        throw Error(message.empty() ? "没有可采集的视频节点" : message);
    }

    auto configured = std::make_shared<detail::Gate>();
    auto measuring = std::make_shared<detail::Gate>();
    configured->total = static_cast<int>(groups.size());
    measuring->total = static_cast<int>(groups.size());
    std::mutex result_mutex;
    std::vector<FrameStream> streams;
    std::vector<std::thread> threads;
    for (const auto& group : groups) {
        threads.emplace_back([&, link = group.first, paths = group.second] {
            try {
                auto got = capture_device(request, link, paths, *configured, *measuring);
                std::lock_guard lock(result_mutex);
                streams.insert(streams.end(), std::make_move_iterator(got.begin()), std::make_move_iterator(got.end()));
            } catch (const std::exception& err) {
                configured->fail();
                measuring->fail();
                std::lock_guard lock(result_mutex);
                for (const auto& path : paths) skipped.push_back(SkippedNode{path, err.what()});
            }
        });
    }
    for (auto& thread : threads) thread.join();

    if (request.strict_nodes) {
        std::size_t expected = 0;
        for (const auto& path : request.paths) {
            if (is_v4l2_capture_node(path)) ++expected;
        }
        if (streams.size() != expected) {
            std::string message;
            for (const auto& item : skipped) {
                if (item.reason.find("不是视频采集") != std::string::npos) continue;
                if (!message.empty()) message += "; ";
                message += item.node + ": " + item.reason;
            }
            throw Error(message.empty() ? "采集节点未全部成功" : message);
        }
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
    report.notes.push_back("Windows 用 Media Foundation 读样本字节。同一设备上的多路视频在一个读取器里同时开流。队列深度参数不作用到采集缓冲。");
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
        ComApartment apartment;
        ensure_media();
        for (const auto& device : enumerate_sources()) {
            if (device.vendor != vendor || device.product != product) continue;
            auto expanded = expand_device_nodes(device);
            nodes.insert(nodes.end(), expanded.begin(), expanded.end());
        }
    } catch (...) {
    }
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    return nodes;
}

}  // namespace linkbench
