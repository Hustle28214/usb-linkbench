#include "linkbench.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

int g_exit = 0;

void fail(const std::string& message) {
    std::cerr << "错误: " << message << "\n";
    g_exit = 1;
}

std::uint16_t parse_hex16(const std::string& text) {
    auto hex = text;
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) hex = hex.substr(2);
    try {
        return static_cast<std::uint16_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
        throw linkbench::Error("无法解析十六进制数值 " + text);
    }
}

std::uint8_t parse_hex8(const std::string& text) {
    return static_cast<std::uint8_t>(parse_hex16(text));
}

struct Argv {
    std::vector<std::string> args;
    bool json = false;
    explicit Argv(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--json") json = true;
            else args.push_back(std::move(arg));
        }
    }
    bool flag(const std::string& name) {
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (*it == name) {
                args.erase(it);
                return true;
            }
        }
        return false;
    }
    std::optional<std::string> option(const std::string& name) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == name && i + 1 < args.size()) {
                auto value = args[i + 1];
                args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i + 2));
                return value;
            }
            if (args[i].rfind(name + "=", 0) == 0) {
                auto value = args[i].substr(name.size() + 1);
                args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
                return value;
            }
        }
        return std::nullopt;
    }
    std::vector<std::string> repeat(const std::string& name) {
        std::vector<std::string> values;
        for (;;) {
            auto value = option(name);
            if (!value) break;
            values.push_back(*value);
        }
        return values;
    }
    double number(const std::string& name, double fallback) {
        auto value = option(name);
        if (!value) return fallback;
        return std::stod(*value);
    }
    std::uint32_t u32(const std::string& name, std::uint32_t fallback) {
        auto value = option(name);
        if (!value) return fallback;
        return static_cast<std::uint32_t>(std::stoul(*value));
    }
};

linkbench::DeviceFilter filter_from(Argv& argv) {
    linkbench::DeviceFilter filter;
    if (auto vid = argv.option("--vid")) filter.vendor_id = parse_hex16(*vid);
    if (auto pid = argv.option("--pid")) filter.product_id = parse_hex16(*pid);
    if (auto bus = argv.option("--bus")) filter.bus_id = *bus;
    if (auto addr = argv.option("--addr")) filter.address = static_cast<std::uint8_t>(std::stoul(*addr));
    filter.cameras_only = argv.flag("--cameras");
    return filter;
}

void emit(bool json, const std::string& text, const std::string& json_text) {
    std::cout << (json ? json_text : text);
    if (!json && (text.empty() || text.back() != '\n')) std::cout << "\n";
    if (json) std::cout << "\n";
}

void write_text(const std::optional<std::string>& path, const std::string& text) {
    if (!path) return;
    std::filesystem::path file(*path);
    if (file.has_parent_path() && !file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out) throw linkbench::Error("无法写入 " + *path);
    out << text;
}

std::vector<const linkbench::ModeBudget*> load_presets(const std::vector<std::string>& ids) {
    std::vector<const linkbench::ModeBudget*> modes;
    for (const auto& id : ids) {
        const auto* mode = linkbench::find_preset(id);
        if (!mode) throw linkbench::Error("未知预设 " + id);
        modes.push_back(mode);
    }
    return modes;
}

std::string preset_names() {
    std::string out;
    for (const auto& mode : linkbench::presets()) {
        if (!out.empty()) out += "\n";
        out += std::string(mode.id) + "  " + mode.summary;
    }
    return out;
}

void usage() {
    std::cout <<
        "usb-linkbench 0.1.0\n"
        "相机 USB 载荷、控制回传、高速链路协商，以及端口与供电报告\n\n"
        "用法: usb-linkbench [--json] <命令> [选项]\n"
        "  list [--vid --pid --bus --addr --cameras]\n"
        "  audit [过滤]\n"
        "  rtt [过滤] [--count 100 --warmup 20 --timeout-ms 200 --samples]\n"
        "  stream [--device PATH] [--color-depth] [--profile quick|stress] [--seconds --passes --interval --buffers --max-drop-ratio]\n"
        "         [--warmup-frames 30 --width --height --fourcc --fps --min-fps --expect-frame-bytes --preset]\n"
        "  budget [--preset --width --height --bpp --fps --speed]\n"
        "  storage --path PATH [--seconds 5 --block-kib 1024 --warmup-mib 4]\n"
        "  bulk [过滤] --interface N --endpoint 0x81 [--seconds 2 --buf-kib 64 --warmup 4 --timeout-ms 500 --detach]\n"
        "  links [--verbose]\n"
        "  timing-check\n"
        "  report [--collect] [--out FILE]\n"
        "  run [过滤] [--seconds --warmup-frames --rtt-count --rtt-warmup --preset --strict --all]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Argv cli(argc, argv);
        if (cli.args.empty() || cli.flag("--help") || cli.flag("-h")) {
            usage();
            return 0;
        }
        if (cli.flag("--version")) {
            std::cout << "usb-linkbench 0.1.0\n";
            return 0;
        }
        const auto command = cli.args.front();
        cli.args.erase(cli.args.begin());
        if (command == "list") {
            auto report = linkbench::list_devices(filter_from(cli));
            emit(cli.json, linkbench::text_list(report), linkbench::json_list(report));
        } else if (command == "audit") {
            auto report = linkbench::audit_one(filter_from(cli));
            emit(cli.json, linkbench::text_audit(report), linkbench::json_audit(report));
        } else if (command == "rtt") {
            linkbench::RttOptions options;
            options.count = cli.u32("--count", 100);
            options.warmup = cli.u32("--warmup", 20);
            options.timeout_ms = cli.u32("--timeout-ms", 200);
            options.keep_samples = cli.flag("--samples");
            auto report = linkbench::control_rtt(filter_from(cli), options);
            emit(cli.json, linkbench::text_rtt(report), linkbench::json_rtt(report));
        } else if (command == "stream") {
            auto devices = cli.repeat("--device");
            auto presets = load_presets(cli.repeat("--preset"));
            const bool explicit_nodes = !devices.empty();
            const bool color_depth = cli.flag("--color-depth");
            auto profile_name = cli.option("--profile");
            std::optional<linkbench::StreamBenchPlan> plan;
            if (profile_name) plan = linkbench::stream_bench_plan(*profile_name);
            linkbench::CaptureRequest request;
            if (auto seconds = cli.option("--seconds")) {
                const auto value = std::stod(*seconds);
                request.seconds = value;
                if (plan) plan->seconds = value;
            } else if (plan) {
                request.seconds = plan->seconds;
            } else {
                request.seconds = 5;
            }
            if (plan) {
                if (auto passes = cli.option("--passes")) plan->passes = static_cast<std::uint32_t>(std::stoul(*passes));
                if (auto interval = cli.option("--interval")) plan->interval_seconds = std::stod(*interval);
                if (auto buffers = cli.option("--buffers")) plan->buffers = static_cast<std::uint32_t>(std::stoul(*buffers));
                if (auto ratio = cli.option("--max-drop-ratio")) plan->max_drop_ratio = std::stod(*ratio);
                request.buffers = plan->buffers;
            } else if (auto buffers = cli.option("--buffers")) {
                request.buffers = static_cast<std::uint32_t>(std::stoul(*buffers));
            }
            request.warmup_frames = cli.u32("--warmup-frames", 30);
            if (auto width = cli.option("--width")) request.width = static_cast<std::uint32_t>(std::stoul(*width));
            if (auto height = cli.option("--height")) request.height = static_cast<std::uint32_t>(std::stoul(*height));
            if (auto fourcc = cli.option("--fourcc")) request.fourcc = *fourcc;
            if (auto fps = cli.option("--fps")) request.fps = static_cast<std::uint32_t>(std::stoul(*fps));
            if (auto min_fps = cli.option("--min-fps")) request.min_fps = std::stod(*min_fps);
            if (auto bytes = cli.option("--expect-frame-bytes")) request.expect_frame_bytes = std::stoull(*bytes);
            const bool format_bits = request.width || request.height || request.fourcc;
            if (format_bits && !(request.width && request.height && request.fourcc)) {
                throw linkbench::Error("设置格式时 width、height、fourcc 要一起给");
            }
            if (color_depth) {
                if (request.width || request.height || request.fourcc || request.fps || !presets.empty()) {
                    throw linkbench::Error("--color-depth 会分别为彩色和深度设置格式，不要同时给 --width、--height、--fourcc、--fps 或 --preset");
                }
                if (!explicit_nodes) {
                    auto listed = linkbench::list_devices(filter_from(cli));
                    std::vector<linkbench::DeviceRecord> cameras;
                    for (const auto& device : listed.devices) {
                        if (!device.video_nodes.empty()) cameras.push_back(device);
                    }
                    if (cameras.empty()) throw linkbench::Error("没有带视频节点的相机");
                    if (cameras.size() > 1) {
                        throw linkbench::Error("有多台相机，--color-depth 请用 --vid、--bus 或 --addr 指定一台");
                    }
                    devices = cameras.front().video_nodes;
                } else {
                    filter_from(cli);
                }
                auto planned = linkbench::plan_color_depth(linkbench::describe_capture_nodes(devices));
                planned.request.seconds = request.seconds;
                planned.request.warmup_frames = request.warmup_frames;
                planned.request.buffers = request.buffers;
                request = std::move(planned.request);
            } else {
                if (devices.empty()) devices = linkbench::video_nodes_for(filter_from(cli));
                else filter_from(cli);
                if (devices.size() == 1 && presets.size() == 1) {
                    if (!request.min_fps) request.min_fps = static_cast<double>(presets[0]->fps) * 0.95;
                    if (!request.expect_frame_bytes) request.expect_frame_bytes = presets[0]->payload_bytes_per_frame();
                }
                request.paths = devices;
                request.strict_nodes = explicit_nodes;
            }
            if (plan) {
                auto report = linkbench::benchmark_stream(request, *plan);
                emit(cli.json, linkbench::text_stream_bench(report), linkbench::json_stream_bench(report));
                if (report.score.failed) g_exit = 2;
            } else {
                auto report = linkbench::capture(request);
                emit(cli.json, linkbench::text_stream(report), linkbench::json_stream(report));
                if (linkbench::verdict_failed(report)) g_exit = 2;
            }
        } else if (command == "budget") {
            auto presets = load_presets(cli.repeat("--preset"));
            std::vector<std::string> labels;
            std::vector<std::uint64_t> payloads;
            for (const auto* mode : presets) {
                labels.push_back(std::string(mode->id) + "  " + mode->summary);
                payloads.push_back(mode->payload_bytes_per_sec());
            }
            auto width = cli.option("--width");
            auto height = cli.option("--height");
            auto bpp = cli.option("--bpp");
            auto fps = cli.option("--fps");
            if (width && height && bpp && fps) {
                const auto w = std::stoul(*width);
                const auto h = std::stoul(*height);
                const auto b = std::stoul(*bpp);
                const auto f = std::stoul(*fps);
                labels.push_back("custom " + std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(b) + " @" +
                                 std::to_string(f) + "fps");
                payloads.push_back(static_cast<std::uint64_t>(w) * h * b * f);
            } else if (width || height || bpp || fps) {
                throw linkbench::Error("自定义预算需要同时给出 --width --height --bpp --fps");
            }
            if (payloads.empty()) {
                std::cout << preset_names() << "\n";
                return 0;
            }
            std::optional<linkbench::LinkSpeed> speed;
            if (auto text = cli.option("--speed")) {
                auto parsed = linkbench::speed_from_name(*text);
                if (!parsed) throw linkbench::Error("无法识别链路速率 " + *text);
                speed = *parsed;
            }
            auto report = linkbench::budget_report(labels, payloads, speed);
            emit(cli.json, linkbench::text_budget(report), linkbench::json_budget(report));
        } else if (command == "storage") {
            auto path = cli.option("--path");
            if (!path) throw linkbench::Error("storage 需要 --path");
            const auto block_kib = cli.u32("--block-kib", 1024);
            if (block_kib == 0 || block_kib % 4 != 0) throw linkbench::Error("block-kib 必须是 4 的正整数倍");
            linkbench::StorageOptions options;
            options.path = *path;
            options.seconds = cli.number("--seconds", 5);
            options.block_bytes = static_cast<std::size_t>(block_kib) * 1024;
            options.warmup_bytes = static_cast<std::uint64_t>(cli.u32("--warmup-mib", 4)) * 1024 * 1024;
            auto report = linkbench::read_direct(options);
            report.link = linkbench::storage_usb(options.path);
            emit(cli.json, linkbench::text_storage(report), linkbench::json_storage(report));
        } else if (command == "bulk") {
            linkbench::BulkOptions options;
            options.interface = static_cast<std::uint8_t>(cli.u32("--interface", 0));
            auto endpoint = cli.option("--endpoint");
            if (!endpoint) throw linkbench::Error("bulk 需要 --endpoint");
            options.endpoint = parse_hex8(*endpoint);
            options.seconds = cli.number("--seconds", 2);
            options.buffer_bytes = static_cast<std::size_t>(cli.u32("--buf-kib", 64)) * 1024;
            options.warmup = cli.u32("--warmup", 4);
            options.timeout_ms = cli.u32("--timeout-ms", 500);
            options.detach = cli.flag("--detach");
            auto report = linkbench::bulk_in(filter_from(cli), options);
            emit(cli.json, linkbench::text_bulk(report), linkbench::json_bulk(report));
        } else if (command == "links") {
            auto report = linkbench::links(cli.flag("--verbose"));
            emit(cli.json, linkbench::text_links(report), linkbench::json_links(report));
        } else if (command == "timing-check") {
            auto report = linkbench::timing_check();
            emit(cli.json, linkbench::text_timing(report), linkbench::json_timing(report));
            if (!report.sleep_ok) g_exit = 2;
        } else if (command == "report") {
            const bool collect = cli.flag("--collect");
            auto out = cli.option("--out");
            if (collect) {
                auto text = linkbench::collect_port_report();
                write_text(out, text);
                if (cli.json) {
                    std::string escaped;
                    for (unsigned char ch : text) {
                        if (ch == '"') escaped += "\\\"";
                        else if (ch == '\\') escaped += "\\\\";
                        else if (ch == '\n') escaped += "\\n";
                        else escaped.push_back(static_cast<char>(ch));
                    }
                    emit(true, text, "{\"text\":\"" + escaped + "\"}");
                } else {
                    emit(false, text, {});
                }
            } else if (cli.json) {
                throw linkbench::Error("测试项说明是文本。读取本机端口时再加 --collect 和 --json。");
            } else {
                auto text = linkbench::test_item_catalog();
                write_text(out, text);
                emit(false, text, {});
            }
        } else if (command == "run") {
            linkbench::RunRequest request;
            request.filter = filter_from(cli);
            if (request.filter.unrestricted()) {
                throw linkbench::Error("run 需要 --vid、--pid、--bus、--addr 或 --cameras，避免对所有 USB 设备做回传。");
            }
            request.seconds = cli.number("--seconds", 5);
            request.warmup_frames = cli.u32("--warmup-frames", 30);
            request.rtt_count = cli.u32("--rtt-count", 100);
            request.rtt_warmup = cli.u32("--rtt-warmup", 20);
            request.presets = load_presets(cli.repeat("--preset"));
            request.strict = cli.flag("--strict");
            request.all_matches = cli.flag("--all");
            auto outcome = linkbench::run_camera(request);
            emit(cli.json, outcome.text, std::string("{\"checks_failed\":") + (outcome.checks_failed ? "true" : "false") + "}");
            if (outcome.checks_failed) g_exit = 2;
        } else {
            throw linkbench::Error("未知命令 " + command);
        }
        if (!cli.args.empty()) throw linkbench::Error("无法识别的参数 " + cli.args.front());
    } catch (const std::exception& err) {
        fail(err.what());
    }
    return g_exit;
}
