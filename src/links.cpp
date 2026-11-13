#include "linkbench.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace linkbench {
namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::string text;
    std::getline(in, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty()) return std::nullopt;
    return text;
}

std::optional<double> read_f64(const fs::path& path) {
    auto text = read_text(path);
    if (!text) return std::nullopt;
    try {
        return std::stod(*text);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> read_u32(const fs::path& path) {
    auto text = read_text(path);
    if (!text) return std::nullopt;
    try {
        return static_cast<std::uint32_t>(std::stoul(*text));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

LinksReport links(bool verbose) {
    LinksReport report;
#if !defined(__linux__)
    (void)verbose;
    report.thunderbolt_skip_reason = "只在 Linux 上读 /sys/bus/thunderbolt";
    report.notes.push_back("PCIe 已训练速率只在 Linux 上读 /sys/bus/pci。");
    return report;
#endif
    const fs::path thunderbolt_root = "/sys/bus/thunderbolt/devices";
    std::error_code ec;
    if (!fs::exists(thunderbolt_root, ec)) {
        report.thunderbolt_skip_reason = "没有 Thunderbolt sysfs（" + ec.message() + "）";
    } else {
        for (const auto& entry : fs::directory_iterator(thunderbolt_root, ec)) {
            const auto& path = entry.path();
            if (!entry.is_directory()) continue;
            if (!fs::is_regular_file(path / "tx_speed") && !fs::is_regular_file(path / "device_name") &&
                !fs::is_regular_file(path / "vendor_name")) {
                continue;
            }
            ThunderboltPort port;
            port.path = path.string();
            port.name = read_text(path / "device_name");
            port.vendor = read_text(path / "vendor_name");
            port.generation = read_text(path / "generation");
            port.tx_speed_gbps = read_f64(path / "tx_speed");
            port.rx_speed_gbps = read_f64(path / "rx_speed");
            port.tx_lanes = read_u32(path / "tx_lanes");
            port.rx_lanes = read_u32(path / "rx_lanes");
            port.authorized = read_text(path / "authorized");
            report.thunderbolt.push_back(std::move(port));
        }
        std::sort(report.thunderbolt.begin(), report.thunderbolt.end(),
                  [](const ThunderboltPort& a, const ThunderboltPort& b) { return a.path < b.path; });
    }
    std::map<std::string, std::size_t> counts;
    std::vector<PcieFunction> functions;
    const fs::path pci = "/sys/bus/pci/devices";
    if (fs::exists(pci, ec)) {
        for (const auto& entry : fs::directory_iterator(pci, ec)) {
            auto current_speed = read_text(entry.path() / "current_link_speed");
            auto current_width = read_u32(entry.path() / "current_link_width");
            if (!current_speed && !current_width) continue;
            auto max_speed = read_text(entry.path() / "max_link_speed");
            auto max_width = read_u32(entry.path() / "max_link_width");
            PcieFunction function;
            function.address = entry.path().filename().string();
            function.current_speed = current_speed;
            function.max_speed = max_speed;
            function.current_width = current_width;
            function.max_width = max_width;
            if (current_speed && max_speed && *current_speed != *max_speed) function.degraded = true;
            if (current_width && max_width && *current_width < *max_width) function.degraded = true;
            if (current_speed) ++counts[*current_speed];
            functions.push_back(std::move(function));
        }
    }
    std::sort(functions.begin(), functions.end(),
              [](const PcieFunction& a, const PcieFunction& b) { return a.address < b.address; });
    report.pcie_functions_with_link = functions.size();
    for (const auto& function : functions) {
        if (function.degraded) report.pcie_degraded.push_back(function);
    }
    for (const auto& [speed, count] : counts) report.pcie_speed_counts.emplace_back(speed, count);
    if (verbose) report.pcie_all = std::move(functions);
    return report;
}

}  // namespace linkbench
