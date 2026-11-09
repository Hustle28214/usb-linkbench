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
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::string bus_id;
    std::uint8_t address = 0;
    std::optional<std::string> manufacturer;
    std::optional<std::string> product;
    std::optional<std::string> serial;
    std::string usb_bcd;
    LinkSpeed speed = LinkSpeed::Unknown;
    std::string speed_source;
    std::optional<double> line_mbit;
    std::optional<std::string> family;
    std::optional<std::string> catalog_name;
    bool has_video_class = false;
    std::optional<std::string> sysfs_path;
    std::vector<std::string> video_nodes;
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
    std::uint8_t interface = 0;
    std::uint8_t endpoint = 0;
    double seconds = 2;
    std::size_t buffer_bytes = 64 * 1024;
    std::uint32_t warmup = 4;
    std::uint64_t timeout_ms = 500;
    bool detach = false;
};

struct BulkReport {
    DeviceRecord device;
    std::uint8_t interface = 0;
    std::uint8_t endpoint = 0;
    std::uint32_t queue_depth = 4;
    bool detached_kernel_driver = false;
    std::uint32_t warmup_discarded = 0;
    std::uint64_t reads = 0;
    std::uint64_t bytes = 0;
    std::uint64_t elapsed_ns = 0;
    double bytes_per_sec = 0;
    Summary timer_overhead_ns;
    std::vector<std::string> notes;
};

ListReport list_devices(const DeviceFilter& filter);
AuditReport audit_one(const DeviceFilter& filter);
RttReport control_rtt(const DeviceFilter& filter, const RttOptions& options);
BulkReport bulk_in(const DeviceFilter& filter, const BulkOptions& options);
std::vector<std::string> video_nodes_for(const DeviceFilter& filter);

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

StreamReport capture(const CaptureRequest& request);
bool verdict_failed(const StreamReport& report);

struct StorageOptions {
    std::string path;
    double seconds = 5;
    std::size_t block_bytes = 1024 * 1024;
    std::uint64_t warmup_bytes = 4ull << 20;
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
};

StorageReport read_direct(const StorageOptions& options);

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
std::string text_storage(const StorageReport& report);
std::string text_links(const LinksReport& report);
std::string text_budget(const BudgetReport& report);
std::string text_timing(const TimingReport& report);

std::string json_list(const ListReport& report);
std::string json_rtt(const RttReport& report);
std::string json_stream(const StreamReport& report);
std::string json_bulk(const BulkReport& report);
std::string json_storage(const StorageReport& report);
std::string json_budget(const BudgetReport& report);
std::string json_timing(const TimingReport& report);
std::string json_audit(const AuditReport& report);
std::string json_links(const LinksReport& report);

}  // namespace linkbench
