#include "linkbench.hpp"

#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* expr, int line) {
    if (!ok) {
        std::cerr << "失败 " << line << ": " << expr << "\n";
        ++failures;
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __LINE__)

linkbench::UsbInterface iface(const char* class_name, const char* driver, const char* tty) {
    linkbench::UsbInterface item;
    item.class_name = class_name;
    if (std::string(class_name) == "hid") item.hid_kind = "boot keyboard";
    if (driver) item.driver = driver;
    if (tty) item.tty = tty;
    return item;
}

linkbench::RawDevice device(const char* name, std::uint32_t bus, double speed, std::uint8_t cls,
                            std::vector<linkbench::UsbInterface> interfaces) {
    linkbench::RawDevice raw;
    raw.busnum = bus;
    raw.is_root = std::string(name).rfind("usb", 0) == 0;
    raw.port.location = name;
    if (!raw.is_root) {
        std::string text = name;
        auto dot = text.rfind('.');
        raw.port.parent = dot == std::string::npos ? "usb" + std::to_string(bus) : text.substr(0, dot);
    }
    raw.port.speed_mbps = speed;
    raw.port.version = speed >= 5000 ? "3.20" : "2.00";
    raw.port.vendor_id = 0x05e3;
    raw.port.product_id = 0x0626;
    raw.port.manufacturer = "Example";
    raw.port.product = name;
    raw.port.device_class = cls;
    raw.port.self_powered = cls == 0x09;
    raw.port.remote_wakeup = true;
    raw.port.max_power_ma = cls == 0x09 ? 0 : 100;
    if (cls == 0x09) raw.port.downstream_ports = 4;
    raw.port.tx_lanes = 1;
    raw.port.rx_lanes = 1;
    raw.port.ltm_capable = false;
    raw.port.removable = "removable";
    raw.port.control_max_packet = speed >= 5000 ? 512 : 64;
    raw.port.interfaces = std::move(interfaces);
    return raw;
}

}  // namespace

int main() {
    CHECK(std::string(linkbench::usb_standard(linkbench::LinkSpeed::Super).name) == "USB 3.0");
    CHECK(linkbench::usb_standard(linkbench::LinkSpeed::Super).meets_usb3);
    CHECK(linkbench::usb_standard(linkbench::LinkSpeed::SuperPlus10).meets_usb3);
    CHECK(std::string(linkbench::usb_standard(linkbench::LinkSpeed::SuperPlus10).name) == "USB 3.1");
    CHECK(std::string(linkbench::usb_standard(linkbench::LinkSpeed::High).name) == "USB 2.0");
    CHECK(!linkbench::usb_standard(linkbench::LinkSpeed::High).meets_usb3);
    const char* mounts =
        "22 1 8:1 / /media/disk rw,relatime - vfat /dev/sdb1 rw\n"
        "23 1 259:2 / /media/disk/sub rw - ext4 /dev/nvme0n1p2 rw\n"
        "24 1 8:17 / /media/usb\\040stick rw - vfat /dev/sdc1 rw\n";
    const auto disk = linkbench::mount_majmin(mounts, "/media/disk/file");
    const auto nested = linkbench::mount_majmin(mounts, "/media/disk/sub/a");
    const auto spaced = linkbench::mount_majmin(mounts, "/media/usb stick/a.bin");
    CHECK(disk && *disk == "8:1");
    CHECK(nested && *nested == "259:2");
    CHECK(spaced && *spaced == "8:17");

    CHECK(linkbench::line_coding_payload_bytes_per_sec(linkbench::LinkSpeed::Super) == 500000000);
    CHECK(linkbench::default_one_isoc_endpoint_bytes_per_sec(linkbench::LinkSpeed::High) == 24576000);
    CHECK(linkbench::default_one_isoc_endpoint_bytes_per_sec(linkbench::LinkSpeed::Super) == 393216000);

    linkbench::PeriodicEndpoint hs;
    hs.speed = linkbench::LinkSpeed::High;
    hs.kind = linkbench::TransferKind::Isochronous;
    hs.max_packet = 1024;
    hs.packets_per_microframe = 3;
    hs.interval = 1;
    CHECK(linkbench::periodic_ceiling_bytes_per_sec(hs) == 24576000);

    linkbench::PeriodicEndpoint ss;
    ss.speed = linkbench::LinkSpeed::Super;
    ss.kind = linkbench::TransferKind::Isochronous;
    ss.max_packet = 1024;
    ss.interval = 1;
    ss.bytes_per_interval = 48 * 1024;
    ss.burst_packets = 16;
    ss.mult_packets = 3;
    CHECK(linkbench::periodic_ceiling_bytes_per_sec(ss) == 48 * 1024 * 8000);

    linkbench::PeriodicEndpoint bulk;
    bulk.speed = linkbench::LinkSpeed::Super;
    bulk.kind = linkbench::TransferKind::Bulk;
    bulk.max_packet = 1024;
    bulk.interval = 1;
    bulk.burst_packets = 16;
    CHECK(!linkbench::periodic_ceiling_bytes_per_sec(bulk));

    const auto* mode = linkbench::find_preset("d435-depth-848x480-z16-90");
    CHECK(mode);
    CHECK(mode->payload_bytes_per_frame() == 848u * 480 * 2);
    CHECK(mode->payload_bytes_per_sec() == 848ull * 480 * 2 * 90);
    const auto* color = linkbench::find_preset("d435-color-1920x1080-yuyv-30");
    CHECK(linkbench::fit_payload(color->payload_bytes_per_sec(),
                                 linkbench::default_one_isoc_endpoint_bytes_per_sec(linkbench::LinkSpeed::High)) ==
          linkbench::Fit::Exceeds);
    CHECK(linkbench::fit_payload(color->payload_bytes_per_sec(),
                                 linkbench::default_one_isoc_endpoint_bytes_per_sec(linkbench::LinkSpeed::Super)) ==
          linkbench::Fit::Fits);
    CHECK(linkbench::speed_from_sysfs("20000") == linkbench::LinkSpeed::SuperPlus20);
    CHECK(linkbench::speed_from_sysfs("10000") == linkbench::LinkSpeed::SuperPlus10);
    CHECK(linkbench::speed_from_sysfs("1.5") == linkbench::LinkSpeed::Low);

    auto orbbec = linkbench::identify_camera(0x2BC5, 0x0670, std::nullopt);
    CHECK(orbbec && orbbec->family == "orbbec");
    CHECK(!linkbench::identify_camera(0x8086, 0x2723, std::string("Wi-Fi 6 AX200")));
    auto d435i = linkbench::identify_camera(0x8086, 0x0B3A, std::nullopt);
    CHECK(d435i && d435i->catalog_name && *d435i->catalog_name == "D435i");
    auto named = linkbench::identify_camera(0x8086, 0x1111, std::string("Intel RealSense D999"));
    CHECK(named && named->family == "realsense");

    auto summary = linkbench::summarize({10, 20, 30, 40, 100});
    CHECK(summary && summary->p50 == 30 && summary->mean == 40 && summary->count == 5);
    CHECK(!linkbench::summarize({}));
    auto single = linkbench::summarize({42});
    CHECK(single && single->stdev == 0 && single->p99 == 42);

    const auto a = linkbench::now_ns();
    const auto b = linkbench::now_ns();
    CHECK(b >= a);
    auto overhead = linkbench::summarize(linkbench::overhead_samples(50));
    CHECK(overhead && overhead->p50 < 1000000);

    auto report = linkbench::assemble(
        {device("usb4", 4, 10000, 0x09, {}), device("4-1", 4, 5000, 0x09, {}), device("4-1.1", 4, 5000, 0x09, {}),
         device("4-1.1.2", 4, 5000, 0x00, {iface("cdc-data", "cdc_ncm", nullptr)}),
         device("3-1.1", 3, 12, 0x00, {iface("hid", nullptr, nullptr)}),
         device("3-2.1", 3, 12, 0x02, {iface("cdc-acm", "cdc_acm", "/dev/ttyACM0")})},
        {linkbench::CanPort{"can0", "up", 500000, 80000000, "can_kvaser"}},
        {linkbench::PowerRail{"adapter", linkbench::RailRole::ExternalInput, 20, 1, 20},
         linkbench::PowerRail{"phone", linkbench::RailRole::PhoneInput, 5, 1, 5},
         linkbench::PowerRail{"downstream", linkbench::RailRole::Output, 5, 0.8, 4}},
        "2026-11-06 13:00:00", "bench", "6.8.0");
    CHECK(report.upstream_usb3.size() == 1 && report.upstream_usb3[0].location == "4-1");
    CHECK(report.upstream_usb3[0].parent_speed_mbps && *report.upstream_usb3[0].parent_speed_mbps == 10000);
    CHECK(report.downstream_usb3.size() == 2);
    CHECK(report.downstream_hid.size() == 1);
    CHECK(report.downstream_serial.size() == 1);
    CHECK(report.downstream_serial[0].interfaces[0].tty && *report.downstream_serial[0].interfaces[0].tty == "/dev/ttyACM0");
    CHECK(report.can[0].bitrate && *report.can[0].bitrate == 500000);
    CHECK(report.power.external_efficiency && std::abs(*report.power.external_efficiency - 0.2) < 1e-9);
    CHECK(report.power.phone_efficiency && std::abs(*report.power.phone_efficiency - 0.8) < 1e-9);
    auto text = linkbench::format_port_report(report);
    CHECK(text.find("## 1. 上行口 USB 3.0") != std::string::npos);
    CHECK(text.find("### 2.2 USB 2.0 HID") != std::string::npos);
    CHECK(text.find("### 2.4 CAN") != std::string::npos);
    CHECK(text.find("80.0%") != std::string::npos);

    auto catalog = linkbench::test_item_catalog();
    for (const char* heading :
         {"## 0. 测试主机环境", "### 0.1 主机与操作系统", "### 0.2 USB 主机控制器", "### 0.3 软件与驱动",
          "### 0.4 当前接入的被测器件", "## 1. 上行口 USB 3.0", "### 2.1 USB 3.0", "### 2.2 USB 2.0 HID",
          "### 2.3 USB 2.0 串口", "### 2.4 CAN", "### 3.1 外供电", "### 3.2 手机供电", "### 3.3 电脑供电",
          "## 4. 相机视频载荷",
          "### 4.2 640×480 彩色与深度同时采集", "## 5. VUT 追踪器", "### 5.2 120 Hz 姿态速率与稳定性",
          "## 6. PEAK-CAN 手部接收", "### 6.2 60 Hz 融合上报接收", "## 7. 供电能力", "### 7.3 满负载实测",
          "## 8. 并发验收", "## 附录 A. 测试项含义", "2026-11-06", "复测", "bMaxPower", "波特率", "采样点", "效率", "融合上报轮次",
          "完整画面"}) {
        CHECK(catalog.find(heading) != std::string::npos);
    }

    auto missing = linkbench::format_port_report(linkbench::assemble({}, {}, {}, "", "", ""));
    CHECK(missing.find("未接入。没有 CDC ACM") != std::string::npos);
    CHECK(missing.find("未接入。sysfs 里没有 CAN") != std::string::npos);
    CHECK(missing.find("未计算") != std::string::npos);

    auto orphan = linkbench::power_section({linkbench::PowerRail{"orphan", linkbench::RailRole::ExternalInput, std::nullopt,
                                                                std::nullopt, 10.0}});
    CHECK(!orphan.external_efficiency);
    CHECK(orphan.external_input_w && *orphan.external_input_w == 10.0);
    CHECK(linkbench::role_from_supply_type("Mains") == linkbench::RailRole::ExternalInput);
    CHECK(linkbench::role_from_supply_type("USB") == linkbench::RailRole::PhoneInput);
    CHECK(linkbench::role_from_supply_type("Battery") == linkbench::RailRole::Unknown);
    CHECK(linkbench::role_from_label("ina226", "vout") == linkbench::RailRole::Output);
    CHECK(linkbench::role_from_label("ina226", "vbus") == linkbench::RailRole::PhoneInput);
    CHECK(linkbench::role_from_label("adapter", "vin") == linkbench::RailRole::ExternalInput);
    auto packet = linkbench::decode_packet(0x1400, 480);
    CHECK(packet.first == 1024 && packet.second == 3);
    packet = linkbench::decode_packet(0x0400, 5000);
    CHECK(packet.first == 1024 && packet.second == 1);
    CHECK(std::string(linkbench::hid_kind(1, 2)) == "boot mouse");

    const auto quick = linkbench::stream_bench_plan("quick");
    CHECK(quick.passes == 3 && quick.seconds == 5 && quick.buffers == 8 && quick.keep_maximum);
    const auto stress = linkbench::stream_bench_plan("stress");
    CHECK(stress.passes == 3 && stress.seconds == 10 && stress.buffers == 2 && !stress.keep_maximum);
    CHECK(stress.fail_on_stream_errors && stress.max_drop_ratio == 0.01);
    bool unknown = false;
    try {
        linkbench::stream_bench_plan("rnd4k");
    } catch (const linkbench::Error&) {
        unknown = true;
    }
    CHECK(unknown);

    linkbench::StreamPassSample slow{100.0 * 1024 * 1024, 30, 150, 0, 0, 0, false};
    linkbench::StreamPassSample peak{180.0 * 1024 * 1024, 30, 150, 0, 0, 0, false};
    linkbench::StreamPassSample mid{120.0 * 1024 * 1024, 29, 98, 2, 0, 0, false};
    auto best = linkbench::score_stream_passes(quick, {slow, peak, mid});
    CHECK(best.pass_index == 2 && best.bytes_per_sec == peak.bytes_per_sec && !best.failed);

    auto worst = linkbench::score_stream_passes(stress, {slow, peak, mid});
    CHECK(worst.pass_index == 1 && worst.bytes_per_sec == slow.bytes_per_sec);
    CHECK(!worst.failed);

    linkbench::StreamPassSample dropped = mid;
    dropped.frames = 90;
    dropped.dropped_frames = 10;
    auto pressured = linkbench::score_stream_passes(stress, {slow, peak, dropped});
    CHECK(pressured.failed);
    CHECK(pressured.reason.find("丢帧比例 2.50% 超过 1.00%") != std::string::npos);
    CHECK(pressured.pass_index == 1);

    linkbench::StreamPassSample exact = slow;
    exact.frames = 99;
    exact.dropped_frames = 1;
    auto boundary = linkbench::score_stream_passes(stress, {exact});
    CHECK(!boundary.failed && std::abs(boundary.drop_ratio - 0.01) < 1e-12);

    linkbench::StreamPassSample stalled = slow;
    stalled.stalled = true;
    stalled.error_frames = 2;
    auto broken = linkbench::score_stream_passes(stress, {stalled});
    CHECK(broken.failed);
    CHECK(broken.reason.find("停滞 1 次") != std::string::npos);
    CHECK(broken.reason.find("错误帧 2") != std::string::npos);
    auto quick_ignores = linkbench::score_stream_passes(quick, {stalled});
    CHECK(!quick_ignores.failed);

    auto with_floor = stress;
    with_floor.min_fps = 31;
    auto slow_fps = linkbench::score_stream_passes(with_floor, {slow});
    CHECK(slow_fps.failed && slow_fps.reason.find("帧率 30.00 低于 31.00") != std::string::npos);

    linkbench::StreamBenchReport shown;
    shown.plan = quick;
    linkbench::StreamReport round;
    round.aggregate_bytes_per_sec = peak.bytes_per_sec;
    linkbench::FrameStream frame;
    frame.window_fps = 30;
    frame.frames = 150;
    round.streams.push_back(frame);
    shown.passes = {round, round};
    shown.score = linkbench::score_stream_passes(quick, {slow, peak});
    shown.notes.push_back("记分说明");
    const auto bench_text = linkbench::text_stream_bench(shown);
    CHECK(bench_text.find("快速性能") != std::string::npos);
    CHECK(bench_text.find("取最高") != std::string::npos);
    CHECK(bench_text.find("判定: 通过") != std::string::npos);
    CHECK(bench_text.find("注: 记分说明") != std::string::npos);
    const auto json = linkbench::json_stream_bench(shown);
    CHECK(json.find("\"profile\":\"quick\"") != std::string::npos);
    CHECK(json.find("\"failed\":false") != std::string::npos);

    bool empty = false;
    try {
        linkbench::score_stream_passes(quick, {});
    } catch (const linkbench::Error&) {
        empty = true;
    }
    CHECK(empty);

    linkbench::VideoMode vga_depth{640, 480, "Z16", 30, 2};
    linkbench::VideoMode wide_depth{1280, 800, "Y16", 30, 2};
    linkbench::VideoMode fast_depth{640, 480, "Z16", 60, 2};
    CHECK(wide_depth.payload_bytes_per_sec() == 1280ull * 800 * 2 * 30);
    const auto deepest = linkbench::pick_depth_mode({vga_depth, fast_depth, wide_depth});
    CHECK(deepest.width == 1280 && deepest.height == 800 && deepest.fourcc == "Y16" && deepest.fps == 30);

    linkbench::VideoMode mjpg_hd{1920, 1080, "MJPG", 30, 0};
    linkbench::VideoMode mjpg_vga{640, 480, "MJPG", 30, 0};
    linkbench::VideoMode mjpg_slow{640, 480, "MJPG", 15, 0};
    linkbench::VideoMode yuyv_hd{1920, 1080, "YUYV", 30, 2};
    const auto color_mode = linkbench::pick_color_mode({mjpg_hd, yuyv_hd, mjpg_slow, mjpg_vga}, 30);
    CHECK(color_mode.fourcc == "MJPG" && color_mode.width == 640 && color_mode.height == 480 && color_mode.fps == 30);
    linkbench::VideoMode yuyv_wide{848, 480, "YUYV", 90, 2};
    linkbench::VideoMode yuyv_vga{640, 480, "YUYV", 90, 2};
    const auto matched = linkbench::pick_color_mode({yuyv_vga, yuyv_wide}, 90, 848, 480);
    CHECK(matched.width == 848 && matched.height == 480 && matched.fps == 90);

    linkbench::NodeModes depth_node;
    depth_node.path = "/dev/video0";
    depth_node.kind = linkbench::StreamKind::Depth;
    depth_node.modes = {vga_depth, wide_depth};
    linkbench::NodeModes color_node;
    color_node.path = "/dev/video6";
    color_node.kind = linkbench::StreamKind::Color;
    color_node.modes = {mjpg_hd, mjpg_vga, yuyv_hd};
    const auto planned = linkbench::plan_color_depth({color_node, depth_node});
    CHECK(planned.depth_node == "/dev/video0" && planned.color_node == "/dev/video6");
    CHECK(planned.depth_budget_bytes_per_sec == 1280ull * 800 * 2 * 30);
    CHECK(planned.color_mode.fourcc == "MJPG" && planned.color_mode.width == 640);
    CHECK(planned.request.strict_nodes && planned.request.paths.size() == 2);
    CHECK(planned.request.node_formats.size() == 2);
    bool depth_expect = false;
    for (const auto& format : planned.request.node_formats) {
        if (format.role == "depth") {
            depth_expect = format.expect_frame_bytes && *format.expect_frame_bytes == 1280ull * 800 * 2;
        }
        if (format.role == "color") CHECK(!format.expect_frame_bytes);
    }
    CHECK(depth_expect);
    bool exceeds_usb2 = false;
    bool under_hundred = false;
    for (const auto& note : planned.request.notes) {
        if (note.find("超过 USB 2.0") != std::string::npos) exceeds_usb2 = true;
        if (note.find("不到 100") != std::string::npos) under_hundred = true;
    }
    CHECK(exceeds_usb2 && !under_hundred);

    depth_node.modes = {vga_depth};
    const auto modest = linkbench::plan_color_depth({depth_node, color_node});
    CHECK(modest.depth_budget_bytes_per_sec == 640ull * 480 * 2 * 30);
    CHECK(modest.depth_budget_bytes_per_sec * 8 > 100'000'000ull);
    bool modest_exceeds = false;
    for (const auto& note : modest.request.notes) {
        if (note.find("超过 USB 2.0") != std::string::npos) modest_exceeds = true;
    }
    CHECK(!modest_exceeds);
    const auto* gemini = linkbench::find_preset("gemini305-depth-1280x800-y16-30");
    CHECK(gemini && gemini->payload_bytes_per_sec() == 1280ull * 800 * 2 * 30);
    const auto* gemini_vga = linkbench::find_preset("gemini305-depth-640x480-z16-30");
    CHECK(gemini_vga && gemini_vga->payload_bytes_per_sec() == 640ull * 480 * 2 * 30);

    if (failures) {
        std::cerr << failures << " 项失败\n";
        return 1;
    }
    std::cout << "通过\n";
    return 0;
}
