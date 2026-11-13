#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace linkbench {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Summary {
    std::size_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t max = 0;
    double mean = 0;
    double stdev = 0;
};

std::optional<Summary> summarize(const std::vector<std::uint64_t>& samples);
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, int percent);
std::string fmt_us(std::uint64_t ns);
std::string fmt_us_f(double ns);

std::uint64_t now_ns();
std::uint64_t elapsed_ns(std::uint64_t start);
std::vector<std::uint64_t> overhead_samples(std::size_t iters);

struct TimingReport {
    Summary timer_overhead_ns;
    std::uint64_t sleep_requested_ns = 0;
    std::uint64_t sleep_measured_ns = 0;
    bool sleep_ok = false;
    std::vector<std::string> notes;
};

TimingReport timing_check();

enum class LinkSpeed { Low, Full, High, Super, SuperPlus10, SuperPlus20, Unknown };

const char* speed_name(LinkSpeed speed);
std::optional<double> line_mbit_per_sec(LinkSpeed speed);
std::optional<std::uint64_t> line_coding_payload_bytes_per_sec(LinkSpeed speed);
std::optional<std::uint64_t> default_one_isoc_endpoint_bytes_per_sec(LinkSpeed speed);
std::optional<LinkSpeed> speed_from_sysfs(const std::string& text);
LinkSpeed speed_from_mbps(double mbps);
std::optional<LinkSpeed> speed_from_name(const std::string& text);

struct UsbStandard {
    const char* name; // 协商速率对应的世代，例如 USB 3.0
    bool meets_usb3 = false; // 5 Gbit/s 及以上算符合 USB 3.0
};

UsbStandard usb_standard(LinkSpeed speed);
// 用这次测到的 Mbit/s 判断。高于 USB 2.0 线速 480 Mbit/s 即符合 USB 3.0。10/20 Gbit/s 再标更高世代。
UsbStandard standard_from_measured_mbit(double mbit_per_sec);
std::optional<LinkSpeed> live_link_speed(std::uint16_t vendor, std::uint16_t product, const std::string& bus_id,
                                        std::uint8_t address);

enum class TransferKind { Control, Isochronous, Bulk, Interrupt };
const char* transfer_name(TransferKind kind);

enum class Fit { Fits, Tight, Exceeds, Unknown };
const char* fit_name(Fit fit);
Fit fit_payload(std::uint64_t payload_bytes_per_sec, std::optional<std::uint64_t> ceiling);

struct PeriodicEndpoint {
    LinkSpeed speed = LinkSpeed::Unknown;
    TransferKind kind = TransferKind::Bulk;
    std::uint32_t max_packet = 0;
    std::uint8_t packets_per_microframe = 1;
    std::uint8_t interval = 0;
    std::optional<std::uint32_t> bytes_per_interval;
    std::optional<std::uint32_t> burst_packets;
    std::optional<std::uint32_t> mult_packets;
};

std::optional<std::uint64_t> periodic_ceiling_bytes_per_sec(const PeriodicEndpoint& endpoint);

struct ModeBudget {
    const char* id;
    const char* family;
    const char* summary;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bytes_per_pixel;
    std::uint32_t fps;
    const char* fourcc;
    std::uint64_t payload_bytes_per_frame() const;
    std::uint64_t payload_bytes_per_sec() const;
};

const std::vector<ModeBudget>& presets();
const ModeBudget* find_preset(const std::string& id);

struct LinkPlan {
    LinkSpeed speed = LinkSpeed::Unknown;
    std::optional<double> line_mbit;
    std::optional<std::uint64_t> line_coding_payload;
    std::optional<std::uint64_t> one_isoc;
    Fit aggregate_vs_bus = Fit::Unknown;
    Fit largest_vs_endpoint = Fit::Unknown;
};

struct BudgetReport {
    std::vector<std::string> labels;
    std::uint64_t aggregate_bytes_per_sec = 0;
    std::vector<LinkPlan> plans;
    std::optional<LinkPlan> highlighted;
    std::vector<std::string> notes;
};

BudgetReport budget_report(std::vector<std::string> labels, const std::vector<std::uint64_t>& payloads,
                           std::optional<LinkSpeed> highlight);
std::string fmt_bytes_per_sec(double bytes_per_sec);
std::string fmt_bytes_per_sec_u(std::uint64_t bytes_per_sec);

struct CameraIdentity {
    std::string family;
    std::optional<std::string> catalog_name;
};

std::optional<CameraIdentity> identify_camera(std::uint16_t vendor, std::uint16_t product,
                                              const std::optional<std::string>& product_string);
const char* class_name(std::uint8_t class_code, std::uint8_t subclass);

struct DeviceFilter {
    std::optional<std::uint16_t> vendor_id;
    std::optional<std::uint16_t> product_id;
    std::optional<std::string> bus_id;
    std::optional<std::uint8_t> address;
    bool cameras_only = false;
    bool unrestricted() const;
};

struct EndpointRecord {
    std::uint8_t address = 0;
    std::string direction;
    TransferKind transfer = TransferKind::Control;
    std::uint32_t max_packet = 0;
    std::uint8_t packets_per_microframe = 1;
    std::uint8_t interval = 0;
    std::optional<std::uint32_t> burst_packets;
    std::optional<std::uint32_t> mult_packets;
    std::optional<std::uint32_t> bytes_per_interval;
    std::optional<std::uint64_t> periodic_ceiling_bytes_per_sec;
};

struct AltRecord {
    std::uint8_t alternate = 0;
    std::string class_name;
    std::vector<EndpointRecord> endpoints;
};

struct InterfaceRecord {
    std::uint8_t number = 0;
    std::vector<AltRecord> alternates;
};

struct ConfigRecord {
    std::uint8_t value = 0;
    bool active = false;
    std::vector<InterfaceRecord> interfaces;
};

struct DeviceRecord {
    std::uint16_t vendor_id = 0; // 16 位厂商 ID
    std::uint16_t product_id = 0; // 厂商自定义产品 ID
    std::string bus_id; // USB 总线号
    std::uint8_t address = 0; // 设备地址
    std::optional<std::string> manufacturer; // 厂商名
    std::optional<std::string> product; // 产品名
    std::optional<std::string> serial; // 序列号
    std::string usb_bcd; // 设备声称兼容的 USB 规范版本
    LinkSpeed speed = LinkSpeed::Unknown; // 当前协商档：LS 1.5 / FS 12 / HS 480 / SS 5G / SSP 10G/20G
    std::string speed_source; // 数据来源
    std::optional<double> line_mbit; // 该档标称线速（Mbit/s）
    std::optional<std::string> family; // 项目内相机识别表，非协议字段
    std::optional<std::string> catalog_name;
    bool has_video_class = false; // 任一接口 bInterfaceClass == 0x0E（Video，UVC）
    std::optional<std::string> sysfs_path; // Linux 设备节点路径，便于继续读接口/端点
    std::vector<std::string> video_nodes; // 内核 V4L2 节点（/dev/video*），不是 USB 描述符
};

struct ListReport {
    std::vector<DeviceRecord> devices;
    std::vector<std::string> notes;
};

struct AuditReport {
    DeviceRecord device;
    std::vector<ConfigRecord> configurations;
    std::optional<std::uint64_t> max_periodic_ceiling_bytes_per_sec;
    std::vector<std::string> notes;
};
// RTT： 往返时延
struct RttOptions {
    std::uint32_t count = 100;
    std::uint32_t warmup = 20;
    std::uint64_t timeout_ms = 200;
    bool keep_samples = false;
};

struct RttReport {
    DeviceRecord device;
    std::uint32_t warmup_discarded = 0;
    std::uint64_t timeout_ms = 0;
    Summary samples_ns;
    Summary timer_overhead_ns;
    std::optional<std::vector<std::uint64_t>> sample_ns;
    std::uint32_t errors = 0;
    std::vector<std::string> notes;
};

struct BulkOptions {
    std::uint8_t interface = 0; // 要占用的接口号
    std::uint8_t endpoint = 0; // Bulk IN 端点地址
    double seconds = 2; // 正式计时窗口
    std::size_t buffer_bytes = 64 * 1024; // 每次传输缓冲区大小
    std::uint32_t warmup = 4; // 计时前先做几次传输并丢弃
    std::uint64_t timeout_ms = 500; // 单次 libusb 传输超时
    bool detach = false; // 是否先卸载该接口上的内核驱动再 claim
};

struct BulkReport {
    DeviceRecord device; // 被测设备身份
    std::uint8_t interface = 0; // 实际用的接口
    std::uint8_t endpoint = 0; // 实际用的端点
    std::uint32_t queue_depth = 4; // 同时在途的传输数（固定 4）
    bool detached_kernel_driver = false; // 是否卸过内核驱动
    std::uint32_t warmup_discarded = 0; // 丢弃的预热次数
    std::uint64_t reads = 0; // 计时窗内完成的传输次数
    std::uint64_t bytes = 0; // 总读取字节数
    std::uint64_t elapsed_ns = 0; // 实测墙钟
    double bytes_per_sec = 0; // 吞吐 = bytes / 秒
    Summary timer_overhead_ns; // 计时器开销统计
    std::vector<std::string> notes; // 说明文字
};

ListReport list_devices(const DeviceFilter& filter);
AuditReport audit_one(const DeviceFilter& filter);
RttReport control_rtt(const DeviceFilter& filter, const RttOptions& options);
BulkReport bulk_in(const DeviceFilter& filter, const BulkOptions& options);
std::vector<std::string> video_nodes_for(const DeviceFilter& filter);

enum class StreamKind { Unknown, Color, Depth, Infrared };

struct VideoMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string fourcc;
    std::uint32_t fps = 0;
    std::uint32_t bytes_per_pixel = 0;
    std::uint64_t payload_bytes_per_sec() const;
};

struct NodeFormat {
    std::string path;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::string> fourcc;
    std::optional<std::uint32_t> fps;
    std::optional<double> min_fps;
    std::optional<std::uint64_t> expect_frame_bytes;
    std::string role; // "color" 或 "depth"；空表示不标注
};

struct CaptureRequest {
    std::vector<std::string> paths;
    double seconds = 5;
    std::uint32_t warmup_frames = 30;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::string> fourcc;
    std::optional<std::uint32_t> fps;
    std::optional<double> min_fps;
    std::optional<std::uint64_t> expect_frame_bytes;
    bool strict_nodes = false;
    std::uint32_t buffers = 4;
    std::vector<NodeFormat> node_formats; // 按节点覆盖上面的统一格式
    std::vector<std::string> notes;
};

struct StreamVerdict {
    std::optional<double> min_fps;
    double achieved_fps = 0;
    std::optional<bool> fps_ok;
    std::optional<std::uint64_t> expected_frame_bytes;
    double mean_frame_bytes = 0;
    std::optional<bool> frame_bytes_ok;
};

struct FrameStream {
    std::string node;
    std::string driver;
    std::string card;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string fourcc;
    std::uint32_t sizeimage = 0;
    std::uint32_t buffer_count = 0;
    std::uint32_t warmup_frames = 0;
    std::uint64_t frames = 0;
    std::uint64_t error_frames = 0;
    std::uint64_t empty_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t elapsed_ns = 0;
    double window_fps = 0;
    double window_bytes_per_sec = 0;
    double mean_frame_bytes = 0;
    std::string interval_source;
    std::optional<double> driver_fps;
    bool stalled = false;
    std::optional<StreamVerdict> verdict;
    std::string role;
};

struct SkippedNode {
    std::string node;
    std::string reason;
};

struct StreamReport {
    std::vector<FrameStream> streams;
    std::vector<SkippedNode> skipped;
    double aggregate_bytes_per_sec = 0;
    Summary timer_overhead_ns;
    std::vector<std::string> notes;
};

struct NodeModes {
    std::string path;
    std::string card;
    StreamKind kind = StreamKind::Unknown;
    std::vector<VideoMode> modes;
    std::string error;
};

struct ColorDepthPlan {
    CaptureRequest request;
    std::string color_node;
    std::string depth_node;
    VideoMode color_mode;
    VideoMode depth_mode;
    std::uint64_t depth_budget_bytes_per_sec = 0;
};

bool is_v4l2_capture_node(const std::string& path);
StreamReport capture(const CaptureRequest& request);
bool verdict_failed(const StreamReport& report);
StreamKind kind_of_fourcc(const std::string& fourcc);
std::uint32_t bytes_per_pixel_of(const std::string& fourcc);
VideoMode pick_depth_mode(const std::vector<VideoMode>& modes, std::uint32_t fps = 0);
VideoMode pick_color_mode(const std::vector<VideoMode>& modes, std::uint32_t depth_fps, std::uint32_t depth_width = 0,
                           std::uint32_t depth_height = 0);
std::vector<NodeModes> describe_capture_nodes(const std::vector<std::string>& paths);
ColorDepthPlan plan_color_depth(const std::vector<NodeModes>& nodes, std::uint32_t fps = 0);

struct StreamPassSample {
    double bytes_per_sec = 0;// 吞吐（字节/秒）
    double fps = 0; //帧率
    std::uint64_t frames = 0;// 采到的帧数
    std::uint64_t dropped_frames = 0; // 丢帧
    std::uint64_t error_frames = 0; // 错误帧
    std::uint64_t empty_frames = 0; // 空帧
    bool stalled = false; // 是否卡住/停滞
};

struct StreamBenchPlan {
    std::string id; // id,title 方案标识和显示名
    std::string title; 
    double seconds = 5; // 每轮测多久
    std::uint32_t passes = 3; // 跑几轮
    double interval_seconds = 0; // 轮与轮之间休息多久
    std::uint32_t buffers = 4; // V4L2 mmap 缓冲队列深度
    bool keep_maximum = true; // true 取最高吞吐，false 取最低
    bool fail_on_stream_errors = false; // 是否因丢帧/错误/停滞判定失败
    double max_drop_ratio = 0.01; // 允许的最大丢帧比例
    std::optional<double> min_fps; // 可选最低帧率门槛
};

struct StreamBenchScore {
    std::uint32_t pass_index = 0; // 被选中的那一轮编号（从 1 起）。keep_maximum 时选吞吐最高轮，否则选最低轮
    double bytes_per_sec = 0; // 只取选中那一轮的吞吐。下面的是帧率
    double fps = 0; //
    std::uint64_t frames = 0; // 有轮次累加的帧统计
    std::uint64_t dropped_frames = 0; 
    std::uint64_t error_frames = 0; // 错误帧的轮数
    std::uint64_t empty_frames = 0; // 空帧的轮数
    std::uint32_t stalled_passes = 0; // 停滞的轮数
    double drop_ratio = 0; // 总丢帧率
    bool failed = false; // 是否判定失败
    std::string reason; // 失败原因
};

struct StreamBenchReport {
    StreamBenchPlan plan;
    std::vector<StreamReport> passes;
    StreamBenchScore score;
    std::vector<std::string> notes;
};

StreamBenchPlan stream_bench_plan(const std::string& name);
StreamBenchScore score_stream_passes(const StreamBenchPlan& plan, const std::vector<StreamPassSample>& passes);
StreamBenchReport benchmark_stream(const CaptureRequest& request, const StreamBenchPlan& plan);

struct StorageOptions {
    std::string path; // 要测的文件或设备路径
    double seconds = 5; // 正式计时多久
    std::size_t block_bytes = 1024 * 1024; // 每次读的块大小
    std::uint64_t warmup_bytes = 4ull << 20; // 计时前预热读取量，这个不计入结果
};

struct StorageLink {
    bool usb = false; // 这块盘挂在 USB 设备上
    LinkSpeed speed = LinkSpeed::Unknown; // sysfs 协商速率
    std::string product;
    std::string location; // 形如 2-1
    std::string detail; // 找不到 USB 设备时的原因
};

struct StorageReport {
    std::string path;
    bool direct = true;
    std::uint64_t block_bytes = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t warmup_bytes = 0;
    std::uint64_t bytes = 0;
    std::uint64_t elapsed_ns = 0;
    double bytes_per_sec = 0;
    std::uint64_t reads = 0;
    std::uint64_t wraps = 0;
    Summary timer_overhead_ns;
    std::vector<std::string> notes;
    StorageLink link;
};

StorageReport read_direct(const StorageOptions& options);

std::optional<std::string> mount_majmin(const std::string& mountinfo, const std::string& path);
StorageLink storage_usb(const std::string& path);

struct ThunderboltPort {
    std::string path;
    std::optional<std::string> name;
    std::optional<std::string> vendor;
    std::optional<std::string> generation;
    std::optional<double> tx_speed_gbps;
    std::optional<double> rx_speed_gbps;
    std::optional<std::uint32_t> tx_lanes;
    std::optional<std::uint32_t> rx_lanes;
    std::optional<std::string> authorized;
};

struct PcieFunction {
    std::string address;
    std::optional<std::string> current_speed;
    std::optional<std::string> max_speed;
    std::optional<std::uint32_t> current_width;
    std::optional<std::uint32_t> max_width;
    bool degraded = false;
};

struct LinksReport {
    std::vector<ThunderboltPort> thunderbolt;
    std::optional<std::string> thunderbolt_skip_reason;
    std::vector<PcieFunction> pcie_degraded;
    std::vector<std::pair<std::string, std::size_t>> pcie_speed_counts;
    std::size_t pcie_functions_with_link = 0;
    std::vector<PcieFunction> pcie_all;
    std::vector<std::string> notes;
};

LinksReport links(bool verbose);

enum class RailRole { ExternalInput, PhoneInput, Output, Unknown };
RailRole role_from_supply_type(const std::string& kind);
RailRole role_from_label(const std::string& sensor, const std::string& label);

struct PowerRail {
    std::string name;
    RailRole role = RailRole::Unknown;
    std::optional<double> voltage_v;
    std::optional<double> current_a;
    std::optional<double> power_w;
};

struct PowerSection {
    std::optional<double> external_input_w;
    std::optional<double> phone_input_w;
    std::optional<double> output_w;
    std::optional<double> external_efficiency;
    std::optional<double> phone_efficiency;
    std::vector<PowerRail> rails;
    std::vector<std::string> notes;
};

PowerSection power_section(std::vector<PowerRail> rails);

struct UsbEndpoint {
    std::uint8_t address = 0;
    std::string direction;
    std::string transfer;
    std::uint16_t max_packet = 0;
    std::uint8_t transactions = 1;
    std::string interval;
};

struct UsbInterface {
    std::uint8_t number = 0;
    std::string class_name;
    std::uint8_t subclass = 0;
    std::uint8_t protocol = 0;
    std::optional<std::string> hid_kind;
    std::optional<std::string> driver;
    std::optional<std::string> tty;
    std::vector<std::string> hidraw;
    std::vector<UsbEndpoint> endpoints;
};

struct UsbPort {
    std::string location;
    std::optional<std::string> parent;
    std::optional<double> parent_speed_mbps;
    double speed_mbps = 0;
    std::string version;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::optional<std::string> manufacturer;
    std::optional<std::string> product;
    std::uint8_t device_class = 0;
    bool self_powered = false;
    bool remote_wakeup = false;
    std::optional<std::uint32_t> max_power_ma;
    std::optional<std::uint32_t> downstream_ports;
    std::optional<std::uint32_t> tx_lanes;
    std::optional<std::uint32_t> rx_lanes;
    std::optional<bool> ltm_capable;
    std::optional<std::string> removable;
    std::optional<std::uint16_t> control_max_packet;
    std::vector<UsbInterface> interfaces;
};

struct CanPort {
    std::string name;
    std::optional<std::string> state;
    std::optional<std::uint32_t> bitrate;
    std::optional<std::uint32_t> clock_hz;
    std::optional<std::string> driver;
};

struct BenchReport {
    std::string collected_at;
    std::string hostname;
    std::string kernel;
    std::vector<UsbPort> host_controllers;
    std::vector<UsbPort> upstream_usb3;
    std::vector<UsbPort> downstream_usb3;
    std::vector<UsbPort> downstream_hid;
    std::vector<UsbPort> downstream_serial;
    std::vector<CanPort> can;
    PowerSection power;
    std::vector<UsbPort> other;
    std::vector<std::string> notes;
};

struct RawDevice {
    UsbPort port;
    std::uint32_t busnum = 0;
    bool is_root = false;
};

BenchReport assemble(std::vector<RawDevice> raws, std::vector<CanPort> can, std::vector<PowerRail> rails,
                     std::string collected_at, std::string hostname, std::string kernel);
std::string format_port_report(const BenchReport& report);
std::pair<std::uint16_t, std::uint8_t> decode_packet(std::uint16_t raw, double speed_mbps);
const char* hid_kind(std::uint8_t subclass, std::uint8_t protocol);
std::string test_item_catalog();
std::string collect_port_report();

struct RunRequest {
    DeviceFilter filter;
    bool all_matches = false;
    double seconds = 5;
    std::uint32_t warmup_frames = 30;
    std::uint32_t rtt_count = 100;
    std::uint32_t rtt_warmup = 20;
    std::vector<const ModeBudget*> presets;
    bool strict = false;
};

struct RunOutcome {
    std::string text;
    bool checks_failed = false;
};

RunOutcome run_camera(const RunRequest& request);

std::string text_list(const ListReport& report);
std::string text_audit(const AuditReport& report);
std::string text_rtt(const RttReport& report);
std::string text_bulk(const BulkReport& report);
std::string text_stream(const StreamReport& report);
std::string text_stream_bench(const StreamBenchReport& report);
std::string text_storage(const StorageReport& report);
std::string text_links(const LinksReport& report);
std::string text_budget(const BudgetReport& report);
std::string text_timing(const TimingReport& report);

std::string json_list(const ListReport& report);
std::string json_rtt(const RttReport& report);
std::string json_stream(const StreamReport& report);
std::string json_stream_bench(const StreamBenchReport& report);
std::string json_bulk(const BulkReport& report);
std::string json_storage(const StorageReport& report);
std::string json_budget(const BudgetReport& report);
std::string json_timing(const TimingReport& report);
std::string json_audit(const AuditReport& report);
std::string json_links(const LinksReport& report);

}  // namespace linkbench
