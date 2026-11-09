#include "linkbench.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace linkbench {
namespace {

namespace fs = std::filesystem;

const char* kSerialDrivers[] = {"cdc_acm", "ch341", "cp210x", "ftdi_sio", "pl2303", "usbserial", "option", "qcserial"};

std::string read_trim(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string text;
    std::getline(in, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    auto start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    return text.substr(start);
}

std::optional<std::uint64_t> read_u64(const fs::path& path) {
    const auto text = read_trim(path);
    if (text.empty()) return std::nullopt;
    try {
        return std::stoull(text, nullptr, 0);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> read_u32(const fs::path& path) {
    auto value = read_u64(path);
    if (!value) return std::nullopt;
    return static_cast<std::uint32_t>(*value);
}

std::uint8_t parse_hex8(const std::string& text) {
    if (text.empty()) return 0;
    try {
        return static_cast<std::uint8_t>(std::stoul(text, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

std::uint16_t parse_hex16(const std::string& text) {
    if (text.empty()) return 0;
    try {
        return static_cast<std::uint16_t>(std::stoul(text, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

std::optional<std::uint32_t> parse_milliamps(const std::string& text) {
    std::string digits;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') digits.push_back(ch);
        else break;
    }
    if (digits.empty()) return std::nullopt;
    return static_cast<std::uint32_t>(std::stoul(digits));
}

bool is_hid(const UsbPort& port) {
    return std::any_of(port.interfaces.begin(), port.interfaces.end(),
                       [](const UsbInterface& iface) { return iface.class_name == "hid"; });
}

bool is_serial(const UsbPort& port) {
    return std::any_of(port.interfaces.begin(), port.interfaces.end(), [](const UsbInterface& iface) {
        if (iface.tty || iface.class_name == "cdc-acm") return true;
        if (!iface.driver) return false;
        for (const char* name : kSerialDrivers) {
            if (*iface.driver == name) return true;
        }
        return false;
    });
}

std::set<std::string> upstream_names(const std::vector<RawDevice>& devices) {
    std::map<std::uint32_t, std::vector<const RawDevice*>> by_bus;
    for (const auto& device : devices) {
        if (device.is_root || device.port.device_class != 0x09 || device.port.speed_mbps < 5000.0) continue;
        by_bus[device.busnum].push_back(&device);
    }
    std::set<std::string> names;
    for (auto& [bus, group] : by_bus) {
        (void)bus;
        const RawDevice* best = nullptr;
        for (const auto* device : group) {
            if (!best) {
                best = device;
                continue;
            }
            const auto dots = std::count(device->port.location.begin(), device->port.location.end(), '.');
            const auto best_dots = std::count(best->port.location.begin(), best->port.location.end(), '.');
            if (std::tuple(dots, device->port.location.size(), device->port.location) <
                std::tuple(best_dots, best->port.location.size(), best->port.location)) {
                best = device;
            }
        }
        if (best) names.insert(best->port.location);
    }
    return names;
}

std::optional<double> rail_watts(const PowerRail& rail) {
    if (rail.power_w && std::isfinite(*rail.power_w) && *rail.power_w >= 0) return *rail.power_w;
    if (rail.voltage_v && rail.current_a && std::isfinite(*rail.voltage_v) && std::isfinite(*rail.current_a) &&
        *rail.voltage_v >= 0 && *rail.current_a >= 0) {
        return *rail.voltage_v * *rail.current_a;
    }
    return std::nullopt;
}

std::optional<double> sum_role(const std::vector<PowerRail>& rails, RailRole role) {
    double total = 0;
    bool any = false;
    for (const auto& rail : rails) {
        if (rail.role != role) continue;
        if (auto watts = rail_watts(rail)) {
            total += *watts;
            any = true;
        }
    }
    if (!any) return std::nullopt;
    return total;
}

std::optional<double> efficiency(std::optional<double> output_w, std::optional<double> input_w) {
    if (output_w && input_w && *input_w > 0 && *output_w >= 0) return *output_w / *input_w;
    return std::nullopt;
}

std::string dash(const std::string& value) { return value.empty() ? "-" : value; }

std::string fmt_speed(double mbps) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    if (mbps >= 1000.0 || std::floor(mbps) == mbps) {
        os.precision(0);
        os << mbps << " Mbit/s";
    } else {
        os.precision(1);
        os << mbps << " Mbit/s";
    }
    return os.str();
}

std::string fmt_watts(std::optional<double> value) {
    if (!value) return "未测";
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << *value << " W";
    return os.str();
}

std::string fmt_eff(std::optional<double> value) {
    if (!value) return "未计算";
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << (*value * 100.0) << "%";
    return os.str();
}

std::string fmt_opt(std::optional<double> value, const char* unit) {
    if (!value) return "-";
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << *value << " " << unit;
    return os.str();
}

std::string yes_no(std::optional<bool> value) {
    if (!value) return "-";
    return *value ? "是" : "否";
}

std::string row(const std::string& label, const std::string& value) { return "- " + label + "：" + value + "\n"; }

void push_notes(std::string& out, const std::vector<std::string>& notes) {
    for (const auto& note : notes) out += "注：" + note + "\n\n";
}

std::string product_name(const UsbPort& port) {
    if (port.manufacturer && port.product) return *port.manufacturer + " " + *port.product;
    if (port.product) return *port.product;
    if (port.manufacturer) return *port.manufacturer;
    return "未命名设备";
}

std::string id_pair(const UsbPort& port) {
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(4) << port.vendor_id << ":" << std::setw(4) << port.product_id;
    return os.str();
}

const char* supply_phrase(const UsbPort& port) { return port.self_powered ? "自供电" : "总线供电"; }

std::string declared_ma(std::optional<std::uint32_t> value) {
    if (!value) return "-";
    return std::to_string(*value) + " mA";
}

const char* rail_role_name(RailRole role) {
    switch (role) {
        case RailRole::ExternalInput:
            return "外供电输入";
        case RailRole::PhoneInput:
            return "手机供电输入";
        case RailRole::Output:
            return "输出";
        case RailRole::Unknown:
            return "未归类";
    }
    return "未归类";
}

std::string conclusion(const BenchReport& report) {
    std::string lines;
    if (report.upstream_usb3.empty()) {
        lines += "上行 USB 3.0 口：未看到。\n";
    } else {
        for (const auto& port : report.upstream_usb3) {
            std::string parent;
            if (port.parent && port.parent_speed_mbps) parent = "，父口 " + *port.parent + " 为 " + fmt_speed(*port.parent_speed_mbps);
            lines += "上行口 " + port.location + " 协商 " + fmt_speed(port.speed_mbps) + "，bcdUSB " + dash(port.version) + "，" +
                     supply_phrase(port) + "，声明供电 " + declared_ma(port.max_power_ma) + parent + "。\n";
        }
    }
    lines += "下行 USB 3.0 " + std::to_string(report.downstream_usb3.size()) + " 个，USB 2.0 HID " +
             std::to_string(report.downstream_hid.size()) + " 个，USB 2.0 串口 " + std::to_string(report.downstream_serial.size()) +
             " 个，CAN " + std::to_string(report.can.size()) + " 个。\n";
    if (report.downstream_serial.empty()) lines += "USB 2.0 串口未接入。\n";
    if (report.can.empty()) lines += "CAN 未接入。\n";
    if (!report.power.external_efficiency && !report.power.phone_efficiency) {
        lines += "外供电效率和手机供电效率都未计算。";
    } else {
        lines += "外供电效率 " + fmt_eff(report.power.external_efficiency) + "，手机供电效率 " +
                 fmt_eff(report.power.phone_efficiency) + "。";
    }
    return lines;
}

void push_port(std::string& out, const UsbPort& port) {
    out += "#### " + port.location + " " + product_name(port) + "\n\n";
    out += row("协商速率", fmt_speed(port.speed_mbps));
    if (port.parent && port.parent_speed_mbps) out += row("父口", *port.parent + " " + fmt_speed(*port.parent_speed_mbps));
    else if (port.parent) out += row("父口", *port.parent);
    else out += row("父口", "-");
    out += row("bcdUSB", dash(port.version));
    out += row("VID:PID", id_pair(port));
    out += row("类", std::string(class_name(port.device_class, 0)) + " (" + [&] {
        std::ostringstream os;
        os << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(port.device_class);
        return os.str();
    }() + ")");
    out += row("供电", std::string(supply_phrase(port)) + "，声明 " + declared_ma(port.max_power_ma));
    out += row("远程唤醒", port.remote_wakeup ? "支持" : "未声明");
    out += row("下行口数量", port.downstream_ports ? std::to_string(*port.downstream_ports) : "-");
    if (port.tx_lanes && port.rx_lanes) {
        out += row("通道", "TX " + std::to_string(*port.tx_lanes) + " / RX " + std::to_string(*port.rx_lanes));
    } else {
        out += row("通道", "-");
    }
    out += row("LTM", yes_no(port.ltm_capable));
    out += row("可拆卸", port.removable.value_or("-"));
    out += row("控制端点最大包", port.control_max_packet ? std::to_string(*port.control_max_packet) + " 字节" : "-");
    out += "\n";
    for (const auto& iface : port.interfaces) {
        out += "- 接口 " + std::to_string(iface.number) + " " + iface.class_name + "/" + std::to_string(iface.subclass) + "/" +
               std::to_string(iface.protocol);
        if (iface.hid_kind) out += "，" + *iface.hid_kind;
        if (iface.driver) out += "，驱动 " + *iface.driver;
        if (iface.tty) out += "，" + *iface.tty;
        if (!iface.hidraw.empty()) {
            out += "，";
            for (std::size_t i = 0; i < iface.hidraw.size(); ++i) {
                if (i) out += "、";
                out += iface.hidraw[i];
            }
        }
        out += "\n";
        for (const auto& endpoint : iface.endpoints) {
            std::ostringstream addr;
            addr << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(endpoint.address);
            out += "  - " + addr.str() + " " + endpoint.direction + " " + endpoint.transfer + "，" +
                   std::to_string(endpoint.max_packet) + " 字节，间隔 " + dash(endpoint.interval);
            if (endpoint.transactions > 1) out += "，每微帧 " + std::to_string(endpoint.transactions) + " 次";
            out += "\n";
        }
    }
    if (!port.interfaces.empty()) out += "\n";
}

void push_port_list(std::string& out, const std::vector<UsbPort>& ports, const std::string& empty) {
    if (ports.empty()) {
        out += empty + "\n\n";
        return;
    }
    for (const auto& port : ports) push_port(out, port);
}

std::optional<UsbEndpoint> read_endpoint(const fs::path& dir, double speed_mbps) {
    const auto address_text = read_trim(dir / "bEndpointAddress");
    if (address_text.empty()) return std::nullopt;
    const auto address = parse_hex8(address_text);
    const auto attributes = parse_hex8(read_trim(dir / "bmAttributes"));
    const auto raw_packet = parse_hex16(read_trim(dir / "wMaxPacketSize"));
    const auto decoded = decode_packet(raw_packet, speed_mbps);
    UsbEndpoint endpoint;
    endpoint.address = address;
    endpoint.direction = (address & 0x80) ? "IN" : "OUT";
    endpoint.transfer = transfer_name(static_cast<TransferKind>(attributes & 0x03));
    endpoint.max_packet = decoded.first;
    endpoint.transactions = decoded.second;
    endpoint.interval = read_trim(dir / "interval");
    return endpoint;
}

UsbInterface read_interface(const fs::path& dir, const std::string& name, double speed_mbps) {
    const auto cls = parse_hex8(read_trim(dir / "bInterfaceClass"));
    const auto subclass = parse_hex8(read_trim(dir / "bInterfaceSubClass"));
    const auto protocol = parse_hex8(read_trim(dir / "bInterfaceProtocol"));
    UsbInterface iface;
    auto number_text = name;
    const auto cut = number_text.find_last_of(":.");
    if (cut != std::string::npos) number_text = number_text.substr(cut + 1);
    try {
        iface.number = static_cast<std::uint8_t>(std::stoul(number_text));
    } catch (...) {
        iface.number = 0;
    }
    iface.class_name = (cls == 0x02 && subclass == 0x02) ? "cdc-acm" : class_name(cls, subclass);
    iface.subclass = subclass;
    iface.protocol = protocol;
    if (cls == 0x03) iface.hid_kind = hid_kind(subclass, protocol);
    std::error_code ec;
    auto driver = fs::read_symlink(dir / "driver", ec);
    if (!ec && driver.has_filename()) iface.driver = driver.filename().string();
    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const auto ep_name = entry.path().filename().string();
            if (ep_name.rfind("ep_", 0) != 0 || ep_name == "ep_00") continue;
            if (auto endpoint = read_endpoint(entry.path(), speed_mbps)) iface.endpoints.push_back(*endpoint);
        }
    }
    std::sort(iface.endpoints.begin(), iface.endpoints.end(),
              [](const UsbEndpoint& a, const UsbEndpoint& b) { return a.address < b.address; });
    return iface;
}

bool interface_path_matches(const std::string& real, const std::string& location, std::uint8_t number) {
    std::stringstream stream(real);
    std::string part;
    while (std::getline(stream, part, '/')) {
        const auto colon = part.find(':');
        if (colon == std::string::npos) continue;
        if (part.substr(0, colon) != location) continue;
        auto iface = part.substr(colon + 1);
        const auto dot = iface.rfind('.');
        if (dot != std::string::npos) iface = iface.substr(dot + 1);
        try {
            if (static_cast<std::uint8_t>(std::stoul(iface)) == number) return true;
        } catch (...) {
        }
    }
    return false;
}

void attach_class(std::vector<RawDevice>& devices, const fs::path& root, bool hidraw) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const auto name = entry.path().filename().string();
        auto real = fs::canonical(entry.path() / "device", ec);
        if (ec) continue;
        const auto text = real.string();
        for (auto& device : devices) {
            for (auto& iface : device.port.interfaces) {
                if (!interface_path_matches(text, device.port.location, iface.number)) continue;
                if (hidraw) iface.hidraw.push_back("/dev/" + name);
                else if (!iface.tty) iface.tty = "/dev/" + name;
            }
        }
    }
}

std::vector<RawDevice> read_devices() {
    std::vector<RawDevice> devices;
    const fs::path root = "/sys/bus/usb/devices";
    std::error_code ec;
    if (!fs::exists(root, ec)) return devices;
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(root, ec)) names.push_back(entry.path().filename().string());
    for (const auto& name : names) {
        if (name.find(':') != std::string::npos) continue;
        const auto dir = root / name;
        const auto speed_text = read_trim(dir / "speed");
        if (speed_text.empty()) continue;
        double speed = 0;
        try {
            speed = std::stod(speed_text);
        } catch (...) {
            continue;
        }
        RawDevice raw;
        raw.busnum = read_u32(dir / "busnum").value_or(0);
        raw.is_root = name.rfind("usb", 0) == 0 && name.find('-') == std::string::npos;
        raw.port.location = name;
        if (!raw.is_root) {
            const auto dot = name.rfind('.');
            raw.port.parent = dot == std::string::npos ? "usb" + std::to_string(raw.busnum) : name.substr(0, dot);
        }
        raw.port.speed_mbps = speed;
        raw.port.version = read_trim(dir / "version");
        raw.port.vendor_id = parse_hex16(read_trim(dir / "idVendor"));
        raw.port.product_id = parse_hex16(read_trim(dir / "idProduct"));
        auto manufacturer = read_trim(dir / "manufacturer");
        auto product = read_trim(dir / "product");
        if (!manufacturer.empty()) raw.port.manufacturer = manufacturer;
        if (!product.empty()) raw.port.product = product;
        const auto attributes = parse_hex8(read_trim(dir / "bmAttributes"));
        raw.port.device_class = parse_hex8(read_trim(dir / "bDeviceClass"));
        raw.port.self_powered = (attributes & 0x40) != 0;
        raw.port.remote_wakeup = (attributes & 0x20) != 0;
        raw.port.max_power_ma = parse_milliamps(read_trim(dir / "bMaxPower"));
        if (raw.port.device_class == 0x09) raw.port.downstream_ports = read_u32(dir / "maxchild");
        raw.port.tx_lanes = read_u32(dir / "tx_lanes");
        raw.port.rx_lanes = read_u32(dir / "rx_lanes");
        auto ltm = read_trim(dir / "ltm_capable");
        if (!ltm.empty()) raw.port.ltm_capable = ltm == "yes";
        auto removable = read_trim(dir / "removable");
        if (!removable.empty()) raw.port.removable = removable;
        if (auto control = read_endpoint(dir / "ep_00", speed)) raw.port.control_max_packet = control->max_packet;
        for (const auto& candidate : names) {
            if (candidate.rfind(name + ":", 0) != 0) continue;
            raw.port.interfaces.push_back(read_interface(root / candidate, candidate, speed));
        }
        devices.push_back(std::move(raw));
    }
    return devices;
}

std::vector<CanPort> read_can() {
    std::vector<CanPort> ports;
    const fs::path root = "/sys/class/net";
    std::error_code ec;
    if (!fs::exists(root, ec)) return ports;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (read_trim(entry.path() / "type") != "280") continue;
        CanPort port;
        port.name = entry.path().filename().string();
        auto state = read_trim(entry.path() / "operstate");
        if (!state.empty()) port.state = state;
        port.bitrate = read_u32(entry.path() / "can_bittiming" / "bitrate");
        port.clock_hz = read_u32(entry.path() / "can_clock");
        auto driver = fs::read_symlink(entry.path() / "device" / "driver", ec);
        if (!ec && driver.has_filename()) port.driver = driver.filename().string();
        ports.push_back(std::move(port));
    }
    std::sort(ports.begin(), ports.end(), [](const CanPort& a, const CanPort& b) { return a.name < b.name; });
    return ports;
}

bool sensor_of_interest(std::string name) {
    for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (name == "nvme" || name == "k10temp" || name == "amdgpu" || name == "coretemp" || name == "acpi") return false;
    return name.find("ina") != std::string::npos || name.find("usb") != std::string::npos ||
           name.find("pd") != std::string::npos || name.find("charger") != std::string::npos;
}

std::vector<PowerRail> read_power_supply() {
    std::vector<PowerRail> rails;
    const fs::path root = "/sys/class/power_supply";
    std::error_code ec;
    if (!fs::exists(root, ec)) return rails;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        PowerRail rail;
        rail.name = entry.path().filename().string();
        rail.role = role_from_supply_type(read_trim(entry.path() / "type"));
        auto voltage = read_u64(entry.path() / "voltage_now");
        if (!voltage) voltage = read_u64(entry.path() / "voltage_avg");
        auto current = read_u64(entry.path() / "current_now");
        if (!current) current = read_u64(entry.path() / "current_avg");
        auto power = read_u64(entry.path() / "power_now");
        if (!power) power = read_u64(entry.path() / "power_avg");
        if (voltage) rail.voltage_v = static_cast<double>(*voltage) / 1'000'000.0;
        if (current) rail.current_a = static_cast<double>(*current) / 1'000'000.0;
        if (power) rail.power_w = static_cast<double>(*power) / 1'000'000.0;
        if (!rail.voltage_v && !rail.current_a && !rail.power_w) continue;
        rails.push_back(std::move(rail));
    }
    return rails;
}

std::vector<PowerRail> hwmon_rails(const fs::path& dir, const std::string& sensor) {
    std::vector<PowerRail> rails;
    std::error_code ec;
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) files.push_back(entry.path().filename().string());
    for (const auto& file : files) {
        if (file.rfind("power", 0) != 0 || file.size() < std::string("power_input").size()) continue;
        if (file.substr(file.size() - 6) != "_input") continue;
        auto index = file.substr(5, file.size() - 5 - 6);
        auto label = read_trim(dir / ("power" + index + "_label"));
        PowerRail rail;
        rail.name = sensor + " power" + index;
        if (!label.empty()) rail.name += " " + label;
        rail.role = role_from_label(sensor, label);
        if (auto micro = read_u64(dir / file)) rail.power_w = static_cast<double>(*micro) / 1'000'000.0;
        rails.push_back(std::move(rail));
    }
    if (!rails.empty()) return rails;
    auto voltage = read_u64(dir / "in1_input");
    if (!voltage) voltage = read_u64(dir / "in0_input");
    auto current = read_u64(dir / "curr1_input");
    if (voltage && current) {
        auto label = read_trim(dir / "in1_label");
        if (label.empty()) label = read_trim(dir / "in0_label");
        PowerRail rail;
        rail.name = sensor;
        if (!label.empty()) rail.name += " " + label;
        rail.role = role_from_label(sensor, label);
        rail.voltage_v = static_cast<double>(*voltage) / 1000.0;
        rail.current_a = static_cast<double>(*current) / 1000.0;
        rail.power_w = *rail.voltage_v * *rail.current_a;
        rails.push_back(std::move(rail));
    }
    return rails;
}

std::vector<PowerRail> read_hwmon() {
    std::vector<PowerRail> rails;
    const fs::path root = "/sys/class/hwmon";
    std::error_code ec;
    if (!fs::exists(root, ec)) return rails;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const auto sensor = read_trim(entry.path() / "name");
        if (!sensor_of_interest(sensor)) continue;
        auto found = hwmon_rails(entry.path(), sensor);
        rails.insert(rails.end(), found.begin(), found.end());
    }
    return rails;
}

std::string local_stamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

}  // namespace

RailRole role_from_supply_type(const std::string& kind) {
    auto text = kind;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
    auto start = text.find_first_not_of(' ');
    if (start == std::string::npos) return RailRole::Unknown;
    text = text.substr(start);
    if (text == "Mains") return RailRole::ExternalInput;
    if (text == "USB") return RailRole::PhoneInput;
    return RailRole::Unknown;
}

RailRole role_from_label(const std::string& sensor, const std::string& label) {
    auto lower = [](std::string text) {
        for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return text;
    };
    const auto sensor_l = lower(sensor);
    const auto label_l = lower(label);
    const bool output = label_l.find("vout") != std::string::npos || label_l.find("out") != std::string::npos;
    const bool input = label_l.find("vin") != std::string::npos || label_l.find("iin") != std::string::npos ||
                       label_l.find("bus") != std::string::npos;
    if (output && !input) return RailRole::Output;
    if (sensor_l.find("usb") != std::string::npos || label_l.find("usb") != std::string::npos ||
        label_l.find("phone") != std::string::npos || label_l.find("vbus") != std::string::npos) {
        return RailRole::PhoneInput;
    }
    if (input || sensor_l.find("mains") != std::string::npos || sensor_l.find("adapter") != std::string::npos ||
        label_l.find("ext") != std::string::npos) {
        return RailRole::ExternalInput;
    }
    return RailRole::Unknown;
}

PowerSection power_section(std::vector<PowerRail> rails) {
    PowerSection section;
    section.external_input_w = sum_role(rails, RailRole::ExternalInput);
    section.phone_input_w = sum_role(rails, RailRole::PhoneInput);
    section.output_w = sum_role(rails, RailRole::Output);
    section.external_efficiency = efficiency(section.output_w, section.external_input_w);
    section.phone_efficiency = efficiency(section.output_w, section.phone_input_w);
    section.notes = {"效率只用传感器给出的瓦特，或由同一路的电压乘电流得到。", "描述符里的 bMaxPower 是申请上限，不算输入或输出功率。"};
    if (!section.external_efficiency && !section.phone_efficiency) {
        section.notes.push_back("没有同时测到输入和输出，效率未计算。");
    }
    if ((section.external_efficiency && *section.external_efficiency > 1.0) ||
        (section.phone_efficiency && *section.phone_efficiency > 1.0)) {
        section.notes.push_back("效率大于 1，这两路传感器不是同一条功率路径。");
    }
    section.rails = std::move(rails);
    return section;
}

std::pair<std::uint16_t, std::uint8_t> decode_packet(std::uint16_t raw, double speed_mbps) {
    if (speed_mbps >= 5000.0 || speed_mbps == 0.0) return {raw, 1};
    return {static_cast<std::uint16_t>(raw & 0x07ff), static_cast<std::uint8_t>(((raw >> 11) & 0x3) + 1)};
}

const char* hid_kind(std::uint8_t subclass, std::uint8_t protocol) {
    if (subclass == 1 && protocol == 1) return "boot keyboard";
    if (subclass == 1 && protocol == 2) return "boot mouse";
    if (subclass == 1) return "boot";
    return "report";
}

BenchReport assemble(std::vector<RawDevice> raws, std::vector<CanPort> can, std::vector<PowerRail> rails,
                     std::string collected_at, std::string hostname, std::string kernel) {
    std::map<std::string, double> speeds;
    for (const auto& raw : raws) speeds[raw.port.location] = raw.port.speed_mbps;
    for (auto& raw : raws) {
        if (raw.port.parent) {
            auto found = speeds.find(*raw.port.parent);
            if (found != speeds.end()) raw.port.parent_speed_mbps = found->second;
        }
    }
    std::sort(raws.begin(), raws.end(),
              [](const RawDevice& a, const RawDevice& b) { return a.port.location < b.port.location; });
    const auto upstream = upstream_names(raws);
    BenchReport report;
    report.collected_at = std::move(collected_at);
    report.hostname = std::move(hostname);
    report.kernel = std::move(kernel);
    for (auto& raw : raws) {
        if (raw.is_root) {
            report.host_controllers.push_back(std::move(raw.port));
            continue;
        }
        if (upstream.count(raw.port.location)) {
            report.upstream_usb3.push_back(std::move(raw.port));
            continue;
        }
        const bool usb3 = raw.port.speed_mbps >= 5000.0;
        const bool usb2 = raw.port.speed_mbps > 0.0 && raw.port.speed_mbps < 5000.0;
        const bool hid = is_hid(raw.port);
        const bool serial = is_serial(raw.port);
        if (usb3) report.downstream_usb3.push_back(raw.port);
        if (usb2 && hid) report.downstream_hid.push_back(raw.port);
        if (usb2 && serial) report.downstream_serial.push_back(raw.port);
        if (!usb3 && !(usb2 && hid) && !(usb2 && serial)) report.other.push_back(std::move(raw.port));
    }
    report.can = std::move(can);
    report.power = power_section(std::move(rails));
    report.notes = {"上行口是每条 SuperSpeed 总线上路径最短的集线器。更深的集线器和 SuperSpeed 功能设备算下行口。",
                    "HID 只收协商速率低于 5 Gbit/s、且有 HID 接口的设备。串口只收同一速率范围内的 CDC ACM、常见 USB 串口驱动，或已经出现 tty 的接口。",
                    "CAN 读网口类型 280 以及 can_bittiming。没有口就记未接入，不填 0 bit/s。"};
    return report;
}

std::string format_port_report(const BenchReport& report) {
    std::string out = "# USB 端口与供电 Benchmark 报告\n\n";
    out += "采集时刻：" + dash(report.collected_at) + "  \n主机：" + dash(report.hostname) + "  \n内核：" + dash(report.kernel) +
           "\n\n";
    out += "本报告记录操作系统已经枚举到的端口参数。协商速率不是吞吐，`bMaxPower` 是描述符里的供电上限，不代入效率。控制回传和视频载荷仍用 `rtt` 与 `stream`。\n\n";
    push_notes(out, report.notes);
    out += "## 1. 上行口 USB 3.0\n\n";
    if (report.upstream_usb3.empty()) out += "未看到 SuperSpeed 集线器。没有可报告的上行口参数。\n\n";
    else {
        out += "每条 SuperSpeed 总线上，离主机最近的集线器算上行口。\n\n";
        for (const auto& port : report.upstream_usb3) push_port(out, port);
    }
    out += "## 2. 下行口\n\n### 2.1 USB 3.0\n\n";
    push_port_list(out, report.downstream_usb3, "未看到 SuperSpeed 下行设备。");
    out += "### 2.2 USB 2.0 HID\n\n";
    push_port_list(out, report.downstream_hid, "未看到 USB 2.0 HID 设备。");
    out += "### 2.3 USB 2.0 串口\n\n";
    push_port_list(out, report.downstream_serial, "未接入。没有 CDC ACM、usbserial 驱动或对应的 tty 设备。");
    out += "### 2.4 CAN\n\n";
    if (report.can.empty()) out += "未接入。sysfs 里没有 CAN 网口。\n\n";
    else {
        for (const auto& can : report.can) {
            out += "#### " + can.name + "\n\n";
            out += row("状态", can.state.value_or("-"));
            out += row("比特率", can.bitrate ? std::to_string(*can.bitrate) + " bit/s" : "-");
            out += row("时钟", can.clock_hz ? std::to_string(*can.clock_hz) + " Hz" : "-");
            out += row("驱动", can.driver.value_or("-"));
            out += "\n";
        }
    }
    out += "## 3. 外供电 / 手机供电效率\n\n";
    out += "效率 = 输出功率 / 输入功率。外供电看市电或适配器输入，手机供电看 USB 总线输入。两边都要有实测瓦特才计算。\n\n";
    out += row("外供电输入", fmt_watts(report.power.external_input_w));
    out += row("手机供电输入", fmt_watts(report.power.phone_input_w));
    out += row("输出", fmt_watts(report.power.output_w));
    out += row("外供电效率", fmt_eff(report.power.external_efficiency));
    out += row("手机供电效率", fmt_eff(report.power.phone_efficiency));
    out += "\n";
    if (report.power.rails.empty()) out += "没有读到可归类的电压、电流或功率传感器。\n\n";
    else {
        out += "| 传感器 | 角色 | 电压 | 电流 | 功率 |\n| --- | --- | --- | --- | --- |\n";
        for (const auto& rail : report.power.rails) {
            auto cell = rail.name;
            for (auto& ch : cell)
                if (ch == '|') ch = '/';
            std::string power = "-";
            if (rail.power_w) {
                std::ostringstream os;
                os.setf(std::ios::fixed);
                os.precision(3);
                os << *rail.power_w << " W";
                power = os.str();
            }
            out += "| " + cell + " | " + rail_role_name(rail.role) + " | " + fmt_opt(rail.voltage_v, "V") + " | " +
                   fmt_opt(rail.current_a, "A") + " | " + power + " |\n";
        }
        out += "\n";
    }
    push_notes(out, report.power.notes);
    out += "## 4. 结论\n\n";
    out += conclusion(report);
    out += "\n\n";
    if (!report.other.empty()) {
        out += "## 附录：未归入上述端口的设备\n\n";
        for (const auto& port : report.other) {
            out += "- " + port.location + " " + fmt_speed(port.speed_mbps) + " " + port.version + "，" + id_pair(port) + "，" +
                   product_name(port) + "\n";
        }
        out += "\n";
    }
    return out;
}

std::string test_item_catalog() {
#ifndef LINKBENCH_CATALOG
    throw Error("未设置测试项文件路径");
#else
    std::ifstream in(LINKBENCH_CATALOG);
    if (!in) throw Error(std::string("打不开测试项文件 ") + LINKBENCH_CATALOG);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
#endif
}

std::string collect_port_report() {
    auto devices = read_devices();
    attach_class(devices, "/sys/class/hidraw", true);
    attach_class(devices, "/sys/class/tty", false);
    auto rails = read_power_supply();
    auto hwmon = read_hwmon();
    rails.insert(rails.end(), hwmon.begin(), hwmon.end());
    std::sort(rails.begin(), rails.end(), [](const PowerRail& a, const PowerRail& b) { return a.name < b.name; });
    auto report = assemble(std::move(devices), read_can(), std::move(rails), local_stamp(), read_trim("/proc/sys/kernel/hostname"),
                           read_trim("/proc/sys/kernel/osrelease"));
    return format_port_report(report);
}

}  // namespace linkbench
