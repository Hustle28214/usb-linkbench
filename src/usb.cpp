#include "linkbench.hpp"

#include <libusb-1.0/libusb.h>

#include <sys/time.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace linkbench {
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
    for (auto& device : scan_sysfs()) {
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
    EndpointRecord record;
    record.address = ep.bEndpointAddress;
    record.direction = (ep.bEndpointAddress & 0x80) ? "in" : "out";
    record.transfer = kind_from_attr(ep.bmAttributes);
    record.interval = ep.bInterval;
    const bool superspeed = speed == LinkSpeed::Super || speed == LinkSpeed::SuperPlus10 || speed == LinkSpeed::SuperPlus20;
    if (!superspeed && speed != LinkSpeed::Unknown) {
        record.packets_per_microframe = static_cast<std::uint8_t>(((ep.wMaxPacketSize >> 11) & 0x3) + 1);
        record.max_packet = ep.wMaxPacketSize & 0x07ff;
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
       << "。Linux 上需要 /dev/bus/usb 的写权限，可参考 contrib/60-usb-linkbench.rules";
    return os.str();
}

}  // namespace

ListReport list_devices(const DeviceFilter& filter) {
    ListReport report;
    for (const auto& device : scan_sysfs()) {
        if (matches(device, filter)) report.devices.push_back(device.record);
    }
    report.notes = {"speed 是操作系统报告的协商链路速率，不是吞吐。",
                    "Linux sysfs 的 speed 能区分 10 Gbit/s 和 20 Gbit/s。"};
    return report;
}

std::vector<std::string> video_nodes_for(const DeviceFilter& filter) {
    std::vector<std::string> nodes;
    for (const auto& device : select_many(filter)) {
        nodes.insert(nodes.end(), device.record.video_nodes.begin(), device.record.video_nodes.end());
    }
    return nodes;
}

AuditReport audit_one(const DeviceFilter& filter) {
    const auto raw = select_one(filter);
    UsbSession session;
    auto* device = find_libusb(session.ctx, raw.busnum, raw.devnum);
    if (!device) throw Error("libusb 没有找到 " + *raw.record.sysfs_path);
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
    auto* device = find_libusb(session.ctx, raw.busnum, raw.devnum);
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
    auto* device = find_libusb(session.ctx, raw.busnum, raw.devnum);
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
