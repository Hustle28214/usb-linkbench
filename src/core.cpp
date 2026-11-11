#include "linkbench.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

#if defined(__unix__)
#include <time.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace linkbench {
namespace {

std::uint64_t rank_percentile(const std::vector<std::uint64_t>& sorted, int percent) {
    const auto n = sorted.size();
    auto rank = (static_cast<std::size_t>(percent) * n + 99) / 100;
    if (rank < 1) {
        rank = 1;
    }
    return sorted[rank - 1];
}

}  // namespace

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, int percent) {
    return rank_percentile(sorted, percent);
}

std::optional<Summary> summarize(const std::vector<std::uint64_t>& samples) {
    if (samples.empty()) {
        return std::nullopt;
    }
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    Summary out;
    out.count = sorted.size();
    out.min = sorted.front();
    out.max = sorted.back();
    out.p50 = rank_percentile(sorted, 50);
    out.p95 = rank_percentile(sorted, 95);
    out.p99 = rank_percentile(sorted, 99);
    long double sum = 0;
    for (auto value : sorted) {
        sum += value;
    }
    out.mean = static_cast<double>(sum / sorted.size());
    if (sorted.size() > 1) {
        long double var = 0;
        for (auto value : samples) {
            const auto d = static_cast<long double>(value) - out.mean;
            var += d * d;
        }
        var /= static_cast<long double>(sorted.size() - 1);
        out.stdev = std::sqrt(static_cast<double>(var));
    }
    return out;
}

std::string fmt_us(std::uint64_t ns) {
    if (ns < 1000) {
        return std::to_string(ns) + " ns";
    }
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << (static_cast<double>(ns) / 1000.0) << " µs";
    return os.str();
}

std::string fmt_us_f(double ns) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    if (ns < 1000.0) {
        os.precision(0);
        os << ns << " ns";
    } else {
        os.precision(1);
        os << (ns / 1000.0) << " µs";
    }
    return os.str();
}

std::uint64_t now_ns() {
#if defined(__unix__)
    timespec ts{};
    if (clock_gettime(4, &ts) != 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);// 类UNIX系统才奏效，WIN系统用不了，且如果改用QueryPerformanceCounter语义上有一点区别。
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#elif defined(_WIN32)
    static const auto freq = [] {
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        return static_cast<std::uint64_t>(hz.QuadPart);
    }();
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    const auto ticks = static_cast<std::uint64_t>(count.QuadPart);
    return ticks / freq * 1000000000ull + (ticks % freq) * 1000000000ull / freq;
#else
    return 0;
#endif
}

std::uint64_t elapsed_ns(std::uint64_t start) { return now_ns() - start; }

std::vector<std::uint64_t> overhead_samples(std::size_t iters) {
    std::vector<std::uint64_t> out;
    out.reserve(iters);
    for (std::size_t i = 0; i < iters; ++i) {
        const auto a = now_ns();
        const auto b = now_ns();
        out.push_back(b - a);
    }
    return out;
}

TimingReport timing_check() {
    constexpr auto requested = 50'000'000ull;
    const auto started = now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto measured = elapsed_ns(started);
    TimingReport report;
    report.timer_overhead_ns = *summarize(overhead_samples(200));
    report.sleep_requested_ns = requested;
    report.sleep_measured_ns = measured;
    report.sleep_ok = measured >= 20'000'000ull && measured <= 1'000'000'000ull;
    return report;
}

const char* speed_name(LinkSpeed speed) {
    switch (speed) {
        case LinkSpeed::Low:
            return "low-speed 1.5 Mbit/s";
        case LinkSpeed::Full:
            return "full-speed 12 Mbit/s";
        case LinkSpeed::High:
            return "high-speed 480 Mbit/s";
        case LinkSpeed::Super:
            return "super-speed 5 Gbit/s";
        case LinkSpeed::SuperPlus10:
            return "super-speed-plus 10 Gbit/s";
        case LinkSpeed::SuperPlus20:
            return "super-speed-plus 20 Gbit/s";
        case LinkSpeed::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::optional<double> line_mbit_per_sec(LinkSpeed speed) {
    switch (speed) {
        case LinkSpeed::Low:
            return 1.5;
        case LinkSpeed::Full:
            return 12;
        case LinkSpeed::High:
            return 480;
        case LinkSpeed::Super:
            return 5000;
        case LinkSpeed::SuperPlus10:
            return 10000;
        case LinkSpeed::SuperPlus20:
            return 20000;
        case LinkSpeed::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> line_coding_payload_bytes_per_sec(LinkSpeed speed) {
    switch (speed) {
        case LinkSpeed::Low:
            return 1'500'000 / 8;
        case LinkSpeed::Full:
            return 12'000'000 / 8;
        case LinkSpeed::High:
            return 480'000'000 / 8;
        case LinkSpeed::Super:
            return 500'000'000;
        case LinkSpeed::SuperPlus10:
            return static_cast<std::uint64_t>(10'000'000'000ull * 128 / 132 / 8);
        case LinkSpeed::SuperPlus20:
            return static_cast<std::uint64_t>(20'000'000'000ull * 128 / 132 / 8);
        case LinkSpeed::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> default_one_isoc_endpoint_bytes_per_sec(LinkSpeed speed) {
    switch (speed) {
        case LinkSpeed::Full:
            return 1'023'000;
        case LinkSpeed::High:
            return 1024ull * 3 * 8000;
        case LinkSpeed::Super:
        case LinkSpeed::SuperPlus10:
        case LinkSpeed::SuperPlus20:
            return 1024ull * 16 * 3 * 8000;
        default:
            return std::nullopt;
    }
}

std::optional<LinkSpeed> speed_from_sysfs(const std::string& text) {
    const auto value = text;
    if (value == "1.5") return LinkSpeed::Low;
    if (value == "12") return LinkSpeed::Full;
    if (value == "480") return LinkSpeed::High;
    if (value == "5000") return LinkSpeed::Super;
    if (value == "10000") return LinkSpeed::SuperPlus10;
    if (value == "20000") return LinkSpeed::SuperPlus20;
    return std::nullopt;
}

LinkSpeed speed_from_mbps(double mbps) {
    if (mbps >= 20000) return LinkSpeed::SuperPlus20;
    if (mbps >= 10000) return LinkSpeed::SuperPlus10;
    if (mbps >= 5000) return LinkSpeed::Super;
    if (mbps >= 480) return LinkSpeed::High;
    if (mbps >= 12) return LinkSpeed::Full;
    if (mbps > 0) return LinkSpeed::Low;
    return LinkSpeed::Unknown;
}

UsbStandard usb_standard(LinkSpeed speed) {
    switch (speed) {
        case LinkSpeed::Super:
            return {"USB 3.0", true};
        case LinkSpeed::SuperPlus10:
            return {"USB 3.1", true};
        case LinkSpeed::SuperPlus20:
            return {"USB 3.2", true};
        case LinkSpeed::High:
            return {"USB 2.0", false};
        case LinkSpeed::Full:
            return {"USB 1.1", false};
        case LinkSpeed::Low:
            return {"低速", false};
        case LinkSpeed::Unknown:
            return {"未知", false};
    }
    return {"未知", false};
}

std::optional<LinkSpeed> speed_from_name(const std::string& text) {
    std::string lower = text;
    for (auto& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower == "1.5" || lower == "low") return LinkSpeed::Low;
    if (lower == "12" || lower == "full") return LinkSpeed::Full;
    if (lower == "480" || lower == "high" || lower == "usb2") return LinkSpeed::High;
    if (lower == "5000" || lower == "super" || lower == "usb3" || lower == "ss") return LinkSpeed::Super;
    if (lower == "10000" || lower == "superplus" || lower == "superplus10" || lower == "ssp")
        return LinkSpeed::SuperPlus10;
    if (lower == "20000" || lower == "superplus20") return LinkSpeed::SuperPlus20;
    return std::nullopt;
}

const char* transfer_name(TransferKind kind) {
    switch (kind) {
        case TransferKind::Control:
            return "control";
        case TransferKind::Isochronous:
            return "isochronous";
        case TransferKind::Bulk:
            return "bulk";
        case TransferKind::Interrupt:
            return "interrupt";
    }
    return "control";
}

const char* fit_name(Fit fit) {
    switch (fit) {
        case Fit::Fits:
            return "fits";
        case Fit::Tight:
            return "tight";
        case Fit::Exceeds:
            return "exceeds";
        case Fit::Unknown:
            return "unknown";
    }
    return "unknown";
}

Fit fit_payload(std::uint64_t payload, std::optional<std::uint64_t> ceiling) {
    if (!ceiling || *ceiling == 0) return Fit::Unknown;
    if (payload > *ceiling) return Fit::Exceeds;
    if (payload * 10 > *ceiling * 8) return Fit::Tight;
    return Fit::Fits;
}

std::optional<std::uint64_t> periodic_ceiling_bytes_per_sec(const PeriodicEndpoint& endpoint) {
    if (endpoint.max_packet == 0 || endpoint.interval == 0 || endpoint.kind == TransferKind::Bulk ||
        endpoint.kind == TransferKind::Control) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> services;
    switch (endpoint.speed) {
        case LinkSpeed::Low:
            if (endpoint.kind != TransferKind::Interrupt) return std::nullopt;
            services = 1000 / endpoint.interval;
            break;
        case LinkSpeed::Full:
            services = 1000 / endpoint.interval;
            break;
        case LinkSpeed::High:
        case LinkSpeed::Super:
        case LinkSpeed::SuperPlus10:
        case LinkSpeed::SuperPlus20: {
            const auto shift = static_cast<unsigned>(endpoint.interval - 1);
            if (shift >= 16) return std::nullopt;
            services = 8000ull / (1ull << shift);
            break;
        }
        case LinkSpeed::Unknown:
            return std::nullopt;
    }
    std::uint64_t per_service = 0;
    const bool superspeed = endpoint.speed == LinkSpeed::Super || endpoint.speed == LinkSpeed::SuperPlus10 ||
                            endpoint.speed == LinkSpeed::SuperPlus20;
    if (superspeed) {
        if (endpoint.bytes_per_interval && *endpoint.bytes_per_interval > 0) {
            per_service = *endpoint.bytes_per_interval;
        } else {
            const auto burst = std::max<std::uint32_t>(endpoint.burst_packets.value_or(1), 1);
            const auto mult = endpoint.kind == TransferKind::Isochronous
                                  ? std::max<std::uint32_t>(endpoint.mult_packets.value_or(1), 1)
                                  : 1;
            per_service = static_cast<std::uint64_t>(endpoint.max_packet) * burst * mult;
        }
    } else if (endpoint.speed == LinkSpeed::High && endpoint.kind == TransferKind::Isochronous) {
        per_service = static_cast<std::uint64_t>(endpoint.max_packet) *
                      std::max<std::uint8_t>(endpoint.packets_per_microframe, 1);
    } else {
        per_service = endpoint.max_packet;
    }
    return per_service * *services;
}

std::uint64_t ModeBudget::payload_bytes_per_frame() const {
    return static_cast<std::uint64_t>(width) * height * bytes_per_pixel;
}

std::uint64_t ModeBudget::payload_bytes_per_sec() const { return payload_bytes_per_frame() * fps; }

const std::vector<ModeBudget>& presets() {
    static const std::vector<ModeBudget> all = {
        {"d435-depth-848x480-z16-90", "realsense", "D435/D435i 深度 848x480 Z16 90 fps", 848, 480, 2, 90, "Z16 "},
        {"d435-depth-1280x720-z16-30", "realsense", "D435/D435i 深度 1280x720 Z16 30 fps", 1280, 720, 2, 30, "Z16 "},
        {"d435-depth-640x480-z16-90", "realsense", "D435/D435i 深度 640x480 Z16 90 fps", 640, 480, 2, 90, "Z16 "},
        {"d435-color-1280x720-yuyv-30", "realsense", "D435/D435i 彩色 1280x720 YUYV 30 fps", 1280, 720, 2, 30, "YUYV"},
        {"d435-color-1920x1080-yuyv-30", "realsense", "D435/D435i 彩色 1920x1080 YUYV 30 fps", 1920, 1080, 2, 30,
         "YUYV"},
        {"d435-ir-1280x720-y8-30", "realsense", "D435/D435i 红外 1280x720 Y8 30 fps", 1280, 720, 1, 30, "GREY"},
        {"d455-depth-848x480-z16-90", "realsense", "D455 深度 848x480 Z16 90 fps", 848, 480, 2, 90, "Z16 "},
        {"gemini2-depth-1280x800-y16-30", "orbbec", "Gemini 2 深度 1280x800 Y16 30 fps", 1280, 800, 2, 30, "Y16 "},
        {"gemini2-color-1280x720-yuyv-30", "orbbec", "Gemini 2 彩色 1280x720 YUYV 30 fps", 1280, 720, 2, 30, "YUYV"},
        {"gemini305-depth-640x480-z16-30", "orbbec", "Gemini 305 深度 640x480 Z16 30 fps", 640, 480, 2, 30, "Z16 "},
        {"gemini305-depth-1280x800-y16-30", "orbbec", "Gemini 305 深度 1280x800 Y16 30 fps", 1280, 800, 2, 30, "Y16 "},
        {"astra-color-640x480-yuyv-30", "orbbec", "Astra 彩色 640x480 YUYV 30 fps", 640, 480, 2, 30, "YUYV"},
    };
    return all;
}

const ModeBudget* find_preset(const std::string& id) {
    for (const auto& mode : presets()) {
        if (id == mode.id) return &mode;
    }
    return nullptr;
}

LinkPlan plan_against(LinkSpeed speed, const std::vector<std::uint64_t>& payloads) {
    std::uint64_t aggregate = 0;
    std::uint64_t largest = 0;
    for (auto value : payloads) {
        aggregate += value;
        largest = std::max(largest, value);
    }
    LinkPlan plan;
    plan.speed = speed;
    plan.line_mbit = line_mbit_per_sec(speed);
    plan.line_coding_payload = line_coding_payload_bytes_per_sec(speed);
    plan.one_isoc = default_one_isoc_endpoint_bytes_per_sec(speed);
    plan.aggregate_vs_bus = fit_payload(aggregate, plan.line_coding_payload);
    plan.largest_vs_endpoint = fit_payload(largest, plan.one_isoc);
    return plan;
}

BudgetReport budget_report(std::vector<std::string> labels, const std::vector<std::uint64_t>& payloads,
                           std::optional<LinkSpeed> highlight) {
    BudgetReport report;
    report.labels = std::move(labels);
    for (auto value : payloads) report.aggregate_bytes_per_sec += value;
    for (auto speed : {LinkSpeed::Full, LinkSpeed::High, LinkSpeed::Super, LinkSpeed::SuperPlus10,
                       LinkSpeed::SuperPlus20}) {
        report.plans.push_back(plan_against(speed, payloads));
    }
    if (highlight) report.highlighted = plan_against(*highlight, payloads);
    return report;
}

std::string fmt_bytes_per_sec(double bytes_per_sec) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << (bytes_per_sec / (1024.0 * 1024.0)) << " MiB/s (";
    os.precision(1);
    os << (bytes_per_sec * 8.0 / 1'000'000.0) << " Mbit/s)";
    return os.str();
}

std::string fmt_bytes_per_sec_u(std::uint64_t bytes_per_sec) {
    return fmt_bytes_per_sec(static_cast<double>(bytes_per_sec));
}

namespace {

constexpr std::uint16_t kOrbbec = 0x2BC5;
constexpr std::uint16_t kIntel = 0x8086;

const std::pair<std::uint16_t, const char*> kRealSense[] = {
    {0x0AD1, "D400"}, {0x0AD2, "D410"}, {0x0AD3, "D415"}, {0x0AD4, "D430"}, {0x0AD5, "D430"},
    {0x0AD6, "D420"}, {0x0AF6, "D420"}, {0x0AFE, "D410"}, {0x0AFF, "D400"}, {0x0B00, "D430"},
    {0x0B01, "D460"}, {0x0B03, "D435"}, {0x0B07, "D435"}, {0x0B0C, "D405"}, {0x0B3A, "D435i"},
    {0x0B5B, "D405"}, {0x0B5C, "D455"}, {0x0B64, "L515"},
};

}  // namespace

std::optional<CameraIdentity> identify_camera(std::uint16_t vendor, std::uint16_t product,
                                              const std::optional<std::string>& product_string) {
    if (vendor == kOrbbec) return CameraIdentity{"orbbec", std::nullopt};
    bool string_match = false;
    if (product_string) {
        std::string lower = *product_string;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        string_match = lower.find("realsense") != std::string::npos;
    }
    std::optional<std::string> catalog;
    for (const auto& item : kRealSense) {
        if (item.first == product) catalog = item.second;
    }
    if (vendor == kIntel && (string_match || catalog)) return CameraIdentity{"realsense", catalog};
    return std::nullopt;
}

const char* class_name(std::uint8_t class_code, std::uint8_t subclass) {
    switch (class_code) {
        case 0x00:
            return "use-interface";
        case 0x01:
            return "audio";
        case 0x02:
            return subclass == 0x02 ? "cdc-acm" : "cdc-control";
        case 0x03:
            return "hid";
        case 0x08:
            return "mass-storage";
        case 0x09:
            return "hub";
        case 0x0A:
            return "cdc-data";
        case 0x0E:
            return subclass == 0x01 ? "video-control" : subclass == 0x02 ? "video-streaming" : "video";
        case 0x0B:
            return "billboard-or-misc";
        case 0xEF:
            return "miscellaneous";
        case 0xFF:
            return "vendor";
        default:
            return "other";
    }
}

bool DeviceFilter::unrestricted() const {
    return !vendor_id && !product_id && !bus_id && !address && !cameras_only;
}

}  // namespace linkbench
