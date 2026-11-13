#include "linkbench.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include "usb_scan.hpp"
#else
#include <sys/time.h>
#endif

#include <libusb.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace linkbench {

#if !defined(__linux__)
std::vector<std::string> capture_nodes_for_usb(std::uint16_t vendor, std::uint16_t product);
#endif

namespace {

namespace fs = std::filesystem;

std::string read_trim(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string text;
    std::getline(in, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

std::optional<std::uint64_t> parse_u64(const std::string& text, int base) {
    if (text.empty()) return std::nullopt;
    try {
        std::size_t used = 0;
        const auto value = std::stoull(text, &used, base);
        if (used != text.size()) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

struct RawDevice {
    DeviceRecord record;
    int busnum = 0;
    int devnum = 0;
};

std::vector<std::string> video_nodes_matching(std::uint16_t vid, std::uint16_t pid) {
    std::vector<std::string> nodes;
    const fs::path root = "/sys/class/video4linux";
    if (!fs::exists(root)) return nodes;
    for (const auto& entry : fs::directory_iterator(root)) {
        fs::path device = entry.path() / "device";
        std::error_code ec;
        auto current = fs::canonical(device, ec);
        if (ec) continue;
        while (true) {
            if (fs::is_regular_file(current / "idVendor") && fs::is_regular_file(current / "idProduct")) {
                const auto vendor = parse_u64(read_trim(current / "idVendor"), 16);
                const auto product = parse_u64(read_trim(current / "idProduct"), 16);
                if (vendor == vid && product == pid) nodes.push_back("/dev/" + entry.path().filename().string());
                break;
            }
            if (!current.has_parent_path() || current == current.root_path()) break;
            current = current.parent_path();
        }
    }
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

std::vector<RawDevice> scan_sysfs() {
    std::vector<RawDevice> devices;
    const fs::path root = "/sys/bus/usb/devices";
    if (!fs::exists(root)) return devices;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (name.find(':') != std::string::npos) continue;
        if (!fs::is_regular_file(entry.path() / "idVendor")) continue;
        RawDevice raw;
        raw.record.sysfs_path = entry.path().string();
        raw.record.vendor_id = static_cast<std::uint16_t>(parse_u64(read_trim(entry.path() / "idVendor"), 16).value_or(0));
        raw.record.product_id = static_cast<std::uint16_t>(parse_u64(read_trim(entry.path() / "idProduct"), 16).value_or(0));
        raw.busnum = static_cast<int>(parse_u64(read_trim(entry.path() / "busnum"), 10).value_or(0));
        raw.devnum = static_cast<int>(parse_u64(read_trim(entry.path() / "devnum"), 10).value_or(0));
        raw.record.bus_id = std::to_string(raw.busnum);
        raw.record.address = static_cast<std::uint8_t>(raw.devnum);
        const auto speed_text = read_trim(entry.path() / "speed");
        if (auto parsed = speed_from_sysfs(speed_text)) {
            raw.record.speed = *parsed;
            raw.record.speed_source = "sysfs";
        } else {
            raw.record.speed = speed_from_mbps(std::strtod(speed_text.c_str(), nullptr));
            raw.record.speed_source = "sysfs";
        }
        raw.record.line_mbit = line_mbit_per_sec(raw.record.speed);
        raw.record.usb_bcd = read_trim(entry.path() / "version");
        auto manufacturer = read_trim(entry.path() / "manufacturer");
        auto product = read_trim(entry.path() / "product");
        auto serial = read_trim(entry.path() / "serial");
        if (!manufacturer.empty()) raw.record.manufacturer = manufacturer;
        if (!product.empty()) raw.record.product = product;
        if (!serial.empty()) raw.record.serial = serial;
        if (auto camera = identify_camera(raw.record.vendor_id, raw.record.product_id, raw.record.product)) {
            raw.record.family = camera->family;
            raw.record.catalog_name = camera->catalog_name;
        }
        raw.record.video_nodes = video_nodes_matching(raw.record.vendor_id, raw.record.product_id);
        for (const auto& iface : fs::directory_iterator(root)) {
            const auto iface_name = iface.path().filename().string();
            if (iface_name.rfind(name + ":", 0) != 0) continue;
            const auto cls = parse_u64(read_trim(iface.path() / "bInterfaceClass"), 16).value_or(0);
            if (cls == 0x0e) raw.record.has_video_class = true;
        }
        devices.push_back(std::move(raw));
    }
    std::sort(devices.begin(), devices.end(),
              [](const RawDevice& a, const RawDevice& b) { return *a.record.sysfs_path < *b.record.sysfs_path; });
    return devices;
}

std::string format_bcd(std::uint16_t bcd) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%02x", (bcd >> 8) & 0xff, bcd & 0xff);
    return buf;
}

LinkSpeed speed_from_libusb(int speed) {
    switch (speed) {
        case LIBUSB_SPEED_LOW:
            return LinkSpeed::Low;
        case LIBUSB_SPEED_FULL:
            return LinkSpeed::Full;
        case LIBUSB_SPEED_HIGH:
            return LinkSpeed::High;
        case LIBUSB_SPEED_SUPER:
            return LinkSpeed::Super;
#ifdef LIBUSB_SPEED_SUPER_PLUS
        case LIBUSB_SPEED_SUPER_PLUS:
            return LinkSpeed::SuperPlus10;
#endif
        default:
            return LinkSpeed::Unknown;
    }
}

#if !defined(__linux__)
void attach_nodes(DeviceRecord& record) {
    auto nodes = capture_nodes_for_usb(record.vendor_id, record.product_id);
    if (record.serial && !record.serial->empty()) {
        std::vector<std::string> matched;
        for (const auto& node : nodes) {
            if (node.find(*record.serial) != std::string::npos) matched.push_back(node);
        }
        if (!matched.empty()) nodes = std::move(matched);
    }
    record.video_nodes = std::move(nodes);
}
#endif

std::vector<RawDevice> scan_libusb() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) throw Error("libusb 初始化失败");
    struct Exit {
        libusb_context* ctx;
        ~Exit() {
            if (ctx) libusb_exit(ctx);
        }
    } guard{ctx};
    libusb_device** list = nullptr;
    const auto count = libusb_get_device_list(ctx, &list);
    if (count < 0) throw Error("libusb 枚举设备失败");
    std::vector<RawDevice> devices;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        RawDevice raw;
        raw.busnum = libusb_get_bus_number(list[i]);
        raw.devnum = libusb_get_device_address(list[i]);
        raw.record.vendor_id = desc.idVendor;
        raw.record.product_id = desc.idProduct;
        raw.record.bus_id = std::to_string(raw.busnum);
        raw.record.address = static_cast<std::uint8_t>(raw.devnum > 255 ? 0 : raw.devnum);
        raw.record.speed = speed_from_libusb(libusb_get_device_speed(list[i]));
        raw.record.speed_source = "libusb";
        raw.record.line_mbit = line_mbit_per_sec(raw.record.speed);
        raw.record.usb_bcd = format_bcd(desc.bcdUSB);
        std::uint8_t ports[8] = {};
        const int nports = libusb_get_port_numbers(list[i], ports, static_cast<int>(sizeof(ports)));
        std::string location = raw.record.bus_id;
        if (nports > 0) {
            location += "-";
            for (int port = 0; port < nports; ++port) {
                if (port) location += ".";
                location += std::to_string(ports[port]);
            }
        }
        raw.record.sysfs_path = location;
        libusb_device_handle* handle = nullptr;
        if (libusb_open(list[i], &handle) == 0) {
            unsigned char text[256];
            if (desc.iManufacturer && libusb_get_string_descriptor_ascii(handle, desc.iManufacturer, text, sizeof(text)) > 0) {
                raw.record.manufacturer = reinterpret_cast<char*>(text);
            }
            if (desc.iProduct && libusb_get_string_descriptor_ascii(handle, desc.iProduct, text, sizeof(text)) > 0) {
                raw.record.product = reinterpret_cast<char*>(text);
            }
            if (desc.iSerialNumber && libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, text, sizeof(text)) > 0) {
                raw.record.serial = reinterpret_cast<char*>(text);
            }
            libusb_close(handle);
        }
        for (std::uint8_t cfg = 0; cfg < desc.bNumConfigurations && !raw.record.has_video_class; ++cfg) {
            libusb_config_descriptor* config = nullptr;
            if (libusb_get_config_descriptor(list[i], cfg, &config) != 0 || !config) continue;
            for (int iface = 0; iface < config->bNumInterfaces && !raw.record.has_video_class; ++iface) {
                for (int alt = 0; alt < config->interface[iface].num_altsetting; ++alt) {
                    if (config->interface[iface].altsetting[alt].bInterfaceClass == 0x0e) raw.record.has_video_class = true;
                }
            }
            libusb_free_config_descriptor(config);
        }
        if (auto camera = identify_camera(raw.record.vendor_id, raw.record.product_id, raw.record.product)) {
            raw.record.family = camera->family;
            raw.record.catalog_name = camera->catalog_name;
        }
        devices.push_back(std::move(raw));
    }
    libusb_free_device_list(list, 1);
    std::sort(devices.begin(), devices.end(), [](const RawDevice& a, const RawDevice& b) {
        return a.record.sysfs_path.value_or("") < b.record.sysfs_path.value_or("");
    });
    return devices;
}

#if defined(_WIN32)
std::vector<RawDevice> scan_windows() {
    auto host = scan_setupapi();
    if (host.empty()) {
        auto devices = scan_libusb();
        for (auto& device : devices) attach_nodes(device.record);
        return devices;
    }
    const auto lib = scan_libusb();
    std::vector<RawDevice> devices;
    for (auto& item : host) {
        RawDevice raw;
        raw.record = std::move(item.record);
        int hits = 0;
        int bus = 0;
        int dev = 0;
        for (const auto& other : lib) {
            if (other.record.vendor_id != raw.record.vendor_id || other.record.product_id != raw.record.product_id) continue;
            if (raw.record.serial && other.record.serial && *raw.record.serial != *other.record.serial) continue;
            ++hits;
            bus = other.busnum;
            dev = other.devnum;
        }
        if (hits == 1) {
            raw.busnum = bus;
            raw.devnum = dev;
        }
        attach_nodes(raw.record);
        devices.push_back(std::move(raw));
    }
    std::sort(devices.begin(), devices.end(), [](const RawDevice& a, const RawDevice& b) {
        return a.record.sysfs_path.value_or("") < b.record.sysfs_path.value_or("");
    });
    return devices;
}
#endif

std::vector<RawDevice> scan_devices() {
#if defined(__linux__)
    if (fs::exists("/sys/bus/usb/devices")) return scan_sysfs();
    return scan_libusb();
#elif defined(_WIN32)
    return scan_windows();
#else
    auto devices = scan_libusb();
    for (auto& device : devices) attach_nodes(device.record);
    return devices;
#endif
}

bool matches(const RawDevice& device, const DeviceFilter& filter) {
    if (filter.vendor_id && device.record.vendor_id != *filter.vendor_id) return false;
    if (filter.product_id && device.record.product_id != *filter.product_id) return false;
    if (filter.bus_id) {
        const auto file = device.record.sysfs_path ? fs::path(*device.record.sysfs_path).filename().string() : std::string{};
        const bool location = filter.bus_id->find('-') != std::string::npos || filter.bus_id->rfind("usb", 0) == 0;
        if (location) {
            if (file != *filter.bus_id) return false;
        } else if (device.record.bus_id != *filter.bus_id) {
            return false;
        }
    }
    if (filter.address && device.record.address != *filter.address) return false;
    if (filter.cameras_only && !device.record.family) return false;
    return true;
}

std::vector<RawDevice> select_many(const DeviceFilter& filter) {
    std::vector<RawDevice> matched;
    for (auto& device : scan_devices()) {
        if (matches(device, filter)) matched.push_back(std::move(device));
    }
    if (matched.empty()) throw Error("没有找到符合条件的 USB 设备");
    return matched;
}

RawDevice select_one(const DeviceFilter& filter) {
    auto matched = select_many(filter);
    if (matched.size() != 1) {
        throw Error("匹配到 " + std::to_string(matched.size()) + " 个设备，请用 --vid、--pid、--bus 或 --addr 收窄");
    }
    return matched.front();
}

libusb_device* find_libusb(libusb_context* ctx, int busnum, int devnum) {
    libusb_device** list = nullptr;
    const auto count = libusb_get_device_list(ctx, &list);
    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        if (libusb_get_bus_number(list[i]) == busnum && libusb_get_device_address(list[i]) == devnum) {
            found = libusb_ref_device(list[i]);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

libusb_device* find_for(libusb_context* ctx, const RawDevice& raw) {
    if (raw.busnum > 0 || raw.devnum > 0) {
        if (auto* found = find_libusb(ctx, raw.busnum, raw.devnum)) return found;
        if (raw.busnum > 0) return nullptr;
    }
    libusb_device** list = nullptr;
    const auto count = libusb_get_device_list(ctx, &list);
    if (count < 0) return nullptr;
    libusb_device* found = nullptr;
    int hits = 0;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != raw.record.vendor_id || desc.idProduct != raw.record.product_id) continue;
        ++hits;
        if (found) libusb_unref_device(found);
        found = libusb_ref_device(list[i]);
    }
    libusb_free_device_list(list, 1);
    if (hits != 1) {
        if (found) libusb_unref_device(found);
        return nullptr;
    }
    return found;
}

struct UsbSession {
    libusb_context* ctx = nullptr;
    UsbSession() {
        if (libusb_init(&ctx) != 0) throw Error("libusb 初始化失败");
    }
    ~UsbSession() {
        if (ctx) libusb_exit(ctx);
    }
    UsbSession(const UsbSession&) = delete;
    UsbSession& operator=(const UsbSession&) = delete;
};
// UsbSession 是一次 libusb 会话的自动开关。创建它时调用 libusb_init 得到上下文 ctx；这个对象离开作用域时，析构函数调用 libusb_exit 把上下文放掉。初始化失败会直接抛出「libusb 初始化失败」。

TransferKind kind_from_attr(std::uint8_t attr) {
    switch (attr & 0x03) {
        case 1:
            return TransferKind::Isochronous;
        case 2:
            return TransferKind::Bulk;
        case 3:
            return TransferKind::Interrupt;
        default:
            return TransferKind::Control;
    }
}

EndpointRecord parse_endpoint(LinkSpeed speed, const libusb_endpoint_descriptor& ep) {
    // parse_endpoint 把描述符里的一个端点拆成记录，并算出这个周期端点每秒最多能排多少字节。
    EndpointRecord record;
    record.address = ep.bEndpointAddress;
    record.direction = (ep.bEndpointAddress & 0x80) ? "in" : "out"; // 地址的最高位决定方向：置位是 IN（设备到主机），否则是 OUT。
    record.transfer = kind_from_attr(ep.bmAttributes); // bmAttributes 的低两位决定传输类型：控制、等时、批量或中断。
    record.interval = ep.bInterval; // bInterval 原样留下，表示主机多久服务一次这个端点。
    const bool superspeed = speed == LinkSpeed::Super || speed == LinkSpeed::SuperPlus10 || speed == LinkSpeed::SuperPlus20;
    if (!superspeed && speed != LinkSpeed::Unknown) {
        // 类型 0x30 是 SuperSpeed 端点伴随描述符。
        // bMaxBurst + 1 是一次能连发的包数；等时端点再取 Mult，即 (bmAttributes & 0x3) + 1。
        // 里面的 wBytesPerInterval 若不为 0，就是每个服务间隔声明的字节数。
        constexpr int kPacketBits = 11;
        record.max_packet = ep.wMaxPacketSize & ((1u<<kPacketBits) - 1);
        record.packets_per_microframe = static_cast<std::uint8_t>(((ep.wMaxPacketSize >> 11) & 0x3) + 1);
    } else {
        record.packets_per_microframe = 1;
        record.max_packet = ep.wMaxPacketSize;
    }
    const auto* extra = ep.extra;
    int left = ep.extra_length;
    while (extra && left >= 2) {
        const int len = extra[0];
        const int type = extra[1];
        if (len < 2 || len > left) break;
        if (type == 0x30 && len >= 6) {
            record.burst_packets = static_cast<std::uint32_t>(extra[2]) + 1;
            if (record.transfer == TransferKind::Isochronous) record.mult_packets = (extra[3] & 0x03) + 1;
            const auto declared = static_cast<std::uint32_t>(extra[4] | (extra[5] << 8));
            if (declared > 0) record.bytes_per_interval = declared;
        } else if (type == 0x31 && len >= 8) {
            const auto declared = static_cast<std::uint32_t>(extra[4] | (extra[5] << 8) | (extra[6] << 16) | (extra[7] << 24));
            if (declared > 0) record.bytes_per_interval = declared;
        }
        extra += len;
        left -= len;
    }
    record.periodic_ceiling_bytes_per_sec = periodic_ceiling_bytes_per_sec(PeriodicEndpoint{
        speed, record.transfer, record.max_packet, record.packets_per_microframe, record.interval,
        record.bytes_per_interval, record.burst_packets, record.mult_packets});
    return record;
}

std::string open_error(const DeviceRecord& device, int rc) {
    std::ostringstream os;
    os << "打开 " << std::hex << device.vendor_id << ":" << device.product_id << std::dec << " 失败: " << libusb_error_name(rc)
#if defined(_WIN32)
       << "。Windows 上 rtt 和 bulk 需要 WinUSB。相机视频请留着系统 UVC 驱动，用 stream 测";
#elif defined(__APPLE__)
       << "。macOS 上打开 USB 设备通常需要 root。相机视频请用 stream";
#else
       << "。Linux 上需要 /dev/bus/usb 的写权限，可参考 contrib/60-usb-linkbench.rules";
#endif
    return os.str();
}

ConfigRecord parse_config_blob(LinkSpeed speed, const unsigned char* data, std::size_t len, int active,
                               std::optional<std::uint64_t>* ceiling) {
    ConfigRecord config;
    if (!data || len < 9 || data[1] != 0x02) return config;
    config.value = data[5];
    config.active = config.value == active;
    std::size_t i = data[0];
    InterfaceRecord* iface = nullptr;
    AltRecord* alt = nullptr;
    while (i + 1 < len) {
        const unsigned dlen = data[i];
        const unsigned dtype = data[i + 1];
        if (dlen < 2 || i + dlen > len) break;
        if (dtype == 0x04 && dlen >= 9) {
            const auto number = data[i + 2];
            if (!iface || iface->number != number) {
                config.interfaces.push_back(InterfaceRecord{});
                iface = &config.interfaces.back();
                iface->number = number;
            }
            iface->alternates.push_back(AltRecord{});
            alt = &iface->alternates.back();
            alt->alternate = data[i + 3];
            alt->class_name = class_name(data[i + 5], data[i + 6]);
        } else if (dtype == 0x05 && dlen >= 7 && alt) {
            std::size_t extra = i + dlen;
            while (extra + 1 < len) {
                const unsigned elen = data[extra];
                const unsigned etype = data[extra + 1];
                if (elen < 2 || extra + elen > len) break;
                if (etype == 0x04 || etype == 0x05) break;
                extra += elen;
            }
            libusb_endpoint_descriptor ep{};
            ep.bLength = data[i];
            ep.bDescriptorType = data[i + 1];
            ep.bEndpointAddress = data[i + 2];
            ep.bmAttributes = data[i + 3];
            ep.wMaxPacketSize = static_cast<std::uint16_t>(data[i + 4] | (data[i + 5] << 8));
            ep.bInterval = data[i + 6];
            if (extra > i + dlen) {
                ep.extra = data + i + dlen;
                ep.extra_length = static_cast<int>(extra - (i + dlen));
            }
            auto endpoint = parse_endpoint(speed, ep);
            if (ceiling && endpoint.periodic_ceiling_bytes_per_sec) {
                *ceiling = std::max(ceiling->value_or(0), *endpoint.periodic_ceiling_bytes_per_sec);
            }
            alt->endpoints.push_back(std::move(endpoint));
            i = extra;
            continue;
        }
        i += dlen;
    }
    return config;
}

}  // namespace

std::optional<LinkSpeed> live_link_speed(std::uint16_t vendor, std::uint16_t product, const std::string& bus_id,
                                        std::uint8_t address) {
    const std::filesystem::path root = "/sys/bus/usb/devices";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    auto trim = [](const std::filesystem::path& path) {
        std::ifstream in(path);
        std::string text;
        std::getline(in, text);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        return text;
    };
    std::optional<LinkSpeed> only;
    int matches = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const auto name = entry.path().filename().string();
        if (name.find(':') != std::string::npos) continue;
        unsigned long vid = 0;
        unsigned long pid = 0;
        try {
            vid = std::stoul(trim(entry.path() / "idVendor"), nullptr, 16);
            pid = std::stoul(trim(entry.path() / "idProduct"), nullptr, 16);
        } catch (...) {
            continue;
        }
        if (vid != vendor || pid != product) continue;
        const auto speed_text = trim(entry.path() / "speed");
        const auto speed = speed_from_sysfs(speed_text);
        if (!speed) continue;
        ++matches;
        only = speed;
        std::string bus;
        unsigned long addr = 0;
        try {
            bus = trim(entry.path() / "busnum");
            addr = std::stoul(trim(entry.path() / "devnum"), nullptr, 10);
        } catch (...) {
            addr = 0;
        }
        if (bus == bus_id && addr == address) return speed;
    }
    if (matches == 1) return only;
    return std::nullopt;
}

ListReport list_devices(const DeviceFilter& filter) {
    ListReport report;
#if defined(__linux__)
    const bool sysfs = std::filesystem::exists("/sys/bus/usb/devices");
#else
    const bool sysfs = false;
#endif
    for (const auto& device : scan_devices()) {
        if (matches(device, filter)) report.devices.push_back(device.record);
    }
#if defined(__linux__)
    if (sysfs) {
        report.notes = {"speed 是操作系统报告的协商链路速率，不是吞吐。",
                        "Linux sysfs 的 speed 能区分 10 Gbit/s 和 20 Gbit/s。"};
    } else {
        report.notes = {"speed 是操作系统报告的协商链路速率，不是吞吐。",
                        "这次没有读到 Linux sysfs，速率来自 libusb。"};
    }
#elif defined(_WIN32)
    (void)sysfs;
    report.notes = {"speed 是操作系统报告的协商链路速率，不是吞吐。",
                    "Windows 用集线器 IOCTL 读速率，SuperSpeed 不一定能拆开 10 Gbit/s 和 20 Gbit/s。",
                    "视频节点来自 Media Foundation。rtt、bulk 要 WinUSB；装上之后 UVC 采集会消失，测视频就不要换驱动。"};
#elif defined(__APPLE__)
    (void)sysfs;
    report.notes = {"speed 是操作系统报告的协商链路速率，不是吞吐。",
                    "macOS 用 libusb 读协商速率，10 Gbit/s 和 20 Gbit/s 不一定能拆开。视频节点是 AVFoundation 的 uniqueID。"};
#else
    (void)sysfs;
    report.notes = {"speed 是操作系统报告的协商链路速率，不是吞吐。", "速率来自 libusb。"};
#endif
    return report;
}
// scan_sysfs() 读取 Linux 内核在 /sys/bus/usb/devices 里列出的 USB 设备，它把身份和协商速率读取出来，整理成一份 RawDevice 列表。
std::vector<std::string> video_nodes_for(const DeviceFilter& filter) {
    std::vector<std::string> nodes;
    for (const auto& device : select_many(filter)) {
        nodes.insert(nodes.end(), device.record.video_nodes.begin(), device.record.video_nodes.end());
    }
    return nodes;
}

AuditReport audit_one(const DeviceFilter& filter) {
    //只读一台 USB 设备的描述符，算出它的周期端点理论上限
    const auto raw = select_one(filter); // 先按厂商号、产品号、总线和地址从 sysfs 里确定唯一的设备。
    UsbSession session;
    auto* device = find_for(session.ctx, raw);
    if (!device) {
#if defined(_WIN32)
        if (auto hub = read_hub_configs(raw.record.vendor_id, raw.record.product_id, raw.record.sysfs_path.value_or(""))) {
            AuditReport report;
            report.device = raw.record;
            std::optional<std::uint64_t> ceiling;
            for (const auto& blob : hub->configurations) {
                auto config = parse_config_blob(raw.record.speed, blob.data(), blob.size(), hub->active, &ceiling);
                report.configurations.push_back(std::move(config));
            }
            report.max_periodic_ceiling_bytes_per_sec = ceiling;
            report.notes = {"periodic_ceiling_bytes_per_sec 由端点描述符和服务间隔公式算出，不是实测吞吐。",
                            "描述符从 USB 集线器 IOCTL 读取，没有占用接口。",
                            "Bulk 端点没有服务间隔预算。相机视频通常走批量或等时端点，用 stream 测载荷。"};
            if (report.configurations.empty()) throw Error("集线器没有返回配置描述符");
            return report;
        }
#endif
        throw Error("libusb 没有找到 " + raw.record.sysfs_path.value_or(std::string("设备")));
    }
    libusb_device_descriptor desc{};
    libusb_get_device_descriptor(device, &desc);
    libusb_device_handle* handle = nullptr;
    const int opened = libusb_open(device, &handle);
    int active = -1;
    if (opened == 0) {
        libusb_config_descriptor* current = nullptr;
        if (libusb_get_active_config_descriptor(device, &current) == 0 && current) {
            active = current->bConfigurationValue;
            libusb_free_config_descriptor(current);
        }
        libusb_close(handle);
    }
    AuditReport report;
    report.device = raw.record;
    for (std::uint8_t i = 0; i < desc.bNumConfigurations; ++i) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(device, i, &cfg) != 0 || !cfg) continue;
        ConfigRecord config;
        config.value = cfg->bConfigurationValue;
        config.active = config.value == active;
        for (int iface = 0; iface < cfg->bNumInterfaces; ++iface) {
            InterfaceRecord interface;
            interface.number = cfg->interface[iface].altsetting[0].bInterfaceNumber;
            for (int alt_i = 0; alt_i < cfg->interface[iface].num_altsetting; ++alt_i) {
                const auto& alt = cfg->interface[iface].altsetting[alt_i];
                AltRecord alternate;
                alternate.alternate = alt.bAlternateSetting;
                alternate.class_name = class_name(alt.bInterfaceClass, alt.bInterfaceSubClass);
                for (int ep = 0; ep < alt.bNumEndpoints; ++ep) {
                    auto endpoint = parse_endpoint(raw.record.speed, alt.endpoint[ep]);
                    if (endpoint.periodic_ceiling_bytes_per_sec) {
                        report.max_periodic_ceiling_bytes_per_sec =
                            std::max(report.max_periodic_ceiling_bytes_per_sec.value_or(0),
                                     *endpoint.periodic_ceiling_bytes_per_sec);
                    }
                    alternate.endpoints.push_back(std::move(endpoint));
                }
                interface.alternates.push_back(std::move(alternate));
            }
            config.interfaces.push_back(std::move(interface));
        }
        libusb_free_config_descriptor(cfg);
        report.configurations.push_back(std::move(config));
    }
    libusb_unref_device(device);
    report.notes = {"periodic_ceiling_bytes_per_sec 由端点描述符和服务间隔公式算出，不是实测吞吐。",
                    "Bulk 端点没有服务间隔预算。相机视频通常走批量或等时端点，用 stream 测载荷。"};
    return report;
}

RttReport control_rtt(const DeviceFilter& filter, const RttOptions& options) {
    if (options.count == 0) throw Error("count 必须大于 0");
    const auto raw = select_one(filter);
    UsbSession session;
    auto* device = find_for(session.ctx, raw);
    if (!device) throw Error("libusb 没有找到设备");
    libusb_device_handle* handle = nullptr;
    const int opened = libusb_open(device, &handle);
    libusb_unref_device(device);
    if (opened != 0) throw Error(open_error(raw.record, opened));
    std::vector<std::uint64_t> samples;
    std::uint32_t errors = 0;
    for (std::uint32_t i = 0; i < options.warmup + options.count; ++i) {
        unsigned char data[2] = {};
        const auto started = now_ns();
        const int rc = libusb_control_transfer(handle, 0x80, 0x00, 0, 0, data, 2,
                                               static_cast<unsigned>(options.timeout_ms));
        const auto elapsed = now_ns() - started;
        if (i < options.warmup) continue;
        if (rc == 2) samples.push_back(elapsed);
        else ++errors;
    }
    libusb_close(handle);
    auto summary = summarize(samples);
    if (!summary) throw Error("没有成功的 GET_STATUS 样本，失败 " + std::to_string(errors) + " 次");
    RttReport report;
    report.device = raw.record;
    report.warmup_discarded = options.warmup;
    report.timeout_ms = options.timeout_ms;
    report.samples_ns = *summary;
    report.timer_overhead_ns = *summarize(overhead_samples(200));
    if (options.keep_samples) report.sample_ns = samples;
    report.errors = errors;
    return report;
}

namespace {

struct BulkSlot {
    libusb_transfer* transfer = nullptr;
    std::vector<unsigned char> buffer;
    bool complete = false;
    int status = 0;
    int actual = 0;
};

void bulk_callback(libusb_transfer* transfer) {
    auto* slot = static_cast<BulkSlot*>(transfer->user_data);
    slot->complete = true;
    slot->status = transfer->status;
    slot->actual = transfer->actual_length;
}

}  // namespace

BulkReport bulk_in(const DeviceFilter& filter, const BulkOptions& options) {
    if (options.seconds <= 0 || options.buffer_bytes == 0) throw Error("秒数和缓冲区必须大于 0");
    if (options.buffer_bytes > 16 * 1024 * 1024) throw Error("缓冲区最大 16 MiB");
    const auto raw = select_one(filter);
    UsbSession session;
    auto* device = find_for(session.ctx, raw);
    if (!device) throw Error("libusb 没有找到设备");
    libusb_device_handle* handle = nullptr;
    const int opened = libusb_open(device, &handle);
    libusb_unref_device(device);
    if (opened != 0) throw Error(open_error(raw.record, opened));
    bool reattached = false;
    if (options.detach && libusb_kernel_driver_active(handle, options.interface) == 1) {
        if (libusb_detach_kernel_driver(handle, options.interface) != 0) {
            libusb_close(handle);
            throw Error("卸载内核驱动失败");
        }
        reattached = true;
    }
    if (libusb_claim_interface(handle, options.interface) != 0) {
        if (reattached) libusb_attach_kernel_driver(handle, options.interface);
        libusb_close(handle);
        throw Error("无法占用接口。相机视频接口通常由内核占用，请改用 stream；只有确认要独占设备时才加 --detach");
    }
    constexpr int kDepth = 4;
    std::vector<BulkSlot> slots(kDepth);
    for (auto& slot : slots) {
        slot.complete = true;
        slot.buffer.resize(options.buffer_bytes);
        slot.transfer = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(slot.transfer, handle, options.endpoint, slot.buffer.data(),
                                  static_cast<int>(slot.buffer.size()), bulk_callback, &slot,
                                  static_cast<unsigned>(options.timeout_ms));
    }
    auto submit = [&](BulkSlot& slot) {
        slot.complete = false;
        slot.transfer->length = static_cast<int>(slot.buffer.size());
        slot.transfer->actual_length = 0;
        const int rc = libusb_submit_transfer(slot.transfer);
        if (rc != 0) throw Error(std::string("提交 Bulk 传输失败: ") + libusb_error_name(rc));
    };
    try {
        for (std::uint32_t i = 0; i < options.warmup; ++i) {
            submit(slots[0]);
            while (!slots[0].complete) libusb_handle_events_completed(session.ctx, nullptr);
            if (slots[0].status != LIBUSB_TRANSFER_COMPLETED) throw Error("预热传输失败");
        }
        const auto window_ns = static_cast<std::uint64_t>(options.seconds * 1e9);
        const auto started = now_ns();
        for (auto& slot : slots) submit(slot);
        std::uint64_t bytes = 0;
        std::uint64_t reads = 0;
        bool failed = false;
        while (!failed && elapsed_ns(started) < window_ns) {
            timeval tv{0, 200000};
            libusb_handle_events_timeout_completed(session.ctx, &tv, nullptr);
            for (auto& slot : slots) {
                if (!slot.complete) continue;
                if (slot.status != LIBUSB_TRANSFER_COMPLETED) {
                    failed = true;
                    break;
                }
                bytes += static_cast<std::uint64_t>(slot.actual);
                ++reads;
                if (elapsed_ns(started) < window_ns) submit(slot);
            }
        }
        const auto elapsed = elapsed_ns(started);
        for (auto& slot : slots) {
            if (slot.transfer && !slot.complete) libusb_cancel_transfer(slot.transfer);
        }
        for (;;) {
            bool pending = false;
            for (const auto& slot : slots) pending = pending || (slot.transfer && !slot.complete);
            if (!pending) break;
            timeval tv{0, 200000};
            libusb_handle_events_timeout_completed(session.ctx, &tv, nullptr);
        }
        for (auto& slot : slots) {
            if (slot.transfer) libusb_free_transfer(slot.transfer);
            slot.transfer = nullptr;
        }
        libusb_release_interface(handle, options.interface);
        if (reattached) libusb_attach_kernel_driver(handle, options.interface);
        libusb_close(handle);
        handle = nullptr;
        if (reads == 0 || elapsed == 0) throw Error("Bulk IN 没有在超时前返回数据。这个端点可能不是持续的数据源。");
        BulkReport report;
        report.device = raw.record;
        report.interface = options.interface;
        report.endpoint = options.endpoint;
        report.queue_depth = kDepth;
        report.detached_kernel_driver = options.detach;
        report.warmup_discarded = options.warmup;
        report.reads = reads;
        report.bytes = bytes;
        report.elapsed_ns = elapsed;
        report.bytes_per_sec = static_cast<double>(bytes) / (static_cast<double>(elapsed) / 1e9);
        report.timer_overhead_ns = *summarize(overhead_samples(200));
        return report;
    } catch (...) {
        for (auto& slot : slots) {
            if (slot.transfer && !slot.complete) libusb_cancel_transfer(slot.transfer);
        }
        for (auto& slot : slots) {
            if (slot.transfer) libusb_free_transfer(slot.transfer);
            slot.transfer = nullptr;
        }
        if (handle) {
            libusb_release_interface(handle, options.interface);
            if (reattached) libusb_attach_kernel_driver(handle, options.interface);
            libusb_close(handle);
        }
        throw;
    }
}

}  // namespace linkbench
