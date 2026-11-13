#include "usb_scan.hpp"

#include "os.hpp"

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>
#include <usbioctl.h>
#include <usbspec.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace linkbench {
namespace {

const GUID kHubGuid = {0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};
const GUID kDeviceGuid = {0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};

struct Hub {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    Hub() = default;
    Hub(std::wstring path_in, HANDLE handle_in) : path(std::move(path_in)), handle(handle_in) {}
    Hub(Hub&& other) noexcept : path(std::move(other.path)), handle(other.handle) { other.handle = INVALID_HANDLE_VALUE; }
    Hub& operator=(Hub&&) = delete;
    Hub(const Hub&) = delete;
    ~Hub() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

std::string bcd_usb(std::uint16_t bcd) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%02x", (bcd >> 8) & 0xff, bcd & 0xff);
    return buf;
}

std::string registry_text(HDEVINFO info, SP_DEVINFO_DATA* device, DWORD property) {
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyW(info, device, property, nullptr, nullptr, 0, &bytes);
    if (bytes < sizeof(wchar_t)) return {};
    std::vector<unsigned char> buf(bytes + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(info, device, property, nullptr, buf.data(), static_cast<DWORD>(buf.size()), nullptr)) {
        return {};
    }
    return wide_to_utf8(reinterpret_cast<const wchar_t*>(buf.data()), static_cast<int>(bytes / sizeof(wchar_t)));
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

std::vector<unsigned char> descriptor(HANDLE hub, ULONG port, USHORT value, USHORT index, USHORT length) {
    std::vector<unsigned char> buf(sizeof(USB_DESCRIPTOR_REQUEST) + length);
    auto* req = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buf.data());
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = 0x80;
    req->SetupPacket.bRequest = 0x06;
    req->SetupPacket.wValue = value;
    req->SetupPacket.wIndex = index;
    req->SetupPacket.wLength = length;
    DWORD got = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf.data(), static_cast<DWORD>(buf.size()), buf.data(),
                         static_cast<DWORD>(buf.size()), &got, nullptr)) {
        return {};
    }
    if (got <= sizeof(USB_DESCRIPTOR_REQUEST)) return {};
    return std::vector<unsigned char>(buf.begin() + static_cast<std::ptrdiff_t>(sizeof(USB_DESCRIPTOR_REQUEST)),
                                      buf.begin() + static_cast<std::ptrdiff_t>(got));
}

std::string usb_string(HANDLE hub, ULONG port, std::uint8_t index) {
    if (index == 0) return {};
    auto data = descriptor(hub, port, static_cast<USHORT>((0x03 << 8) | index), 0x0409, 255);
    if (data.size() < 4 || data[1] != 0x03) data = descriptor(hub, port, static_cast<USHORT>((0x03 << 8) | index), 0, 255);
    if (data.size() < 4 || data[1] != 0x03) return {};
    const int chars = (static_cast<int>(data[0]) - 2) / 2;
    if (chars <= 0 || static_cast<std::size_t>(2 + chars * 2) > data.size()) return {};
    return wide_to_utf8(reinterpret_cast<const wchar_t*>(data.data() + 2), chars);
}

std::vector<unsigned char> config_blob(HANDLE hub, ULONG port, std::uint8_t index) {
    auto head = descriptor(hub, port, static_cast<USHORT>((0x02 << 8) | index), 0, 9);
    if (head.size() < 9 || head[1] != 0x02) return {};
    const auto total = static_cast<USHORT>(head[2] | (head[3] << 8));
    if (total < 9) return {};
    auto full = descriptor(hub, port, static_cast<USHORT>((0x02 << 8) | index), 0, total);
    if (full.size() < 9 || full[1] != 0x02) return {};
    return full;
}

bool blob_has_video(const std::vector<unsigned char>& blob) {
    std::size_t i = 0;
    while (i + 1 < blob.size()) {
        const unsigned len = blob[i];
        const unsigned type = blob[i + 1];
        if (len < 2 || i + len > blob.size()) break;
        if (type == 0x04 && len >= 6 && blob[i + 5] == 0x0e) return true;
        i += len;
    }
    return false;
}

LinkSpeed speed_of(unsigned char speed, HANDLE hub, ULONG port) {
    LinkSpeed parsed = LinkSpeed::Unknown;
    switch (speed) {
        case UsbLowSpeed:
            parsed = LinkSpeed::Low;
            break;
        case UsbFullSpeed:
            parsed = LinkSpeed::Full;
            break;
        case UsbHighSpeed:
            parsed = LinkSpeed::High;
            break;
        case UsbSuperSpeed:
            parsed = LinkSpeed::Super;
            break;
        default:
            break;
    }
#if NTDDI_VERSION >= 0x06020000
    USB_NODE_CONNECTION_INFORMATION_EX_V2 extra{};
    extra.ConnectionIndex = port;
    extra.Length = sizeof(extra);
    DWORD got = 0;
    if (DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, &extra, sizeof(extra), &extra, sizeof(extra), &got,
                        nullptr) &&
        extra.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher) {
        parsed = LinkSpeed::SuperPlus10;
    }
#else
    (void)hub;
    (void)port;
#endif
    return parsed;
}

std::vector<Hub> open_hubs() {
    std::vector<Hub> hubs;
    HDEVINFO info = SetupDiGetClassDevsW(&kHubGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return hubs;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &kHubGuid, index, &iface); ++index) {
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<unsigned char> buf(bytes);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, bytes, nullptr, nullptr)) continue;
        HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            handle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr);
        }
        if (handle == INVALID_HANDLE_VALUE) continue;
        hubs.emplace_back(detail->DevicePath, handle);
    }
    SetupDiDestroyDeviceInfoList(info);
    return hubs;
}

HostUsbDevice device_from_connection(HANDLE hub, int hub_index, ULONG port, const USB_NODE_CONNECTION_INFORMATION_EX& conn) {
    HostUsbDevice raw;
    const auto& desc = conn.DeviceDescriptor;
    raw.record.vendor_id = desc.idVendor;
    raw.record.product_id = desc.idProduct;
    raw.record.bus_id = std::to_string(hub_index);
    raw.record.address = static_cast<std::uint8_t>(conn.DeviceAddress <= 255 ? conn.DeviceAddress : port);
    raw.record.sysfs_path = std::to_string(hub_index) + "-" + std::to_string(port);
    raw.record.speed = speed_of(conn.Speed, hub, port);
    raw.record.speed_source = "hub";
    raw.record.line_mbit = line_mbit_per_sec(raw.record.speed);
    raw.record.usb_bcd = bcd_usb(desc.bcdUSB);
    auto manufacturer = usb_string(hub, port, desc.iManufacturer);
    auto product = usb_string(hub, port, desc.iProduct);
    auto serial = usb_string(hub, port, desc.iSerialNumber);
    if (!manufacturer.empty()) raw.record.manufacturer = manufacturer;
    if (!product.empty()) raw.record.product = product;
    if (!serial.empty()) raw.record.serial = serial;
    if (auto camera = identify_camera(raw.record.vendor_id, raw.record.product_id, raw.record.product)) {
        raw.record.family = camera->family;
        raw.record.catalog_name = camera->catalog_name;
    }
    for (std::uint8_t cfg = 0; cfg < desc.bNumConfigurations && !raw.record.has_video_class; ++cfg) {
        if (blob_has_video(config_blob(hub, port, cfg))) raw.record.has_video_class = true;
    }
    return raw;
}

std::vector<HostUsbDevice> scan_device_interfaces() {
    std::vector<HostUsbDevice> devices;
    HDEVINFO info = SetupDiGetClassDevsW(&kDeviceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return devices;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(info, index, &device); ++index) {
        const auto hardware = registry_text(info, &device, SPDRP_HARDWAREID);
        std::uint16_t vendor = 0;
        std::uint16_t product = 0;
        if (!parse_vid_pid(hardware, &vendor, &product)) continue;
        HostUsbDevice raw;
        raw.record.vendor_id = vendor;
        raw.record.product_id = product;
        raw.record.bus_id = std::to_string(index);
        raw.record.address = 0;
        wchar_t instance[512] = {};
        if (CM_Get_Device_IDW(device.DevInst, instance, 512, 0) == CR_SUCCESS) {
            raw.record.sysfs_path = wide_to_utf8(instance);
            const auto id = *raw.record.sysfs_path;
            const auto slash = id.rfind('\\');
            if (slash != std::string::npos && slash + 1 < id.size()) raw.record.serial = id.substr(slash + 1);
        }
        auto product_name = registry_text(info, &device, SPDRP_FRIENDLYNAME);
        if (product_name.empty()) product_name = registry_text(info, &device, SPDRP_DEVICEDESC);
        if (!product_name.empty()) raw.record.product = product_name;
        raw.record.speed = LinkSpeed::Unknown;
        raw.record.speed_source = "setupapi";
        if (auto camera = identify_camera(raw.record.vendor_id, raw.record.product_id, raw.record.product)) {
            raw.record.family = camera->family;
            raw.record.catalog_name = camera->catalog_name;
        }
        devices.push_back(std::move(raw));
    }
    SetupDiDestroyDeviceInfoList(info);
    return devices;
}

}  // namespace

std::vector<HostUsbDevice> scan_setupapi() {
    std::vector<HostUsbDevice> devices;
    auto hubs = open_hubs();
    int hub_index = 0;
    for (auto& hub : hubs) {
        ++hub_index;
        USB_NODE_INFORMATION node{};
        DWORD got = 0;
        if (!DeviceIoControl(hub.handle, IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), &node, sizeof(node), &got, nullptr)) {
            continue;
        }
        const int ports = node.u.HubInformation.HubDescriptor.bNumberOfPorts;
        for (int port = 1; port <= ports; ++port) {
            std::vector<unsigned char> buf(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 32 * sizeof(USB_PIPE_INFO));
            auto* conn = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buf.data());
            conn->ConnectionIndex = static_cast<ULONG>(port);
            if (!DeviceIoControl(hub.handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf.data(), static_cast<DWORD>(buf.size()),
                                 buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                continue;
            }
            if (conn->ConnectionStatus != DeviceConnected) continue;
            if (conn->DeviceDescriptor.idVendor == 0 && conn->DeviceDescriptor.idProduct == 0) continue;
            devices.push_back(device_from_connection(hub.handle, hub_index, static_cast<ULONG>(port), *conn));
        }
    }
    if (!devices.empty()) return devices;
    return scan_device_interfaces();
}

std::optional<HubConfigs> read_hub_configs(std::uint16_t vendor, std::uint16_t product, const std::string& location) {
    auto hubs = open_hubs();
    int hub_index = 0;
    for (auto& hub : hubs) {
        ++hub_index;
        USB_NODE_INFORMATION node{};
        DWORD got = 0;
        if (!DeviceIoControl(hub.handle, IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), &node, sizeof(node), &got, nullptr)) {
            continue;
        }
        const int ports = node.u.HubInformation.HubDescriptor.bNumberOfPorts;
        for (int port = 1; port <= ports; ++port) {
            std::vector<unsigned char> buf(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 32 * sizeof(USB_PIPE_INFO));
            auto* conn = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buf.data());
            conn->ConnectionIndex = static_cast<ULONG>(port);
            if (!DeviceIoControl(hub.handle, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf.data(), static_cast<DWORD>(buf.size()),
                                 buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                continue;
            }
            if (conn->ConnectionStatus != DeviceConnected) continue;
            const auto id = std::to_string(hub_index) + "-" + std::to_string(port);
            if (conn->DeviceDescriptor.idVendor != vendor || conn->DeviceDescriptor.idProduct != product) continue;
            if (!location.empty() && location != id) continue;
            HubConfigs configs;
            configs.active = conn->CurrentConfigurationValue;
            for (std::uint8_t cfg = 0; cfg < conn->DeviceDescriptor.bNumConfigurations; ++cfg) {
                auto blob = config_blob(hub.handle, static_cast<ULONG>(port), cfg);
                if (!blob.empty()) configs.configurations.push_back(std::move(blob));
            }
            if (!configs.configurations.empty()) return configs;
        }
    }
    return std::nullopt;
}

}  // namespace linkbench
