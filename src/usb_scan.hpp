#pragma once

#include "linkbench.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace linkbench {

struct HostUsbDevice {
    DeviceRecord record;
    int busnum = 0;
    int devnum = 0;
};

struct HubConfigs {
    int active = -1;
    std::vector<std::vector<unsigned char>> configurations;
};

std::vector<HostUsbDevice> scan_setupapi();
std::optional<HubConfigs> read_hub_configs(std::uint16_t vendor, std::uint16_t product, const std::string& location);

}  // namespace linkbench
