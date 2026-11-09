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

    if (failures) {
        std::cerr << failures << " 项失败\n";
        return 1;
    }
    std::cout << "通过\n";
    return 0;
}
